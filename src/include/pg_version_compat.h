/*-------------------------------------------------------------------------
 *
 * pg_version_compat.h
 *	  Compatibility macros for writing code agnostic to PostgreSQL versions
 *
 * Copyright (c) Citus Data, Inc.
 *
 *-------------------------------------------------------------------------
 */

#ifndef PG_VERSION_COMPAT_H
#define PG_VERSION_COMPAT_H

#include "pg_version_constants.h"

/* we need these for PG-18’s PushActiveSnapshot/PopActiveSnapshot APIs */
#include "access/xact.h"
#include "utils/snapmgr.h"

#if PG_VERSION_NUM >= PG_VERSION_18
#define create_foreignscan_path_compat(a, b, c, d, e, f, g, h, i, j, k) \
	create_foreignscan_path( \
		(a),            /* root            */ \
		(b),            /* rel             */ \
		(c),            /* target          */ \
		(d),            /* rows            */ \
		0,              /* disabled_nodes  */ \
		(e),            /* startup_cost    */ \
		(f),            /* total_cost      */ \
		(g),            /* pathkeys        */ \
		(h),            /* required_outer  */ \
		(i),            /* fdw_outerpath   */ \
		(j),            /* fdw_restrictinfo*/ \
		(k)             /* fdw_private     */ \
		)

/* PG-18 introduced get_op_index_interpretation, old name was get_op_btree_interpretation */
#define get_op_btree_interpretation(opno) get_op_index_interpretation(opno)

/* PG-18 unified row-compare operator codes under COMPARE_* */
#define ROWCOMPARE_NE COMPARE_NE

#define CATALOG_INSERT_WITH_SNAPSHOT(rel, tup) \
	do { \
		Snapshot __snap = GetTransactionSnapshot(); \
		PushActiveSnapshot(__snap); \
		CatalogTupleInsert((rel), (tup)); \
		PopActiveSnapshot(); \
	} while (0)

#elif PG_VERSION_NUM >= PG_VERSION_17
#define create_foreignscan_path_compat(a, b, c, d, e, f, g, h, i, j, k) \
	create_foreignscan_path( \
		(a), (b), (c), (d), \
		(e), (f), \
		(g), (h), (i), (j), (k) \
		)

/* no-op wrapper on older PGs */
#define CATALOG_INSERT_WITH_SNAPSHOT(rel, tup) \
	CatalogTupleInsert((rel), (tup))
#endif

#if PG_VERSION_NUM >= PG_VERSION_17

#include "catalog/pg_am.h"
#include "catalog/pg_amop.h"
#include "catalog/pg_amproc.h"
#include "catalog/pg_attrdef.h"
#include "catalog/pg_auth_members.h"
#include "catalog/pg_authid.h"
#include "catalog/pg_cast.h"
#include "catalog/pg_class.h"
#include "catalog/pg_collation.h"
#include "catalog/pg_constraint.h"
#include "catalog/pg_conversion.h"
#include "catalog/pg_database.h"
#include "catalog/pg_default_acl.h"
#include "catalog/pg_depend.h"
#include "catalog/pg_event_trigger.h"
#include "catalog/pg_extension.h"
#include "catalog/pg_foreign_data_wrapper.h"
#include "catalog/pg_foreign_server.h"
#include "catalog/pg_init_privs.h"
#include "catalog/pg_language.h"
#include "catalog/pg_largeobject.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_opclass.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_opfamily.h"
#include "catalog/pg_parameter_acl.h"
#include "catalog/pg_policy.h"
#include "catalog/pg_proc.h"
#include "catalog/pg_publication.h"
#include "catalog/pg_publication_namespace.h"
#include "catalog/pg_publication_rel.h"
#include "catalog/pg_rewrite.h"
#include "catalog/pg_statistic_ext.h"
#include "catalog/pg_subscription.h"
#include "catalog/pg_tablespace.h"
#include "catalog/pg_transform.h"
#include "catalog/pg_trigger.h"
#include "catalog/pg_ts_config.h"
#include "catalog/pg_ts_dict.h"
#include "catalog/pg_ts_parser.h"
#include "catalog/pg_ts_template.h"
#include "catalog/pg_type.h"
#include "catalog/pg_user_mapping.h"

/*
 * This enum covers all system catalogs whose OIDs can appear in
 * pg_depend.classId or pg_shdepend.classId.
 */
typedef enum ObjectClass
{
	OCLASS_CLASS,               /* pg_class */
	OCLASS_PROC,                /* pg_proc */
	OCLASS_TYPE,                /* pg_type */
	OCLASS_CAST,                /* pg_cast */
	OCLASS_COLLATION,           /* pg_collation */
	OCLASS_CONSTRAINT,          /* pg_constraint */
	OCLASS_CONVERSION,          /* pg_conversion */
	OCLASS_DEFAULT,             /* pg_attrdef */
	OCLASS_LANGUAGE,            /* pg_language */
	OCLASS_LARGEOBJECT,         /* pg_largeobject */
	OCLASS_OPERATOR,            /* pg_operator */
	OCLASS_OPCLASS,             /* pg_opclass */
	OCLASS_OPFAMILY,            /* pg_opfamily */
	OCLASS_AM,                  /* pg_am */
	OCLASS_AMOP,                /* pg_amop */
	OCLASS_AMPROC,              /* pg_amproc */
	OCLASS_REWRITE,             /* pg_rewrite */
	OCLASS_TRIGGER,             /* pg_trigger */
	OCLASS_SCHEMA,              /* pg_namespace */
	OCLASS_STATISTIC_EXT,       /* pg_statistic_ext */
	OCLASS_TSPARSER,            /* pg_ts_parser */
	OCLASS_TSDICT,              /* pg_ts_dict */
	OCLASS_TSTEMPLATE,          /* pg_ts_template */
	OCLASS_TSCONFIG,            /* pg_ts_config */
	OCLASS_ROLE,                /* pg_authid */
	OCLASS_ROLE_MEMBERSHIP,     /* pg_auth_members */
	OCLASS_DATABASE,            /* pg_database */
	OCLASS_TBLSPACE,            /* pg_tablespace */
	OCLASS_FDW,                 /* pg_foreign_data_wrapper */
	OCLASS_FOREIGN_SERVER,      /* pg_foreign_server */
	OCLASS_USER_MAPPING,        /* pg_user_mapping */
	OCLASS_DEFACL,              /* pg_default_acl */
	OCLASS_EXTENSION,           /* pg_extension */
	OCLASS_EVENT_TRIGGER,       /* pg_event_trigger */
	OCLASS_PARAMETER_ACL,       /* pg_parameter_acl */
	OCLASS_POLICY,              /* pg_policy */
	OCLASS_PUBLICATION,         /* pg_publication */
	OCLASS_PUBLICATION_NAMESPACE,   /* pg_publication_namespace */
	OCLASS_PUBLICATION_REL,     /* pg_publication_rel */
	OCLASS_SUBSCRIPTION,        /* pg_subscription */
	OCLASS_TRANSFORM,           /* pg_transform */
} ObjectClass;

#define LAST_OCLASS OCLASS_TRANSFORM

/*
 * Determine the class of a given object identified by objectAddress.
 *
 * We implement it as a function instead of an array because the OIDs aren't
 * consecutive.
 */
static inline ObjectClass
getObjectClass(const ObjectAddress *object)
{
	/* only pg_class entries can have nonzero objectSubId */
	if (object->classId != RelationRelationId &&
		object->objectSubId != 0)
	{
		elog(ERROR, "invalid non-zero objectSubId for object class %u",
			 object->classId);
	}

	switch (object->classId)
	{
		case RelationRelationId:
		{
			/* caller must check objectSubId */
			return OCLASS_CLASS;
		}

		case ProcedureRelationId:
		{
			return OCLASS_PROC;
		}

		case TypeRelationId:
		{
			return OCLASS_TYPE;
		}

		case CastRelationId:
		{
			return OCLASS_CAST;
		}

		case CollationRelationId:
		{
			return OCLASS_COLLATION;
		}

		case ConstraintRelationId:
		{
			return OCLASS_CONSTRAINT;
		}

		case ConversionRelationId:
		{
			return OCLASS_CONVERSION;
		}

		case AttrDefaultRelationId:
		{
			return OCLASS_DEFAULT;
		}

		case LanguageRelationId:
		{
			return OCLASS_LANGUAGE;
		}

		case LargeObjectRelationId:
		{
			return OCLASS_LARGEOBJECT;
		}

		case OperatorRelationId:
		{
			return OCLASS_OPERATOR;
		}

		case OperatorClassRelationId:
		{
			return OCLASS_OPCLASS;
		}

		case OperatorFamilyRelationId:
		{
			return OCLASS_OPFAMILY;
		}

		case AccessMethodRelationId:
		{
			return OCLASS_AM;
		}

		case AccessMethodOperatorRelationId:
		{
			return OCLASS_AMOP;
		}

		case AccessMethodProcedureRelationId:
		{
			return OCLASS_AMPROC;
		}

		case RewriteRelationId:
		{
			return OCLASS_REWRITE;
		}

		case TriggerRelationId:
		{
			return OCLASS_TRIGGER;
		}

		case NamespaceRelationId:
		{
			return OCLASS_SCHEMA;
		}

		case StatisticExtRelationId:
		{
			return OCLASS_STATISTIC_EXT;
		}

		case TSParserRelationId:
		{
			return OCLASS_TSPARSER;
		}

		case TSDictionaryRelationId:
		{
			return OCLASS_TSDICT;
		}

		case TSTemplateRelationId:
		{
			return OCLASS_TSTEMPLATE;
		}

		case TSConfigRelationId:
		{
			return OCLASS_TSCONFIG;
		}

		case AuthIdRelationId:
		{
			return OCLASS_ROLE;
		}

		case AuthMemRelationId:
		{
			return OCLASS_ROLE_MEMBERSHIP;
		}

		case DatabaseRelationId:
		{
			return OCLASS_DATABASE;
		}

		case TableSpaceRelationId:
		{
			return OCLASS_TBLSPACE;
		}

		case ForeignDataWrapperRelationId:
		{
			return OCLASS_FDW;
		}

		case ForeignServerRelationId:
		{
			return OCLASS_FOREIGN_SERVER;
		}

		case UserMappingRelationId:
		{
			return OCLASS_USER_MAPPING;
		}

		case DefaultAclRelationId:
		{
			return OCLASS_DEFACL;
		}

		case ExtensionRelationId:
		{
			return OCLASS_EXTENSION;
		}

		case EventTriggerRelationId:
		{
			return OCLASS_EVENT_TRIGGER;
		}

		case ParameterAclRelationId:
		{
			return OCLASS_PARAMETER_ACL;
		}

		case PolicyRelationId:
		{
			return OCLASS_POLICY;
		}

		case PublicationNamespaceRelationId:
		{
			return OCLASS_PUBLICATION_NAMESPACE;
		}

		case PublicationRelationId:
		{
			return OCLASS_PUBLICATION;
		}

		case PublicationRelRelationId:
		{
			return OCLASS_PUBLICATION_REL;
		}

		case SubscriptionRelationId:
		{
			return OCLASS_SUBSCRIPTION;
		}

		case TransformRelationId:
			return OCLASS_TRANSFORM;
	}

	/* shouldn't get here */
	elog(ERROR, "unrecognized object class: %u", object->classId);
	return OCLASS_CLASS;        /* keep compiler quiet */
}


#include "commands/tablecmds.h"

static inline void
RangeVarCallbackOwnsTable(const RangeVar *relation,
						  Oid relId, Oid oldRelId, void *arg)
{
	return RangeVarCallbackMaintainsTable(relation, relId, oldRelId, arg);
}


#include "catalog/pg_attribute.h"
#include "utils/syscache.h"

static inline int
getAttstattarget_compat(HeapTuple attTuple)
{
	bool isnull;
	Datum dat = SysCacheGetAttr(ATTNUM, attTuple,
								Anum_pg_attribute_attstattarget, &isnull);
	return (isnull ? -1 : DatumGetInt16(dat));
}


#include "catalog/pg_statistic_ext.h"

static inline int
getStxstattarget_compat(HeapTuple tup)
{
	bool isnull;
	Datum dat = SysCacheGetAttr(STATEXTOID, tup,
								Anum_pg_statistic_ext_stxstattarget, &isnull);
	return (isnull ? -1 : DatumGetInt16(dat));
}


#define getAlterStatsStxstattarget_compat(a) ((Node *) makeInteger(a))
#define getIntStxstattarget_compat(a) (intVal(a))

#define WaitEventSetTracker_compat CurrentResourceOwner

#define identitySequenceRelation_compat(a) (a)

#define matched_compat(a) (a->matchKind == MERGE_WHEN_MATCHED)

#define getProcNo_compat(a) (a->vxid.procNumber)
#define getLxid_compat(a) (a->vxid.lxid)

#else

#define Anum_pg_collation_colllocale Anum_pg_collation_colliculocale
#define Anum_pg_database_datlocale Anum_pg_database_daticulocale

#include "access/htup_details.h"
static inline int
getAttstattarget_compat(HeapTuple attTuple)
{
	return ((Form_pg_attribute) GETSTRUCT(attTuple))->attstattarget;
}


#include "catalog/pg_statistic_ext.h"
static inline int
getStxstattarget_compat(HeapTuple tup)
{
	return ((Form_pg_statistic_ext) GETSTRUCT(tup))->stxstattarget;
}


#define getAlterStatsStxstattarget_compat(a) (a)
#define getIntStxstattarget_compat(a) (a)

#define WaitEventSetTracker_compat CurrentMemoryContext

#define identitySequenceRelation_compat(a) (RelationGetRelid(a))

#define matched_compat(a) (a->matched)

#define create_foreignscan_path_compat(a, b, c, d, e, f, g, h, i, j, \
									   k) create_foreignscan_path(a, b, c, d, e, f, g, h, \
																  i, k)

/* no-op wrapper on older PGs */
#define CATALOG_INSERT_WITH_SNAPSHOT(rel, tup) \
	CatalogTupleInsert((rel), (tup))

#define getProcNo_compat(a) (a->pgprocno)
#define getLxid_compat(a) (a->lxid)

#define COLLPROVIDER_BUILTIN 'b'

#endif

#if PG_VERSION_NUM >= PG_VERSION_16

#include "utils/guc_tables.h"

#define pg_clean_ascii_compat(a, b) pg_clean_ascii(a, b)

#define RelationPhysicalIdentifier_compat(a) ((a)->rd_locator)
#define RelationTablespace_compat(a) (a.spcOid)
#define RelationPhysicalIdentifierNumber_compat(a) (a.relNumber)
#define RelationPhysicalIdentifierNumberPtr_compat(a) (a->relNumber)
#define RelationPhysicalIdentifierBackend_compat(a) (a->smgr_rlocator.locator)

#define float_abs(a) fabs(a)

#define tuplesort_getdatum_compat(a, b, c, d, e, f) tuplesort_getdatum(a, b, c, d, e, f)

static inline struct config_generic **
get_guc_variables_compat(int *gucCount)
{
	return get_guc_variables(gucCount);
}


#define PG_FUNCNAME_MACRO __func__

#define stringToQualifiedNameList_compat(a) stringToQualifiedNameList(a, NULL)
#define typeStringToTypeName_compat(a, b) typeStringToTypeName(a, b)

#define get_relids_in_jointree_compat(a, b, c) get_relids_in_jointree(a, b, c)

#define object_ownercheck(a, b, c) object_ownercheck(a, b, c)
#define object_aclcheck(a, b, c, d) object_aclcheck(a, b, c, d)

#define pgstat_fetch_stat_local_beentry(a) pgstat_get_local_beentry_by_index(a)

#define have_createdb_privilege() have_createdb_privilege()

#else

#include "miscadmin.h"

#include "catalog/pg_authid.h"
#include "catalog/pg_class_d.h"
#include "catalog/pg_database_d.h"
#include "catalog/pg_namespace.h"
#include "catalog/pg_proc_d.h"
#include "storage/relfilenode.h"
#include "utils/guc.h"
#include "utils/guc_tables.h"
#include "utils/syscache.h"

#define pg_clean_ascii_compat(a, b) pg_clean_ascii(a)

#define RelationPhysicalIdentifier_compat(a) ((a)->rd_node)
#define RelationTablespace_compat(a) (a.spcNode)
#define RelationPhysicalIdentifierNumber_compat(a) (a.relNode)
#define RelationPhysicalIdentifierNumberPtr_compat(a) (a->relNode)
#define RelationPhysicalIdentifierBackend_compat(a) (a->smgr_rnode.node)
typedef RelFileNode RelFileLocator;
typedef Oid RelFileNumber;
#define RelidByRelfilenumber(a, b) RelidByRelfilenode(a, b)

#define float_abs(a) Abs(a)

#define tuplesort_getdatum_compat(a, b, c, d, e, f) tuplesort_getdatum(a, b, d, e, f)

static inline struct config_generic **
get_guc_variables_compat(int *gucCount)
{
	*gucCount = GetNumConfigOptions();
	return get_guc_variables();
}


#define stringToQualifiedNameList_compat(a) stringToQualifiedNameList(a)
#define typeStringToTypeName_compat(a, b) typeStringToTypeName(a)

#define get_relids_in_jointree_compat(a, b, c) get_relids_in_jointree(a, b)

static inline bool
object_ownercheck(Oid classid, Oid objectid, Oid roleid)
{
	switch (classid)
	{
		case RelationRelationId:
		{
			return pg_class_ownercheck(objectid, roleid);
		}

		case NamespaceRelationId:
		{
			return pg_namespace_ownercheck(objectid, roleid);
		}

		case ProcedureRelationId:
		{
			return pg_proc_ownercheck(objectid, roleid);
		}

		case DatabaseRelationId:
		{
			return pg_database_ownercheck(objectid, roleid);
		}

		default:
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("Missing classid:%d",
																	classid)));
		}
	}
}


static inline AclResult
object_aclcheck(Oid classid, Oid objectid, Oid roleid, AclMode mode)
{
	switch (classid)
	{
		case NamespaceRelationId:
		{
			return pg_namespace_aclcheck(objectid, roleid, mode);
		}

		case ProcedureRelationId:
		{
			return pg_proc_aclcheck(objectid, roleid, mode);
		}

		default:
		{
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("Missing classid:%d",
																	classid)));
		}
	}
}


static inline bool
have_createdb_privilege(void)
{
	bool result = false;
	HeapTuple utup;

	/* Superusers can always do everything */
	if (superuser())
	{
		return true;
	}

	utup = SearchSysCache1(AUTHOID, ObjectIdGetDatum(GetUserId()));
	if (HeapTupleIsValid(utup))
	{
		result = ((Form_pg_authid) GETSTRUCT(utup))->rolcreatedb;
		ReleaseSysCache(utup);
	}
	return result;
}


typedef bool TU_UpdateIndexes;

/*
 * we define RTEPermissionInfo for PG16 compatibility
 * There are some functions that need to include RTEPermissionInfo in their signature
 * for PG14/PG15 we pass a NULL argument in these functions
 */
typedef RangeTblEntry RTEPermissionInfo;

#endif

#define SetListCellPtr(a, b) ((a)->ptr_value = (b))
#define RangeTableEntryFromNSItem(a) ((a)->p_rte)
#define fcGetArgValue(fc, n) ((fc)->args[n].value)
#define fcGetArgNull(fc, n) ((fc)->args[n].isnull)
#define fcSetArgExt(fc, n, val, is_null) \
	(((fc)->args[n].isnull = (is_null)), ((fc)->args[n].value = (val)))
#define fcSetArg(fc, n, value) fcSetArgExt(fc, n, value, false)
#define fcSetArgNull(fc, n) fcSetArgExt(fc, n, (Datum) 0, true)

#define CREATE_SEQUENCE_COMMAND \
	"CREATE %sSEQUENCE IF NOT EXISTS %s AS %s INCREMENT BY " INT64_FORMAT \
	" MINVALUE " INT64_FORMAT " MAXVALUE " INT64_FORMAT \
	" START WITH " INT64_FORMAT " CACHE " INT64_FORMAT " %sCYCLE"

#endif   /* PG_VERSION_COMPAT_H */
