/*-------------------------------------------------------------------------
 *
 * multi_executor.c
 *
 * Entrypoint into distributed query execution.
 *
 * Copyright (c) Citus Data, Inc.
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "miscadmin.h"

#include "access/xact.h"
#include "catalog/dependency.h"
#include "catalog/namespace.h"
#include "catalog/pg_class.h"
#include "commands/copy.h"
#include "executor/execdebug.h"
#include "nodes/execnodes.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "parser/parse_oper.h"
#include "parser/parsetree.h"
#include "storage/lmgr.h"
#include "tcop/dest.h"
#include "tcop/pquery.h"
#include "tcop/utility.h"
#include "utils/fmgrprotos.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "pg_version_constants.h"

#include "distributed/backend_data.h"
#include "distributed/citus_custom_scan.h"
#include "distributed/combine_query_planner.h"
#include "distributed/commands/multi_copy.h"
#include "distributed/commands/utility_hook.h"
#include "distributed/coordinator_protocol.h"
#include "distributed/distributed_planner.h"
#include "distributed/function_call_delegation.h"
#include "distributed/insert_select_executor.h"
#include "distributed/insert_select_planner.h"
#include "distributed/listutils.h"
#include "distributed/local_executor.h"
#include "distributed/multi_executor.h"
#include "distributed/multi_router_planner.h"
#include "distributed/multi_server_executor.h"
#include "distributed/relation_access_tracking.h"
#include "distributed/resource_lock.h"
#include "distributed/transaction_management.h"
#include "distributed/version_compat.h"
#include "distributed/worker_protocol.h"
#include "distributed/worker_shard_visibility.h"


/*
 * Controls the connection type for multi shard modifications, DDLs
 * TRUNCATE and multi-shard SELECT queries.
 */
int MultiShardConnectionType = PARALLEL_CONNECTION;
bool WritableStandbyCoordinator = false;
bool AllowModificationsFromWorkersToReplicatedTables = true;

/*
 * Controlled by the GUC citus.skip_constraint_validation
 */
bool SkipConstraintValidation = false;

/*
 * Setting that controls whether distributed queries should be
 * allowed within a task execution.
 */
bool AllowNestedDistributedExecution = false;

/*
 * Pointer to bound parameters of the current ongoing call to ExecutorRun.
 * If executor is not running, then this value is meaningless.
 */
ParamListInfo executorBoundParams = NULL;


/* sort the returning to get consistent outputs, used only for testing */
bool SortReturning = false;

/*
 * How many nested executors have we started? This can happen for SQL
 * UDF calls. The outer query starts an executor, then postgres opens
 * another executor to run the SQL UDF.
 */
int ExecutorLevel = 0;


/* local function forward declarations */
static Relation StubRelation(TupleDesc tupleDescriptor);
static char * GetObjectTypeString(ObjectType objType);
static bool AlterTableConstraintCheck(QueryDesc *queryDesc);
static List * FindCitusCustomScanStates(PlanState *planState);
static bool CitusCustomScanStateWalker(PlanState *planState,
									   List **citusCustomScanStates);
static bool IsTaskExecutionAllowed(bool isRemote);
static bool InLocalTaskExecutionOnShard(void);
static bool MaybeInRemoteTaskExecution(void);
static bool InTrigger(void);


/*
 * CitusExecutorStart is the ExecutorStart_hook that gets called when
 * Postgres prepares for execution or EXPLAIN.
 */
void
CitusExecutorStart(QueryDesc *queryDesc, int eflags)
{
	PlannedStmt *plannedStmt = queryDesc->plannedstmt;

	/*
	 * We cannot modify XactReadOnly on Windows because it is not
	 * declared with PGDLLIMPORT.
	 */
#ifndef WIN32
	if (RecoveryInProgress() && WritableStandbyCoordinator &&
		IsCitusPlan(plannedStmt->planTree))
	{
		PG_TRY();
		{
			/*
			 * To enable writes from a hot standby we cheat our way through
			 * the checks in standard_ExecutorStart by temporarily setting
			 * XactReadOnly to false.
			 */
			XactReadOnly = false;
			standard_ExecutorStart(queryDesc, eflags);
			XactReadOnly = true;
		}
		PG_CATCH();
		{
			XactReadOnly = true;
			PG_RE_THROW();
		}
		PG_END_TRY();
	}
	else
#endif
	{
		standard_ExecutorStart(queryDesc, eflags);
	}
}


/*
 * CitusExecutorRun is the ExecutorRun_hook that gets called when postgres
 * executes a query.
 */
void
CitusExecutorRun(QueryDesc *queryDesc,
				 ScanDirection direction, uint64 count, bool execute_once)
{
	DestReceiver *dest = queryDesc->dest;

	ParamListInfo savedBoundParams = executorBoundParams;

	/*
	 * Save a pointer to query params so UDFs can access them by calling
	 * ExecutorBoundParams().
	 */
	executorBoundParams = queryDesc->params;

	/*
	 * We do some potentially time consuming operations ourself now before we hand off
	 * control to postgres' executor. To make sure that time spent is accurately measured
	 * we remove the totaltime instrumentation from the queryDesc. Instead we will start
	 * and stop the instrumentation of the total time and put it back on the queryDesc
	 * before returning (or rethrowing) from this function.
	 */
	Instrumentation *volatile totalTime = queryDesc->totaltime;
	queryDesc->totaltime = NULL;

	PG_TRY();
	{
		ExecutorLevel++;

		if (totalTime)
		{
			InstrStartNode(totalTime);
		}

		/*
		 * Disable execution of ALTER TABLE constraint validation queries. These
		 * constraints will be validated in worker nodes, so running these queries
		 * from the coordinator would be redundant.
		 *
		 * For example, ALTER TABLE ... ATTACH PARTITION checks that the new
		 * partition doesn't violate constraints of the parent table, which
		 * might involve running some SELECT queries.
		 *
		 * Ideally we'd completely skip these checks in the coordinator, but we don't
		 * have any means to tell postgres to skip the checks. So the best we can do is
		 * to not execute the queries and return an empty result set, as if this table has
		 * no rows, so no constraints will be violated.
		 */
		if (AlterTableConstraintCheck(queryDesc))
		{
			EState *estate = queryDesc->estate;

			estate->es_processed = 0;

			/* start and shutdown tuple receiver to simulate empty result */
			dest->rStartup(queryDesc->dest, CMD_SELECT, queryDesc->tupDesc);
			dest->rShutdown(dest);
		}
		else
		{
			/* switch into per-query memory context before calling PreExecScan */
			MemoryContext oldcontext = MemoryContextSwitchTo(
				queryDesc->estate->es_query_cxt);

			/*
			 * Call PreExecScan for all citus custom scan nodes prior to starting the
			 * postgres exec scan to give some citus scan nodes some time to initialize
			 * state that would be too late if it were to initialize when the first tuple
			 * would need to return.
			 */
			List *citusCustomScanStates = FindCitusCustomScanStates(queryDesc->planstate);
			CitusScanState *citusScanState = NULL;
			foreach_declared_ptr(citusScanState, citusCustomScanStates)
			{
				if (citusScanState->PreExecScan)
				{
					citusScanState->PreExecScan(citusScanState);
				}
			}

			/* postgres will switch here again and will restore back on its own */
			MemoryContextSwitchTo(oldcontext);

			#if PG_VERSION_NUM >= PG_VERSION_18

			/* PG18+ drops the “execute_once” argument */
			standard_ExecutorRun(queryDesc,
								 direction,
								 count);
		#else

			/* PG17-: original four-arg signature */
			standard_ExecutorRun(queryDesc,
								 direction,
								 count,
								 execute_once);
		#endif
		}

		if (totalTime)
		{
			InstrStopNode(totalTime, queryDesc->estate->es_processed);
			queryDesc->totaltime = totalTime;
		}

		executorBoundParams = savedBoundParams;
		ExecutorLevel--;

		if (ExecutorLevel == 0 && PlannerLevel == 0)
		{
			/*
			 * We are leaving Citus code so no one should have any references to
			 * cache entries. Release them now to not hold onto memory in long
			 * transactions.
			 */
			CitusTableCacheFlushInvalidatedEntries();
			InTopLevelDelegatedFunctionCall = false;
		}

		/*
		 * Within a 2PC, when a function is delegated to a remote node, we pin
		 * the distribution argument as the shard key for all the SQL in the
		 * function's block. The restriction is imposed to not to access other
		 * nodes from the current node, and violate the transactional integrity
		 * of the 2PC. Now that the query is ending, reset the shard key to NULL.
		 */
		CheckAndResetAllowedShardKeyValueIfNeeded();
	}
	PG_CATCH();
	{
		if (totalTime)
		{
			queryDesc->totaltime = totalTime;
		}

		executorBoundParams = savedBoundParams;
		ExecutorLevel--;

		if (ExecutorLevel == 0 && PlannerLevel == 0)
		{
			InTopLevelDelegatedFunctionCall = false;
		}

		/*
		 * In case of an exception, reset the pinned shard-key, for more
		 * details see the function header.
		 */
		CheckAndResetAllowedShardKeyValueIfNeeded();

		PG_RE_THROW();
	}
	PG_END_TRY();
}


/*
 * FindCitusCustomScanStates returns a list of all citus custom scan states in it.
 */
static List *
FindCitusCustomScanStates(PlanState *planState)
{
	List *citusCustomScanStates = NIL;
	CitusCustomScanStateWalker(planState, &citusCustomScanStates);
	return citusCustomScanStates;
}


/*
 * CitusCustomScanStateWalker walks a planState tree structure and adds all
 * CitusCustomState nodes to the list passed by reference as the second argument.
 */
static bool
CitusCustomScanStateWalker(PlanState *planState, List **citusCustomScanStates)
{
	if (IsCitusCustomState(planState))
	{
		CitusScanState *css = (CitusScanState *) planState;
		*citusCustomScanStates = lappend(*citusCustomScanStates, css);

		/* breaks the walking of this tree */
		return true;
	}
	return planstate_tree_walker(planState, CitusCustomScanStateWalker,
								 citusCustomScanStates);
}


/*
 * ReturnTupleFromTuplestore reads the next tuple from the tuple store of the
 * given Citus scan node and returns it. It returns null if all tuples are read
 * from the tuple store.
 */
TupleTableSlot *
ReturnTupleFromTuplestore(CitusScanState *scanState)
{
	Tuplestorestate *tupleStore = scanState->tuplestorestate;
	bool forwardScanDirection = true;

	if (tupleStore == NULL)
	{
		return NULL;
	}

	EState *executorState = ScanStateGetExecutorState(scanState);
	ScanDirection scanDirection = executorState->es_direction;
	Assert(ScanDirectionIsValid(scanDirection));

	if (ScanDirectionIsBackward(scanDirection))
	{
		forwardScanDirection = false;
	}

	ExprState *qual = scanState->customScanState.ss.ps.qual;
	ProjectionInfo *projInfo = scanState->customScanState.ss.ps.ps_ProjInfo;
	ExprContext *econtext = scanState->customScanState.ss.ps.ps_ExprContext;

	if (!qual && !projInfo)
	{
		/* no quals, nor projections return directly from the tuple store. */
		TupleTableSlot *slot = scanState->customScanState.ss.ss_ScanTupleSlot;
		tuplestore_gettupleslot(tupleStore, forwardScanDirection, false, slot);
		return slot;
	}

	for (;;)
	{
		/*
		 * If there is a very selective qual on the Citus Scan node we might block
		 * interrupts for a longer time if we would not check for interrupts in this loop
		 */
		CHECK_FOR_INTERRUPTS();

		/*
		 * Reset per-tuple memory context to free any expression evaluation
		 * storage allocated in the previous tuple cycle.
		 */
		ResetExprContext(econtext);

		TupleTableSlot *slot = scanState->customScanState.ss.ss_ScanTupleSlot;
		tuplestore_gettupleslot(tupleStore, forwardScanDirection, false, slot);

		if (TupIsNull(slot))
		{
			/*
			 * When the tuple is null we have reached the end of the tuplestore. We will
			 * return a null tuple, however, depending on the existence of a projection we
			 * need to either return the scan tuple or the projected tuple.
			 */
			if (projInfo)
			{
				return ExecClearTuple(projInfo->pi_state.resultslot);
			}
			else
			{
				return slot;
			}
		}

		/* place the current tuple into the expr context */
		econtext->ecxt_scantuple = slot;

		if (!ExecQual(qual, econtext))
		{
			/* skip nodes that do not satisfy the qual (filter) */
			InstrCountFiltered1(scanState, 1);
			continue;
		}

		/* found a satisfactory scan tuple */
		if (projInfo)
		{
			/*
			 * Form a projection tuple, store it in the result tuple slot and return it.
			 * ExecProj works on the ecxt_scantuple on the context stored earlier.
			 */
			return ExecProject(projInfo);
		}
		else
		{
			/* Here, we aren't projecting, so just return scan tuple */
			return slot;
		}
	}
}


/*
 * ReadFileIntoTupleStore parses the records in a COPY-formatted file according
 * according to the given tuple descriptor and stores the records in a tuple
 * store.
 */
void
ReadFileIntoTupleStore(char *fileName, char *copyFormat, TupleDesc tupleDescriptor,
					   Tuplestorestate *tupstore)
{
	/*
	 * Trick BeginCopyFrom into using our tuple descriptor by pretending it belongs
	 * to a relation.
	 */
	Relation stubRelation = StubRelation(tupleDescriptor);

	EState *executorState = CreateExecutorState();
	MemoryContext executorTupleContext = GetPerTupleMemoryContext(executorState);
	ExprContext *executorExpressionContext = GetPerTupleExprContext(executorState);

	int columnCount = tupleDescriptor->natts;
	Datum *columnValues = palloc0(columnCount * sizeof(Datum));
	bool *columnNulls = palloc0(columnCount * sizeof(bool));

	List *copyOptions = NIL;

	int location = -1; /* "unknown" token location */
	DefElem *copyOption = makeDefElem("format", (Node *) makeString(copyFormat),
									  location);
	copyOptions = lappend(copyOptions, copyOption);

	CopyFromState copyState = BeginCopyFrom(NULL, stubRelation, NULL,
											fileName, false, NULL,
											NULL, copyOptions);

	while (true)
	{
		ResetPerTupleExprContext(executorState);
		MemoryContext oldContext = MemoryContextSwitchTo(executorTupleContext);

		bool nextRowFound = NextCopyFrom(copyState, executorExpressionContext,
										 columnValues, columnNulls);
		if (!nextRowFound)
		{
			MemoryContextSwitchTo(oldContext);
			break;
		}

		tuplestore_putvalues(tupstore, tupleDescriptor, columnValues, columnNulls);
		MemoryContextSwitchTo(oldContext);
	}

	EndCopyFrom(copyState);
	pfree(columnValues);
	pfree(columnNulls);
}


/*
 * SortTupleStore gets a CitusScanState and sorts the tuplestore by all the
 * entries in the target entry list, starting from the first one and
 * ending with the last entry.
 *
 * The sorting is done in ASC order.
 */
void
SortTupleStore(CitusScanState *scanState)
{
	TupleDesc tupleDescriptor = ScanStateGetTupleDescriptor(scanState);
	Tuplestorestate *tupleStore = scanState->tuplestorestate;

	List *targetList = scanState->customScanState.ss.ps.plan->targetlist;
	uint32 expectedColumnCount = list_length(targetList);

	/* Convert list-ish representation to arrays wanted by executor */
	int numberOfSortKeys = expectedColumnCount;
	AttrNumber *sortColIdx = (AttrNumber *) palloc(numberOfSortKeys * sizeof(AttrNumber));
	Oid *sortOperators = (Oid *) palloc(numberOfSortKeys * sizeof(Oid));
	Oid *collations = (Oid *) palloc(numberOfSortKeys * sizeof(Oid));
	bool *nullsFirst = (bool *) palloc(numberOfSortKeys * sizeof(bool));

	int sortKeyIndex = 0;

	/*
	 * Iterate on the returning target list and generate the necessary information
	 * for sorting the tuples.
	 */
	TargetEntry *returningEntry = NULL;
	foreach_declared_ptr(returningEntry, targetList)
	{
		Oid sortop = InvalidOid;

		/* determine the sortop, we don't need anything else */
		get_sort_group_operators(exprType((Node *) returningEntry->expr),
								 true, false, false,
								 &sortop, NULL, NULL,
								 NULL);

		sortColIdx[sortKeyIndex] = sortKeyIndex + 1;
		sortOperators[sortKeyIndex] = sortop;
		collations[sortKeyIndex] = exprCollation((Node *) returningEntry->expr);
		nullsFirst[sortKeyIndex] = false;

		sortKeyIndex++;
	}

	Tuplesortstate *tuplesortstate =
		tuplesort_begin_heap(tupleDescriptor, numberOfSortKeys, sortColIdx, sortOperators,
							 collations, nullsFirst, work_mem, NULL, false);

	while (true)
	{
		TupleTableSlot *slot = ReturnTupleFromTuplestore(scanState);

		if (TupIsNull(slot))
		{
			break;
		}

		/* tuplesort_puttupleslot copies the slot into sort context */
		tuplesort_puttupleslot(tuplesortstate, slot);
	}

	/* perform the actual sort operation */
	tuplesort_performsort(tuplesortstate);

	/*
	 * Truncate the existing tupleStore, because we'll fill it back
	 * from the sorted tuplestore.
	 */
	tuplestore_clear(tupleStore);

	/* iterate over all the sorted tuples, add them to original tuplestore */
	while (true)
	{
		TupleTableSlot *newSlot = MakeSingleTupleTableSlot(tupleDescriptor,
														   &TTSOpsMinimalTuple);
		bool found = tuplesort_gettupleslot(tuplesortstate, true, false, newSlot, NULL);

		if (!found)
		{
			break;
		}

		/* tuplesort_puttupleslot copies the slot into the tupleStore context */
		tuplestore_puttupleslot(tupleStore, newSlot);
	}

	tuplestore_rescan(scanState->tuplestorestate);

	/* terminate the sort, clear unnecessary resources */
	tuplesort_end(tuplesortstate);
}


/*
 * StubRelation creates a stub Relation from the given tuple descriptor.
 * To be able to use copy.c, we need a Relation descriptor. As there is no
 * relation corresponding to the data loaded from workers, we need to fake one.
 * We just need the bare minimal set of fields accessed by BeginCopyFrom().
 */
static Relation
StubRelation(TupleDesc tupleDescriptor)
{
	Relation stubRelation = palloc0(sizeof(RelationData));
	stubRelation->rd_att = tupleDescriptor;
	stubRelation->rd_rel = palloc0(sizeof(FormData_pg_class));
	stubRelation->rd_rel->relkind = RELKIND_RELATION;

	return stubRelation;
}


/*
 * ExecuteQueryStringIntoDestReceiver plans and executes a query and sends results
 * to the given DestReceiver.
 */
void
ExecuteQueryStringIntoDestReceiver(const char *queryString, ParamListInfo params,
								   DestReceiver *dest)
{
	Query *query = ParseQueryString(queryString, NULL, 0);

	ExecuteQueryIntoDestReceiver(query, params, dest);
}


/*
 * ParseQuery parses query string and returns a Query struct.
 */
Query *
ParseQueryString(const char *queryString, Oid *paramOids, int numParams)
{
	RawStmt *rawStmt = (RawStmt *) ParseTreeRawStmt(queryString);

	/* rewrite the parsed RawStmt to produce a Query */
	Query *query = RewriteRawQueryStmt(rawStmt, queryString, paramOids, numParams);

	return query;
}


/*
 * RewriteRawQueryStmt rewrites the given parsed RawStmt according to the other
 * parameters and returns a Query struct.
 */
Query *
RewriteRawQueryStmt(RawStmt *rawStmt, const char *queryString, Oid *paramOids, int
					numParams)
{
	List *queryTreeList =
		pg_analyze_and_rewrite_fixedparams(rawStmt, queryString, paramOids, numParams,
										   NULL);

	if (list_length(queryTreeList) != 1)
	{
		ereport(ERROR, (errmsg("can only execute a single query")));
	}

	Query *query = (Query *) linitial(queryTreeList);

	return query;
}


/*
 * ExecuteQueryIntoDestReceiver plans and executes a query and sends results to the given
 * DestReceiver.
 */
void
ExecuteQueryIntoDestReceiver(Query *query, ParamListInfo params, DestReceiver *dest)
{
	int cursorOptions = CURSOR_OPT_PARALLEL_OK;

	if (query->commandType == CMD_UTILITY)
	{
		/* can only execute DML/SELECT via this path */
		ereport(ERROR, (errmsg("cannot execute utility commands")));
	}

	/* plan the subquery, this may be another distributed query */
	PlannedStmt *queryPlan = pg_plan_query(query, NULL, cursorOptions, params);

	ExecutePlanIntoDestReceiver(queryPlan, params, dest);
}


/*
 * ExecutePlanIntoDestReceiver executes a query plan and sends results to the given
 * DestReceiver.
 */
uint64
ExecutePlanIntoDestReceiver(PlannedStmt *queryPlan, ParamListInfo params,
							DestReceiver *dest)
{
	int eflags = 0;
	long count = FETCH_ALL;

	/* create a new portal for executing the query */
	Portal portal = CreateNewPortal();

	/* don't display the portal in pg_cursors, it is for internal use only */
	portal->visible = false;

	PortalDefineQuery(
		portal,
		NULL,                 /* no prepared statement name */
		"",                   /* query text */
		CMDTAG_SELECT,        /* command tag */
		list_make1(queryPlan),/* list of PlannedStmt* */
		NULL                  /* no CachedPlan */
		);

	PortalStart(portal, params, eflags, GetActiveSnapshot());


	QueryCompletion qc = { 0 };

#if PG_VERSION_NUM >= PG_VERSION_18

/* PG 18+: six-arg signature (drop the run_once bool) */
	PortalRun(portal,
			  count,  /* how many rows to fetch */
			  false,  /* isTopLevel */
			  dest,   /* DestReceiver *dest */
			  dest,   /* DestReceiver *altdest */
			  &qc);  /* QueryCompletion *qc */
#else

/* PG 17-: original seven-arg signature */
	PortalRun(portal,
			  count,  /* how many rows to fetch */
			  false,  /* isTopLevel */
			  true,   /* run_once */
			  dest,   /* DestReceiver *dest */
			  dest,   /* DestReceiver *altdest */
			  &qc);  /* QueryCompletion *qc */
#endif

	PortalDrop(portal, false);

	return qc.nprocessed;
}


/*
 * SetLocalMultiShardModifyModeToSequential is simply a C interface for setting
 * the following:
 *      SET LOCAL citus.multi_shard_modify_mode = 'sequential';
 */
void
SetLocalMultiShardModifyModeToSequential()
{
	set_config_option("citus.multi_shard_modify_mode", "sequential",
					  (superuser() ? PGC_SUSET : PGC_USERSET), PGC_S_SESSION,
					  GUC_ACTION_LOCAL, true, 0, false);
}


/*
 * EnsureSequentialMode makes sure that the current transaction is already in
 * sequential mode, or can still safely be put in sequential mode, it errors if that is
 * not possible. The error contains information for the user to retry the transaction with
 * sequential mode set from the beginning.
 *
 * Takes an ObjectType to use in the error/debug messages.
 */
void
EnsureSequentialMode(ObjectType objType)
{
	char *objTypeString = GetObjectTypeString(objType);

	if (ParallelQueryExecutedInTransaction())
	{
		ereport(ERROR, (errmsg("cannot run %s command because there was a "
							   "parallel operation on a distributed table in the "
							   "transaction", objTypeString),
						errdetail("When running command on/for a distributed %s, Citus "
								  "needs to perform all operations over a single "
								  "connection per node to ensure consistency.",
								  objTypeString),
						errhint("Try re-running the transaction with "
								"\"SET LOCAL citus.multi_shard_modify_mode TO "
								"\'sequential\';\"")));
	}

	ereport(DEBUG1, (errmsg("switching to sequential query execution mode"),
					 errdetail(
						 "A command for a distributed %s is run. To make sure subsequent "
						 "commands see the %s correctly we need to make sure to "
						 "use only one connection for all future commands",
						 objTypeString, objTypeString)));

	SetLocalMultiShardModifyModeToSequential();
}


/*
 * GetObjectTypeString takes an ObjectType and returns the string version of it.
 * We (for now) call this function only in EnsureSequentialMode, and use the returned
 * string to generate error/debug messages.
 *
 * If GetObjectTypeString gets called with an ObjectType that is not in the switch
 * statement, the function will return the string "object", and emit a debug message.
 * In that case, make sure you've added the newly supported type to the switch statement.
 */
static char *
GetObjectTypeString(ObjectType objType)
{
	switch (objType)
	{
		case OBJECT_AGGREGATE:
		{
			return "aggregate";
		}

		case OBJECT_COLLATION:
		{
			return "collation";
		}

		case OBJECT_DATABASE:
		{
			return "database";
		}

		case OBJECT_DOMAIN:
		{
			return "domain";
		}

		case OBJECT_EXTENSION:
		{
			return "extension";
		}

		case OBJECT_FOREIGN_SERVER:
		{
			return "foreign server";
		}

		case OBJECT_FUNCTION:
		{
			return "function";
		}

		case OBJECT_PUBLICATION:
		{
			return "publication";
		}

		case OBJECT_SCHEMA:
		{
			return "schema";
		}

		case OBJECT_TSCONFIGURATION:
		{
			return "text search configuration";
		}

		case OBJECT_TSDICTIONARY:
		{
			return "text search dictionary";
		}

		case OBJECT_TYPE:
		{
			return "type";
		}

		case OBJECT_VIEW:
		{
			return "view";
		}

		default:
		{
			ereport(DEBUG1, (errmsg("unsupported object type"),
							 errdetail("Please add string conversion for the object.")));
			return "object";
		}
	}
}


/*
 * AlterTableConstraintCheck returns if the given query is an ALTER TABLE
 * constraint check query.
 *
 * Postgres uses SPI to execute these queries. To see examples of how these
 * constraint check queries look like, see RI_Initial_Check() and RI_Fkey_check().
 */
static bool
AlterTableConstraintCheck(QueryDesc *queryDesc)
{
	if (!AlterTableInProgress())
	{
		return false;
	}

	/*
	 * These queries are one or more SELECT queries, where postgres checks
	 * their results either for NULL values or existence of a row at all.
	 */
	if (queryDesc->plannedstmt->commandType != CMD_SELECT)
	{
		return false;
	}

	/*
	 * While an ALTER TABLE is in progress, we might do SELECTs on some
	 * catalog tables too. For example, when dropping a column, citus_drop_trigger()
	 * runs some SELECTs on catalog tables. These are not constraint check queries.
	 */
	if (!IsCitusPlan(queryDesc->plannedstmt->planTree))
	{
		return false;
	}

	return true;
}


/*
 * ExecutorBoundParams returns the bound parameters of the current ongoing call
 * to ExecutorRun. This is meant to be used by UDFs which need to access bound
 * parameters.
 */
ParamListInfo
ExecutorBoundParams(void)
{
	Assert(ExecutorLevel > 0);
	return executorBoundParams;
}


/*
 * EnsureTaskExecutionAllowed ensures that we do not perform remote
 * execution from within a task. That could happen when the user calls
 * a function in a query that gets pushed down to the worker, and the
 * function performs a query on a distributed table.
 */
void
EnsureTaskExecutionAllowed(bool isRemote)
{
	if (IsTaskExecutionAllowed(isRemote))
	{
		return;
	}

	ereport(ERROR, (errmsg("cannot execute a distributed query from a query on a "
						   "shard"),
					errdetail("Executing a distributed query in a function call that "
							  "may be pushed to a remote node can lead to incorrect "
							  "results."),
					errhint("Avoid nesting of distributed queries or use alter user "
							"current_user set citus.allow_nested_distributed_execution "
							"to on to allow it with possible incorrectness.")));
}


/*
 * IsTaskExecutionAllowed determines whether task execution is currently allowed.
 * In general, nested distributed execution is not allowed, except in a few cases
 * (forced function call delegation, triggers).
 *
 * We distinguish between local and remote tasks because triggers only disallow
 * remote task execution.
 */
static bool
IsTaskExecutionAllowed(bool isRemote)
{
	if (AllowNestedDistributedExecution)
	{
		/* user explicitly allows nested execution */
		return true;
	}

	if (!isRemote)
	{
		if (AllowedDistributionColumnValue.isActive)
		{
			/*
			 * When we are in a forced delegated function call, we explicitly check
			 * whether local tasks use the same distribution column value in
			 * EnsureForceDelegationDistributionKey.
			 */
			return true;
		}

		if (InTrigger())
		{
			/*
			 * In triggers on shards we only disallow remote tasks. This has a few
			 * reasons:
			 *
			 * - We want to enable access to co-located shards, but do not have additional
			 *   checks yet.
			 * - Users need to explicitly set enable_unsafe_triggers in order to create
			 *   triggers on distributed tables.
			 * - Triggers on Citus local tables should be able to access other Citus local
			 *   tables.
			 */
			return true;
		}
	}

	return !InLocalTaskExecutionOnShard() && !MaybeInRemoteTaskExecution();
}


/*
 * InLocalTaskExecutionOnShard returns whether we are currently in the local executor
 * and it is working on a shard of a distributed table.
 *
 * In general, we can allow distributed queries inside of local executor, because
 * we can correctly assign tasks to connections. However, we preemptively protect
 * against distributed queries inside of queries on shards of a distributed table,
 * because those might start failing after a shard move.
 */
static bool
InLocalTaskExecutionOnShard(void)
{
	if (LocalExecutorShardId == INVALID_SHARD_ID)
	{
		/* local executor is not active or is processing a task without shards */
		return false;
	}

	if (!DistributedTableShardId(LocalExecutorShardId))
	{
		/*
		 * Local executor is processing a query on a shard, but the shard belongs
		 * to a reference table or Citus local table. We do not expect those to
		 * move.
		 */
		return false;
	}

	return true;
}


/*
 * MaybeInRemoteTaskExecution returns whether we could in a remote task execution.
 *
 * We consider anything that happens in a Citus-internal backend, except deleged
 * function or procedure calls as a potential task execution.
 *
 * This function will also return true in other scenarios, such as during metadata
 * syncing. However, since this function is mainly used for restricting (dangerous)
 * nested executions, it is good to be pessimistic.
 */
static bool
MaybeInRemoteTaskExecution(void)
{
	if (!IsCitusInternalBackend())
	{
		/* in a regular, client-initiated backend doing a regular task */
		return false;
	}

	if (InTopLevelDelegatedFunctionCall || InDelegatedProcedureCall)
	{
		/* in a citus-initiated backend, but also in a delegated a procedure call */
		return false;
	}

	return true;
}


/*
 * InTrigger returns whether the execution is currently in a trigger.
 */
static bool
InTrigger(void)
{
	return DatumGetInt32(pg_trigger_depth(NULL)) > 0;
}
