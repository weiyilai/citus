/*
 * node_metadata.c
 *	  Functions that operate on pg_dist_node
 *
 * Copyright (c) Citus Data, Inc.
 */
#include "postgres.h"

#include "funcapi.h"
#include "miscadmin.h"

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup.h"
#include "access/htup_details.h"
#include "access/skey.h"
#include "access/tupmacs.h"
#include "access/xact.h"
#include "catalog/indexing.h"
#include "catalog/namespace.h"
#include "commands/sequence.h"
#include "executor/spi.h"
#include "lib/stringinfo.h"
#include "postmaster/postmaster.h"
#include "storage/bufmgr.h"
#include "storage/fd.h"
#include "storage/lmgr.h"
#include "storage/lock.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/plancache.h"
#include "utils/rel.h"
#include "utils/relcache.h"

#include "distributed/citus_acquire_lock.h"
#include "distributed/citus_safe_lib.h"
#include "distributed/colocation_utils.h"
#include "distributed/commands.h"
#include "distributed/commands/utility_hook.h"
#include "distributed/connection_management.h"
#include "distributed/coordinator_protocol.h"
#include "distributed/maintenanced.h"
#include "distributed/metadata/distobject.h"
#include "distributed/metadata/pg_dist_object.h"
#include "distributed/metadata_cache.h"
#include "distributed/metadata_sync.h"
#include "distributed/metadata_utility.h"
#include "distributed/multi_join_order.h"
#include "distributed/multi_partitioning_utils.h"
#include "distributed/multi_router_planner.h"
#include "distributed/pg_dist_node.h"
#include "distributed/pg_dist_node_metadata.h"
#include "distributed/reference_table_utils.h"
#include "distributed/remote_commands.h"
#include "distributed/resource_lock.h"
#include "distributed/shardinterval_utils.h"
#include "distributed/shared_connection_stats.h"
#include "distributed/string_utils.h"
#include "distributed/transaction_recovery.h"
#include "distributed/version_compat.h"
#include "distributed/worker_manager.h"
#include "distributed/worker_transaction.h"

#define INVALID_GROUP_ID -1

/* default group size */
int GroupSize = 1;

/* config variable managed via guc.c */
char *CurrentCluster = "default";

/* did current transaction modify pg_dist_node? */
bool TransactionModifiedNodeMetadata = false;

bool EnableMetadataSync = true;

typedef struct NodeMetadata
{
	int32 groupId;
	char *nodeRack;
	bool hasMetadata;
	bool metadataSynced;
	bool isActive;
	Oid nodeRole;
	bool shouldHaveShards;
	char *nodeCluster;
} NodeMetadata;

/* local function forward declarations */
static void RemoveNodeFromCluster(char *nodeName, int32 nodePort);
static void ErrorIfNodeContainsNonRemovablePlacements(WorkerNode *workerNode);
static bool PlacementHasActivePlacementOnAnotherGroup(GroupShardPlacement
													  *sourcePlacement);
static int AddNodeMetadata(char *nodeName, int32 nodePort, NodeMetadata *nodeMetadata,
						   bool *nodeAlreadyExists, bool localOnly);
static int AddNodeMetadataViaMetadataContext(char *nodeName, int32 nodePort,
											 NodeMetadata *nodeMetadata,
											 bool *nodeAlreadyExists);
static HeapTuple GetNodeTuple(const char *nodeName, int32 nodePort);
static HeapTuple GetNodeByNodeId(int32 nodeId);
static int32 GetNextGroupId(void);
static int GetNextNodeId(void);
static void InsertPlaceholderCoordinatorRecord(void);
static void InsertNodeRow(int nodeid, char *nodename, int32 nodeport,
						  NodeMetadata *nodeMetadata);
static void DeleteNodeRow(char *nodename, int32 nodeport);
static void BlockDistributedQueriesOnMetadataNodes(void);
static WorkerNode * TupleToWorkerNode(TupleDesc tupleDescriptor, HeapTuple heapTuple);
static bool NodeIsLocal(WorkerNode *worker);
static void SetLockTimeoutLocally(int32 lock_cooldown);
static void UpdateNodeLocation(int32 nodeId, char *newNodeName, int32 newNodePort,
							   bool localOnly);
static bool UnsetMetadataSyncedForAllWorkers(void);
static char * GetMetadataSyncCommandToSetNodeColumn(WorkerNode *workerNode,
													int columnIndex,
													Datum value);
static char * NodeHasmetadataUpdateCommand(uint32 nodeId, bool hasMetadata);
static char * NodeMetadataSyncedUpdateCommand(uint32 nodeId, bool metadataSynced);
static void ErrorIfCoordinatorMetadataSetFalse(WorkerNode *workerNode, Datum value,
											   char *field);
static WorkerNode * SetShouldHaveShards(WorkerNode *workerNode, bool shouldHaveShards);
static WorkerNode * FindNodeAnyClusterByNodeId(uint32 nodeId);
static void ErrorIfAnyNodeNotExist(List *nodeList);
static void UpdateLocalGroupIdsViaMetadataContext(MetadataSyncContext *context);
static void SendDeletionCommandsForReplicatedTablePlacements(
	MetadataSyncContext *context);
static void SyncNodeMetadata(MetadataSyncContext *context);
static void SetNodeStateViaMetadataContext(MetadataSyncContext *context,
										   WorkerNode *workerNode,
										   Datum value);
static void MarkNodesNotSyncedInLoopBackConnection(MetadataSyncContext *context,
												   pid_t parentSessionPid);
static void EnsureParentSessionHasExclusiveLockOnPgDistNode(pid_t parentSessionPid);
static void SetNodeMetadata(MetadataSyncContext *context, bool localOnly);
static void EnsureTransactionalMetadataSyncMode(void);
static void LockShardsInWorkerPlacementList(WorkerNode *workerNode, LOCKMODE
											lockMode);
static BackgroundWorkerHandle * CheckBackgroundWorkerToObtainLocks(int32 lock_cooldown);
static BackgroundWorkerHandle * LockPlacementsWithBackgroundWorkersInPrimaryNode(
	WorkerNode *workerNode, bool force, int32 lock_cooldown);

/* Function definitions go here */

/* declarations for dynamic loading */
PG_FUNCTION_INFO_V1(citus_set_coordinator_host);
PG_FUNCTION_INFO_V1(citus_add_node);
PG_FUNCTION_INFO_V1(master_add_node);
PG_FUNCTION_INFO_V1(citus_add_inactive_node);
PG_FUNCTION_INFO_V1(master_add_inactive_node);
PG_FUNCTION_INFO_V1(citus_add_secondary_node);
PG_FUNCTION_INFO_V1(master_add_secondary_node);
PG_FUNCTION_INFO_V1(citus_set_node_property);
PG_FUNCTION_INFO_V1(master_set_node_property);
PG_FUNCTION_INFO_V1(citus_remove_node);
PG_FUNCTION_INFO_V1(master_remove_node);
PG_FUNCTION_INFO_V1(citus_disable_node);
PG_FUNCTION_INFO_V1(master_disable_node);
PG_FUNCTION_INFO_V1(citus_activate_node);
PG_FUNCTION_INFO_V1(master_activate_node);
PG_FUNCTION_INFO_V1(citus_update_node);
PG_FUNCTION_INFO_V1(citus_pause_node_within_txn);
PG_FUNCTION_INFO_V1(master_update_node);
PG_FUNCTION_INFO_V1(get_shard_id_for_distribution_column);
PG_FUNCTION_INFO_V1(citus_nodename_for_nodeid);
PG_FUNCTION_INFO_V1(citus_nodeport_for_nodeid);
PG_FUNCTION_INFO_V1(citus_coordinator_nodeid);
PG_FUNCTION_INFO_V1(citus_is_coordinator);
PG_FUNCTION_INFO_V1(citus_internal_mark_node_not_synced);
PG_FUNCTION_INFO_V1(citus_is_primary_node);

/*
 * DefaultNodeMetadata creates a NodeMetadata struct with the fields set to
 * sane defaults, e.g. nodeRack = WORKER_DEFAULT_RACK.
 */
static NodeMetadata
DefaultNodeMetadata()
{
	NodeMetadata nodeMetadata;

	/* ensure uninitialized padding doesn't escape the function */
	memset_struct_0(nodeMetadata);
	nodeMetadata.nodeRack = WORKER_DEFAULT_RACK;
	nodeMetadata.shouldHaveShards = true;
	nodeMetadata.groupId = INVALID_GROUP_ID;

	return nodeMetadata;
}


/*
 * citus_set_coordinator_host configures the hostname and port through which worker
 * nodes can connect to the coordinator.
 */
Datum
citus_set_coordinator_host(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	text *nodeName = PG_GETARG_TEXT_P(0);
	int32 nodePort = PG_GETARG_INT32(1);
	char *nodeNameString = text_to_cstring(nodeName);

	NodeMetadata nodeMetadata = DefaultNodeMetadata();
	nodeMetadata.groupId = 0;
	nodeMetadata.shouldHaveShards = false;
	nodeMetadata.nodeRole = PG_GETARG_OID(2);

	Name nodeClusterName = PG_GETARG_NAME(3);
	nodeMetadata.nodeCluster = NameStr(*nodeClusterName);

	/*
	 * We do not allow metadata operations on secondary nodes in nontransactional
	 * sync mode.
	 */
	if (nodeMetadata.nodeRole == SecondaryNodeRoleId())
	{
		EnsureTransactionalMetadataSyncMode();
	}

	/* prevent concurrent modification */
	LockRelationOid(DistNodeRelationId(), RowExclusiveLock);

	bool isCoordinatorInMetadata = false;
	WorkerNode *coordinatorNode = PrimaryNodeForGroup(COORDINATOR_GROUP_ID,
													  &isCoordinatorInMetadata);
	if (!isCoordinatorInMetadata)
	{
		bool nodeAlreadyExists = false;
		bool localOnly = false;

		/* add the coordinator to pg_dist_node if it was not already added */
		AddNodeMetadata(nodeNameString, nodePort, &nodeMetadata,
						&nodeAlreadyExists, localOnly);

		/* we just checked */
		Assert(!nodeAlreadyExists);
	}
	else
	{
		/*
		 * since AddNodeMetadata takes an exclusive lock on pg_dist_node, we
		 * do not need to worry about concurrent changes (e.g. deletion) and
		 * can proceed to update immediately.
		 */
		bool localOnly = false;
		UpdateNodeLocation(coordinatorNode->nodeId, nodeNameString, nodePort, localOnly);

		/* clear cached plans that have the old host/port */
		ResetPlanCache();
	}

	TransactionModifiedNodeMetadata = true;

	PG_RETURN_VOID();
}


/*
 * EnsureTransactionalMetadataSyncMode ensures metadata sync mode is transactional.
 */
static void
EnsureTransactionalMetadataSyncMode(void)
{
	if (MetadataSyncTransMode == METADATA_SYNC_NON_TRANSACTIONAL)
	{
		ereport(ERROR, (errmsg("this operation cannot be completed in nontransactional "
							   "metadata sync mode"),
						errhint("SET citus.metadata_sync_mode to 'transactional'")));
	}
}


/*
 * citus_add_node function adds a new node to the cluster and returns its id. It also
 * replicates all reference tables to the new node.
 */
Datum
citus_add_node(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	EnsureSuperUser();
	EnsureCoordinator();

	text *nodeName = PG_GETARG_TEXT_P(0);
	int32 nodePort = PG_GETARG_INT32(1);
	char *nodeNameString = text_to_cstring(nodeName);

	NodeMetadata nodeMetadata = DefaultNodeMetadata();
	bool nodeAlreadyExists = false;
	nodeMetadata.groupId = PG_GETARG_INT32(2);

	/*
	 * During tests this function is called before nodeRole and nodeCluster have been
	 * created.
	 */
	if (PG_NARGS() == 3)
	{
		nodeMetadata.nodeRole = InvalidOid;
		nodeMetadata.nodeCluster = "default";
	}
	else
	{
		Name nodeClusterName = PG_GETARG_NAME(4);
		nodeMetadata.nodeCluster = NameStr(*nodeClusterName);

		nodeMetadata.nodeRole = PG_GETARG_OID(3);
	}

	if (nodeMetadata.groupId == COORDINATOR_GROUP_ID)
	{
		/* by default, we add the coordinator without shards */
		nodeMetadata.shouldHaveShards = false;
	}

	/*
	 * We do not allow metadata operations on secondary nodes in nontransactional
	 * sync mode.
	 */
	if (nodeMetadata.nodeRole == SecondaryNodeRoleId())
	{
		EnsureTransactionalMetadataSyncMode();
	}

	if (MetadataSyncTransMode == METADATA_SYNC_NON_TRANSACTIONAL &&
		IsMultiStatementTransaction())
	{
		/*
		 * prevent inside transaction block as we use bare connections which can
		 * lead deadlock
		 */
		ereport(ERROR, (errmsg("do not add node in transaction block "
							   "when the sync mode is nontransactional"),
						errhint("add the node after SET citus.metadata_sync_mode "
								"TO 'transactional'")));
	}

	int nodeId = AddNodeMetadataViaMetadataContext(nodeNameString, nodePort,
												   &nodeMetadata,
												   &nodeAlreadyExists);
	TransactionModifiedNodeMetadata = true;

	PG_RETURN_INT32(nodeId);
}


/*
 * master_add_node is a wrapper function for old UDF name.
 */
Datum
master_add_node(PG_FUNCTION_ARGS)
{
	return citus_add_node(fcinfo);
}


/*
 * citus_add_inactive_node function adds a new node to the cluster as inactive node
 * and returns id of the newly added node. It does not replicate reference
 * tables to the new node, it only adds new node to the pg_dist_node table.
 */
Datum
citus_add_inactive_node(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	text *nodeName = PG_GETARG_TEXT_P(0);
	int32 nodePort = PG_GETARG_INT32(1);
	char *nodeNameString = text_to_cstring(nodeName);
	Name nodeClusterName = PG_GETARG_NAME(4);

	NodeMetadata nodeMetadata = DefaultNodeMetadata();
	bool nodeAlreadyExists = false;
	nodeMetadata.groupId = PG_GETARG_INT32(2);
	nodeMetadata.nodeRole = PG_GETARG_OID(3);
	nodeMetadata.nodeCluster = NameStr(*nodeClusterName);

	if (nodeMetadata.groupId == COORDINATOR_GROUP_ID)
	{
		ereport(ERROR, (errmsg("coordinator node cannot be added as inactive node")));
	}

	/*
	 * We do not allow metadata operations on secondary nodes in nontransactional
	 * sync mode.
	 */
	if (nodeMetadata.nodeRole == SecondaryNodeRoleId())
	{
		EnsureTransactionalMetadataSyncMode();
	}

	bool localOnly = false;
	int nodeId = AddNodeMetadata(nodeNameString, nodePort, &nodeMetadata,
								 &nodeAlreadyExists, localOnly);
	TransactionModifiedNodeMetadata = true;

	PG_RETURN_INT32(nodeId);
}


/*
 * master_add_inactive_node is a wrapper function for old UDF name.
 */
Datum
master_add_inactive_node(PG_FUNCTION_ARGS)
{
	return citus_add_inactive_node(fcinfo);
}


/*
 * citus_add_secondary_node adds a new secondary node to the cluster. It accepts as
 * arguments the primary node it should share a group with.
 */
Datum
citus_add_secondary_node(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	text *nodeName = PG_GETARG_TEXT_P(0);
	int32 nodePort = PG_GETARG_INT32(1);
	char *nodeNameString = text_to_cstring(nodeName);

	text *primaryName = PG_GETARG_TEXT_P(2);
	int32 primaryPort = PG_GETARG_INT32(3);
	char *primaryNameString = text_to_cstring(primaryName);

	Name nodeClusterName = PG_GETARG_NAME(4);
	NodeMetadata nodeMetadata = DefaultNodeMetadata();
	bool nodeAlreadyExists = false;

	nodeMetadata.groupId = GroupForNode(primaryNameString, primaryPort);
	nodeMetadata.nodeCluster = NameStr(*nodeClusterName);
	nodeMetadata.nodeRole = SecondaryNodeRoleId();
	nodeMetadata.isActive = true;

	/*
	 * We do not allow metadata operations on secondary nodes in nontransactional
	 * sync mode.
	 */
	EnsureTransactionalMetadataSyncMode();

	bool localOnly = false;
	int nodeId = AddNodeMetadata(nodeNameString, nodePort, &nodeMetadata,
								 &nodeAlreadyExists, localOnly);
	TransactionModifiedNodeMetadata = true;

	PG_RETURN_INT32(nodeId);
}


/*
 * master_add_secondary_node is a wrapper function for old UDF name.
 */
Datum
master_add_secondary_node(PG_FUNCTION_ARGS)
{
	return citus_add_secondary_node(fcinfo);
}


/*
 * citus_remove_node function removes the provided node from the pg_dist_node table of
 * the master node and all nodes with metadata.
 * The call to the citus_remove_node should be done by the super user and the specified
 * node should not have any active placements.
 * This function also deletes all reference table placements belong to the given node from
 * pg_dist_placement, but it does not drop actual placement at the node. In the case of
 * re-adding the node, citus_add_node first drops and re-creates the reference tables.
 */
Datum
citus_remove_node(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	text *nodeNameText = PG_GETARG_TEXT_P(0);
	int32 nodePort = PG_GETARG_INT32(1);

	RemoveNodeFromCluster(text_to_cstring(nodeNameText), nodePort);
	TransactionModifiedNodeMetadata = true;

	PG_RETURN_VOID();
}


/*
 * master_remove_node is a wrapper function for old UDF name.
 */
Datum
master_remove_node(PG_FUNCTION_ARGS)
{
	return citus_remove_node(fcinfo);
}


/*
 * citus_disable_node function sets isactive value of the provided node as inactive at
 * coordinator and all nodes with metadata regardless of the node having an active shard
 * placement.
 *
 * The call to the citus_disable_node must be done by the super user.
 *
 * This function also deletes all reference table placements belong to the given node
 * from pg_dist_placement, but it does not drop actual placement at the node. In the case
 * of re-activating the node, citus_add_node first drops and re-creates the reference
 * tables.
 */
Datum
citus_disable_node(PG_FUNCTION_ARGS)
{
	text *nodeNameText = PG_GETARG_TEXT_P(0);
	int32 nodePort = PG_GETARG_INT32(1);

	bool synchronousDisableNode = 1;
	Assert(PG_NARGS() == 2 || PG_NARGS() == 3);
	if (PG_NARGS() == 3)
	{
		synchronousDisableNode = PG_GETARG_BOOL(2);
	}

	char *nodeName = text_to_cstring(nodeNameText);
	WorkerNode *workerNode = ModifiableWorkerNode(nodeName, nodePort);

	/* there is no concept of invalid coordinator */
	bool isActive = false;
	ErrorIfCoordinatorMetadataSetFalse(workerNode, BoolGetDatum(isActive),
									   "isactive");

	/*
	 * We do not allow metadata operations on secondary nodes in nontransactional
	 * sync mode.
	 */
	if (NodeIsSecondary(workerNode))
	{
		EnsureTransactionalMetadataSyncMode();
	}

	WorkerNode *firstWorkerNode = GetFirstPrimaryWorkerNode();
	bool disablingFirstNode =
		(firstWorkerNode && firstWorkerNode->nodeId == workerNode->nodeId);

	if (disablingFirstNode && !synchronousDisableNode)
	{
		/*
		 * We sync metadata async and optionally in the background worker,
		 * it would mean that some nodes might get the updates while other
		 * not. And, if the node metadata that is changing is the first
		 * worker node, the problem gets nasty. We serialize modifications
		 * to replicated tables by acquiring locks on the first worker node.
		 *
		 * If some nodes get the metadata changes and some do not, they'd be
		 * acquiring the locks on different nodes. Hence, having the
		 * possibility of diverged shard placements for the same shard.
		 *
		 * To prevent that, we currently do not allow disabling the first
		 * worker node unless it is explicitly opted synchronous.
		 */
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
						errmsg("disabling the first worker node in the "
							   "metadata is not allowed"),
						errhint("You can force disabling node, SELECT "
								"citus_disable_node('%s', %d, "
								"synchronous:=true);",
								workerNode->workerName,
								nodePort),
						errdetail("Citus uses the first worker node in the "
								  "metadata for certain internal operations when "
								  "replicated tables are modified. Synchronous mode "
								  "ensures that all nodes have the same view of the "
								  "first worker node, which is used for certain "
								  "locking operations.")));
	}

	/*
	 * First, locally mark the node as inactive. We'll later trigger background
	 * worker to sync the metadata changes to the relevant nodes.
	 */
	workerNode =
		SetWorkerColumnLocalOnly(workerNode,
								 Anum_pg_dist_node_isactive,
								 BoolGetDatum(isActive));
	if (NodeIsPrimary(workerNode))
	{
		/*
		 * We do not allow disabling nodes if it contains any
		 * primary placement that is the "only" active placement
		 * for any given shard.
		 */
		ErrorIfNodeContainsNonRemovablePlacements(workerNode);
	}

	TransactionModifiedNodeMetadata = true;

	if (synchronousDisableNode)
	{
		/*
		 * The user might pick between sync vs async options.
		 *      - Pros for the sync option:
		 *          (a) the changes become visible on the cluster immediately
		 *          (b) even if the first worker node is disabled, there is no
		 *              risk of divergence of the placements of replicated shards
		 *      - Cons for the sync options:
		 *          (a) Does not work within 2PC transaction (e.g., BEGIN;
		 *              citus_disable_node(); PREPARE TRANSACTION ...);
		 *          (b) If there are multiple node failures (e.g., one another node
		 *              than the current node being disabled), the sync option would
		 *              fail because it'd try to sync the metadata changes to a node
		 *              that is not up and running.
		 */
		if (firstWorkerNode && firstWorkerNode->nodeId == workerNode->nodeId)
		{
			/*
			 * We cannot let any modification query on a replicated table to run
			 * concurrently with citus_disable_node() on the first worker node. If
			 * we let that, some worker nodes might calculate FirstWorkerNode()
			 * different than others. See LockShardListResourcesOnFirstWorker()
			 * for the details.
			 */
			BlockDistributedQueriesOnMetadataNodes();
		}

		SyncNodeMetadataToNodes();
	}
	else if (UnsetMetadataSyncedForAllWorkers())
	{
		/*
		 * We have not propagated the node metadata changes yet, make sure that all the
		 * active nodes get the metadata updates. We defer this operation to the
		 * background worker to make it possible disabling nodes when multiple nodes
		 * are down.
		 *
		 * Note that the active placements reside on the active nodes. Hence, when
		 * Citus finds active placements, it filters out the placements that are on
		 * the disabled nodes. That's why, we don't have to change/sync placement
		 * metadata at this point. Instead, we defer that to citus_activate_node()
		 * where we expect all nodes up and running.
		 */

		TriggerNodeMetadataSyncOnCommit();
	}

	PG_RETURN_VOID();
}


/*
 * BlockDistributedQueriesOnMetadataNodes blocks all the modification queries on
 * all nodes. Hence, should be used with caution.
 */
static void
BlockDistributedQueriesOnMetadataNodes(void)
{
	/* first, block on the coordinator */
	LockRelationOid(DistNodeRelationId(), ExclusiveLock);

	/*
	 * Note that we might re-design this lock to be more granular than
	 * pg_dist_node, scoping only for modifications on the replicated
	 * tables. However, we currently do not have any such mechanism and
	 * given that citus_disable_node() runs instantly, it seems acceptable
	 * to block reads (or modifications on non-replicated tables) for
	 * a while.
	 */

	/* only superuser can disable node */
	Assert(superuser());

	SendCommandToWorkersWithMetadata(
		"LOCK TABLE pg_catalog.pg_dist_node IN EXCLUSIVE MODE;");
}


/*
 * master_disable_node is a wrapper function for old UDF name.
 */
Datum
master_disable_node(PG_FUNCTION_ARGS)
{
	return citus_disable_node(fcinfo);
}


/*
 * citus_set_node_property sets a property of the node
 */
Datum
citus_set_node_property(PG_FUNCTION_ARGS)
{
	text *nodeNameText = PG_GETARG_TEXT_P(0);
	int32 nodePort = PG_GETARG_INT32(1);
	text *propertyText = PG_GETARG_TEXT_P(2);
	bool value = PG_GETARG_BOOL(3);

	WorkerNode *workerNode = ModifiableWorkerNode(text_to_cstring(nodeNameText),
												  nodePort);

	/*
	 * We do not allow metadata operations on secondary nodes in nontransactional
	 * sync mode.
	 */
	if (NodeIsSecondary(workerNode))
	{
		EnsureTransactionalMetadataSyncMode();
	}

	if (strcmp(text_to_cstring(propertyText), "shouldhaveshards") == 0)
	{
		SetShouldHaveShards(workerNode, value);
	}
	else
	{
		ereport(ERROR, (errmsg(
							"only the 'shouldhaveshards' property can be set using this function")));
	}

	TransactionModifiedNodeMetadata = true;

	PG_RETURN_VOID();
}


/*
 * master_set_node_property is a wrapper function for old UDF name.
 */
Datum
master_set_node_property(PG_FUNCTION_ARGS)
{
	return citus_set_node_property(fcinfo);
}


/*
 * ModifiableWorkerNode gets the requested WorkerNode and also gets locks
 * required for modifying it. This fails if the node does not exist.
 */
WorkerNode *
ModifiableWorkerNode(const char *nodeName, int32 nodePort)
{
	CheckCitusVersion(ERROR);
	EnsureCoordinator();

	/* take an exclusive lock on pg_dist_node to serialize pg_dist_node changes */
	LockRelationOid(DistNodeRelationId(), ExclusiveLock);

	WorkerNode *workerNode = FindWorkerNodeAnyCluster(nodeName, nodePort);
	if (workerNode == NULL)
	{
		ereport(ERROR, (errmsg("node at \"%s:%u\" does not exist", nodeName, nodePort)));
	}

	return workerNode;
}


/*
 * citus_activate_node UDF activates the given node. It sets the node's isactive
 * value to active and replicates all reference tables to that node.
 */
Datum
citus_activate_node(PG_FUNCTION_ARGS)
{
	text *nodeNameText = PG_GETARG_TEXT_P(0);
	int32 nodePort = PG_GETARG_INT32(1);

	char *nodeNameString = text_to_cstring(nodeNameText);
	WorkerNode *workerNode = ModifiableWorkerNode(nodeNameString, nodePort);

	/*
	 * We do not allow metadata operations on secondary nodes in nontransactional
	 * sync mode.
	 */
	if (NodeIsSecondary(workerNode))
	{
		EnsureTransactionalMetadataSyncMode();
	}

	/*
	 * Create MetadataSyncContext which is used throughout nodes' activation.
	 * It contains activated nodes, bare connections if the mode is nontransactional,
	 * and a memory context for allocation.
	 */
	bool collectCommands = false;
	bool nodesAddedInSameTransaction = false;
	MetadataSyncContext *context = CreateMetadataSyncContext(list_make1(workerNode),
															 collectCommands,
															 nodesAddedInSameTransaction);

	ActivateNodeList(context);
	TransactionModifiedNodeMetadata = true;

	PG_RETURN_INT32(workerNode->nodeId);
}


/*
 * master_activate_node is a wrapper function for old UDF name.
 */
Datum
master_activate_node(PG_FUNCTION_ARGS)
{
	return citus_activate_node(fcinfo);
}


/*
 * GroupForNode returns the group which a given node belongs to.
 *
 * It only works if the requested node is a part of CurrentCluster.
 */
uint32
GroupForNode(char *nodeName, int nodePort)
{
	WorkerNode *workerNode = FindWorkerNode(nodeName, nodePort);

	if (workerNode == NULL)
	{
		ereport(ERROR, (errmsg("node at \"%s:%u\" does not exist", nodeName, nodePort)));
	}

	return workerNode->groupId;
}


/*
 * NodeIsPrimaryAndRemote returns whether the argument represents the remote
 * primary node.
 */
bool
NodeIsPrimaryAndRemote(WorkerNode *worker)
{
	return NodeIsPrimary(worker) && !NodeIsLocal(worker);
}


/*
 * NodeIsPrimary returns whether the argument represents a primary node.
 */
bool
NodeIsPrimary(WorkerNode *worker)
{
	Oid primaryRole = PrimaryNodeRoleId();

	/* if nodeRole does not yet exist, all nodes are primary nodes */
	if (primaryRole == InvalidOid)
	{
		return true;
	}

	return worker->nodeRole == primaryRole;
}


/*
 * NodeIsLocal returns whether the argument represents the local node.
 */
static bool
NodeIsLocal(WorkerNode *worker)
{
	return worker->groupId == GetLocalGroupId();
}


/*
 * NodeIsSecondary returns whether the argument represents a secondary node.
 */
bool
NodeIsSecondary(WorkerNode *worker)
{
	Oid secondaryRole = SecondaryNodeRoleId();

	/* if nodeRole does not yet exist, all nodes are primary nodes */
	if (secondaryRole == InvalidOid)
	{
		return false;
	}

	return worker->nodeRole == secondaryRole;
}


/*
 * NodeIsReadable returns whether we're allowed to send SELECT queries to this
 * node.
 */
bool
NodeIsReadable(WorkerNode *workerNode)
{
	if (ReadFromSecondaries == USE_SECONDARY_NODES_NEVER &&
		NodeIsPrimary(workerNode))
	{
		return true;
	}

	if (ReadFromSecondaries == USE_SECONDARY_NODES_ALWAYS &&
		NodeIsSecondary(workerNode))
	{
		return true;
	}

	return false;
}


/*
 * PrimaryNodeForGroup returns the (unique) primary in the specified group.
 *
 * If there are any nodes in the requested group and groupContainsNodes is not NULL
 * it will set the bool groupContainsNodes references to true.
 */
WorkerNode *
PrimaryNodeForGroup(int32 groupId, bool *groupContainsNodes)
{
	WorkerNode *workerNode = NULL;
	HASH_SEQ_STATUS status;
	HTAB *workerNodeHash = GetWorkerNodeHash();

	hash_seq_init(&status, workerNodeHash);

	while ((workerNode = hash_seq_search(&status)) != NULL)
	{
		int32 workerNodeGroupId = workerNode->groupId;
		if (workerNodeGroupId != groupId)
		{
			continue;
		}

		if (groupContainsNodes != NULL)
		{
			*groupContainsNodes = true;
		}

		if (NodeIsPrimary(workerNode))
		{
			hash_seq_term(&status);
			return workerNode;
		}
	}

	return NULL;
}


/*
 * MarkNodesNotSyncedInLoopBackConnection unsets metadatasynced flag in separate
 * connection to localhost by calling the udf `citus_internal_mark_node_not_synced`.
 */
static void
MarkNodesNotSyncedInLoopBackConnection(MetadataSyncContext *context,
									   pid_t parentSessionPid)
{
	Assert(context->transactionMode == METADATA_SYNC_NON_TRANSACTIONAL);
	Assert(!MetadataSyncCollectsCommands(context));

	/*
	 * Set metadatasynced to false for all activated nodes to mark the nodes as not synced
	 * in case nontransactional metadata sync fails before we activate the nodes inside
	 * metadataSyncContext.
	 * We set metadatasynced to false at coordinator to mark the nodes as not synced. But we
	 * do not set isactive and hasmetadata flags to false as we still want to route queries
	 * to the nodes if their isactive flag is true and propagate DDL to the nodes if possible.
	 *
	 * NOTES:
	 * 1) We use separate connection to localhost as we would rollback the local
	 * transaction in case of failure.
	 * 2) Operator should handle problems at workers if any. Wworkers probably fail
	 * due to improper metadata when a query hits. Or DDL might fail due to desynced
	 * nodes. (when hasmetadata = true, metadatasynced = false)
	 * In those cases, proper metadata sync for the workers should be done.)
	 */

	/*
	 * Because we try to unset metadatasynced flag with a separate transaction,
	 * we could not find the new node if the node is added in the current local
	 * transaction. But, hopefully, we do not need to unset metadatasynced for
	 * the new node as local transaction would rollback in case of a failure.
	 */
	if (context->nodesAddedInSameTransaction)
	{
		return;
	}

	if (context->activatedWorkerNodeList == NIL)
	{
		return;
	}

	int connectionFlag = FORCE_NEW_CONNECTION;
	MultiConnection *connection = GetNodeConnection(connectionFlag, LocalHostName,
													PostPortNumber);

	List *commandList = NIL;
	WorkerNode *workerNode = NULL;
	foreach_declared_ptr(workerNode, context->activatedWorkerNodeList)
	{
		/*
		 * We need to prevent self deadlock when we access pg_dist_node using separate
		 * connection to localhost. To achieve this, we check if the caller session's
		 * pid holds the Exclusive lock on pg_dist_node. After ensuring that (we are
		 * called from parent session which holds the Exclusive lock), we can safely
		 * update node metadata by acquiring the relaxed lock.
		 */
		StringInfo metadatasyncCommand = makeStringInfo();
		appendStringInfo(metadatasyncCommand, CITUS_INTERNAL_MARK_NODE_NOT_SYNCED,
						 parentSessionPid, workerNode->nodeId);
		commandList = lappend(commandList, metadatasyncCommand->data);
	}

	SendCommandListToWorkerOutsideTransactionWithConnection(connection, commandList);
	CloseConnection(connection);
}


/*
 * SetNodeMetadata sets isactive, metadatasynced and hasmetadata flags locally
 * and, if required, remotely.
 */
static void
SetNodeMetadata(MetadataSyncContext *context, bool localOnly)
{
	/* do not execute local transaction if we collect commands */
	if (!MetadataSyncCollectsCommands(context))
	{
		List *updatedActivatedNodeList = NIL;

		WorkerNode *node = NULL;
		foreach_declared_ptr(node, context->activatedWorkerNodeList)
		{
			node = SetWorkerColumnLocalOnly(node, Anum_pg_dist_node_isactive,
											BoolGetDatum(true));
			node = SetWorkerColumnLocalOnly(node, Anum_pg_dist_node_metadatasynced,
											BoolGetDatum(true));
			node = SetWorkerColumnLocalOnly(node, Anum_pg_dist_node_hasmetadata,
											BoolGetDatum(true));

			updatedActivatedNodeList = lappend(updatedActivatedNodeList, node);
		}

		/* reset activated nodes inside metadataSyncContext afer local update */
		SetMetadataSyncNodesFromNodeList(context, updatedActivatedNodeList);
	}

	if (!localOnly && EnableMetadataSync)
	{
		WorkerNode *node = NULL;
		foreach_declared_ptr(node, context->activatedWorkerNodeList)
		{
			SetNodeStateViaMetadataContext(context, node, BoolGetDatum(true));
		}
	}
}


/*
 * ActivateNodeList does some sanity checks and acquire Exclusive lock on pg_dist_node,
 * and then activates the nodes inside given metadataSyncContext.
 *
 * The function operates in 3 different modes according to transactionMode inside
 * metadataSyncContext.
 *
 * 1. MetadataSyncCollectsCommands(context):
 *      Only collect commands instead of sending them to workers,
 * 2. context.transactionMode == METADATA_SYNC_TRANSACTIONAL:
 *      Send all commands using coordinated transaction,
 * 3. context.transactionMode == METADATA_SYNC_NON_TRANSACTIONAL:
 *      Send all commands using bare (no transaction block) connections.
 */
void
ActivateNodeList(MetadataSyncContext *context)
{
	if (context->transactionMode == METADATA_SYNC_NON_TRANSACTIONAL &&
		IsMultiStatementTransaction())
	{
		/*
		 * prevent inside transaction block as we use bare connections which can
		 * lead deadlock
		 */
		ereport(ERROR, (errmsg("do not sync metadata in transaction block "
							   "when the sync mode is nontransactional"),
						errhint("resync after SET citus.metadata_sync_mode "
								"TO 'transactional'")));
	}

	/*
	 * We currently require the object propagation to happen via superuser,
	 * see #5139. While activating a node, we sync both metadata and object
	 * propagation.
	 *
	 * In order to have a fully transactional semantics with add/activate
	 * node operations, we require superuser. Note that for creating
	 * non-owned objects, we already require a superuser connection.
	 * By ensuring the current user to be a superuser, we can guarantee
	 * to send all commands within the same remote transaction.
	 */
	EnsureSuperUser();

	/*
	 * Take an exclusive lock on pg_dist_node to serialize pg_dist_node
	 * changes.
	 */
	LockRelationOid(DistNodeRelationId(), ExclusiveLock);

	/*
	 * Error if there is concurrent change to node table before acquiring
	 * the lock
	 */
	ErrorIfAnyNodeNotExist(context->activatedWorkerNodeList);

	/*
	 * we need to unset metadatasynced flag to false at coordinator in separate
	 * transaction only at nontransactional sync mode and if we do not collect
	 * commands.
	 *
	 * We make sure we set the flag to false at the start of nontransactional
	 * metadata sync to mark those nodes are not synced in case of a failure in
	 * the middle of the sync.
	 */
	if (context->transactionMode == METADATA_SYNC_NON_TRANSACTIONAL &&
		!MetadataSyncCollectsCommands(context))
	{
		MarkNodesNotSyncedInLoopBackConnection(context, MyProcPid);
	}

	/*
	 * Delete existing reference and replicated table placements on the
	 * given groupId if the group has been disabled earlier (e.g., isActive
	 * set to false).
	 */
	SendDeletionCommandsForReplicatedTablePlacements(context);

	/*
	 * SetNodeMetadata sets isactive, metadatasynced and hasmetadata flags
	 * locally for following reasons:
	 *
	 * 1) Set isactive to true locally so that we can find activated nodes amongst
	 *    active workers,
	 * 2) Do not fail just because the current metadata is not synced. (see
	 *    ErrorIfAnyMetadataNodeOutOfSync),
	 * 3) To propagate activated nodes nodemetadata correctly.
	 *
	 * We are going to sync the metadata anyway in this transaction, set
	 * isactive, metadatasynced, and hasmetadata to true locally.
	 * The changes would rollback in case of failure.
	 */
	bool localOnly = true;
	SetNodeMetadata(context, localOnly);

	/*
	 * Update local group ids so that upcoming transactions can see its effect.
	 * Object dependency logic requires to have updated local group id.
	 */
	UpdateLocalGroupIdsViaMetadataContext(context);

	/*
	 * Sync node metadata so that placement insertion does not fail due to
	 * EnsureShardPlacementMetadataIsSane.
	 */
	SyncNodeMetadata(context);

	/*
	 * Sync all dependencies and distributed objects with their pg_dist_xx tables to
	 * metadata nodes inside metadataSyncContext. Depends on node metadata.
	 */
	SyncDistributedObjects(context);

	/*
	 * Let all nodes to be active and synced after all operations succeeded.
	 * we make sure that the metadata sync is idempotent and safe overall with multiple
	 * other transactions, if nontransactional mode is used.
	 *
	 * We already took Exclusive lock on node metadata, which prevents modification
	 * on node metadata on coordinator. The step will rollback, in case of a failure,
	 * to the state where metadatasynced=false.
	 */
	localOnly = false;
	SetNodeMetadata(context, localOnly);
}


/*
 * Acquires shard metadata locks on all shards residing in the given worker node
 *
 * TODO: This function is not compatible with query from any node feature.
 * To ensure proper behavior, it is essential to acquire locks on placements across all nodes
 * rather than limiting it to just the coordinator (or the specific node from which this function is called)
 */
void
LockShardsInWorkerPlacementList(WorkerNode *workerNode, LOCKMODE lockMode)
{
	List *placementList = AllShardPlacementsOnNodeGroup(workerNode->groupId);
	LockShardsInPlacementListMetadata(placementList, lockMode);
}


/*
 * This function is used to start a background worker to kill backends holding conflicting
 * locks with this backend. It returns NULL if the background worker could not be started.
 */
BackgroundWorkerHandle *
CheckBackgroundWorkerToObtainLocks(int32 lock_cooldown)
{
	BackgroundWorkerHandle *handle = StartLockAcquireHelperBackgroundWorker(MyProcPid,
																			lock_cooldown);
	if (!handle)
	{
		/*
		 * We failed to start a background worker, which probably means that we exceeded
		 * max_worker_processes, and this is unlikely to be resolved by retrying. We do not want
		 * to repeatedly throw an error because if citus_update_node is called to complete a
		 * failover then finishing is the only way to bring the cluster back up. Therefore we
		 * give up on killing other backends and simply wait for the lock. We do set
		 * lock_timeout to lock_cooldown, because we don't want to wait forever to get a lock.
		 */
		SetLockTimeoutLocally(lock_cooldown);
		ereport(WARNING, (errmsg(
							  "could not start background worker to kill backends with conflicting"
							  " locks to force the update. Degrading to acquiring locks "
							  "with a lock time out."),
						  errhint(
							  "Increasing max_worker_processes might help.")));
	}
	return handle;
}


/*
 * This function is used to lock shards in a primary node.
 * If force is true, we start a background worker to kill backends holding
 * conflicting locks with this backend.
 *
 * If the node is a primary node we block reads and writes.
 *
 * This lock has two purposes:
 *
 * - Ensure buggy code in Citus doesn't cause failures when the
 *   nodename/nodeport of a node changes mid-query
 *
 * - Provide fencing during failover, after this function returns all
 *   connections will use the new node location.
 *
 * Drawback:
 *
 * - This function blocks until all previous queries have finished. This
 *   means that long-running queries will prevent failover.
 *
 *   In case of node failure said long-running queries will fail in the end
 *   anyway as they will be unable to commit successfully on the failed
 *   machine. To cause quick failure of these queries use force => true
 *   during the invocation of citus_update_node to terminate conflicting
 *   backends proactively.
 *
 * It might be worth blocking reads to a secondary for the same reasons,
 * though we currently only query secondaries on follower clusters
 * where these locks will have no effect.
 */
BackgroundWorkerHandle *
LockPlacementsWithBackgroundWorkersInPrimaryNode(WorkerNode *workerNode, bool force, int32
												 lock_cooldown)
{
	BackgroundWorkerHandle *handle = NULL;

	if (NodeIsPrimary(workerNode))
	{
		if (force)
		{
			handle = CheckBackgroundWorkerToObtainLocks(lock_cooldown);
		}
		LockShardsInWorkerPlacementList(workerNode, AccessExclusiveLock);
	}
	return handle;
}


/*
 * citus_update_node moves the requested node to a different nodename and nodeport. It
 * locks to ensure no queries are running concurrently; and is intended for customers who
 * are running their own failover solution.
 */
Datum
citus_update_node(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	int32 nodeId = PG_GETARG_INT32(0);

	text *newNodeName = PG_GETARG_TEXT_P(1);
	int32 newNodePort = PG_GETARG_INT32(2);

	/*
	 * force is used when an update needs to happen regardless of conflicting locks. This
	 * feature is important to force the update during a failover due to failure, eg. by
	 * a high-availability system such as pg_auto_failover. The strategy is to start a
	 * background worker that actively cancels backends holding conflicting locks with
	 * this backend.
	 *
	 * Defaults to false
	 */
	bool force = PG_GETARG_BOOL(3);
	int32 lock_cooldown = PG_GETARG_INT32(4);

	char *newNodeNameString = text_to_cstring(newNodeName);

	WorkerNode *workerNodeWithSameAddress = FindWorkerNodeAnyCluster(newNodeNameString,
																	 newNodePort);
	if (workerNodeWithSameAddress != NULL)
	{
		/* a node with the given hostname and port already exists in the metadata */

		if (workerNodeWithSameAddress->nodeId == nodeId)
		{
			/* it's the node itself, meaning this is a noop update */
			PG_RETURN_VOID();
		}
		else
		{
			ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
							errmsg("there is already another node with the specified "
								   "hostname and port")));
		}
	}

	WorkerNode *workerNode = FindNodeAnyClusterByNodeId(nodeId);
	if (workerNode == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND),
						errmsg("node %u not found", nodeId)));
	}

	/*
	 * We do not allow metadata operations on secondary nodes in nontransactional
	 * sync mode.
	 */
	if (NodeIsSecondary(workerNode))
	{
		EnsureTransactionalMetadataSyncMode();
	}

	BackgroundWorkerHandle *handle = LockPlacementsWithBackgroundWorkersInPrimaryNode(
		workerNode, force,
		lock_cooldown);

	/*
	 * if we have planned statements such as prepared statements, we should clear the cache so that
	 * the planned cache doesn't return the old nodename/nodepost.
	 */
	ResetPlanCache();

	bool localOnly = true;
	UpdateNodeLocation(nodeId, newNodeNameString, newNodePort, localOnly);

	/* we should be able to find the new node from the metadata */
	workerNode = FindWorkerNodeAnyCluster(newNodeNameString, newNodePort);
	Assert(workerNode->nodeId == nodeId);

	/*
	 * Propagate the updated pg_dist_node entry to all metadata workers.
	 * citus-ha uses citus_update_node() in a prepared transaction, and
	 * we don't support coordinated prepared transactions, so we cannot
	 * propagate the changes to the worker nodes here. Instead we mark
	 * all metadata nodes as not-synced and ask maintenanced to do the
	 * propagation.
	 *
	 * It is possible that maintenance daemon does the first resync too
	 * early, but that's fine, since this will start a retry loop with
	 * 5 second intervals until sync is complete.
	 */
	if (UnsetMetadataSyncedForAllWorkers())
	{
		TriggerNodeMetadataSyncOnCommit();
	}

	if (handle != NULL)
	{
		/*
		 * this will be called on memory context cleanup as well, if the worker has been
		 * terminated already this will be a noop
		 */
		TerminateBackgroundWorker(handle);
	}

	TransactionModifiedNodeMetadata = true;

	PG_RETURN_VOID();
}


/*
 * This function is designed to obtain locks for all the shards in a worker placement list.
 * Once the transaction is committed, the acquired locks will be automatically released.
 * Therefore, it is essential to invoke this function within a transaction.
 * This function proves beneficial when there is a need to temporarily disable writes to a specific node within a transaction.
 */
Datum
citus_pause_node_within_txn(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	int32 nodeId = PG_GETARG_INT32(0);
	bool force = PG_GETARG_BOOL(1);
	int32 lock_cooldown = PG_GETARG_INT32(2);

	WorkerNode *workerNode = FindNodeAnyClusterByNodeId(nodeId);
	if (workerNode == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND),
						errmsg("node %u not found", nodeId)));
	}

	LockPlacementsWithBackgroundWorkersInPrimaryNode(workerNode, force, lock_cooldown);

	PG_RETURN_VOID();
}


/*
 * master_update_node is a wrapper function for old UDF name.
 */
Datum
master_update_node(PG_FUNCTION_ARGS)
{
	return citus_update_node(fcinfo);
}


/*
 * SetLockTimeoutLocally sets the lock_timeout to the given value.
 * This setting is local.
 */
static void
SetLockTimeoutLocally(int32 lockCooldown)
{
	set_config_option("lock_timeout", ConvertIntToString(lockCooldown),
					  (superuser() ? PGC_SUSET : PGC_USERSET), PGC_S_SESSION,
					  GUC_ACTION_LOCAL, true, 0, false);
}


static void
UpdateNodeLocation(int32 nodeId, char *newNodeName, int32 newNodePort, bool localOnly)
{
	const bool indexOK = true;

	ScanKeyData scanKey[1];
	Datum values[Natts_pg_dist_node];
	bool isnull[Natts_pg_dist_node];
	bool replace[Natts_pg_dist_node];

	Relation pgDistNode = table_open(DistNodeRelationId(), RowExclusiveLock);
	TupleDesc tupleDescriptor = RelationGetDescr(pgDistNode);

	ScanKeyInit(&scanKey[0], Anum_pg_dist_node_nodeid,
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(nodeId));

	SysScanDesc scanDescriptor = systable_beginscan(pgDistNode, DistNodeNodeIdIndexId(),
													indexOK,
													NULL, 1, scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	if (!HeapTupleIsValid(heapTuple))
	{
		ereport(ERROR, (errmsg("could not find valid entry for node \"%s:%d\"",
							   newNodeName, newNodePort)));
	}

	memset(replace, 0, sizeof(replace));

	values[Anum_pg_dist_node_nodeport - 1] = Int32GetDatum(newNodePort);
	isnull[Anum_pg_dist_node_nodeport - 1] = false;
	replace[Anum_pg_dist_node_nodeport - 1] = true;

	values[Anum_pg_dist_node_nodename - 1] = CStringGetTextDatum(newNodeName);
	isnull[Anum_pg_dist_node_nodename - 1] = false;
	replace[Anum_pg_dist_node_nodename - 1] = true;

	heapTuple = heap_modify_tuple(heapTuple, tupleDescriptor, values, isnull, replace);

	CatalogTupleUpdate(pgDistNode, &heapTuple->t_self, heapTuple);

	CitusInvalidateRelcacheByRelid(DistNodeRelationId());

	CommandCounterIncrement();

	if (!localOnly && EnableMetadataSync)
	{
		WorkerNode *updatedNode = FindWorkerNodeAnyCluster(newNodeName, newNodePort);
		Assert(updatedNode->nodeId == nodeId);

		/* send the delete command to all primary nodes with metadata */
		char *nodeDeleteCommand = NodeDeleteCommand(updatedNode->nodeId);
		SendCommandToWorkersWithMetadata(nodeDeleteCommand);

		/* send the insert command to all primary nodes with metadata */
		char *nodeInsertCommand = NodeListInsertCommand(list_make1(updatedNode));
		SendCommandToWorkersWithMetadata(nodeInsertCommand);
	}

	systable_endscan(scanDescriptor);
	table_close(pgDistNode, NoLock);
}


/*
 * get_shard_id_for_distribution_column function takes a distributed table name and a
 * distribution value then returns shard id of the shard which belongs to given table and
 * contains given value. This function only works for hash distributed tables.
 */
Datum
get_shard_id_for_distribution_column(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	ShardInterval *shardInterval = NULL;

	/*
	 * To have optional parameter as NULL, we defined this UDF as not strict, therefore
	 * we need to check all parameters for NULL values.
	 */
	if (PG_ARGISNULL(0))
	{
		ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
						errmsg("relation cannot be NULL")));
	}

	Oid relationId = PG_GETARG_OID(0);
	EnsureTablePermissions(relationId, ACL_SELECT, ACLMASK_ANY);

	if (!IsCitusTable(relationId))
	{
		ereport(ERROR, (errcode(ERRCODE_INVALID_TABLE_DEFINITION),
						errmsg("relation is not distributed")));
	}

	if (!HasDistributionKey(relationId))
	{
		List *shardIntervalList = LoadShardIntervalList(relationId);
		if (shardIntervalList == NIL)
		{
			PG_RETURN_INT64(0);
		}

		shardInterval = (ShardInterval *) linitial(shardIntervalList);
	}
	else if (IsCitusTableType(relationId, HASH_DISTRIBUTED) ||
			 IsCitusTableType(relationId, RANGE_DISTRIBUTED))
	{
		CitusTableCacheEntry *cacheEntry = GetCitusTableCacheEntry(relationId);

		/* if given table is not reference table, distributionValue cannot be NULL */
		if (PG_ARGISNULL(1))
		{
			ereport(ERROR, (errcode(ERRCODE_NULL_VALUE_NOT_ALLOWED),
							errmsg("distribution value cannot be NULL for tables other "
								   "than reference tables.")));
		}

		Datum inputDatum = PG_GETARG_DATUM(1);
		Oid inputDataType = get_fn_expr_argtype(fcinfo->flinfo, 1);
		char *distributionValueString = DatumToString(inputDatum, inputDataType);

		Var *distributionColumn = DistPartitionKeyOrError(relationId);
		Oid distributionDataType = distributionColumn->vartype;

		Datum distributionValueDatum = StringToDatum(distributionValueString,
													 distributionDataType);

		shardInterval = FindShardInterval(distributionValueDatum, cacheEntry);
	}
	else
	{
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
						errmsg("finding shard id of given distribution value is only "
							   "supported for hash partitioned tables, range partitioned "
							   "tables and reference tables.")));
	}

	if (shardInterval != NULL)
	{
		PG_RETURN_INT64(shardInterval->shardId);
	}

	PG_RETURN_INT64(0);
}


/*
 * citus_nodename_for_nodeid returns the node name for the node with given node id
 */
Datum
citus_nodename_for_nodeid(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	int nodeId = PG_GETARG_INT32(0);

	WorkerNode *node = FindNodeAnyClusterByNodeId(nodeId);

	if (node == NULL)
	{
		PG_RETURN_NULL();
	}

	PG_RETURN_TEXT_P(cstring_to_text(node->workerName));
}


/*
 * citus_nodeport_for_nodeid returns the node port for the node with given node id
 */
Datum
citus_nodeport_for_nodeid(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	int nodeId = PG_GETARG_INT32(0);

	WorkerNode *node = FindNodeAnyClusterByNodeId(nodeId);

	if (node == NULL)
	{
		PG_RETURN_NULL();
	}

	PG_RETURN_INT32(node->workerPort);
}


/*
 * citus_coordinator_nodeid returns the node id of the coordinator node
 */
Datum
citus_coordinator_nodeid(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	int coordinatorNodeId = FindCoordinatorNodeId();

	if (coordinatorNodeId == -1)
	{
		PG_RETURN_INT32(0);
	}

	PG_RETURN_INT32(coordinatorNodeId);
}


/*
 * citus_is_coordinator returns whether the current node is a coordinator.
 * We consider the node a coordinator if its group ID is 0 and it has
 * pg_dist_node entries (only group ID 0 could indicate a worker without
 * metadata).
 */
Datum
citus_is_coordinator(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	bool isCoordinator = false;

	if (GetLocalGroupId() == COORDINATOR_GROUP_ID &&
		ActiveReadableNodeCount() > 0)
	{
		isCoordinator = true;
	}

	PG_RETURN_BOOL(isCoordinator);
}


/*
 * citus_is_primary_node returns whether the current node is a primary for
 * a given group_id. We consider the node a primary if it has
 * pg_dist_node entries marked as primary
 */
Datum
citus_is_primary_node(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	int32 groupId = GetLocalGroupId();
	WorkerNode *workerNode = PrimaryNodeForGroup(groupId, NULL);
	if (workerNode == NULL)
	{
		ereport(WARNING, (errmsg("could not find the current node in pg_dist_node"),
						  errdetail("If this is the coordinator node, consider adding it "
									"into the metadata by using citus_set_coordinator_host() "
									"UDF. Otherwise, if you're going to use this node as a "
									"worker node for a new cluster, make sure to add this "
									"node into the metadata from the coordinator by using "
									"citus_add_node() UDF.")));
		PG_RETURN_NULL();
	}

	bool isPrimary = workerNode->nodeId == GetLocalNodeId();

	PG_RETURN_BOOL(isPrimary);
}


/*
 * EnsureParentSessionHasExclusiveLockOnPgDistNode ensures given session id
 * holds Exclusive lock on pg_dist_node.
 */
static void
EnsureParentSessionHasExclusiveLockOnPgDistNode(pid_t parentSessionPid)
{
	StringInfo checkIfParentLockCommandStr = makeStringInfo();

	int spiConnectionResult = SPI_connect();
	if (spiConnectionResult != SPI_OK_CONNECT)
	{
		ereport(ERROR, (errmsg("could not connect to SPI manager")));
	}

	char *checkIfParentLockCommand = "SELECT pid FROM pg_locks WHERE "
									 "pid = %d AND database = %d AND relation = %d AND "
									 "mode = 'ExclusiveLock' AND granted = TRUE";
	appendStringInfo(checkIfParentLockCommandStr, checkIfParentLockCommand,
					 parentSessionPid, MyDatabaseId, DistNodeRelationId());

	bool readOnly = true;
	int spiQueryResult = SPI_execute(checkIfParentLockCommandStr->data, readOnly, 0);
	if (spiQueryResult != SPI_OK_SELECT)
	{
		ereport(ERROR, (errmsg("execution was not successful \"%s\"",
							   checkIfParentLockCommandStr->data)));
	}

	bool parentHasExclusiveLock = SPI_processed > 0;

	SPI_finish();

	if (!parentHasExclusiveLock)
	{
		ereport(ERROR, (errmsg("lock is not held by the caller. Unexpected caller "
							   "for citus_internal.mark_node_not_synced")));
	}
}


/*
 * citus_internal_mark_node_not_synced unsets metadatasynced flag in separate connection
 * to localhost. Should only be called by `MarkNodesNotSyncedInLoopBackConnection`.
 * See it for details.
 */
Datum
citus_internal_mark_node_not_synced(PG_FUNCTION_ARGS)
{
	CheckCitusVersion(ERROR);

	/* only called by superuser */
	EnsureSuperUser();

	pid_t parentSessionPid = PG_GETARG_INT32(0);

	/* fetch node by id */
	int nodeId = PG_GETARG_INT32(1);
	HeapTuple heapTuple = GetNodeByNodeId(nodeId);

	/* ensure that parent session holds Exclusive lock to pg_dist_node */
	EnsureParentSessionHasExclusiveLockOnPgDistNode(parentSessionPid);

	/*
	 * We made sure parent session holds the ExclusiveLock, so we can unset
	 * metadatasynced for the node safely with the relaxed lock here.
	 */
	Relation pgDistNode = table_open(DistNodeRelationId(), AccessShareLock);
	TupleDesc tupleDescriptor = RelationGetDescr(pgDistNode);

	Datum values[Natts_pg_dist_node];
	bool isnull[Natts_pg_dist_node];
	bool replace[Natts_pg_dist_node];

	memset(replace, 0, sizeof(replace));
	values[Anum_pg_dist_node_metadatasynced - 1] = DatumGetBool(false);
	isnull[Anum_pg_dist_node_metadatasynced - 1] = false;
	replace[Anum_pg_dist_node_metadatasynced - 1] = true;

	heapTuple = heap_modify_tuple(heapTuple, tupleDescriptor, values, isnull, replace);

	CatalogTupleUpdate(pgDistNode, &heapTuple->t_self, heapTuple);

	CitusInvalidateRelcacheByRelid(DistNodeRelationId());
	CommandCounterIncrement();

	table_close(pgDistNode, NoLock);

	PG_RETURN_VOID();
}


/*
 * FindWorkerNode searches over the worker nodes and returns the workerNode
 * if it already exists. Else, the function returns NULL.
 *
 * NOTE: A special case that this handles is when nodeName and nodePort are set
 * to LocalHostName and PostPortNumber. In that case we return the primary node
 * for the local group.
 */
WorkerNode *
FindWorkerNode(const char *nodeName, int32 nodePort)
{
	HTAB *workerNodeHash = GetWorkerNodeHash();
	bool handleFound = false;

	WorkerNode *searchedNode = (WorkerNode *) palloc0(sizeof(WorkerNode));
	strlcpy(searchedNode->workerName, nodeName, WORKER_LENGTH);
	searchedNode->workerPort = nodePort;

	void *hashKey = (void *) searchedNode;
	WorkerNode *cachedWorkerNode = (WorkerNode *) hash_search(workerNodeHash, hashKey,
															  HASH_FIND,
															  &handleFound);
	if (handleFound)
	{
		WorkerNode *workerNode = (WorkerNode *) palloc(sizeof(WorkerNode));
		*workerNode = *cachedWorkerNode;
		return workerNode;
	}

	if (strcmp(LocalHostName, nodeName) == 0 && nodePort == PostPortNumber)
	{
		return PrimaryNodeForGroup(GetLocalGroupId(), NULL);
	}

	return NULL;
}


/*
 * FindWorkerNode searches over the worker nodes and returns the workerNode
 * if it exists otherwise it errors out.
 */
WorkerNode *
FindWorkerNodeOrError(const char *nodeName, int32 nodePort)
{
	WorkerNode *node = FindWorkerNode(nodeName, nodePort);
	if (node == NULL)
	{
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND),
						errmsg("node %s:%d not found", nodeName, nodePort)));
	}
	return node;
}


/*
 * FindWorkerNodeAnyCluster returns the workerNode no matter which cluster it is a part
 * of. FindWorkerNodes, like almost every other function, acts as if nodes in other
 * clusters do not exist.
 */
WorkerNode *
FindWorkerNodeAnyCluster(const char *nodeName, int32 nodePort)
{
	WorkerNode *workerNode = NULL;

	Relation pgDistNode = table_open(DistNodeRelationId(), AccessShareLock);
	TupleDesc tupleDescriptor = RelationGetDescr(pgDistNode);

	HeapTuple heapTuple = GetNodeTuple(nodeName, nodePort);
	if (heapTuple != NULL)
	{
		workerNode = TupleToWorkerNode(tupleDescriptor, heapTuple);
	}

	table_close(pgDistNode, NoLock);
	return workerNode;
}


/*
 * FindNodeAnyClusterByNodeId searches pg_dist_node and returns the node with
 * the nodeId. If the node can't be found returns NULL.
 */
static WorkerNode *
FindNodeAnyClusterByNodeId(uint32 nodeId)
{
	bool includeNodesFromOtherClusters = true;
	List *nodeList = ReadDistNode(includeNodesFromOtherClusters);
	WorkerNode *node = NULL;

	foreach_declared_ptr(node, nodeList)
	{
		if (node->nodeId == nodeId)
		{
			return node;
		}
	}

	return NULL;
}


/*
 * FindNodeWithNodeId searches pg_dist_node and returns the node with the nodeId.
 * If the node cannot be found this functions errors.
 */
WorkerNode *
FindNodeWithNodeId(int nodeId, bool missingOk)
{
	List *nodeList = ActiveReadableNodeList();
	WorkerNode *node = NULL;

	foreach_declared_ptr(node, nodeList)
	{
		if (node->nodeId == nodeId)
		{
			return node;
		}
	}

	/* there isn't any node with nodeId in pg_dist_node */
	if (!missingOk)
	{
		elog(ERROR, "node with node id %d could not be found", nodeId);
	}

	return NULL;
}


/*
 * FindCoordinatorNodeId returns the node id of the coordinator node
 */
int
FindCoordinatorNodeId()
{
	bool includeNodesFromOtherClusters = false;
	List *nodeList = ReadDistNode(includeNodesFromOtherClusters);
	WorkerNode *node = NULL;

	foreach_declared_ptr(node, nodeList)
	{
		if (NodeIsCoordinator(node))
		{
			return node->nodeId;
		}
	}

	return -1;
}


/*
 * ReadDistNode iterates over pg_dist_node table, converts each row
 * into its memory representation (i.e., WorkerNode) and adds them into
 * a list. Lastly, the list is returned to the caller.
 *
 * It skips nodes which are not in the current clusters unless requested to do otherwise
 * by includeNodesFromOtherClusters.
 */
List *
ReadDistNode(bool includeNodesFromOtherClusters)
{
	ScanKeyData scanKey[1];
	int scanKeyCount = 0;
	List *workerNodeList = NIL;

	Relation pgDistNode = table_open(DistNodeRelationId(), AccessShareLock);

	SysScanDesc scanDescriptor = systable_beginscan(pgDistNode,
													InvalidOid, false,
													NULL, scanKeyCount, scanKey);

	TupleDesc tupleDescriptor = RelationGetDescr(pgDistNode);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	while (HeapTupleIsValid(heapTuple))
	{
		WorkerNode *workerNode = TupleToWorkerNode(tupleDescriptor, heapTuple);

		if (includeNodesFromOtherClusters ||
			strncmp(workerNode->nodeCluster, CurrentCluster, WORKER_LENGTH) == 0)
		{
			/* the coordinator acts as if it never sees nodes not in its cluster */
			workerNodeList = lappend(workerNodeList, workerNode);
		}

		heapTuple = systable_getnext(scanDescriptor);
	}

	systable_endscan(scanDescriptor);
	table_close(pgDistNode, NoLock);

	return workerNodeList;
}


/*
 * RemoveNodeFromCluster removes the provided node from the pg_dist_node table of
 * the master node and all nodes with metadata.
 * The call to the master_remove_node should be done by the super user. If there are
 * active shard placements on the node; the function errors out.
 * This function also deletes all reference table placements belong to the given node from
 * pg_dist_placement, but it does not drop actual placement at the node. It also
 * modifies replication factor of the colocation group of reference tables, so that
 * replication factor will be equal to worker count.
 */
static void
RemoveNodeFromCluster(char *nodeName, int32 nodePort)
{
	WorkerNode *workerNode = ModifiableWorkerNode(nodeName, nodePort);

	/*
	 * We do not allow metadata operations on secondary nodes in nontransactional
	 * sync mode.
	 */
	if (NodeIsSecondary(workerNode))
	{
		EnsureTransactionalMetadataSyncMode();
	}

	if (NodeIsPrimary(workerNode))
	{
		ErrorIfNodeContainsNonRemovablePlacements(workerNode);

		/*
		 * Delete reference table placements so they are not taken into account
		 * for the check if there are placements after this.
		 */
		bool localOnly = false;
		DeleteAllReplicatedTablePlacementsFromNodeGroup(workerNode->groupId,
														localOnly);

		/*
		 * Secondary nodes are read-only, never 2PC is used.
		 * Hence, no items can be inserted to pg_dist_transaction
		 * for secondary nodes.
		 */
		DeleteWorkerTransactions(workerNode);
	}

	DeleteNodeRow(workerNode->workerName, nodePort);

	/* make sure we don't have any lingering session lifespan connections */
	CloseNodeConnectionsAfterTransaction(workerNode->workerName, nodePort);

	if (EnableMetadataSync)
	{
		char *nodeDeleteCommand = NodeDeleteCommand(workerNode->nodeId);

		SendCommandToWorkersWithMetadata(nodeDeleteCommand);
	}
}


/*
 * ErrorIfNodeContainsNonRemovablePlacements throws an error if the input node
 * contains at least one placement on the node that is the last active
 * placement.
 */
static void
ErrorIfNodeContainsNonRemovablePlacements(WorkerNode *workerNode)
{
	int32 groupId = workerNode->groupId;
	List *shardPlacements = AllShardPlacementsOnNodeGroup(groupId);

	/* sort the list to prevent regression tests getting flaky */
	shardPlacements = SortList(shardPlacements, CompareGroupShardPlacements);

	GroupShardPlacement *placement = NULL;
	foreach_declared_ptr(placement, shardPlacements)
	{
		if (!PlacementHasActivePlacementOnAnotherGroup(placement))
		{
			Oid relationId = RelationIdForShard(placement->shardId);
			char *qualifiedRelationName = generate_qualified_relation_name(relationId);

			ereport(ERROR, (errmsg("cannot remove or disable the node "
								   "%s:%d because because it contains "
								   "the only shard placement for "
								   "shard " UINT64_FORMAT,
								   workerNode->workerName,
								   workerNode->workerPort, placement->shardId),
							errdetail("One of the table(s) that prevents the operation "
									  "complete successfully is %s",
									  qualifiedRelationName),
							errhint("To proceed, either drop the tables or use "
									"undistribute_table() function to convert "
									"them to local tables")));
		}
	}
}


/*
 * PlacementHasActivePlacementOnAnotherGroup returns true if there is at least
 * one more active placement of the input sourcePlacement on another group.
 */
static bool
PlacementHasActivePlacementOnAnotherGroup(GroupShardPlacement *sourcePlacement)
{
	uint64 shardId = sourcePlacement->shardId;
	List *activePlacementList = ActiveShardPlacementList(shardId);

	bool foundActivePlacementOnAnotherGroup = false;
	ShardPlacement *activePlacement = NULL;
	foreach_declared_ptr(activePlacement, activePlacementList)
	{
		if (activePlacement->groupId != sourcePlacement->groupId)
		{
			foundActivePlacementOnAnotherGroup = true;
			break;
		}
	}

	return foundActivePlacementOnAnotherGroup;
}


/* CountPrimariesWithMetadata returns the number of primary nodes which have metadata. */
uint32
CountPrimariesWithMetadata(void)
{
	uint32 primariesWithMetadata = 0;
	WorkerNode *workerNode = NULL;

	HASH_SEQ_STATUS status;
	HTAB *workerNodeHash = GetWorkerNodeHash();

	hash_seq_init(&status, workerNodeHash);

	while ((workerNode = hash_seq_search(&status)) != NULL)
	{
		if (workerNode->hasMetadata && NodeIsPrimary(workerNode))
		{
			primariesWithMetadata++;
		}
	}

	return primariesWithMetadata;
}


/*
 * AddNodeMetadata checks the given node information and adds the specified node to the
 * pg_dist_node table of the master and workers with metadata.
 * If the node already exists, the function returns the id of the node.
 * If not, the following procedure is followed while adding a node: If the groupId is not
 * explicitly given by the user, the function picks the group that the new node should
 * be in with respect to GroupSize. Then, the new node is inserted into the local
 * pg_dist_node as well as the nodes with hasmetadata=true if localOnly is false.
 */
static int
AddNodeMetadata(char *nodeName, int32 nodePort, NodeMetadata *nodeMetadata,
				bool *nodeAlreadyExists, bool localOnly)
{
	EnsureCoordinator();

	*nodeAlreadyExists = false;

	WorkerNode *workerNode = FindWorkerNodeAnyCluster(nodeName, nodePort);
	if (workerNode != NULL)
	{
		/* return early without holding locks when the node already exists */
		*nodeAlreadyExists = true;

		return workerNode->nodeId;
	}

	/*
	 * We are going to change pg_dist_node, prevent any concurrent reads that
	 * are not tolerant to concurrent node addition by taking an exclusive
	 * lock (conflicts with all but AccessShareLock).
	 *
	 * We may want to relax or have more fine-grained locking in the future
	 * to allow users to add multiple nodes concurrently.
	 */
	LockRelationOid(DistNodeRelationId(), ExclusiveLock);

	/* recheck in case 2 node additions pass the first check concurrently */
	workerNode = FindWorkerNodeAnyCluster(nodeName, nodePort);
	if (workerNode != NULL)
	{
		*nodeAlreadyExists = true;

		return workerNode->nodeId;
	}

	if (nodeMetadata->groupId != COORDINATOR_GROUP_ID &&
		strcmp(nodeName, "localhost") != 0)
	{
		/*
		 * User tries to add a worker with a non-localhost address. If the coordinator
		 * is added with "localhost" as well, the worker won't be able to connect.
		 */

		bool isCoordinatorInMetadata = false;
		WorkerNode *coordinatorNode = PrimaryNodeForGroup(COORDINATOR_GROUP_ID,
														  &isCoordinatorInMetadata);
		if (isCoordinatorInMetadata &&
			strcmp(coordinatorNode->workerName, "localhost") == 0)
		{
			ereport(ERROR, (errmsg("cannot add a worker node when the coordinator "
								   "hostname is set to localhost"),
							errdetail("Worker nodes need to be able to connect to the "
									  "coordinator to transfer data."),
							errhint("Use SELECT citus_set_coordinator_host('<hostname>') "
									"to configure the coordinator hostname")));
		}
	}

	/*
	 * When adding the first worker when the coordinator has shard placements,
	 * print a notice on how to drain the coordinator.
	 */
	if (nodeMetadata->groupId != COORDINATOR_GROUP_ID && CoordinatorAddedAsWorkerNode() &&
		ActivePrimaryNonCoordinatorNodeCount() == 0 &&
		NodeGroupHasShardPlacements(COORDINATOR_GROUP_ID))
	{
		WorkerNode *coordinator = CoordinatorNodeIfAddedAsWorkerOrError();

		ereport(NOTICE, (errmsg("shards are still on the coordinator after adding the "
								"new node"),
						 errhint("Use SELECT rebalance_table_shards(); to balance "
								 "shards data between workers and coordinator or "
								 "SELECT citus_drain_node(%s,%d); to permanently "
								 "move shards away from the coordinator.",
								 quote_literal_cstr(coordinator->workerName),
								 coordinator->workerPort)));
	}

	/* user lets Citus to decide on the group that the newly added node should be in */
	if (nodeMetadata->groupId == INVALID_GROUP_ID)
	{
		nodeMetadata->groupId = GetNextGroupId();
	}

	if (nodeMetadata->groupId == COORDINATOR_GROUP_ID)
	{
		/*
		 * Coordinator has always the authoritative metadata, reflect this
		 * fact in the pg_dist_node.
		 */
		nodeMetadata->hasMetadata = true;
		nodeMetadata->metadataSynced = true;

		/*
		 * There is no concept of "inactive" coordinator, so hard code it.
		 */
		nodeMetadata->isActive = true;
	}

	/* if nodeRole hasn't been added yet there's a constraint for one-node-per-group */
	if (nodeMetadata->nodeRole != InvalidOid && nodeMetadata->nodeRole ==
		PrimaryNodeRoleId())
	{
		WorkerNode *existingPrimaryNode = PrimaryNodeForGroup(nodeMetadata->groupId,
															  NULL);

		if (existingPrimaryNode != NULL)
		{
			ereport(ERROR, (errmsg("group %d already has a primary node",
								   nodeMetadata->groupId)));
		}
	}

	if (nodeMetadata->nodeRole == PrimaryNodeRoleId())
	{
		if (strncmp(nodeMetadata->nodeCluster,
					WORKER_DEFAULT_CLUSTER,
					WORKER_LENGTH) != 0)
		{
			ereport(ERROR, (errmsg("primaries must be added to the default cluster")));
		}
	}

	/* generate the new node id from the sequence */
	int nextNodeIdInt = GetNextNodeId();

	InsertNodeRow(nextNodeIdInt, nodeName, nodePort, nodeMetadata);

	workerNode = FindWorkerNodeAnyCluster(nodeName, nodePort);

	if (EnableMetadataSync && !localOnly)
	{
		/* send the delete command to all primary nodes with metadata */
		char *nodeDeleteCommand = NodeDeleteCommand(workerNode->nodeId);
		SendCommandToWorkersWithMetadata(nodeDeleteCommand);

		/* finally prepare the insert command and send it to all primary nodes */
		uint32 primariesWithMetadata = CountPrimariesWithMetadata();
		if (primariesWithMetadata != 0)
		{
			List *workerNodeList = list_make1(workerNode);
			char *nodeInsertCommand = NodeListInsertCommand(workerNodeList);

			SendCommandToWorkersWithMetadata(nodeInsertCommand);
		}
	}

	return workerNode->nodeId;
}


/*
 * AddNodeMetadataViaMetadataContext does the same thing as AddNodeMetadata but
 * make use of metadata sync context to send commands to workers to support both
 * transactional and nontransactional sync modes.
 */
static int
AddNodeMetadataViaMetadataContext(char *nodeName, int32 nodePort,
								  NodeMetadata *nodeMetadata, bool *nodeAlreadyExists)
{
	bool localOnly = true;
	int nodeId = AddNodeMetadata(nodeName, nodePort, nodeMetadata, nodeAlreadyExists,
								 localOnly);

	/* do nothing as the node already exists */
	if (*nodeAlreadyExists)
	{
		return nodeId;
	}

	/*
	 * Create metadata sync context that is used throughout node addition
	 * and activation if necessary.
	 */
	WorkerNode *node = ModifiableWorkerNode(nodeName, nodePort);

	/* we should always set active flag to true if we call citus_add_node */
	node = SetWorkerColumnLocalOnly(node, Anum_pg_dist_node_isactive, DatumGetBool(true));

	/*
	 * After adding new node, if the node did not already exist, we will activate
	 * the node.
	 * If the worker is not marked as a coordinator, check that
	 * the node is not trying to add itself
	 */
	if (node != NULL &&
		node->groupId != COORDINATOR_GROUP_ID &&
		node->nodeRole != SecondaryNodeRoleId() &&
		IsWorkerTheCurrentNode(node))
	{
		ereport(ERROR, (errmsg("Node cannot add itself as a worker."),
						errhint(
							"Add the node as a coordinator by using: "
							"SELECT citus_set_coordinator_host('%s', %d);",
							node->workerName, node->workerPort)));
	}

	List *nodeList = list_make1(node);
	bool collectCommands = false;
	bool nodesAddedInSameTransaction = true;
	MetadataSyncContext *context = CreateMetadataSyncContext(nodeList, collectCommands,
															 nodesAddedInSameTransaction);

	if (EnableMetadataSync)
	{
		/* send the delete command to all primary nodes with metadata */
		char *nodeDeleteCommand = NodeDeleteCommand(node->nodeId);
		SendOrCollectCommandListToMetadataNodes(context, list_make1(nodeDeleteCommand));

		/* finally prepare the insert command and send it to all primary nodes */
		uint32 primariesWithMetadata = CountPrimariesWithMetadata();
		if (primariesWithMetadata != 0)
		{
			char *nodeInsertCommand = NULL;
			if (context->transactionMode == METADATA_SYNC_TRANSACTIONAL)
			{
				nodeInsertCommand = NodeListInsertCommand(nodeList);
			}
			else if (context->transactionMode == METADATA_SYNC_NON_TRANSACTIONAL)
			{
				/*
				 * We need to ensure node insertion is idempotent in nontransactional
				 * sync mode.
				 */
				nodeInsertCommand = NodeListIdempotentInsertCommand(nodeList);
			}
			Assert(nodeInsertCommand != NULL);
			SendOrCollectCommandListToMetadataNodes(context,
													list_make1(nodeInsertCommand));
		}
	}

	ActivateNodeList(context);

	return nodeId;
}


/*
 * SetWorkerColumn function sets the column with the specified index
 * on the worker in pg_dist_node, by calling SetWorkerColumnLocalOnly.
 * It also sends the same command for node update to other metadata nodes.
 * If anything fails during the transaction, we rollback it.
 * Returns the new worker node after the modification.
 */
WorkerNode *
SetWorkerColumn(WorkerNode *workerNode, int columnIndex, Datum value)
{
	workerNode = SetWorkerColumnLocalOnly(workerNode, columnIndex, value);

	if (EnableMetadataSync)
	{
		char *metadataSyncCommand =
			GetMetadataSyncCommandToSetNodeColumn(workerNode, columnIndex, value);

		SendCommandToWorkersWithMetadata(metadataSyncCommand);
	}

	return workerNode;
}


/*
 * SetNodeStateViaMetadataContext sets or unsets isactive, metadatasynced, and hasmetadata
 * flags via metadataSyncContext.
 */
static void
SetNodeStateViaMetadataContext(MetadataSyncContext *context, WorkerNode *workerNode,
							   Datum value)
{
	char *isActiveCommand =
		GetMetadataSyncCommandToSetNodeColumn(workerNode, Anum_pg_dist_node_isactive,
											  value);
	char *metadatasyncedCommand =
		GetMetadataSyncCommandToSetNodeColumn(workerNode,
											  Anum_pg_dist_node_metadatasynced, value);
	char *hasmetadataCommand =
		GetMetadataSyncCommandToSetNodeColumn(workerNode, Anum_pg_dist_node_hasmetadata,
											  value);
	List *commandList = list_make3(isActiveCommand, metadatasyncedCommand,
								   hasmetadataCommand);

	SendOrCollectCommandListToMetadataNodes(context, commandList);
}


/*
 * SetWorkerColumnOptional function sets the column with the specified index
 * on the worker in pg_dist_node, by calling SetWorkerColumnLocalOnly.
 * It also sends the same command optionally for node update to other metadata nodes,
 * meaning that failures are ignored. Returns the new worker node after the modification.
 */
WorkerNode *
SetWorkerColumnOptional(WorkerNode *workerNode, int columnIndex, Datum value)
{
	char *metadataSyncCommand = GetMetadataSyncCommandToSetNodeColumn(workerNode,
																	  columnIndex,
																	  value);

	List *workerNodeList = TargetWorkerSetNodeList(NON_COORDINATOR_METADATA_NODES,
												   ShareLock);

	/* open connections in parallel */
	WorkerNode *worker = NULL;
	foreach_declared_ptr(worker, workerNodeList)
	{
		bool success = SendOptionalMetadataCommandListToWorkerInCoordinatedTransaction(
			worker->workerName, worker->workerPort,
			CurrentUserName(),
			list_make1(metadataSyncCommand));

		if (!success)
		{
			/* metadata out of sync, mark the worker as not synced */
			ereport(WARNING, (errmsg("Updating the metadata of the node %s:%d "
									 "is failed on node %s:%d. "
									 "Metadata on %s:%d is marked as out of sync.",
									 workerNode->workerName, workerNode->workerPort,
									 worker->workerName, worker->workerPort,
									 worker->workerName, worker->workerPort)));

			SetWorkerColumnLocalOnly(worker, Anum_pg_dist_node_metadatasynced,
									 BoolGetDatum(false));
		}
		else if (workerNode->nodeId == worker->nodeId)
		{
			/*
			 * If this is the node we want to update and it is updated succesfully,
			 * then we can safely update the flag on the coordinator as well.
			 */
			SetWorkerColumnLocalOnly(workerNode, columnIndex, value);
		}
	}

	return FindWorkerNode(workerNode->workerName, workerNode->workerPort);
}


/*
 * SetWorkerColumnLocalOnly function sets the column with the specified index
 * (see pg_dist_node.h) on the worker in pg_dist_node.
 * It returns the new worker node after the modification.
 */
WorkerNode *
SetWorkerColumnLocalOnly(WorkerNode *workerNode, int columnIndex, Datum value)
{
	Relation pgDistNode = table_open(DistNodeRelationId(), RowExclusiveLock);
	TupleDesc tupleDescriptor = RelationGetDescr(pgDistNode);
	HeapTuple heapTuple = GetNodeTuple(workerNode->workerName, workerNode->workerPort);

	Datum values[Natts_pg_dist_node];
	bool isnull[Natts_pg_dist_node];
	bool replace[Natts_pg_dist_node];

	if (heapTuple == NULL)
	{
		ereport(ERROR, (errmsg("could not find valid entry for node \"%s:%d\"",
							   workerNode->workerName, workerNode->workerPort)));
	}

	memset(replace, 0, sizeof(replace));
	values[columnIndex - 1] = value;
	isnull[columnIndex - 1] = false;
	replace[columnIndex - 1] = true;

	heapTuple = heap_modify_tuple(heapTuple, tupleDescriptor, values, isnull, replace);

	CatalogTupleUpdate(pgDistNode, &heapTuple->t_self, heapTuple);

	CitusInvalidateRelcacheByRelid(DistNodeRelationId());
	CommandCounterIncrement();

	WorkerNode *newWorkerNode = TupleToWorkerNode(tupleDescriptor, heapTuple);

	table_close(pgDistNode, NoLock);

	return newWorkerNode;
}


/*
 * GetMetadataSyncCommandToSetNodeColumn checks if the given workerNode and value is
 * valid or not. Then it returns the necessary metadata sync command as a string.
 */
static char *
GetMetadataSyncCommandToSetNodeColumn(WorkerNode *workerNode, int columnIndex, Datum
									  value)
{
	char *metadataSyncCommand = NULL;

	switch (columnIndex)
	{
		case Anum_pg_dist_node_hasmetadata:
		{
			ErrorIfCoordinatorMetadataSetFalse(workerNode, value, "hasmetadata");
			metadataSyncCommand = NodeHasmetadataUpdateCommand(workerNode->nodeId,
															   DatumGetBool(value));
			break;
		}

		case Anum_pg_dist_node_isactive:
		{
			ErrorIfCoordinatorMetadataSetFalse(workerNode, value, "isactive");

			metadataSyncCommand = NodeStateUpdateCommand(workerNode->nodeId,
														 DatumGetBool(value));
			break;
		}

		case Anum_pg_dist_node_shouldhaveshards:
		{
			metadataSyncCommand = ShouldHaveShardsUpdateCommand(workerNode->nodeId,
																DatumGetBool(value));
			break;
		}

		case Anum_pg_dist_node_metadatasynced:
		{
			ErrorIfCoordinatorMetadataSetFalse(workerNode, value, "metadatasynced");
			metadataSyncCommand = NodeMetadataSyncedUpdateCommand(workerNode->nodeId,
																  DatumGetBool(value));
			break;
		}

		default:
		{
			ereport(ERROR, (errmsg("could not find valid entry for node \"%s:%d\"",
								   workerNode->workerName, workerNode->workerPort)));
		}
	}

	return metadataSyncCommand;
}


/*
 * NodeHasmetadataUpdateCommand generates and returns a SQL UPDATE command
 * that updates the hasmetada column of pg_dist_node, for the given nodeid.
 */
static char *
NodeHasmetadataUpdateCommand(uint32 nodeId, bool hasMetadata)
{
	StringInfo updateCommand = makeStringInfo();
	char *hasMetadataString = hasMetadata ? "TRUE" : "FALSE";
	appendStringInfo(updateCommand,
					 "UPDATE pg_dist_node SET hasmetadata = %s "
					 "WHERE nodeid = %u",
					 hasMetadataString, nodeId);
	return updateCommand->data;
}


/*
 * NodeMetadataSyncedUpdateCommand generates and returns a SQL UPDATE command
 * that updates the metadataSynced column of pg_dist_node, for the given nodeid.
 */
static char *
NodeMetadataSyncedUpdateCommand(uint32 nodeId, bool metadataSynced)
{
	StringInfo updateCommand = makeStringInfo();
	char *hasMetadataString = metadataSynced ? "TRUE" : "FALSE";
	appendStringInfo(updateCommand,
					 "UPDATE pg_dist_node SET metadatasynced = %s "
					 "WHERE nodeid = %u",
					 hasMetadataString, nodeId);
	return updateCommand->data;
}


/*
 * ErrorIfCoordinatorMetadataSetFalse throws an error if the input node
 * is the coordinator and the value is false.
 */
static void
ErrorIfCoordinatorMetadataSetFalse(WorkerNode *workerNode, Datum value, char *field)
{
	bool valueBool = DatumGetBool(value);
	if (!valueBool && workerNode->groupId == COORDINATOR_GROUP_ID)
	{
		ereport(ERROR, (errmsg("cannot change \"%s\" field of the "
							   "coordinator node",
							   field)));
	}
}


/*
 * SetShouldHaveShards function sets the shouldhaveshards column of the
 * specified worker in pg_dist_node. also propagates this to other metadata nodes.
 * It returns the new worker node after the modification.
 */
static WorkerNode *
SetShouldHaveShards(WorkerNode *workerNode, bool shouldHaveShards)
{
	return SetWorkerColumn(workerNode, Anum_pg_dist_node_shouldhaveshards, BoolGetDatum(
							   shouldHaveShards));
}


/*
 * GetNodeTuple function returns the heap tuple of given nodeName and nodePort. If the
 * node is not found this function returns NULL.
 *
 * This function may return worker nodes from other clusters.
 */
static HeapTuple
GetNodeTuple(const char *nodeName, int32 nodePort)
{
	Relation pgDistNode = table_open(DistNodeRelationId(), AccessShareLock);
	const int scanKeyCount = 2;
	const bool indexOK = false;

	ScanKeyData scanKey[2];
	HeapTuple nodeTuple = NULL;

	ScanKeyInit(&scanKey[0], Anum_pg_dist_node_nodename,
				BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(nodeName));
	ScanKeyInit(&scanKey[1], Anum_pg_dist_node_nodeport,
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(nodePort));
	SysScanDesc scanDescriptor = systable_beginscan(pgDistNode, InvalidOid, indexOK,
													NULL, scanKeyCount, scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	if (HeapTupleIsValid(heapTuple))
	{
		nodeTuple = heap_copytuple(heapTuple);
	}

	systable_endscan(scanDescriptor);
	table_close(pgDistNode, NoLock);

	return nodeTuple;
}


/*
 * GetNodeByNodeId returns the heap tuple for given node id by looking up catalog.
 */
static HeapTuple
GetNodeByNodeId(int32 nodeId)
{
	Relation pgDistNode = table_open(DistNodeRelationId(), AccessShareLock);
	const int scanKeyCount = 1;
	const bool indexOK = false;

	ScanKeyData scanKey[1];
	HeapTuple nodeTuple = NULL;

	ScanKeyInit(&scanKey[0], Anum_pg_dist_node_nodeid,
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(nodeId));
	SysScanDesc scanDescriptor = systable_beginscan(pgDistNode, InvalidOid, indexOK,
													NULL, scanKeyCount, scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	if (HeapTupleIsValid(heapTuple))
	{
		nodeTuple = heap_copytuple(heapTuple);
	}
	else
	{
		ereport(ERROR, (errmsg("could not find valid entry for node id %d", nodeId)));
	}

	systable_endscan(scanDescriptor);
	table_close(pgDistNode, NoLock);

	return nodeTuple;
}


/*
 * GetNextGroupId allocates and returns a unique groupId for the group
 * to be created. This allocation occurs both in shared memory and in write
 * ahead logs; writing to logs avoids the risk of having groupId collisions.
 *
 * Please note that the caller is still responsible for finalizing node data
 * and the groupId with the master node. Further note that this function relies
 * on an internal sequence created in initdb to generate unique identifiers.
 */
int32
GetNextGroupId()
{
	text *sequenceName = cstring_to_text(GROUPID_SEQUENCE_NAME);
	Oid sequenceId = ResolveRelationId(sequenceName, false);
	Datum sequenceIdDatum = ObjectIdGetDatum(sequenceId);
	Oid savedUserId = InvalidOid;
	int savedSecurityContext = 0;

	GetUserIdAndSecContext(&savedUserId, &savedSecurityContext);
	SetUserIdAndSecContext(CitusExtensionOwner(), SECURITY_LOCAL_USERID_CHANGE);

	/* generate new and unique shardId from sequence */
	Datum groupIdDatum = DirectFunctionCall1(nextval_oid, sequenceIdDatum);

	SetUserIdAndSecContext(savedUserId, savedSecurityContext);

	int32 groupId = DatumGetInt32(groupIdDatum);

	return groupId;
}


/*
 * GetNextNodeId allocates and returns a unique nodeId for the node
 * to be added. This allocation occurs both in shared memory and in write
 * ahead logs; writing to logs avoids the risk of having nodeId collisions.
 *
 * Please note that the caller is still responsible for finalizing node data
 * and the nodeId with the master node. Further note that this function relies
 * on an internal sequence created in initdb to generate unique identifiers.
 */
int
GetNextNodeId()
{
	text *sequenceName = cstring_to_text(NODEID_SEQUENCE_NAME);
	Oid sequenceId = ResolveRelationId(sequenceName, false);
	Datum sequenceIdDatum = ObjectIdGetDatum(sequenceId);
	Oid savedUserId = InvalidOid;
	int savedSecurityContext = 0;

	GetUserIdAndSecContext(&savedUserId, &savedSecurityContext);
	SetUserIdAndSecContext(CitusExtensionOwner(), SECURITY_LOCAL_USERID_CHANGE);

	/* generate new and unique shardId from sequence */
	Datum nextNodeIdDatum = DirectFunctionCall1(nextval_oid, sequenceIdDatum);

	SetUserIdAndSecContext(savedUserId, savedSecurityContext);

	int nextNodeId = DatumGetUInt32(nextNodeIdDatum);

	return nextNodeId;
}


/*
 * EnsureCoordinator checks if the current node is the coordinator. If it does not,
 * the function errors out.
 */
void
EnsureCoordinator(void)
{
	int32 localGroupId = GetLocalGroupId();

	if (localGroupId != 0)
	{
		ereport(ERROR, (errmsg("operation is not allowed on this node"),
						errhint("Connect to the coordinator and run it again.")));
	}
}


/*
 * EnsurePropagationToCoordinator checks whether the coordinator is added to the
 * metadata if we're not on the coordinator.
 *
 * Given that metadata syncing skips syncing metadata to the coordinator, we need
 * too make sure that the coordinator is added to the metadata before propagating
 * a command from a worker. For this reason, today we use this only for the commands
 * that we support propagating from workers.
 */
void
EnsurePropagationToCoordinator(void)
{
	if (!IsCoordinator())
	{
		EnsureCoordinatorIsInMetadata();
	}
}


/*
 * EnsureCoordinatorIsInMetadata checks whether the coordinator is added to the
 * metadata, which is required for many operations.
 */
void
EnsureCoordinatorIsInMetadata(void)
{
	bool isCoordinatorInMetadata = false;
	PrimaryNodeForGroup(COORDINATOR_GROUP_ID, &isCoordinatorInMetadata);
	if (isCoordinatorInMetadata)
	{
		return;
	}

	/* be more descriptive when we're not on coordinator */
	if (IsCoordinator())
	{
		ereport(ERROR, (errmsg("coordinator is not added to the metadata"),
						errhint("Use SELECT citus_set_coordinator_host('<hostname>') "
								"to configure the coordinator hostname")));
	}
	else
	{
		ereport(ERROR, (errmsg("coordinator is not added to the metadata"),
						errhint("Use SELECT citus_set_coordinator_host('<hostname>') "
								"on coordinator to configure the coordinator hostname")));
	}
}


/*
 * InsertCoordinatorIfClusterEmpty can be used to ensure Citus tables can be
 * created even on a node that has just performed CREATE EXTENSION citus;
 */
void
InsertCoordinatorIfClusterEmpty(void)
{
	/* prevent concurrent node additions */
	Relation pgDistNode = table_open(DistNodeRelationId(), RowShareLock);

	if (!HasAnyNodes())
	{
		/*
		 * create_distributed_table being called for the first time and there are
		 * no pg_dist_node records. Add a record for the coordinator.
		 */
		InsertPlaceholderCoordinatorRecord();
	}

	/*
	 * We release the lock, if InsertPlaceholderCoordinatorRecord was called
	 * we already have a strong (RowExclusive) lock.
	 */
	table_close(pgDistNode, RowShareLock);
}


/*
 * InsertPlaceholderCoordinatorRecord inserts a placeholder record for the coordinator
 * to be able to create distributed tables on a single node.
 */
static void
InsertPlaceholderCoordinatorRecord(void)
{
	NodeMetadata nodeMetadata = DefaultNodeMetadata();
	nodeMetadata.groupId = 0;
	nodeMetadata.shouldHaveShards = true;
	nodeMetadata.nodeRole = PrimaryNodeRoleId();
	nodeMetadata.nodeCluster = "default";

	bool nodeAlreadyExists = false;
	bool localOnly = false;

	/* as long as there is a single node, localhost should be ok */
	AddNodeMetadata(LocalHostName, PostPortNumber, &nodeMetadata, &nodeAlreadyExists,
					localOnly);
}


/*
 * InsertNodeRow opens the node system catalog, and inserts a new row with the
 * given values into that system catalog.
 *
 * NOTE: If you call this function you probably need to have taken a
 * ShareRowExclusiveLock then checked that you're not adding a second primary to
 * an existing group. If you don't it's possible for the metadata to become inconsistent.
 */
static void
InsertNodeRow(int nodeid, char *nodeName, int32 nodePort, NodeMetadata *nodeMetadata)
{
	Datum values[Natts_pg_dist_node];
	bool isNulls[Natts_pg_dist_node];

	Datum nodeClusterStringDatum = CStringGetDatum(nodeMetadata->nodeCluster);
	Datum nodeClusterNameDatum = DirectFunctionCall1(namein, nodeClusterStringDatum);

	/* form new shard tuple */
	memset(values, 0, sizeof(values));
	memset(isNulls, false, sizeof(isNulls));

	values[Anum_pg_dist_node_nodeid - 1] = UInt32GetDatum(nodeid);
	values[Anum_pg_dist_node_groupid - 1] = Int32GetDatum(nodeMetadata->groupId);
	values[Anum_pg_dist_node_nodename - 1] = CStringGetTextDatum(nodeName);
	values[Anum_pg_dist_node_nodeport - 1] = UInt32GetDatum(nodePort);
	values[Anum_pg_dist_node_noderack - 1] = CStringGetTextDatum(nodeMetadata->nodeRack);
	values[Anum_pg_dist_node_hasmetadata - 1] = BoolGetDatum(nodeMetadata->hasMetadata);
	values[Anum_pg_dist_node_metadatasynced - 1] = BoolGetDatum(
		nodeMetadata->metadataSynced);
	values[Anum_pg_dist_node_isactive - 1] = BoolGetDatum(nodeMetadata->isActive);
	values[Anum_pg_dist_node_noderole - 1] = ObjectIdGetDatum(nodeMetadata->nodeRole);
	values[Anum_pg_dist_node_nodecluster - 1] = nodeClusterNameDatum;
	values[Anum_pg_dist_node_shouldhaveshards - 1] = BoolGetDatum(
		nodeMetadata->shouldHaveShards);

	Relation pgDistNode = table_open(DistNodeRelationId(), RowExclusiveLock);

	TupleDesc tupleDescriptor = RelationGetDescr(pgDistNode);
	HeapTuple heapTuple = heap_form_tuple(tupleDescriptor, values, isNulls);

	CATALOG_INSERT_WITH_SNAPSHOT(pgDistNode, heapTuple);

	CitusInvalidateRelcacheByRelid(DistNodeRelationId());

	/* increment the counter so that next command can see the row */
	CommandCounterIncrement();

	/* close relation */
	table_close(pgDistNode, NoLock);
}


/*
 * DeleteNodeRow removes the requested row from pg_dist_node table if it exists.
 */
static void
DeleteNodeRow(char *nodeName, int32 nodePort)
{
	const int scanKeyCount = 2;
	bool indexOK = false;

	ScanKeyData scanKey[2];
	Relation pgDistNode = table_open(DistNodeRelationId(), RowExclusiveLock);

	/*
	 * simple_heap_delete() expects that the caller has at least an
	 * AccessShareLock on primary key index.
	 *
	 * XXX: This does not seem required, do we really need to acquire this lock?
	 * Postgres doesn't acquire such locks on indexes before deleting catalog tuples.
	 * Linking here the reasons we added this lock acquirement:
	 * https://github.com/citusdata/citus/pull/2851#discussion_r306569462
	 * https://github.com/citusdata/citus/pull/2855#discussion_r313628554
	 * https://github.com/citusdata/citus/issues/1890
	 */
#if PG_VERSION_NUM >= PG_VERSION_18

	/* PG 18+ adds a bool “deferrable_ok” parameter */
	Relation replicaIndex =
		index_open(RelationGetPrimaryKeyIndex(pgDistNode, false),
				   AccessShareLock);
#else
	Relation replicaIndex =
		index_open(RelationGetPrimaryKeyIndex(pgDistNode),
				   AccessShareLock);
#endif


	ScanKeyInit(&scanKey[0], Anum_pg_dist_node_nodename,
				BTEqualStrategyNumber, F_TEXTEQ, CStringGetTextDatum(nodeName));
	ScanKeyInit(&scanKey[1], Anum_pg_dist_node_nodeport,
				BTEqualStrategyNumber, F_INT4EQ, Int32GetDatum(nodePort));

	SysScanDesc heapScan = systable_beginscan(pgDistNode, InvalidOid, indexOK,
											  NULL, scanKeyCount, scanKey);

	HeapTuple heapTuple = systable_getnext(heapScan);

	if (!HeapTupleIsValid(heapTuple))
	{
		ereport(ERROR, (errmsg("could not find valid entry for node \"%s:%d\"",
							   nodeName, nodePort)));
	}

	simple_heap_delete(pgDistNode, &(heapTuple->t_self));

	systable_endscan(heapScan);

	/* ensure future commands don't use the node we just removed */
	CitusInvalidateRelcacheByRelid(DistNodeRelationId());

	/* increment the counter so that next command won't see the row */
	CommandCounterIncrement();

	table_close(replicaIndex, AccessShareLock);
	table_close(pgDistNode, NoLock);
}


/*
 * TupleToWorkerNode takes in a heap tuple from pg_dist_node, and
 * converts this tuple to an equivalent struct in memory. The function assumes
 * the caller already has locks on the tuple, and doesn't perform any locking.
 */
static WorkerNode *
TupleToWorkerNode(TupleDesc tupleDescriptor, HeapTuple heapTuple)
{
	Datum datumArray[Natts_pg_dist_node];
	bool isNullArray[Natts_pg_dist_node];

	Assert(!HeapTupleHasNulls(heapTuple));

	/*
	 * This function can be called before "ALTER TABLE ... ADD COLUMN nodecluster ...",
	 * therefore heap_deform_tuple() won't set the isNullArray for this column. We
	 * initialize it true to be safe in that case.
	 */
	memset(isNullArray, true, sizeof(isNullArray));

	/*
	 * We use heap_deform_tuple() instead of heap_getattr() to expand tuple
	 * to contain missing values when ALTER TABLE ADD COLUMN happens.
	 */
	heap_deform_tuple(heapTuple, tupleDescriptor, datumArray, isNullArray);

	char *nodeName = TextDatumGetCString(datumArray[Anum_pg_dist_node_nodename - 1]);
	char *nodeRack = TextDatumGetCString(datumArray[Anum_pg_dist_node_noderack - 1]);

	WorkerNode *workerNode = (WorkerNode *) palloc0(sizeof(WorkerNode));
	workerNode->nodeId = DatumGetUInt32(datumArray[Anum_pg_dist_node_nodeid - 1]);
	workerNode->workerPort = DatumGetUInt32(datumArray[Anum_pg_dist_node_nodeport - 1]);
	workerNode->groupId = DatumGetInt32(datumArray[Anum_pg_dist_node_groupid - 1]);
	strlcpy(workerNode->workerName, nodeName, WORKER_LENGTH);
	strlcpy(workerNode->workerRack, nodeRack, WORKER_LENGTH);
	workerNode->hasMetadata = DatumGetBool(datumArray[Anum_pg_dist_node_hasmetadata - 1]);
	workerNode->metadataSynced =
		DatumGetBool(datumArray[Anum_pg_dist_node_metadatasynced - 1]);
	workerNode->isActive = DatumGetBool(datumArray[Anum_pg_dist_node_isactive - 1]);
	workerNode->nodeRole = DatumGetObjectId(datumArray[Anum_pg_dist_node_noderole - 1]);
	workerNode->shouldHaveShards = DatumGetBool(
		datumArray[Anum_pg_dist_node_shouldhaveshards -
				   1]);

	/*
	 * nodecluster column can be missing. In the case of extension creation/upgrade,
	 * master_initialize_node_metadata function is called before the nodecluster
	 * column is added to pg_dist_node table.
	 */
	if (!isNullArray[Anum_pg_dist_node_nodecluster - 1])
	{
		Name nodeClusterName =
			DatumGetName(datumArray[Anum_pg_dist_node_nodecluster - 1]);
		char *nodeClusterString = NameStr(*nodeClusterName);
		strlcpy(workerNode->nodeCluster, nodeClusterString, NAMEDATALEN);
	}

	return workerNode;
}


/*
 * StringToDatum transforms a string representation into a Datum.
 */
Datum
StringToDatum(char *inputString, Oid dataType)
{
	Oid typIoFunc = InvalidOid;
	Oid typIoParam = InvalidOid;
	int32 typeModifier = -1;

	getTypeInputInfo(dataType, &typIoFunc, &typIoParam);
	getBaseTypeAndTypmod(dataType, &typeModifier);

	Datum datum = OidInputFunctionCall(typIoFunc, inputString, typIoParam, typeModifier);

	return datum;
}


/*
 * DatumToString returns the string representation of the given datum.
 */
char *
DatumToString(Datum datum, Oid dataType)
{
	Oid typIoFunc = InvalidOid;
	bool typIsVarlena = false;

	getTypeOutputInfo(dataType, &typIoFunc, &typIsVarlena);
	char *outputString = OidOutputFunctionCall(typIoFunc, datum);

	return outputString;
}


/*
 * UnsetMetadataSyncedForAllWorkers sets the metadatasynced column of all metadata
 * worker nodes to false. It returns true if it updated at least a node.
 */
static bool
UnsetMetadataSyncedForAllWorkers(void)
{
	bool updatedAtLeastOne = false;
	ScanKeyData scanKey[3];
	int scanKeyCount = 3;
	bool indexOK = false;

	/*
	 * Concurrent citus_update_node() calls might iterate and try to update
	 * pg_dist_node in different orders. To protect against deadlock, we
	 * get an exclusive lock here.
	 */
	Relation relation = table_open(DistNodeRelationId(), ExclusiveLock);
	TupleDesc tupleDescriptor = RelationGetDescr(relation);
	ScanKeyInit(&scanKey[0], Anum_pg_dist_node_hasmetadata,
				BTEqualStrategyNumber, F_BOOLEQ, BoolGetDatum(true));
	ScanKeyInit(&scanKey[1], Anum_pg_dist_node_metadatasynced,
				BTEqualStrategyNumber, F_BOOLEQ, BoolGetDatum(true));

	/* coordinator always has the up to date metadata */
	ScanKeyInit(&scanKey[2], Anum_pg_dist_node_groupid,
				BTGreaterStrategyNumber, F_INT4GT,
				Int32GetDatum(COORDINATOR_GROUP_ID));

	CatalogIndexState indstate = CatalogOpenIndexes(relation);

	SysScanDesc scanDescriptor = systable_beginscan(relation,
													InvalidOid, indexOK,
													NULL, scanKeyCount, scanKey);

	HeapTuple heapTuple = systable_getnext(scanDescriptor);
	if (HeapTupleIsValid(heapTuple))
	{
		updatedAtLeastOne = true;
	}

	while (HeapTupleIsValid(heapTuple))
	{
		Datum values[Natts_pg_dist_node];
		bool isnull[Natts_pg_dist_node];
		bool replace[Natts_pg_dist_node];

		memset(replace, false, sizeof(replace));
		memset(isnull, false, sizeof(isnull));
		memset(values, 0, sizeof(values));

		values[Anum_pg_dist_node_metadatasynced - 1] = BoolGetDatum(false);
		replace[Anum_pg_dist_node_metadatasynced - 1] = true;

		HeapTuple newHeapTuple = heap_modify_tuple(heapTuple, tupleDescriptor, values,
												   isnull,
												   replace);

		CatalogTupleUpdateWithInfo(relation, &newHeapTuple->t_self, newHeapTuple,
								   indstate);

		CommandCounterIncrement();

		heap_freetuple(newHeapTuple);

		heapTuple = systable_getnext(scanDescriptor);
	}

	systable_endscan(scanDescriptor);
	CatalogCloseIndexes(indstate);
	table_close(relation, NoLock);

	return updatedAtLeastOne;
}


/*
 * ErrorIfAnyNodeNotExist errors if any node in given list not found.
 */
static void
ErrorIfAnyNodeNotExist(List *nodeList)
{
	WorkerNode *node = NULL;
	foreach_declared_ptr(node, nodeList)
	{
		/*
		 * First, locally mark the node is active, if everything goes well,
		 * we are going to sync this information to all the metadata nodes.
		 */
		WorkerNode *workerNode =
			FindWorkerNodeAnyCluster(node->workerName, node->workerPort);
		if (workerNode == NULL)
		{
			ereport(ERROR, (errmsg("node at \"%s:%u\" does not exist", node->workerName,
								   node->workerPort)));
		}
	}
}


/*
 * UpdateLocalGroupIdsViaMetadataContext updates local group ids for given list
 * of nodes with transactional or nontransactional mode according to transactionMode
 * inside metadataSyncContext.
 */
static void
UpdateLocalGroupIdsViaMetadataContext(MetadataSyncContext *context)
{
	int activatedPrimaryCount = list_length(context->activatedWorkerNodeList);
	int nodeIdx = 0;
	for (nodeIdx = 0; nodeIdx < activatedPrimaryCount; nodeIdx++)
	{
		WorkerNode *node = list_nth(context->activatedWorkerNodeList, nodeIdx);
		List *commandList = list_make1(LocalGroupIdUpdateCommand(node->groupId));

		/* send commands to new workers, the current user should be a superuser */
		Assert(superuser());

		SendOrCollectCommandListToSingleNode(context, commandList, nodeIdx);
	}
}


/*
 * SendDeletionCommandsForReplicatedTablePlacements sends commands to delete replicated
 * placement for the metadata nodes with transactional or nontransactional mode according
 * to transactionMode inside metadataSyncContext.
 */
static void
SendDeletionCommandsForReplicatedTablePlacements(MetadataSyncContext *context)
{
	WorkerNode *node = NULL;
	foreach_declared_ptr(node, context->activatedWorkerNodeList)
	{
		if (!node->isActive)
		{
			bool localOnly = false;
			int32 groupId = node->groupId;
			DeleteAllReplicatedTablePlacementsFromNodeGroupViaMetadataContext(context,
																			  groupId,
																			  localOnly);
		}
	}
}


/*
 * SyncNodeMetadata syncs node metadata with transactional or nontransactional
 * mode according to transactionMode inside metadataSyncContext.
 */
static void
SyncNodeMetadata(MetadataSyncContext *context)
{
	CheckCitusVersion(ERROR);

	if (!EnableMetadataSync)
	{
		return;
	}

	/*
	 * Do not fail when we call this method from activate_node_snapshot
	 * from workers.
	 */
	if (!MetadataSyncCollectsCommands(context))
	{
		EnsureCoordinator();
	}

	EnsureModificationsCanRun();
	EnsureSequentialModeMetadataOperations();

	LockRelationOid(DistNodeRelationId(), ExclusiveLock);

	/* generate the queries which drop the node metadata */
	List *dropMetadataCommandList = NodeMetadataDropCommands();

	/* generate the queries which create the node metadata from scratch */
	List *createMetadataCommandList = NodeMetadataCreateCommands();

	List *recreateNodeSnapshotCommandList = dropMetadataCommandList;
	recreateNodeSnapshotCommandList = list_concat(recreateNodeSnapshotCommandList,
												  createMetadataCommandList);

	/*
	 * We should have already added node metadata to metadata workers. Sync node
	 * metadata just for activated workers.
	 */
	SendOrCollectCommandListToActivatedNodes(context, recreateNodeSnapshotCommandList);
}
