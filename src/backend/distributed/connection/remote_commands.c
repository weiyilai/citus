/*-------------------------------------------------------------------------
 *
 * remote_commands.c
 *   Helpers to make it easier to execute command on remote nodes.
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#include "postgres.h"

#include "libpq-fe.h"
#include "miscadmin.h"
#include "pgstat.h"

#include "catalog/pg_collation.h"
#include "lib/stringinfo.h"
#include "storage/latch.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"
#include "utils/palloc.h"

#include "distributed/cancel_utils.h"
#include "distributed/connection_management.h"
#include "distributed/errormessage.h"
#include "distributed/listutils.h"
#include "distributed/log_utils.h"
#include "distributed/remote_commands.h"


/*
 * Setting that controls how many bytes of COPY data libpq is allowed to buffer
 * internally before we force a flush.
 */
int RemoteCopyFlushThreshold = 8 * 1024 * 1024;


/* GUC, determining whether statements sent to remote nodes are logged */
bool LogRemoteCommands = false;
char *GrepRemoteCommands = "";


static bool ClearResultsInternal(MultiConnection *connection, bool raiseErrors,
								 bool discardWarnings);
static bool FinishConnectionIO(MultiConnection *connection, bool raiseInterrupts);
static WaitEventSet * BuildWaitEventSet(MultiConnection **allConnections,
										int totalConnectionCount,
										int pendingConnectionsStartIndex);


/* simple helpers */

/*
 * IsResponseOK checks whether the result is a successful one.
 */
bool
IsResponseOK(PGresult *result)
{
	ExecStatusType resultStatus = PQresultStatus(result);

	if (resultStatus == PGRES_SINGLE_TUPLE || resultStatus == PGRES_TUPLES_OK ||
		resultStatus == PGRES_COMMAND_OK)
	{
		return true;
	}

	return false;
}


/*
 * ForgetResults clears a connection from pending activity.
 *
 * Note that this might require network IO. If that's not acceptable, use
 * ClearResultsIfReady().
 *
 * ClearResults is variant of this function which can also raise errors.
 */
void
ForgetResults(MultiConnection *connection)
{
	ClearResults(connection, false);
}


/*
 * ClearResults clears a connection from pending activity,
 * returns true if all pending commands return success. It raises
 * error if raiseErrors flag is set, any command fails and transaction
 * is marked critical.
 *
 * Note that this might require network IO. If that's not acceptable, use
 * ClearResultsIfReady().
 */
bool
ClearResults(MultiConnection *connection, bool raiseErrors)
{
	return ClearResultsInternal(connection, raiseErrors, false);
}


/*
 * ClearResultsDiscardWarnings does the same thing as ClearResults, but doesn't
 * emit warnings.
 */
bool
ClearResultsDiscardWarnings(MultiConnection *connection, bool raiseErrors)
{
	return ClearResultsInternal(connection, raiseErrors, true);
}


/*
 * ClearResultsInternal is used by ClearResults and ClearResultsDiscardWarnings.
 */
static bool
ClearResultsInternal(MultiConnection *connection, bool raiseErrors, bool discardWarnings)
{
	bool success = true;

	while (true)
	{
		PGresult *result = GetRemoteCommandResult(connection, raiseErrors);
		if (result == NULL)
		{
			break;
		}

		/*
		 * End any pending copy operation. Transaction will be marked
		 * as failed by the following part.
		 */
		if (PQresultStatus(result) == PGRES_COPY_IN)
		{
			PQputCopyEnd(connection->pgConn, NULL);
		}

		if (!IsResponseOK(result))
		{
			if (!discardWarnings)
			{
				ReportResultError(connection, result, WARNING);
			}

			MarkRemoteTransactionFailed(connection, raiseErrors);

			success = false;

			/* an error happened, there is nothing we can do more */
			if (PQresultStatus(result) == PGRES_FATAL_ERROR)
			{
				PQclear(result);

				break;
			}
		}

		PQclear(result);
	}

	return success;
}


/*
 * ClearResultsIfReady clears a connection from pending activity if doing
 * so does not require network IO. Returns true if successful, false
 * otherwise.
 */
bool
ClearResultsIfReady(MultiConnection *connection)
{
	PGconn *pgConn = connection->pgConn;

	if (PQstatus(pgConn) != CONNECTION_OK)
	{
		return false;
	}

	Assert(PQisnonblocking(pgConn));

	while (true)
	{
		/*
		 * If busy, there might still be results already received and buffered
		 * by the OS. As connection is in non-blocking mode, we can check for
		 * that without blocking.
		 */
		if (PQisBusy(pgConn))
		{
			if (PQflush(pgConn) == -1)
			{
				/* write failed */
				return false;
			}
			if (PQconsumeInput(pgConn) == 0)
			{
				/* some low-level failure */
				return false;
			}
		}

		/* clearing would require blocking IO, return */
		if (PQisBusy(pgConn))
		{
			return false;
		}

		PGresult *result = PQgetResult(pgConn);
		if (result == NULL)
		{
			/* no more results available */
			return true;
		}

		ExecStatusType resultStatus = PQresultStatus(result);

		/* only care about the status, can clear now */
		PQclear(result);

		if (resultStatus == PGRES_COPY_IN || resultStatus == PGRES_COPY_OUT)
		{
			/* in copy, can't reliably recover without blocking */
			return false;
		}

		if (!(resultStatus == PGRES_SINGLE_TUPLE || resultStatus == PGRES_TUPLES_OK ||
			  resultStatus == PGRES_COMMAND_OK))
		{
			/* an error occurred just when we were aborting */
			return false;
		}

		/* check if there are more results to consume */
	}

	pg_unreachable();
}


/* report errors & warnings */

/*
 * Report libpq failure that's not associated with a result.
 */
void
ReportConnectionError(MultiConnection *connection, int elevel)
{
	char *userName = connection->user;
	char *nodeName = connection->hostname;
	int nodePort = connection->port;
	PGconn *pgConn = connection->pgConn;
	char *messageDetail = NULL;

	if (pgConn != NULL)
	{
		messageDetail = pchomp(PQerrorMessage(pgConn));
		if (messageDetail == NULL || messageDetail[0] == '\0')
		{
			/* give a similar messages to Postgres */
			messageDetail = "connection not open";
		}
	}

	if (messageDetail)
	{
		ereport(elevel, (errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("connection to the remote node %s@%s:%d failed with the "
								"following error: %s", userName, nodeName, nodePort,
								messageDetail)));
	}
	else
	{
		ereport(elevel, (errcode(ERRCODE_CONNECTION_FAILURE),
						 errmsg("connection to the remote node %s@%s:%d failed",
								userName, nodeName, nodePort)));
	}
}


/*
 * ReportResultError reports libpq failure associated with a result.
 */
void
ReportResultError(MultiConnection *connection, PGresult *result, int elevel)
{
	/* we release PQresult when throwing an error because the caller can't */
	PG_TRY();
	{
		char *sqlStateString = PQresultErrorField(result, PG_DIAG_SQLSTATE);
		char *messagePrimary = PQresultErrorField(result, PG_DIAG_MESSAGE_PRIMARY);
		char *messageDetail = PQresultErrorField(result, PG_DIAG_MESSAGE_DETAIL);
		char *messageHint = PQresultErrorField(result, PG_DIAG_MESSAGE_HINT);
		char *messageContext = PQresultErrorField(result, PG_DIAG_CONTEXT);

		char *nodeName = connection->hostname;
		int nodePort = connection->port;
		int sqlState = ERRCODE_INTERNAL_ERROR;

		if (sqlStateString != NULL)
		{
			sqlState = MAKE_SQLSTATE(sqlStateString[0],
									 sqlStateString[1],
									 sqlStateString[2],
									 sqlStateString[3],
									 sqlStateString[4]);
		}

		/*
		 * If the PGresult did not contain a message, the connection may provide a
		 * suitable top level one. At worst, this is an empty string.
		 */
		if (messagePrimary == NULL)
		{
			messagePrimary = pchomp(PQerrorMessage(connection->pgConn));
		}

		ereport(elevel, (errcode(sqlState), errmsg("%s", messagePrimary),
						 messageDetail ?
						 errdetail("%s", messageDetail) : 0,
						 messageHint ? errhint("%s", messageHint) : 0,
						 messageContext ? errcontext("%s", messageContext) : 0,
						 errcontext("while executing command on %s:%d",
									nodeName, nodePort)));
	}
	PG_CATCH();
	{
		PQclear(result);
		PG_RE_THROW();
	}
	PG_END_TRY();
}


/* *INDENT-ON* */

/*
 * LogRemoteCommand logs commands send to remote nodes if
 * citus.log_remote_commands wants us to do so.
 */
void
LogRemoteCommand(MultiConnection *connection, const char *command)
{
	if (!LogRemoteCommands)
	{
		return;
	}

	if (!CommandMatchesLogGrepPattern(command))
	{
		return;
	}

	ereport(NOTICE, (errmsg("issuing %s", command),
					 errdetail("on server %s@%s:%d connectionId: %ld", connection->user,
							   connection->hostname,
							   connection->port, connection->connectionId)));
}


/*
 * CommandMatchesLogGrepPattern returns true of the input command matches
 * the pattern specified by citus.grep_remote_commands.
 *
 * If citus.grep_remote_commands set to an empty string, all commands are
 * considered as a match.
 */
bool
CommandMatchesLogGrepPattern(const char *command)
{
	if (GrepRemoteCommands && strnlen(GrepRemoteCommands, NAMEDATALEN) > 0)
	{
		Datum boolDatum =
			DirectFunctionCall2Coll(textlike, DEFAULT_COLLATION_OID,
									CStringGetTextDatum(command),
									CStringGetTextDatum(GrepRemoteCommands));

		return DatumGetBool(boolDatum);
	}

	return true;
}


/* wrappers around libpq functions, with command logging support */


/*
 * ExecuteCriticalRemoteCommandList calls ExecuteCriticalRemoteCommand for every
 * command in the commandList.
 */
void
ExecuteCriticalRemoteCommandList(MultiConnection *connection, List *commandList)
{
	const char *command = NULL;
	foreach_declared_ptr(command, commandList)
	{
		ExecuteCriticalRemoteCommand(connection, command);
	}
}


/*
 * ExecuteCriticalRemoteCommand executes a remote command that is critical
 * to the transaction. If the command fails then the transaction aborts.
 */
void
ExecuteCriticalRemoteCommand(MultiConnection *connection, const char *command)
{
	bool raiseInterrupts = true;

	int querySent = SendRemoteCommand(connection, command);
	if (querySent == 0)
	{
		ReportConnectionError(connection, ERROR);
	}

	PGresult *result = GetRemoteCommandResult(connection, raiseInterrupts);
	if (!IsResponseOK(result))
	{
		ReportResultError(connection, result, ERROR);
	}

	PQclear(result);
	ForgetResults(connection);
}


/*
 * ExecuteRemoteCommandInConnectionList executes a remote command, on all connections
 * given in the list, that is critical to the transaction. If the command fails then
 * the transaction aborts.
 */
void
ExecuteRemoteCommandInConnectionList(List *nodeConnectionList, const char *command)
{
	MultiConnection *connection = NULL;

	foreach_declared_ptr(connection, nodeConnectionList)
	{
		int querySent = SendRemoteCommand(connection, command);

		if (querySent == 0)
		{
			ReportConnectionError(connection, ERROR);
		}
	}

	/* Process the result */
	foreach_declared_ptr(connection, nodeConnectionList)
	{
		bool raiseInterrupts = true;
		PGresult *result = GetRemoteCommandResult(connection, raiseInterrupts);

		if (!IsResponseOK(result))
		{
			ReportResultError(connection, result, ERROR);
		}

		PQclear(result);
		ForgetResults(connection);
	}
}


/*
 * ExecuteOptionalRemoteCommand executes a remote command. If the command fails a WARNING
 * is emitted but execution continues.
 *
 * could return 0, QUERY_SEND_FAILED, or RESPONSE_NOT_OKAY
 * result is only set if there was no error
 */
int
ExecuteOptionalRemoteCommand(MultiConnection *connection, const char *command,
							 PGresult **result)
{
	bool raiseInterrupts = true;

	int querySent = SendRemoteCommand(connection, command);
	if (querySent == 0)
	{
		ReportConnectionError(connection, WARNING);
		return QUERY_SEND_FAILED;
	}

	PGresult *localResult = GetRemoteCommandResult(connection, raiseInterrupts);
	if (!IsResponseOK(localResult))
	{
		ReportResultError(connection, localResult, WARNING);
		PQclear(localResult);
		ForgetResults(connection);
		return RESPONSE_NOT_OKAY;
	}

	/*
	 * store result if result has been set, when the user is not interested in the result
	 * a NULL pointer could be passed and the result will be cleared.
	 */
	if (result != NULL)
	{
		*result = localResult;
	}
	else
	{
		PQclear(localResult);
		ForgetResults(connection);
	}

	return RESPONSE_OKAY;
}


/*
 * SendRemoteCommandParams is a PQsendQueryParams wrapper that logs remote commands,
 * and accepts a MultiConnection instead of a plain PGconn. It makes sure it can
 * send commands asynchronously without blocking (at the potential expense of
 * an additional memory allocation). The command string can only include a single
 * command since PQsendQueryParams() supports only that.
 */
int
SendRemoteCommandParams(MultiConnection *connection, const char *command,
						int parameterCount, const Oid *parameterTypes,
						const char *const *parameterValues, bool binaryResults)
{
	PGconn *pgConn = connection->pgConn;

	LogRemoteCommand(connection, command);

	/*
	 * Don't try to send command if connection is entirely gone
	 * (PQisnonblocking() would crash).
	 */
	if (!pgConn || PQstatus(pgConn) != CONNECTION_OK)
	{
		return 0;
	}

	Assert(PQisnonblocking(pgConn));

	int rc = PQsendQueryParams(pgConn, command, parameterCount, parameterTypes,
							   parameterValues, NULL, NULL, binaryResults ? 1 : 0);

	return rc;
}


/*
 * SendRemoteCommand is a PQsendQuery wrapper that logs remote commands, and
 * accepts a MultiConnection instead of a plain PGconn. It makes sure it can
 * send commands asynchronously without blocking (at the potential expense of
 * an additional memory allocation). The command string can include multiple
 * commands since PQsendQuery() supports that.
 */
int
SendRemoteCommand(MultiConnection *connection, const char *command)
{
	PGconn *pgConn = connection->pgConn;

	LogRemoteCommand(connection, command);

	/*
	 * Don't try to send command if connection is entirely gone
	 * (PQisnonblocking() would crash).
	 */
	if (!pgConn || PQstatus(pgConn) != CONNECTION_OK)
	{
		return 0;
	}

	Assert(PQisnonblocking(pgConn));

	int rc = PQsendQuery(pgConn, command);

	return rc;
}


/*
 * ExecuteRemoteCommandAndCheckResult executes the given command in the remote node and
 * checks if the result is equal to the expected result. If the result is equal to the
 * expected result, the function returns true, otherwise it returns false.
 */
bool
ExecuteRemoteCommandAndCheckResult(MultiConnection *connection, char *command,
								   char *expected)
{
	if (!SendRemoteCommand(connection, command))
	{
		/* if we cannot connect, we warn and report false */
		ReportConnectionError(connection, WARNING);
		return false;
	}
	bool raiseInterrupts = true;
	PGresult *queryResult = GetRemoteCommandResult(connection, raiseInterrupts);

	/* if remote node throws an error, we also throw an error */
	if (!IsResponseOK(queryResult))
	{
		ReportResultError(connection, queryResult, ERROR);
	}

	StringInfo queryResultString = makeStringInfo();

	/* Evaluate the queryResult and store it into the queryResultString */
	bool success = EvaluateSingleQueryResult(connection, queryResult, queryResultString);
	bool result = false;
	if (success && strcmp(queryResultString->data, expected) == 0)
	{
		result = true;
	}

	PQclear(queryResult);
	ForgetResults(connection);

	return result;
}


/*
 * ReadFirstColumnAsText reads the first column of result tuples from the given
 * PGresult struct and returns them in a StringInfo list.
 */
List *
ReadFirstColumnAsText(PGresult *queryResult)
{
	List *resultRowList = NIL;
	const int columnIndex = 0;
	int64 rowCount = 0;

	ExecStatusType status = PQresultStatus(queryResult);
	if (status == PGRES_TUPLES_OK)
	{
		rowCount = PQntuples(queryResult);
	}

	for (int64 rowIndex = 0; rowIndex < rowCount; rowIndex++)
	{
		char *rowValue = PQgetvalue(queryResult, rowIndex, columnIndex);

		StringInfo rowValueString = makeStringInfo();
		appendStringInfoString(rowValueString, rowValue);

		resultRowList = lappend(resultRowList, rowValueString);
	}

	return resultRowList;
}


/*
 * GetRemoteCommandResult is a wrapper around PQgetResult() that handles interrupts.
 *
 * If raiseInterrupts is true and an interrupt arrives, e.g. the query is
 * being cancelled, CHECK_FOR_INTERRUPTS() will be called, which then throws
 * an error.
 *
 * If raiseInterrupts is false and an interrupt arrives that'd otherwise raise
 * an error, GetRemoteCommandResult returns NULL, and the transaction is
 * marked as having failed. While that's not a perfect way to signal failure,
 * callers will usually treat that as an error, and it's easy to use.
 *
 * Handling of interrupts is important to allow queries being cancelled while
 * waiting on remote nodes. In a distributed deadlock scenario cancelling
 * might be the only way to resolve the deadlock.
 */
PGresult *
GetRemoteCommandResult(MultiConnection *connection, bool raiseInterrupts)
{
	PGconn *pgConn = connection->pgConn;

	/*
	 * Short circuit tests around the more expensive parts of this
	 * routine. This'd also trigger a return in the, unlikely, case of a
	 * failed/nonexistant connection.
	 */
	if (!PQisBusy(pgConn))
	{
		return PQgetResult(connection->pgConn);
	}

	if (!FinishConnectionIO(connection, raiseInterrupts))
	{
		/* some error(s) happened while doing the I/O, signal the callers */
		if (PQstatus(pgConn) == CONNECTION_BAD)
		{
			return PQmakeEmptyPGresult(pgConn, PGRES_FATAL_ERROR);
		}

		return NULL;
	}

	/* no IO should be necessary to get result */
	Assert(!PQisBusy(pgConn));

	PGresult *result = PQgetResult(connection->pgConn);

	return result;
}


/*
 * PutRemoteCopyData is a wrapper around PQputCopyData() that handles
 * interrupts.
 *
 * Returns false if PQputCopyData() failed, true otherwise.
 */
bool
PutRemoteCopyData(MultiConnection *connection, const char *buffer, int nbytes)
{
	PGconn *pgConn = connection->pgConn;
	bool allowInterrupts = true;

	if (PQstatus(pgConn) != CONNECTION_OK)
	{
		return false;
	}

	Assert(PQisnonblocking(pgConn));

	int copyState = PQputCopyData(pgConn, buffer, nbytes);
	if (copyState <= 0)
	{
		return false;
	}

	/*
	 * PQputCopyData may have queued up part of the data even if it managed
	 * to send some of it successfully. We provide back pressure by waiting
	 * until the socket is writable to prevent the internal libpq buffers
	 * from growing excessively.
	 *
	 * We currently allow the internal buffer to grow to 8MB before
	 * providing back pressure based on experimentation that showed
	 * throughput get worse at 4MB and lower due to the number of CPU
	 * cycles spent in networking system calls.
	 */

	connection->copyBytesWrittenSinceLastFlush += nbytes;
	if (connection->copyBytesWrittenSinceLastFlush > RemoteCopyFlushThreshold)
	{
		connection->copyBytesWrittenSinceLastFlush = 0;
		return FinishConnectionIO(connection, allowInterrupts);
	}

	return true;
}


/*
 * PutRemoteCopyEnd is a wrapper around PQputCopyEnd() that handles
 * interrupts.
 *
 * Returns false if PQputCopyEnd() failed, true otherwise.
 */
bool
PutRemoteCopyEnd(MultiConnection *connection, const char *errormsg)
{
	PGconn *pgConn = connection->pgConn;
	bool allowInterrupts = true;

	if (PQstatus(pgConn) != CONNECTION_OK)
	{
		return false;
	}

	Assert(PQisnonblocking(pgConn));

	int copyState = PQputCopyEnd(pgConn, errormsg);
	if (copyState == -1)
	{
		return false;
	}

	/* see PutRemoteCopyData() */

	connection->copyBytesWrittenSinceLastFlush = 0;

	return FinishConnectionIO(connection, allowInterrupts);
}


/*
 * FinishConnectionIO performs pending IO for the connection, while accepting
 * interrupts.
 *
 * See GetRemoteCommandResult() for documentation of interrupt handling
 * behaviour.
 *
 * Returns true if IO was successfully completed, false otherwise.
 */
static bool
FinishConnectionIO(MultiConnection *connection, bool raiseInterrupts)
{
	PGconn *pgConn = connection->pgConn;
	int sock = PQsocket(pgConn);

	Assert(pgConn);
	Assert(PQisnonblocking(pgConn));

	if (raiseInterrupts)
	{
		CHECK_FOR_INTERRUPTS();
	}

	/* perform the necessary IO */
	while (true)
	{
		int waitFlags = WL_POSTMASTER_DEATH | WL_LATCH_SET;

		/* try to send all pending data */
		int sendStatus = PQflush(pgConn);

		/* if sending failed, there's nothing more we can do */
		if (sendStatus == -1)
		{
			return false;
		}
		else if (sendStatus == 1)
		{
			waitFlags |= WL_SOCKET_WRITEABLE;
		}

		/* if reading fails, there's not much we can do */
		if (PQconsumeInput(pgConn) == 0)
		{
			return false;
		}
		if (PQisBusy(pgConn))
		{
			waitFlags |= WL_SOCKET_READABLE;
		}

		if ((waitFlags & (WL_SOCKET_READABLE | WL_SOCKET_WRITEABLE)) == 0)
		{
			/* no IO necessary anymore, we're done */
			return true;
		}

		int rc = WaitLatchOrSocket(MyLatch, waitFlags, sock, 0, PG_WAIT_EXTENSION);
		if (rc & WL_POSTMASTER_DEATH)
		{
			ereport(ERROR, (errmsg("postmaster was shut down, exiting")));
		}

		if (rc & WL_LATCH_SET)
		{
			ResetLatch(MyLatch);

			/* if allowed raise errors */
			if (raiseInterrupts)
			{
				CHECK_FOR_INTERRUPTS();
			}

			/*
			 * If raising errors allowed, or called within in a section with
			 * interrupts held, return instead, and mark the transaction as
			 * failed.
			 */
			if (IsHoldOffCancellationReceived())
			{
				connection->remoteTransaction.transactionFailed = true;
				break;
			}
		}
	}

	return false;
}


/*
 * WaitForAllConnections blocks until all connections in the list are no
 * longer busy, meaning the pending command has either finished or failed.
 */
void
WaitForAllConnections(List *connectionList, bool raiseInterrupts)
{
	int totalConnectionCount = list_length(connectionList);
	int pendingConnectionsStartIndex = 0;
	int connectionIndex = 0;

	MultiConnection **allConnections =
		palloc(totalConnectionCount * sizeof(MultiConnection *));
	WaitEvent *events = palloc(totalConnectionCount * sizeof(WaitEvent));
	bool *connectionReady = palloc(totalConnectionCount * sizeof(bool));
	WaitEventSet *volatile waitEventSet = NULL;

	/* convert connection list to an array such that we can move items around */
	MultiConnection *connectionItem = NULL;
	foreach_declared_ptr(connectionItem, connectionList)
	{
		allConnections[connectionIndex] = connectionItem;
		connectionReady[connectionIndex] = false;
		connectionIndex++;
	}

	/* make an initial pass to check for failed and idle connections */
	for (connectionIndex = 0; connectionIndex < totalConnectionCount; connectionIndex++)
	{
		MultiConnection *connection = allConnections[connectionIndex];

		if (PQstatus(connection->pgConn) == CONNECTION_BAD ||
			!PQisBusy(connection->pgConn))
		{
			/* connection is already done; keep non-ready connections at the end */
			allConnections[connectionIndex] =
				allConnections[pendingConnectionsStartIndex];
			pendingConnectionsStartIndex++;
		}
	}

	PG_TRY();
	{
		bool rebuildWaitEventSet = true;

		while (pendingConnectionsStartIndex < totalConnectionCount)
		{
			bool cancellationReceived = false;
			int eventIndex = 0;
			long timeout = -1;
			int pendingConnectionCount = totalConnectionCount -
										 pendingConnectionsStartIndex;

			/* rebuild the WaitEventSet whenever connections are ready */
			if (rebuildWaitEventSet)
			{
				if (waitEventSet != NULL)
				{
					FreeWaitEventSet(waitEventSet);
				}

				waitEventSet = BuildWaitEventSet(allConnections, totalConnectionCount,
												 pendingConnectionsStartIndex);

				rebuildWaitEventSet = false;
			}

			/* wait for I/O events */
			int eventCount = WaitEventSetWait(waitEventSet, timeout, events,
											  pendingConnectionCount,
											  WAIT_EVENT_CLIENT_READ);

			/* process I/O events */
			for (; eventIndex < eventCount; eventIndex++)
			{
				WaitEvent *event = &events[eventIndex];
				bool connectionIsReady = false;

				if (event->events & WL_POSTMASTER_DEATH)
				{
					ereport(ERROR, (errmsg("postmaster was shut down, exiting")));
				}

				if (event->events & WL_LATCH_SET)
				{
					ResetLatch(MyLatch);

					if (raiseInterrupts)
					{
						CHECK_FOR_INTERRUPTS();
					}

					if (IsHoldOffCancellationReceived())
					{
						/*
						 * Break out of event loop immediately in case of cancellation.
						 * We cannot use "return" here inside a PG_TRY() block since
						 * then the exception stack won't be reset.
						 */
						cancellationReceived = true;
						break;
					}

					continue;
				}

				MultiConnection *connection = (MultiConnection *) event->user_data;

				if (event->events & WL_SOCKET_WRITEABLE)
				{
					int sendStatus = PQflush(connection->pgConn);
					if (sendStatus == -1)
					{
						/* send failed, done with this connection */
						connectionIsReady = true;
					}
					else if (sendStatus == 0)
					{
						/* done writing, only wait for read events */
						bool success =
							CitusModifyWaitEvent(waitEventSet, event->pos,
												 WL_SOCKET_READABLE, NULL);
						if (!success)
						{
							ereport(ERROR, (errcode(ERRCODE_CONNECTION_FAILURE),
											errmsg("connection establishment for "
												   "node %s:%d failed",
												   connection->hostname,
												   connection->port),
											errhint("Check both the local and remote "
													"server logs for the connection "
													"establishment errors.")));
						}
					}
				}

				/*
				 * Check whether the connection is done is the socket is either readable
				 * or writable. If it was only writable, we performed a PQflush which
				 * might have read from the socket, meaning we may not see the socket
				 * becoming readable again, so better to check it now.
				 */
				if (event->events & (WL_SOCKET_READABLE | WL_SOCKET_WRITEABLE))
				{
					int receiveStatus = PQconsumeInput(connection->pgConn);
					if (receiveStatus == 0)
					{
						/* receive failed, done with this connection */
						connectionIsReady = true;
					}
					else if (!PQisBusy(connection->pgConn))
					{
						/* result was received */
						connectionIsReady = true;
					}
				}

				if (connectionIsReady)
				{
					/*
					 * All pending connections are kept at the end of the allConnections
					 * array and the connectionReady array matches the allConnections
					 * array. The wait event set corresponds to the pending connections
					 * subarray so we can get the index in the allConnections array by
					 * taking the event index + the offset of the subarray.
					 */
					connectionIndex = event->pos + pendingConnectionsStartIndex;

					connectionReady[connectionIndex] = true;

					/*
					 * When a connection is ready, we should build a new wait event
					 * set that excludes this connection.
					 */
					rebuildWaitEventSet = true;
				}
			}

			if (cancellationReceived)
			{
				break;
			}

			/* move non-ready connections to the back of the array */
			for (connectionIndex = pendingConnectionsStartIndex;
				 connectionIndex < totalConnectionCount; connectionIndex++)
			{
				if (connectionReady[connectionIndex])
				{
					/*
					 * Replace the ready connection with a connection from
					 * the start of the pending connections subarray. This
					 * may be the connection itself, in which case this is
					 * a noop.
					 */
					allConnections[connectionIndex] =
						allConnections[pendingConnectionsStartIndex];

					/* offset of the pending connections subarray is now 1 higher */
					pendingConnectionsStartIndex++;

					/*
					 * We've moved a pending connection into this position,
					 * so we must reset the ready flag. Otherwise, we'd
					 * falsely interpret it as ready in the next round.
					 */
					connectionReady[connectionIndex] = false;
				}
			}
		}

		if (waitEventSet != NULL)
		{
			FreeWaitEventSet(waitEventSet);
			waitEventSet = NULL;
		}

		pfree(allConnections);
		pfree(events);
		pfree(connectionReady);
	}
	PG_CATCH();
	{
		/* make sure the epoll file descriptor is always closed */
		if (waitEventSet != NULL)
		{
			FreeWaitEventSet(waitEventSet);
			waitEventSet = NULL;
		}

		pfree(allConnections);
		pfree(events);
		pfree(connectionReady);

		PG_RE_THROW();
	}
	PG_END_TRY();
}


/*
 * BuildWaitEventSet creates a WaitEventSet for the given array of connections
 * which can be used to wait for any of the sockets to become read-ready or
 * write-ready.
 */
static WaitEventSet *
BuildWaitEventSet(MultiConnection **allConnections, int totalConnectionCount,
				  int pendingConnectionsStartIndex)
{
	int pendingConnectionCount = totalConnectionCount - pendingConnectionsStartIndex;

	/*
	 * subtract 3 to make room for WL_POSTMASTER_DEATH, WL_LATCH_SET, and
	 * pgwin32_signal_event.
	 */
	if (pendingConnectionCount > FD_SETSIZE - 3)
	{
		pendingConnectionCount = FD_SETSIZE - 3;
	}

	/* allocate pending connections + 2 for the signal latch and postmaster death */
	/* (CreateWaitEventSet makes room for pgwin32_signal_event automatically) */
	WaitEventSet *waitEventSet = CreateWaitEventSet(WaitEventSetTracker_compat,
													pendingConnectionCount + 2);

	for (int connectionIndex = 0; connectionIndex < pendingConnectionCount;
		 connectionIndex++)
	{
		MultiConnection *connection = allConnections[pendingConnectionsStartIndex +
													 connectionIndex];
		int sock = PQsocket(connection->pgConn);

		/*
		 * Always start by polling for both readability (server sent bytes)
		 * and writeability (server is ready to receive bytes).
		 */
		int eventMask = WL_SOCKET_READABLE | WL_SOCKET_WRITEABLE;
		int waitEventSetIndex =
			CitusAddWaitEventSetToSet(waitEventSet, eventMask, sock,
									  NULL, (void *) connection);
		if (waitEventSetIndex == WAIT_EVENT_SET_INDEX_FAILED)
		{
			ereport(ERROR, (errcode(ERRCODE_CONNECTION_FAILURE),
							errmsg("connection establishment for node %s:%d failed",
								   connection->hostname, connection->port),
							errhint("Check both the local and remote server logs for the "
									"connection establishment errors.")));
		}
	}

	/*
	 * Put the wait events for the signal latch and postmaster death at the end such that
	 * event index + pendingConnectionsStartIndex = the connection index in the array.
	 */
	AddWaitEventToSet(waitEventSet, WL_POSTMASTER_DEATH, PGINVALID_SOCKET, NULL, NULL);
	AddWaitEventToSet(waitEventSet, WL_LATCH_SET, PGINVALID_SOCKET, MyLatch, NULL);

	return waitEventSet;
}


/*
 * SendCancelationRequest sends a cancelation request on the given connection.
 * Return value indicates whether the cancelation request was sent successfully.
 */
bool
SendCancelationRequest(MultiConnection *connection)
{
	char errorBuffer[ERROR_BUFFER_SIZE] = { 0 };

	PGcancel *cancelObject = PQgetCancel(connection->pgConn);
	if (cancelObject == NULL)
	{
		/* this can happen if connection is invalid */
		return false;
	}

	bool cancelSent = PQcancel(cancelObject, errorBuffer, sizeof(errorBuffer));
	if (!cancelSent)
	{
		ereport(WARNING, (errmsg("could not issue cancel request"),
						  errdetail("Client error: %s", errorBuffer)));
	}

	PQfreeCancel(cancelObject);

	return cancelSent;
}


/*
 * EvaluateSingleQueryResult gets the query result from connection and returns
 * true if the query is executed successfully, false otherwise. A query result
 * or an error message is returned in queryResultString. The function requires
 * that the query returns a single column/single row result. It returns an
 * error otherwise.
 */
bool
EvaluateSingleQueryResult(MultiConnection *connection, PGresult *queryResult,
						  StringInfo queryResultString)
{
	bool success = false;

	ExecStatusType resultStatus = PQresultStatus(queryResult);
	if (resultStatus == PGRES_COMMAND_OK)
	{
		char *commandStatus = PQcmdStatus(queryResult);
		appendStringInfo(queryResultString, "%s", commandStatus);
		success = true;
	}
	else if (resultStatus == PGRES_TUPLES_OK)
	{
		int ntuples = PQntuples(queryResult);
		int nfields = PQnfields(queryResult);

		/* error if query returns more than 1 rows, or more than 1 fields */
		if (nfields != 1)
		{
			appendStringInfo(queryResultString,
							 "expected a single column in query target");
		}
		else if (ntuples > 1)
		{
			appendStringInfo(queryResultString,
							 "expected a single row in query result");
		}
		else
		{
			int row = 0;
			int column = 0;
			if (!PQgetisnull(queryResult, row, column))
			{
				char *queryResultValue = PQgetvalue(queryResult, row, column);
				appendStringInfo(queryResultString, "%s", queryResultValue);
			}
			success = true;
		}
	}
	else
	{
		StoreErrorMessage(connection, queryResultString);
	}

	return success;
}


/*
 * StoreErrorMessage gets the error message from connection and stores it
 * in queryResultString. It should be called only when error is present
 * otherwise it would return a default error message.
 */
void
StoreErrorMessage(MultiConnection *connection, StringInfo queryResultString)
{
	char *errorMessage = PQerrorMessage(connection->pgConn);
	if (errorMessage != NULL)
	{
		/* copy the error message to a writable memory */
		errorMessage = pnstrdup(errorMessage, strlen(errorMessage));

		char *firstNewlineIndex = strchr(errorMessage, '\n');

		/* trim the error message at the line break */
		if (firstNewlineIndex != NULL)
		{
			*firstNewlineIndex = '\0';
		}
	}
	else
	{
		/* put a default error message if no error message is reported */
		errorMessage = "An error occurred while running the query";
	}

	appendStringInfo(queryResultString, "%s", errorMessage);
}


/*
 * IsSettingSafeToPropagate returns whether a SET LOCAL is safe to propagate.
 *
 * We exclude settings that are highly specific to the client or session and also
 * ban propagating the citus.propagate_set_commands setting (not for correctness,
 * more to avoid confusion).
 */
bool
IsSettingSafeToPropagate(const char *name)
{
	/* if this list grows considerably we should switch to bsearch */
	const char *skipSettings[] = {
		"application_name",
		"citus.propagate_set_commands",
		"client_encoding",
		"exit_on_error",
		"max_stack_depth"
	};

	for (Index settingIndex = 0; settingIndex < lengthof(skipSettings); settingIndex++)
	{
		if (pg_strcasecmp(skipSettings[settingIndex], name) == 0)
		{
			return false;
		}
	}

	return true;
}
