// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cstdint>
#include <libHX/string.h>
#include <gromox/defs.h>
#include "rops.h"
#include <gromox/rop_util.hpp>
#include "common_util.h"
#include <gromox/proc_common.h>
#include "exmdb_client.h"
#include "logon_object.h"
#include "rop_processor.h"


uint32_t rop_logon_pmb(uint8_t logon_flags, uint32_t open_flags,
    uint32_t store_stat, char *pessdn, size_t dnmax, uint64_t *pfolder_id,
    uint8_t *presponse_flags, GUID *pmailbox_guid, uint16_t *preplica_id,
	GUID *preplica_guid, LOGON_TIME *plogon_time,
	uint64_t *pgwart_time, uint32_t *pstore_stat,
	void *plogmap, uint8_t logon_id, uint32_t *phout)
{
	int handle;
	int user_id;
	int logon_mode;
	struct tm *ptm;
	time_t cur_time;
	struct tm tmp_tm;
	char maildir[256];
	char username[UADDR_SIZE];
	uint32_t permission;
	PROPTAG_ARRAY proptags;
	TPROPVAL_ARRAY propvals;
	uint32_t proptag_buff[2];
	
	auto rpc_info = get_rpc_info();
	if (open_flags & LOGON_OPEN_FLAG_ALTERNATE_SERVER) {
		auto pdomain = strchr(rpc_info.username, '@');
		if (NULL == pdomain) {
			return ecUnknownUser;
		}
		pdomain ++;
		common_util_domain_to_essdn(pdomain, pessdn, dnmax);
		return ecWrongServer;
	}
	if (!common_util_essdn_to_username(pessdn, username, GX_ARRAY_SIZE(username)))
		return ecUnknownUser;
	if (FALSE == common_util_get_id_from_username(username, &user_id)) {
		return ecUnknownUser;
	}
	if (0 != strcasecmp(username, rpc_info.username)) {
		if (open_flags & LOGON_OPEN_FLAG_USE_ADMIN_PRIVILEGE) {
			return ecLoginPerm;
		}
		if (FALSE == common_util_get_maildir(username, maildir)) {
			return ecError;
		}
		if (FALSE == exmdb_client_check_mailbox_permission(maildir,
			rpc_info.username, &permission)) {
			return ecError;
		}
		if (permission == rightsNone)
			return ecLoginPerm;
		*presponse_flags = RESPONSE_FLAG_RESERVED;
		if (permission & frightsGromoxSendAs) {
			*presponse_flags |= RESPONSE_FLAG_SENDASRIGHT;
			logon_mode = LOGON_MODE_DELEGATE;
		} else {
			logon_mode = LOGON_MODE_GUEST;
		}
		if (permission & frightsGromoxStoreOwner) {
			permission ^= frightsGromoxStoreOwner;
			*presponse_flags |= RESPONSE_FLAG_OWNERRIGHT;
			logon_mode = LOGON_MODE_OWNER;
		}
	} else {
		*presponse_flags = RESPONSE_FLAG_RESERVED |
							RESPONSE_FLAG_OWNERRIGHT |
							RESPONSE_FLAG_SENDASRIGHT;
		gx_strlcpy(maildir, rpc_info.maildir, GX_ARRAY_SIZE(maildir));
		logon_mode = LOGON_MODE_OWNER;
	}
	proptags.count = 2;
	proptags.pproptag = proptag_buff;
	proptag_buff[0] = PR_STORE_RECORD_KEY;
	proptag_buff[1] = PROP_TAG_OUTOFOFFICESTATE;
	if (FALSE == exmdb_client_get_store_properties(
		maildir, 0, &proptags, &propvals)) {
		return ecError;
	}
	auto pvalue = common_util_get_propvals(&propvals, PR_STORE_RECORD_KEY);
	if (NULL == pvalue) {
		return ecError;
	}
	*pmailbox_guid = rop_util_binary_to_guid(static_cast<BINARY *>(pvalue));
	pvalue = common_util_get_propvals(&propvals, PROP_TAG_OUTOFOFFICESTATE);
	if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
		*presponse_flags |= RESPONSE_FLAG_OOF;
	}
	
	pfolder_id[0]  = rop_util_make_eid_ex(1, PRIVATE_FID_ROOT);
	pfolder_id[1]  = rop_util_make_eid_ex(1, PRIVATE_FID_DEFERRED_ACTION);
	pfolder_id[2]  = rop_util_make_eid_ex(1, PRIVATE_FID_SPOOLER_QUEUE);
	pfolder_id[3]  = rop_util_make_eid_ex(1, PRIVATE_FID_IPMSUBTREE);
	pfolder_id[4]  = rop_util_make_eid_ex(1, PRIVATE_FID_INBOX);
	pfolder_id[5]  = rop_util_make_eid_ex(1, PRIVATE_FID_OUTBOX);
	pfolder_id[6]  = rop_util_make_eid_ex(1, PRIVATE_FID_SENT_ITEMS);
	pfolder_id[7]  = rop_util_make_eid_ex(1, PRIVATE_FID_DELETED_ITEMS);
	pfolder_id[8]  = rop_util_make_eid_ex(1, PRIVATE_FID_COMMON_VIEWS);
	pfolder_id[9]  = rop_util_make_eid_ex(1, PRIVATE_FID_SCHEDULE);
	pfolder_id[10] = rop_util_make_eid_ex(1, PRIVATE_FID_FINDER);
	pfolder_id[11] = rop_util_make_eid_ex(1, PRIVATE_FID_VIEWS);
	pfolder_id[12] = rop_util_make_eid_ex(1, PRIVATE_FID_SHORTCUTS);
	
	*preplica_id = 0xFFFF;
	preplica_guid->time_low = user_id;
	preplica_guid->time_mid = 0x7f26;
	preplica_guid->time_hi_and_version = 0xedf8;
	preplica_guid->clock_seq[0] = 0xb3;
	preplica_guid->clock_seq[1] = 0x2a;
	preplica_guid->node[0] = 0xce;
	preplica_guid->node[1] = 0x7a;
	preplica_guid->node[2] = 0x6d;
	preplica_guid->node[3] = 0xa2;
	preplica_guid->node[4] = 0xe3;
	preplica_guid->node[5] = 0xb5;
	
	time(&cur_time);
	ptm = gmtime_r(&cur_time, &tmp_tm);
	plogon_time->second = ptm->tm_sec;
	plogon_time->minute = ptm->tm_min;
	plogon_time->hour = ptm->tm_hour;
	plogon_time->day_of_week = ptm->tm_wday;
	plogon_time->day = ptm->tm_mday;
	plogon_time->month = ptm->tm_mon + 1;
	plogon_time->year = ptm->tm_year + 1900;
	
	*pgwart_time = rop_util_unix_to_nttime(cur_time);
	
	*pstore_stat = 0;
	
	auto plogon = logon_object_create(logon_flags, open_flags, logon_mode,
	              user_id, username, maildir, *pmailbox_guid);
	if (NULL == plogon) {
		return ecMAPIOOM;
	}
	/* create logon map and logon object */
	handle = rop_processor_create_logon_item(plogmap, logon_id, plogon.get());
	if (handle < 0) {
		return ecError;
	}
	plogon.release();
	*phout = handle;
	return ecSuccess;
}
	
uint32_t rop_logon_pf(uint8_t logon_flags, uint32_t open_flags,
	uint32_t store_stat, char *pessdn, uint64_t *pfolder_id,
	uint16_t *preplica_id, GUID *preplica_guid, GUID *pper_user_guid,
	void *plogmap, uint8_t logon_id, uint32_t *phout)
{
	int handle;
	int org_id;
	int org_id1;
	void *pvalue;
	int domain_id;
	int domain_id1;
	char homedir[256];
	GUID mailbox_guid;
	const char *pdomain;
	const char *pdomain1;
	
	
	if (0 == (open_flags & LOGON_OPEN_FLAG_PUBLIC) ||
		open_flags & LOGON_OPEN_FLAG_ALTERNATE_SERVER) {
		return ecLoginFailure;
	}
	auto rpc_info = get_rpc_info();
	pdomain = strchr(rpc_info.username, '@');
	if (NULL == pdomain) {
		return ecUnknownUser;
	}
	pdomain ++;
	if (FALSE == common_util_get_domain_ids(pdomain, &domain_id, &org_id)) {
		return ecUnknownUser;
	}
	if (NULL != pessdn) {
		pdomain1 = common_util_essdn_to_domain(pessdn);
		if (NULL != pdomain1 && 0 != strcasecmp(pdomain, pdomain1)) {
			if (0 == org_id) {
				return ecLoginFailure;
			}
			if (FALSE == common_util_get_domain_ids(
				pdomain1, &domain_id1, &org_id1)) {
				return ecError;
			}
			if (org_id != org_id1) {
				return ecLoginFailure;
			}
			domain_id = domain_id1;
			pdomain = pdomain1;
		}
	}
	if (FALSE == common_util_get_homedir_by_id(domain_id, homedir)) {
		return ecError;
	}
	/* like EXCHANGE 2013 or later, we only
		return four folder_ids to client */
	pfolder_id[0]  = rop_util_make_eid_ex(1, PUBLIC_FID_ROOT);
	pfolder_id[1]  = rop_util_make_eid_ex(1, PUBLIC_FID_IPMSUBTREE);
	pfolder_id[2]  = rop_util_make_eid_ex(1, PUBLIC_FID_NONIPMSUBTREE);
	pfolder_id[3]  = rop_util_make_eid_ex(1, PUBLIC_FID_EFORMSREGISTRY);
	pfolder_id[4]  = 0;
	pfolder_id[5]  = 0;
	pfolder_id[6]  = 0;
	pfolder_id[7]  = 0;
	pfolder_id[8]  = 0;
	pfolder_id[9]  = 0;
	pfolder_id[10] = 0;
	pfolder_id[11] = 0;
	pfolder_id[12] = 0;
	
	*preplica_id = 0xFFFF;
	preplica_guid->time_low = domain_id;
	preplica_guid->time_mid = 0xe361;
	preplica_guid->time_hi_and_version = 0x8fde;
	preplica_guid->clock_seq[0] = 0xa2;
	preplica_guid->clock_seq[1] = 0x3c;
	preplica_guid->node[0] = 0xc4;
	preplica_guid->node[1] = 0xf6;
	preplica_guid->node[2] = 0x7e;
	preplica_guid->node[3] = 0x1d;
	preplica_guid->node[4] = 0x2f;
	preplica_guid->node[5] = 0xc6;
	memset(pper_user_guid, 0, sizeof(GUID));
	
	if (!exmdb_client_get_store_property(homedir, 0, PR_STORE_RECORD_KEY, &pvalue))
		return ecError;
	if (NULL == pvalue) {
		return ecError;
	}
	mailbox_guid = rop_util_binary_to_guid(static_cast<BINARY *>(pvalue));
	auto plogon = logon_object_create(logon_flags, open_flags,
	              LOGON_MODE_GUEST, domain_id, pdomain, homedir, mailbox_guid);
	if (NULL == plogon) {
		return ecMAPIOOM;
	}
	/* create logon map and logon object */
	handle = rop_processor_create_logon_item(plogmap, logon_id, plogon.get());
	if (handle < 0) {
		return ecError;
	}
	plogon.release();
	*phout = handle;
	return ecSuccess;
}

uint32_t rop_getreceivefolder(const char *pstr_class,
	uint64_t *pfolder_id, char **ppstr_explicit,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	int object_type;
	
	if (FALSE == common_util_check_message_class(pstr_class)) {
		return ecInvalidParam;
	}
	auto plogon = static_cast<LOGON_OBJECT *>(rop_processor_get_object(plogmap, logon_id, hin, &object_type));
	if (NULL == plogon) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_LOGON != object_type) {
		return ecNotSupported;
	}
	if (!plogon->check_private())
		return ecNotSupported;
	*ppstr_explicit = cu_alloc<char>(256);
	if (NULL == *ppstr_explicit) {
		return ecMAPIOOM;
	}
	if (!exmdb_client_get_folder_by_class(plogon->get_dir(), pstr_class,
	    pfolder_id, *ppstr_explicit))
		return ecError;
	return ecSuccess;
}

uint32_t rop_setreceivefolder(uint64_t folder_id,
	const char *pstr_class, void *plogmap,
	uint8_t logon_id, uint32_t hin)
{
	void *pvalue;
	BOOL b_result;
	int object_type;
	
	if (FALSE == common_util_check_message_class(pstr_class)) {
		return ecInvalidParam;
	}
	if ('\0' == pstr_class[0] && 0 == folder_id) {
		return ecError;
	}
	if (0 == strcasecmp(pstr_class, "IPM") ||
		0 == strcasecmp(pstr_class, "REPORT.IPM")) {
		return ecAccessDenied;
	}
	auto plogon = static_cast<LOGON_OBJECT *>(rop_processor_get_object(plogmap, logon_id, hin, &object_type));
	if (NULL == plogon) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_LOGON != object_type) {
		return ecNotSupported;
	}
	if (!plogon->check_private())
		return ecNotSupported;
	if (0 != folder_id) {
		if (!exmdb_client_get_folder_property(plogon->get_dir(), 0,
		    folder_id, PR_FOLDER_TYPE, &pvalue))
			return ecError;
		if (NULL == pvalue) {
			return ecNotFound;
		}
		if (*static_cast<uint32_t *>(pvalue) == FOLDER_SEARCH)
			return ecNotSupported;
	}
	if (plogon->logon_mode != LOGON_MODE_OWNER)
		return ecAccessDenied;
	if (!exmdb_client_set_folder_by_class(plogon->get_dir(),
	    folder_id, pstr_class, &b_result))
		return ecError;
	if (FALSE == b_result) {
		return ecNotFound;
	}
	return ecSuccess;
}

uint32_t rop_getreceivefoldertable(PROPROW_SET *prows,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	int object_type;
	PROPTAG_ARRAY columns;
	TARRAY_SET class_table;
	uint32_t proptags[3] = {PROP_TAG_FOLDERID, PROP_TAG_MESSAGECLASS_STRING8, PR_LAST_MODIFICATION_TIME};
	
	columns.count = 3;
	columns.pproptag = proptags;
	auto plogon = static_cast<LOGON_OBJECT *>(rop_processor_get_object(plogmap, logon_id, hin, &object_type));
	if (NULL == plogon) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_LOGON != object_type) {
		return ecNotSupported;
	}
	if (!plogon->check_private())
		return ecNotSupported;
	if (!exmdb_client_get_folder_class_table(plogon->get_dir(), &class_table))
		return ecError;
	if (0 == class_table.count) {
		return ecNoReceiveFolder;
	}
	prows->count = class_table.count;
	prows->prows = cu_alloc<PROPERTY_ROW>(class_table.count);
	if (NULL == prows->prows) {
		return ecMAPIOOM;
	}
	for (size_t i = 0; i < class_table.count; ++i) {
		if (FALSE == common_util_propvals_to_row(
			class_table.pparray[i], &columns, prows->prows + i)) {
			return ecMAPIOOM;
		}
	}
	return ecSuccess;
}

uint32_t rop_getstorestat(uint32_t *pstat,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	/* just like EXCHANGE 2010 or later,
		we do not implement this rop */
	return NotImplemented;
}

uint32_t rop_getowningservers(
	uint64_t folder_id, GHOST_SERVER *pghost,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	GUID guid;
	BOOL b_found;
	int domain_id;
	int object_type;
	uint16_t replid;
	
	auto plogon = static_cast<LOGON_OBJECT *>(rop_processor_get_object(plogmap, logon_id, hin, &object_type));
	if (NULL == plogon) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_LOGON != object_type) {
		return ecNotSupported;
	}
	if (plogon->check_private())
		return ecNotSupported;
	pghost->server_count = 1;
	pghost->cheap_server_count = 1;
	pghost->ppservers = cu_alloc<char *>();
	if (NULL == pghost->ppservers) {
		return ecMAPIOOM;
	}
	replid = rop_util_get_replid(folder_id);
	if (1 != replid) {
		if (!exmdb_client_get_mapping_guid(plogon->get_dir(), replid,
		    &b_found, &guid))
			return ecError;
		if (FALSE == b_found) {
			return ecNotFound;
		}
		domain_id = rop_util_make_domain_id(guid);
		if (-1 == domain_id) {
			return ecNotFound;
		}
		if (domain_id != plogon->account_id &&
		    !common_util_check_same_org(domain_id, plogon->account_id))
			return ecNotFound;
	} else {
		// domain_id = plogon->account_id;
	}
	static constexpr size_t dnmax = 256;
	pghost->ppservers[0] = cu_alloc<char>(dnmax);
	if (NULL == pghost->ppservers[0]) {
		return ecMAPIOOM;
	}
	common_util_domain_to_essdn(plogon->get_account(), pghost->ppservers[0], dnmax);
	return ecSuccess;
}

uint32_t rop_publicfolderisghosted(
	uint64_t folder_id, GHOST_SERVER **ppghost,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	int object_type;
	uint16_t replid;
	
	auto plogon = static_cast<LOGON_OBJECT *>(rop_processor_get_object(plogmap, logon_id, hin, &object_type));
	if (NULL == plogon) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_LOGON != object_type) {
		return ecNotSupported;
	}
	if (plogon->check_private()) {
		*ppghost = NULL;
		return ecSuccess;
	}
	replid = rop_util_get_replid(folder_id);
	if (1 == replid) {
		*ppghost = NULL;
		return ecSuccess;
	}
	*ppghost = cu_alloc<GHOST_SERVER>();
	if (NULL == *ppghost) {
		return ecMAPIOOM;
	}
	return rop_getowningservers(folder_id,
			*ppghost, plogmap, logon_id, hin);
}

uint32_t rop_longtermidfromid(uint64_t id,
	LONG_TERM_ID *plong_term_id,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	BOOL b_found;
	uint16_t replid;
	int object_type;
	
	auto plogon = static_cast<LOGON_OBJECT *>(rop_processor_get_object(plogmap, logon_id, hin, &object_type));
	if (NULL == plogon) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_LOGON != object_type) {
		return ecNotSupported;
	}
	memset(plong_term_id, 0, sizeof(LONG_TERM_ID));
	if (plogon->check_private()) {
		if (1 != rop_util_get_replid(id)) {
			return ecInvalidParam;
		}
		plong_term_id->guid = rop_util_make_user_guid(plogon->account_id);
	} else {
		replid = rop_util_get_replid(id);
		if (1 == replid) {
			plong_term_id->guid = rop_util_make_domain_guid(plogon->account_id);
		} else {
			if (!exmdb_client_get_mapping_guid(plogon->get_dir(),
			    replid, &b_found, &plong_term_id->guid))
				return ecError;
			if (FALSE == b_found) {
				return ecNotFound;
			}
		}	
	}
	rop_util_get_gc_array(id, plong_term_id->global_counter);
	return ecSuccess;
}	

uint32_t rop_idfromlongtermid(
	const LONG_TERM_ID *plong_term_id, uint64_t *pid,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	BOOL b_found;
	int domain_id;
	int object_type;
	uint16_t replid;
	
	auto plogon = static_cast<LOGON_OBJECT *>(rop_processor_get_object(plogmap, logon_id, hin, &object_type));
	if (NULL == plogon) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_LOGON != object_type) {
		return ecNotSupported;
	}
	if (plogon->check_private()) {
		auto tmp_guid = rop_util_make_user_guid(plogon->account_id);
		if (0 != memcmp(&tmp_guid, &plong_term_id->guid, sizeof(GUID))) {
			return ecInvalidParam;
		}
		*pid = rop_util_make_eid(1, plong_term_id->global_counter);
	} else {
		domain_id = rop_util_make_domain_id(plong_term_id->guid);
		if (-1 == domain_id) {
			return ecInvalidParam;
		}
		if (domain_id == plogon->account_id) {
			replid = 1;
		} else {
			if (!common_util_check_same_org(domain_id, plogon->account_id))
				return ecInvalidParam;
			if (!exmdb_client_get_mapping_replid(plogon->get_dir(),
			    plong_term_id->guid, &b_found, &replid))
				return ecError;
			if (FALSE == b_found) {
				return ecNotFound;
			}
		}
		*pid = rop_util_make_eid(replid, plong_term_id->global_counter);
	}
	return ecSuccess;
}

uint32_t rop_getperuserlongtermids(const GUID *pguid,
	LONG_TERM_ID_ARRAY *plong_term_ids,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	int object_type;
	
	auto plogon = static_cast<LOGON_OBJECT *>(rop_processor_get_object(plogmap, logon_id, hin, &object_type));
	if (NULL == plogon) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_LOGON != object_type) {
		return ecNotSupported;
	}
	if (plogon->check_private()) {
		plong_term_ids->count = 0;
		return ecSuccess;
	}
	return ecNotSupported;
}

uint32_t rop_getperuserguid(
	const LONG_TERM_ID *plong_term_id, GUID *pguid,
	void *plogmap,uint8_t logon_id,  uint32_t hin)
{
	int object_type;
	
	auto plogon = static_cast<LOGON_OBJECT *>(rop_processor_get_object(plogmap, logon_id, hin, &object_type));
	if (NULL == plogon) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_LOGON != object_type) {
		return ecNotSupported;
	}
	if (plogon->check_private())
		return ecNotFound;
	return ecNotSupported;
}

uint32_t rop_readperuserinformation(
	const LONG_TERM_ID *plong_folder_id,
	uint8_t reserved, uint32_t data_offset,
	uint16_t max_data_size, uint8_t *phas_finished,
	BINARY *pdata, void *plogmap, uint8_t logon_id,
	uint32_t hin)
{
	int object_type;
	
	auto plogon = static_cast<LOGON_OBJECT *>(rop_processor_get_object(plogmap, logon_id, hin, &object_type));
	if (NULL == plogon) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_LOGON != object_type) {
		return ecNotSupported;
	}
	*phas_finished = 1;
	pdata->cb = 0;
	pdata->pb = NULL;
	return ecSuccess;
}

uint32_t rop_writeperuserinformation(
	const LONG_TERM_ID *plong_folder_id,
	uint8_t has_finished, uint32_t offset,
	const BINARY *pdata, const GUID *pguid,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	int object_type;
	
	auto plogon = static_cast<LOGON_OBJECT *>(rop_processor_get_object(plogmap, logon_id, hin, &object_type));
	if (NULL == plogon) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_LOGON != object_type) {
		return ecNotSupported;
	}
	return ecSuccess;
}
