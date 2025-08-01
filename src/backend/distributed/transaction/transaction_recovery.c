/*-------------------------------------------------------------------------
 *
 * transaction_recovery.c
 *
 * Routines for recovering two-phase commits started by this node if a
 * failure occurs between prepare and commit/abort.
 *
 * Copyright (c) Citus Data, Inc.
 *
 * $Id$
 *
 *-------------------------------------------------------------------------
 */

#include <sys/stat.h>
#include <unistd.h>

#include "postgres.h"

#include "libpq-fe.h"
#include "miscadmin.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/relscan.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "lib/stringinfo.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "storage/procarray.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/syscache.h"
#include "utils/xid8.h"

#include "pg_version_constants.h"

#include "distributed/backend_data.h"
#include "distributed/connection_management.h"
#include "distributed/listutils.h"
#include "distributed/metadata_cache.h"
#include "distributed/pg_dist_transaction.h"
#include "distributed/remote_commands.h"
#include "distributed/resource_lock.h"
#include "distributed/transaction_recovery.h"
#include "distributed/version_compat.h"
#include "distributed/worker_manager.h"


/* exports for SQL callable functions */
PG_FUNCTION_INFO_V1(recover_prepared_transactions);


/* Local functions forward declarations */
static int RecoverWorkerTransactions(WorkerNode *workerNode,
									 MultiConnection *connection);
static List * PendingWorkerTransactionList(MultiConnection *connection);
static bool IsTransactionInProgress(HTAB *activeTransactionNumberSet,
									char *preparedTransactionName);
static bool RecoverPreparedTransactionOnWorker(MultiConnection *connection,
											   char *transactionName, bool shouldCommit);


/*
 * recover_prepared_transactions recovers any pending prepared
 * transactions started by this node on other nodes.
 */
Datum
recover_prepared_transactions(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	int recoveredTransactionCount = RecoverTwoPhaseCommits();

	PG_RETURN_INT32(recoveredTransactionCount);
}


/*
 * LogTransactionRecord registers the fact that a transaction has been
 * prepared on a worker. The presence of this record indicates that the
 * prepared transaction should be committed.
 */
void
LogTransactionRecord(int32 groupId, char *transactionName, FullTransactionId outerXid)
{
	Datum values[Natts_pg_dist_transaction];
	bool isNulls[Natts_pg_dist_transaction];

	/* form new transaction tuple */
	memset(values, 0, sizeof(values));
	memset(isNulls, false, sizeof(isNulls));

	values[Anum_pg_dist_transaction_groupid - 1] = Int32GetDatum(groupId);
	values[Anum_pg_dist_transaction_gid - 1] = CStringGetTextDatum(transactionName);
	values[Anum_pg_dist_transaction_outerxid - 1] = FullTransactionIdGetDatum(outerXid);

	/* open transaction relation and insert new tuple */
	Relation pgDistTransaction = table_open(DistTransactionRelationId(),
											RowExclusiveLock);

	TupleDesc tupleDescriptor = RelationGetDescr(pgDistTransaction);
	HeapTuple heapTuple = heap_form_tuple(tupleDescriptor, values, isNulls);

	CATALOG_INSERT_WITH_SNAPSHOT(pgDistTransaction, heapTuple);

	CommandCounterIncrement();

	/* close relation and invalidate previous cache entry */
	table_close(pgDistTransaction, NoLock);
}


/*
 * RecoverTwoPhaseCommits recovers any pending prepared
 * transactions started by this node on other nodes.
 */
int
RecoverTwoPhaseCommits(void)
{
	int recoveredTransactionCount = 0;

	/* take advisory lock first to avoid running concurrently */
	LockTransactionRecovery(ShareUpdateExclusiveLock);

	List *workerList = ActivePrimaryNodeList(NoLock);
	List *workerConnections = NIL;
	WorkerNode *workerNode = NULL;
	MultiConnection *connection = NULL;

	/*
	 * Pre-establish all connections to worker nodes.
	 *
	 * We do this to enforce a consistent lock acquisition order and prevent deadlocks.
	 * Currently, during extension updates, we take strong locks on the Citus
	 * catalog tables in a specific order: first on pg_dist_authinfo, then on
	 * pg_dist_transaction. It's critical that any operation locking these two
	 * tables adheres to this order, or a deadlock could occur.
	 *
	 * Note that RecoverWorkerTransactions() retains its lock until the end
	 * of the transaction, while GetNodeConnection() releases its lock after
	 * the catalog lookup. So when there are multiple workers in the active primary
	 * node list, the lock acquisition order may reverse in subsequent iterations
	 * of the loop calling RecoverWorkerTransactions(), increasing the risk
	 * of deadlock.
	 *
	 * By establishing all worker connections upfront, we ensure that
	 * RecoverWorkerTransactions() deals with a single distributed catalog table,
	 * thereby preventing deadlocks regardless of the lock acquisition sequence
	 * used in the upgrade extension script.
	 */

	foreach_declared_ptr(workerNode, workerList)
	{
		int connectionFlags = 0;
		char *nodeName = workerNode->workerName;
		int nodePort = workerNode->workerPort;

		connection = GetNodeConnection(connectionFlags, nodeName, nodePort);
		Assert(connection != NULL);

		/*
		 * We don't verify connection validity here.
		 * Instead, RecoverWorkerTransactions() performs the necessary
		 * sanity checks on the connection state.
		 */
		workerConnections = lappend(workerConnections, connection);
	}
	forboth_ptr(workerNode, workerList, connection, workerConnections)
	{
		recoveredTransactionCount += RecoverWorkerTransactions(workerNode, connection);
	}

	return recoveredTransactionCount;
}


/*
 * RecoverWorkerTransactions recovers any pending prepared transactions
 * started by this node on the specified worker.
 */
static int
RecoverWorkerTransactions(WorkerNode *workerNode, MultiConnection *connection)
{
	int recoveredTransactionCount = 0;

	int32 groupId = workerNode->groupId;
	char *nodeName = workerNode->workerName;
	int nodePort = workerNode->workerPort;


	ScanKeyData scanKey[1];
	int scanKeyCount = 1;
	bool indexOK = true;
	HeapTuple heapTuple = NULL;

	HASH_SEQ_STATUS status;

	bool recoveryFailed = false;

	Assert(connection != NULL);
	if (connection->pgConn == NULL || PQstatus(connection->pgConn) != CONNECTION_OK)
	{
		ereport(WARNING, (errmsg("transaction recovery cannot connect to %s:%d",
								 nodeName, nodePort)));

		return 0;
	}

	MemoryContext localContext = AllocSetContextCreateInternal(CurrentMemoryContext,
															   "RecoverWorkerTransactions",
															   ALLOCSET_DEFAULT_MINSIZE,
															   ALLOCSET_DEFAULT_INITSIZE,
															   ALLOCSET_DEFAULT_MAXSIZE);

	MemoryContext oldContext = MemoryContextSwitchTo(localContext);

	Relation pgDistTransaction = table_open(DistTransactionRelationId(),
											RowExclusiveLock);
	TupleDesc tupleDescriptor = RelationGetDescr(pgDistTransaction);

	/*
	 * We're going to check the list of prepared transactions on the worker,
	 * but some of those prepared transactions might belong to ongoing
	 * distributed transactions.
	 *
	 * We could avoid this by temporarily blocking new prepared transactions
	 * from being created by taking an ExclusiveLock on pg_dist_transaction.
	 * However, this hurts write performance, so instead we avoid blocking
	 * by consulting the list of active distributed transactions, and follow
	 * a carefully chosen order to avoid race conditions:
	 *
	 * 1) P = prepared transactions on worker
	 * 2) A = active distributed transactions
	 * 3) T = pg_dist_transaction snapshot
	 * 4) Q = prepared transactions on worker
	 *
	 * By observing A after P, we get a conclusive answer to which distributed
	 * transactions we observed in P are still in progress. It is safe to recover
	 * the transactions in P - A based on the presence or absence of a record
	 * in T.
	 *
	 * We also remove records in T if there is no prepared transaction, which
	 * we assume means the transaction committed. However, a transaction could
	 * have left prepared transactions and committed between steps 1 and 2.
	 * In that case, we would incorrectly remove the records, while the
	 * prepared transaction is still in place.
	 *
	 * We therefore observe the set of prepared transactions one more time in
	 * step 4. The aforementioned transactions would show up in Q, but not in
	 * P. We can skip those transactions and recover them later.
	 */

	/* find stale prepared transactions on the remote node */
	List *pendingTransactionList = PendingWorkerTransactionList(connection);
	HTAB *pendingTransactionSet = ListToHashSet(pendingTransactionList, NAMEDATALEN,
												true);

	/* find in-progress distributed transactions */
	List *activeTransactionNumberList = ActiveDistributedTransactionNumbers();
	HTAB *activeTransactionNumberSet = ListToHashSet(activeTransactionNumberList,
													 sizeof(uint64), false);

	/* scan through all recovery records of the current worker */
	ScanKeyInit(&scanKey[0], Anum_pg_dist_transaction_groupid,
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(groupId));

	/* get a snapshot of pg_dist_transaction */
	SysScanDesc scanDescriptor = systable_beginscan(pgDistTransaction,
													DistTransactionGroupIndexId(),
													indexOK,
													NULL, scanKeyCount, scanKey);

	/* find stale prepared transactions on the remote node */
	List *recheckTransactionList = PendingWorkerTransactionList(connection);
	HTAB *recheckTransactionSet = ListToHashSet(recheckTransactionList, NAMEDATALEN,
												true);

	while (HeapTupleIsValid(heapTuple = systable_getnext(scanDescriptor)))
	{
		bool isNull = false;
		bool foundPreparedTransactionBeforeCommit = false;
		bool foundPreparedTransactionAfterCommit = false;

		Datum transactionNameDatum = heap_getattr(heapTuple,
												  Anum_pg_dist_transaction_gid,
												  tupleDescriptor, &isNull);
		char *transactionName = TextDatumGetCString(transactionNameDatum);

		bool isTransactionInProgress = IsTransactionInProgress(activeTransactionNumberSet,
															   transactionName);
		if (isTransactionInProgress)
		{
			/*
			 * Do not touch in progress transactions as we might mistakenly
			 * commit a transaction that is actually in the process of
			 * aborting or vice-versa.
			 */
			continue;
		}

		bool outerXidIsNull = false;
		Datum outerXidDatum = 0;
		if (EnableVersionChecks ||
			SearchSysCacheExistsAttName(DistTransactionRelationId(), "outer_xid"))
		{
			/* Check if the transaction is created by an outer transaction from a non-main database */
			outerXidDatum = heap_getattr(heapTuple,
										 Anum_pg_dist_transaction_outerxid,
										 tupleDescriptor, &outerXidIsNull);
		}
		else
		{
			/*
			 * Normally we don't try to recover prepared transactions when the
			 * binary version doesn't match the sql version. However, we skip
			 * those checks in regression tests by disabling
			 * citus.enable_version_checks. And when this is the case, while
			 * the C code looks for "outer_xid" attribute, pg_dist_transaction
			 * doesn't yet have it.
			 */
			Assert(!EnableVersionChecks);
		}

		TransactionId outerXid = 0;
		if (!outerXidIsNull)
		{
			FullTransactionId outerFullXid = DatumGetFullTransactionId(outerXidDatum);
			outerXid = XidFromFullTransactionId(outerFullXid);
		}

		if (outerXid != 0)
		{
			bool outerXactIsInProgress = TransactionIdIsInProgress(outerXid);
			bool outerXactDidCommit = TransactionIdDidCommit(outerXid);
			if (outerXactIsInProgress && !outerXactDidCommit)
			{
				/*
				 * The transaction is initiated from an outer transaction and the outer
				 * transaction is not yet committed, so we should not commit either.
				 * We remove this transaction from the pendingTransactionSet so it'll
				 * not be aborted by the loop below.
				 */
				hash_search(pendingTransactionSet, transactionName, HASH_REMOVE,
							&foundPreparedTransactionBeforeCommit);
				continue;
			}
			else if (!outerXactIsInProgress && !outerXactDidCommit)
			{
				/*
				 * Since outer transaction isn't in progress and did not commit we need to
				 * abort the prepared transaction too. We do this by simply doing the same
				 * thing we would  do for transactions that are initiated from the main
				 * database.
				 */
				continue;
			}
			else
			{
				/*
				 * Outer transaction did commit, so we can try to commit the prepared
				 * transaction too.
				 */
			}
		}

		/*
		 * Remove the transaction from the pending list such that only transactions
		 * that need to be aborted remain at the end.
		 */
		hash_search(pendingTransactionSet, transactionName, HASH_REMOVE,
					&foundPreparedTransactionBeforeCommit);

		hash_search(recheckTransactionSet, transactionName, HASH_FIND,
					&foundPreparedTransactionAfterCommit);

		if (foundPreparedTransactionBeforeCommit && foundPreparedTransactionAfterCommit)
		{
			/*
			 * The transaction was committed, but the prepared transaction still exists
			 * on the worker. Try committing it.
			 *
			 * We double check that the recovery record exists both before and after
			 * checking ActiveDistributedTransactionNumbers(), since we may have
			 * observed a prepared transaction that was committed immediately after.
			 */
			bool shouldCommit = true;
			bool commitSucceeded = RecoverPreparedTransactionOnWorker(connection,
																	  transactionName,
																	  shouldCommit);
			if (!commitSucceeded)
			{
				/*
				 * Failed to commit on the current worker. Stop without throwing
				 * an error to allow recover_prepared_transactions to continue with
				 * other workers.
				 */
				recoveryFailed = true;
				break;
			}

			recoveredTransactionCount++;

			/*
			 * We successfully committed the prepared transaction, safe to delete
			 * the recovery record.
			 */
		}
		else if (foundPreparedTransactionAfterCommit)
		{
			/*
			 * We found a committed pg_dist_transaction record that initially did
			 * not have a prepared transaction, but did when we checked again.
			 *
			 * If a transaction started and committed just after we observed the
			 * set of prepared transactions, and just before we called
			 * ActiveDistributedTransactionNumbers, then we would see a recovery
			 * record without a prepared transaction in pendingTransactionSet,
			 * but there may be prepared transactions that failed to commit.
			 * We should not delete the records for those prepared transactions,
			 * since we would otherwise roll back them on the next call to
			 * recover_prepared_transactions.
			 *
			 * In addition, if the transaction started after the call to
			 * ActiveDistributedTransactionNumbers and finished just before our
			 * pg_dist_transaction snapshot, then it may still be in the process
			 * of committing the prepared transactions in the post-commit callback
			 * and we should not touch the prepared transactions.
			 *
			 * To handle these cases, we just leave the records and prepared
			 * transactions for the next call to recover_prepared_transactions
			 * and skip them here.
			 */

			continue;
		}
		else
		{
			/*
			 * We found a recovery record without any prepared transaction. It
			 * must have already been committed, so it's safe to delete the
			 * recovery record.
			 *
			 * Transactions that started after we observed pendingTransactionSet,
			 * but successfully committed their prepared transactions before
			 * ActiveDistributedTransactionNumbers are indistinguishable from
			 * transactions that committed at an earlier time, in which case it's
			 * safe delete the recovery record as well.
			 */
		}

		simple_heap_delete(pgDistTransaction, &heapTuple->t_self);
	}

	systable_endscan(scanDescriptor);
	table_close(pgDistTransaction, NoLock);

	if (!recoveryFailed)
	{
		char *pendingTransactionName = NULL;
		bool abortSucceeded = true;

		/*
		 * All remaining prepared transactions that are not part of an in-progress
		 * distributed transaction should be aborted since we did not find a recovery
		 * record, which implies the disributed transaction aborted.
		 */
		hash_seq_init(&status, pendingTransactionSet);

		while ((pendingTransactionName = hash_seq_search(&status)) != NULL)
		{
			bool isTransactionInProgress = IsTransactionInProgress(
				activeTransactionNumberSet,
				pendingTransactionName);
			if (isTransactionInProgress)
			{
				continue;
			}

			bool shouldCommit = false;
			abortSucceeded = RecoverPreparedTransactionOnWorker(connection,
																pendingTransactionName,
																shouldCommit);
			if (!abortSucceeded)
			{
				hash_seq_term(&status);
				break;
			}

			recoveredTransactionCount++;
		}
	}

	MemoryContextSwitchTo(oldContext);
	MemoryContextDelete(localContext);

	return recoveredTransactionCount;
}


/*
 * PendingWorkerTransactionList returns a list of pending prepared
 * transactions on a remote node that were started by this node.
 */
static List *
PendingWorkerTransactionList(MultiConnection *connection)
{
	StringInfo command = makeStringInfo();
	bool raiseInterrupts = true;
	List *transactionNames = NIL;
	int32 coordinatorId = GetLocalGroupId();

	appendStringInfo(command,
					 "SELECT gid FROM pg_prepared_xacts "
					 "WHERE gid COLLATE pg_catalog.default LIKE 'citus\\_%d\\_%%' COLLATE pg_catalog.default AND database = current_database()",
					 coordinatorId);

	int querySent = SendRemoteCommand(connection, command->data);
	if (querySent == 0)
	{
		ReportConnectionError(connection, ERROR);
	}

	PGresult *result = GetRemoteCommandResult(connection, raiseInterrupts);
	if (!IsResponseOK(result))
	{
		ReportResultError(connection, result, ERROR);
	}

	int rowCount = PQntuples(result);

	for (int rowIndex = 0; rowIndex < rowCount; rowIndex++)
	{
		const int columnIndex = 0;
		char *transactionName = PQgetvalue(result, rowIndex, columnIndex);

		transactionNames = lappend(transactionNames, pstrdup(transactionName));
	}

	PQclear(result);
	ForgetResults(connection);

	return transactionNames;
}


/*
 * IsTransactionInProgress returns whether the distributed transaction to which
 * preparedTransactionName belongs is still in progress, or false if the
 * transaction name cannot be parsed. This can happen when the user manually
 * inserts into pg_dist_transaction.
 */
static bool
IsTransactionInProgress(HTAB *activeTransactionNumberSet, char *preparedTransactionName)
{
	int32 groupId = 0;
	int procId = 0;
	uint32 connectionNumber = 0;
	uint64 transactionNumber = 0;
	bool isTransactionInProgress = false;

	bool isValidName = ParsePreparedTransactionName(preparedTransactionName, &groupId,
													&procId,
													&transactionNumber,
													&connectionNumber);
	if (isValidName)
	{
		hash_search(activeTransactionNumberSet, &transactionNumber, HASH_FIND,
					&isTransactionInProgress);
	}

	return isTransactionInProgress;
}


/*
 * RecoverPreparedTransactionOnWorker recovers a single prepared transaction over
 * the given connection. If shouldCommit is true we send
 */
static bool
RecoverPreparedTransactionOnWorker(MultiConnection *connection, char *transactionName,
								   bool shouldCommit)
{
	StringInfo command = makeStringInfo();
	PGresult *result = NULL;
	bool raiseInterrupts = false;

	if (shouldCommit)
	{
		/* should have committed this prepared transaction */
		appendStringInfo(command, "COMMIT PREPARED %s",
						 quote_literal_cstr(transactionName));
	}
	else
	{
		/* should have aborted this prepared transaction */
		appendStringInfo(command, "ROLLBACK PREPARED %s",
						 quote_literal_cstr(transactionName));
	}

	int executeCommand = ExecuteOptionalRemoteCommand(connection, command->data, &result);
	if (executeCommand == QUERY_SEND_FAILED)
	{
		return false;
	}
	if (executeCommand == RESPONSE_NOT_OKAY)
	{
		return false;
	}

	PQclear(result);
	ClearResults(connection, raiseInterrupts);

	ereport(LOG, (errmsg("recovered a prepared transaction on %s:%d",
						 connection->hostname, connection->port),
				  errcontext("%s", command->data)));

	return true;
}


/*
 * DeleteWorkerTransactions deletes the entries on pg_dist_transaction for a given
 * worker node. It's implemented to be called at master_remove_node.
 */
void
DeleteWorkerTransactions(WorkerNode *workerNode)
{
	if (workerNode == NULL)
	{
		/*
		 * We don't expect this, but let's be defensive since crashing is much worse
		 * than leaving pg_dist_transction entries.
		 */
		return;
	}

	bool indexOK = true;
	int scanKeyCount = 1;
	ScanKeyData scanKey[1];
	int32 groupId = workerNode->groupId;
	HeapTuple heapTuple = NULL;

	Relation pgDistTransaction = table_open(DistTransactionRelationId(),
											RowExclusiveLock);

	ScanKeyInit(&scanKey[0], Anum_pg_dist_transaction_groupid,
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(groupId));

	SysScanDesc scanDescriptor = systable_beginscan(pgDistTransaction,
													DistTransactionGroupIndexId(),
													indexOK,
													NULL, scanKeyCount, scanKey);

	while (HeapTupleIsValid(heapTuple = systable_getnext(scanDescriptor)))
	{
		simple_heap_delete(pgDistTransaction, &heapTuple->t_self);
	}

	CommandCounterIncrement();
	systable_endscan(scanDescriptor);
	table_close(pgDistTransaction, NoLock);
}
