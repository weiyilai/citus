/*-------------------------------------------------------------------------
 *
 * citus_copyfuncs.c
 *    Citus specific node copy functions
 *
 * Portions Copyright (c) 1996-2014, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 * Portions Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "utils/datum.h"

#include "distributed/citus_nodefuncs.h"
#include "distributed/listutils.h"
#include "distributed/multi_server_executor.h"


/*
 * Macros to simplify copying of different kinds of fields.  Use these
 * wherever possible to reduce the chance for silly typos.  Note that these
 * hard-wire the convention that the local variables in a Copy routine are
 * named 'newnode' and 'from'.
 */
static inline Node *
CitusSetTag(Node *node, int tag)
{
	CitusNode *citus_node = (CitusNode *) node;
	citus_node->citus_tag = tag;
	return node;
}


#define DECLARE_FROM_AND_NEW_NODE(nodeTypeName) \
	nodeTypeName *newnode = \
		(nodeTypeName *) CitusSetTag((Node *) target_node, T_ ## nodeTypeName); \
	nodeTypeName *from = (nodeTypeName *) source_node

/* Copy a simple scalar field (int, float, bool, enum, etc) */
#define COPY_SCALAR_FIELD(fldname) \
	(newnode->fldname = from->fldname)

/* Copy a field that is a pointer to some kind of Node or Node tree */
#define COPY_NODE_FIELD(fldname) \
	(newnode->fldname = copyObject(from->fldname))

/* Copy a field that is a pointer to a C string, or perhaps NULL */
#define COPY_STRING_FIELD(fldname) \
	(newnode->fldname = from->fldname ? pstrdup(from->fldname) : (char *) NULL)

/* Copy a node array. Target array is also allocated. */
#define COPY_NODE_ARRAY(fldname, type, count) \
	do { \
		int i = 0; \
		newnode->fldname = (type **) palloc(count * sizeof(type *)); \
		for (i = 0; i < count; ++i) \
		{ \
			newnode->fldname[i] = copyObject(from->fldname[i]); \
		} \
	} \
	while (0)

/* Copy a scalar array. Target array is also allocated. */
#define COPY_SCALAR_ARRAY(fldname, type, count) \
	do { \
		int i = 0; \
		newnode->fldname = (type *) palloc(count * sizeof(type)); \
		for (i = 0; i < count; ++i) \
		{ \
			newnode->fldname[i] = from->fldname[i]; \
		} \
	} \
	while (0)

#define COPY_STRING_LIST(fldname) \
	do { \
		char *curString = NULL; \
		List *newList = NIL; \
		foreach_declared_ptr(curString, from->fldname) { \
			char *newString = curString ? pstrdup(curString) : (char *) NULL; \
			newList = lappend(newList, newString); \
		} \
		newnode->fldname = newList; \
	} \
	while (0)

static void CopyTaskQuery(Task *newnode, Task *from);

static void
copyJobInfo(Job *newnode, Job *from)
{
	COPY_SCALAR_FIELD(jobId);
	COPY_NODE_FIELD(jobQuery);
	COPY_NODE_FIELD(taskList);
	COPY_NODE_FIELD(dependentJobList);
	COPY_SCALAR_FIELD(subqueryPushdown);
	COPY_SCALAR_FIELD(requiresCoordinatorEvaluation);
	COPY_SCALAR_FIELD(deferredPruning);
	COPY_NODE_FIELD(partitionKeyValue);
	COPY_NODE_FIELD(localPlannedStatements);
	COPY_SCALAR_FIELD(parametersInJobQueryResolved);
}


void
CopyNodeJob(COPYFUNC_ARGS)
{
	DECLARE_FROM_AND_NEW_NODE(Job);

	copyJobInfo(newnode, from);
}


void
CopyNodeDistributedPlan(COPYFUNC_ARGS)
{
	DECLARE_FROM_AND_NEW_NODE(DistributedPlan);

	COPY_SCALAR_FIELD(planId);
	COPY_SCALAR_FIELD(modLevel);
	COPY_SCALAR_FIELD(expectResults);

	COPY_NODE_FIELD(workerJob);
	COPY_NODE_FIELD(combineQuery);
	COPY_SCALAR_FIELD(queryId);
	COPY_NODE_FIELD(relationIdList);
	COPY_SCALAR_FIELD(targetRelationId);
	COPY_NODE_FIELD(modifyQueryViaCoordinatorOrRepartition);
	COPY_NODE_FIELD(selectPlanForModifyViaCoordinatorOrRepartition);
	COPY_SCALAR_FIELD(modifyWithSelectMethod);
	COPY_STRING_FIELD(intermediateResultIdPrefix);

	COPY_NODE_FIELD(subPlanList);
	COPY_NODE_FIELD(usedSubPlanNodeList);
	COPY_SCALAR_FIELD(fastPathRouterPlan);
	COPY_SCALAR_FIELD(numberOfTimesExecuted);
	COPY_NODE_FIELD(planningError);
}


void
CopyNodeDistributedSubPlan(COPYFUNC_ARGS)
{
	DECLARE_FROM_AND_NEW_NODE(DistributedSubPlan);

	COPY_SCALAR_FIELD(subPlanId);
	COPY_NODE_FIELD(plan);
	COPY_SCALAR_FIELD(bytesSentPerWorker);
	COPY_SCALAR_FIELD(remoteWorkerCount);
	COPY_SCALAR_FIELD(durationMillisecs);
	COPY_SCALAR_FIELD(writeLocalFile);

	if (newnode->totalExplainOutput)
	{
		MemSet(newnode->totalExplainOutput, 0, sizeof(newnode->totalExplainOutput));
	}

	/* copy each SubPlanExplainOutput element */
	for (int i = 0; i < from->numTasksOutput; i++)
	{
		/* copy the explainOutput string pointer */
		COPY_STRING_FIELD(totalExplainOutput[i].explainOutput);

		/* copy the executionDuration (double) */
		COPY_SCALAR_FIELD(totalExplainOutput[i].executionDuration);

		/* copy the totalReceivedTupleData (uint64) */
		COPY_SCALAR_FIELD(totalExplainOutput[i].totalReceivedTupleData);
	}

	COPY_SCALAR_FIELD(numTasksOutput);
	COPY_SCALAR_FIELD(ntuples);
}


void
CopyNodeUsedDistributedSubPlan(COPYFUNC_ARGS)
{
	DECLARE_FROM_AND_NEW_NODE(UsedDistributedSubPlan);

	COPY_STRING_FIELD(subPlanId);
	COPY_SCALAR_FIELD(accessType);
}


void
CopyNodeShardInterval(COPYFUNC_ARGS)
{
	DECLARE_FROM_AND_NEW_NODE(ShardInterval);

	COPY_SCALAR_FIELD(relationId);
	COPY_SCALAR_FIELD(storageType);
	COPY_SCALAR_FIELD(valueTypeId);
	COPY_SCALAR_FIELD(valueTypeLen);
	COPY_SCALAR_FIELD(valueByVal);
	COPY_SCALAR_FIELD(minValueExists);
	COPY_SCALAR_FIELD(maxValueExists);

	if (from->minValueExists)
	{
		newnode->minValue = datumCopy(from->minValue,
									  from->valueByVal,
									  from->valueTypeLen);
	}
	if (from->maxValueExists)
	{
		newnode->maxValue = datumCopy(from->maxValue,
									  from->valueByVal,
									  from->valueTypeLen);
	}

	COPY_SCALAR_FIELD(shardId);
	COPY_SCALAR_FIELD(shardIndex);
}


void
CopyNodeMapMergeJob(COPYFUNC_ARGS)
{
	DECLARE_FROM_AND_NEW_NODE(MapMergeJob);

	copyJobInfo(&newnode->job, &from->job);

	COPY_SCALAR_FIELD(partitionType);
	COPY_NODE_FIELD(partitionColumn);
	COPY_SCALAR_FIELD(partitionCount);
	COPY_SCALAR_FIELD(sortedShardIntervalArrayLength);

	int arrayLength = from->sortedShardIntervalArrayLength;

	/* now build & read sortedShardIntervalArray */
	COPY_NODE_ARRAY(sortedShardIntervalArray, ShardInterval, arrayLength);

	COPY_NODE_FIELD(mapTaskList);
	COPY_NODE_FIELD(mergeTaskList);
}


void
CopyNodeShardPlacement(COPYFUNC_ARGS)
{
	DECLARE_FROM_AND_NEW_NODE(ShardPlacement);

	COPY_SCALAR_FIELD(placementId);
	COPY_SCALAR_FIELD(shardId);
	COPY_SCALAR_FIELD(shardLength);
	COPY_SCALAR_FIELD(groupId);
	COPY_STRING_FIELD(nodeName);
	COPY_SCALAR_FIELD(nodePort);
	COPY_SCALAR_FIELD(nodeId);
	COPY_SCALAR_FIELD(partitionMethod);
	COPY_SCALAR_FIELD(colocationGroupId);
	COPY_SCALAR_FIELD(representativeValue);
}


void
CopyNodeGroupShardPlacement(COPYFUNC_ARGS)
{
	DECLARE_FROM_AND_NEW_NODE(GroupShardPlacement);

	COPY_SCALAR_FIELD(placementId);
	COPY_SCALAR_FIELD(shardId);
	COPY_SCALAR_FIELD(shardLength);
	COPY_SCALAR_FIELD(groupId);
}


void
CopyNodeRelationShard(COPYFUNC_ARGS)
{
	DECLARE_FROM_AND_NEW_NODE(RelationShard);

	COPY_SCALAR_FIELD(relationId);
	COPY_SCALAR_FIELD(shardId);
}


void
CopyNodeRelationRowLock(COPYFUNC_ARGS)
{
	DECLARE_FROM_AND_NEW_NODE(RelationRowLock);

	COPY_SCALAR_FIELD(relationId);
	COPY_SCALAR_FIELD(rowLockStrength);
}


static void
CopyTaskQuery(Task *newnode, Task *from)
{
	COPY_SCALAR_FIELD(taskQuery.queryType);
	switch (from->taskQuery.queryType)
	{
		case TASK_QUERY_TEXT:
		{
			COPY_STRING_FIELD(taskQuery.data.queryStringLazy);
			break;
		}

		case TASK_QUERY_OBJECT:
		{
			COPY_NODE_FIELD(taskQuery.data.jobQueryReferenceForLazyDeparsing);
			break;
		}

		case TASK_QUERY_TEXT_LIST:
		{
			COPY_STRING_LIST(taskQuery.data.queryStringList);
			break;
		}

		case TASK_QUERY_LOCAL_PLAN:
		{
			newnode->taskQuery.data.localCompiled =
				(LocalCompilation *) palloc0(sizeof(LocalCompilation));
			COPY_NODE_FIELD(taskQuery.data.localCompiled->plan);
			COPY_NODE_FIELD(taskQuery.data.localCompiled->query);
			break;
		}

		default:
		{
			break;
		}
	}
}


void
CopyNodeTask(COPYFUNC_ARGS)
{
	DECLARE_FROM_AND_NEW_NODE(Task);

	COPY_SCALAR_FIELD(taskType);
	COPY_SCALAR_FIELD(jobId);
	COPY_SCALAR_FIELD(taskId);
	CopyTaskQuery(newnode, from);
	COPY_SCALAR_FIELD(anchorDistributedTableId);
	COPY_SCALAR_FIELD(anchorShardId);
	COPY_NODE_FIELD(taskPlacementList);
	COPY_NODE_FIELD(dependentTaskList);
	COPY_SCALAR_FIELD(partitionId);
	COPY_SCALAR_FIELD(upstreamTaskId);
	COPY_NODE_FIELD(shardInterval);
	COPY_SCALAR_FIELD(assignmentConstrained);
	COPY_SCALAR_FIELD(replicationModel);
	COPY_SCALAR_FIELD(modifyWithSubquery);
	COPY_NODE_FIELD(relationShardList);
	COPY_NODE_FIELD(relationRowLockList);
	COPY_NODE_FIELD(rowValuesLists);
	COPY_SCALAR_FIELD(partiallyLocalOrRemote);
	COPY_SCALAR_FIELD(parametersInQueryStringResolved);
	COPY_SCALAR_FIELD(tupleDest);
	COPY_SCALAR_FIELD(queryCount);
	COPY_SCALAR_FIELD(totalReceivedTupleData);
	COPY_SCALAR_FIELD(fetchedExplainAnalyzePlacementIndex);
	COPY_STRING_FIELD(fetchedExplainAnalyzePlan);
	COPY_SCALAR_FIELD(fetchedExplainAnalyzeExecutionDuration);
	COPY_SCALAR_FIELD(isLocalTableModification);
	COPY_SCALAR_FIELD(cannotBeExecutedInTransaction);
}


void
CopyNodeLocalPlannedStatement(COPYFUNC_ARGS)
{
	DECLARE_FROM_AND_NEW_NODE(LocalPlannedStatement);

	COPY_SCALAR_FIELD(shardId);
	COPY_SCALAR_FIELD(localGroupId);
	COPY_NODE_FIELD(localPlan);
}


void
CopyNodeDeferredErrorMessage(COPYFUNC_ARGS)
{
	DECLARE_FROM_AND_NEW_NODE(DeferredErrorMessage);

	COPY_SCALAR_FIELD(code);
	COPY_STRING_FIELD(message);
	COPY_STRING_FIELD(detail);
	COPY_STRING_FIELD(hint);
	COPY_STRING_FIELD(filename);
	COPY_SCALAR_FIELD(linenumber);
	COPY_STRING_FIELD(functionname);
}
