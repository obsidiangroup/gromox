// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <climits>
#include <cstdint>
#include <memory>
#include <gromox/defs.h>
#include <gromox/mapidefs.h>
#include "rops.h"
#include <gromox/guid.hpp>
#include <gromox/idset.hpp>
#include <gromox/rop_util.hpp>
#include <gromox/eid_array.hpp>
#include "common_util.h"
#include <gromox/defs.h>
#include <gromox/proc_common.h>
#include "exmdb_client.h"
#include "folder_object.h"
#include "rop_processor.h"
#include <gromox/tpropval_array.hpp>
#include "message_object.h"
#include "icsupctx_object.h"
#include "emsmdb_interface.h"
#include "fastupctx_object.h"
#include "icsdownctx_object.h"
#include "attachment_object.h"
#include "fastdownctx_object.h"


static EID_ARRAY* oxcfxics_load_folder_messages(
	LOGON_OBJECT *plogon, uint64_t folder_id,
	const char *username, BOOL b_fai)
{
	uint64_t *pmid;
	uint32_t table_id;
	uint32_t row_count;
	TARRAY_SET tmp_set;
	uint32_t tmp_proptag;
	PROPTAG_ARRAY proptags;
	RESTRICTION restriction;
	EID_ARRAY *pmessage_ids;
	RESTRICTION_PROPERTY res_prop;
	uint8_t tmp_associated = !!b_fai;
	
	restriction.rt = RES_PROPERTY;
	restriction.pres = &res_prop;
	res_prop.relop = RELOP_EQ;
	res_prop.proptag = PR_ASSOCIATED;
	res_prop.propval.proptag = res_prop.proptag;
	res_prop.propval.pvalue = &tmp_associated;
	if (!exmdb_client_load_content_table(plogon->get_dir(), 0, folder_id,
	    username, TABLE_FLAG_NONOTIFICATIONS, &restriction, nullptr,
	    &table_id, &row_count))
		return NULL;	
	proptags.count = 1;
	proptags.pproptag = &tmp_proptag;
	tmp_proptag = PROP_TAG_MID;
	if (!exmdb_client_query_table(plogon->get_dir(), nullptr, 0, table_id,
	    &proptags, 0, row_count, &tmp_set))
		return NULL;	
	exmdb_client_unload_table(plogon->get_dir(), table_id);
	pmessage_ids = eid_array_init();
	if (NULL == pmessage_ids) {
		return NULL;
	}
	for (size_t i = 0; i < tmp_set.count; ++i) {
		pmid = static_cast<uint64_t *>(common_util_get_propvals(
		       tmp_set.pparray[i], PROP_TAG_MID));
		if (NULL == pmid) {
			eid_array_free(pmessage_ids);
			return NULL;
		}
		if (!eid_array_append(pmessage_ids, *pmid)) {
			eid_array_free(pmessage_ids);
			return NULL;
		}
	}
	return pmessage_ids;
}

static std::unique_ptr<FOLDER_CONTENT> oxcfxics_load_folder_content(
	LOGON_OBJECT *plogon, uint64_t folder_id,
	BOOL b_fai, BOOL b_normal, BOOL b_sub)
{
	BOOL b_found;
	BINARY *pbin;
	uint16_t replid;
	uint32_t table_id;
	uint32_t row_count;
	TARRAY_SET tmp_set;
	char tmp_essdn[256];
	uint32_t permission;
	const char *username;
	uint64_t *pfolder_id;
	uint32_t tmp_proptag;
	EID_ARRAY *pmessage_ids;
	LONG_TERM_ID long_term_id;
	TAGGED_PROPVAL tmp_propval;
	PROPTAG_ARRAY tmp_proptags;
	TPROPVAL_ARRAY tmp_propvals;
	
	if (plogon->logon_mode != LOGON_MODE_OWNER) {
		auto rpc_info = get_rpc_info();
		username = rpc_info.username;
		if (!exmdb_client_check_folder_permission(plogon->get_dir(),
		    folder_id, username, &permission))
			return NULL;	
		if (!(permission & (frightsReadAny | frightsOwner)))
			return NULL;
	} else {
		username = NULL;
	}
	auto pfldctnt = folder_content_init();
	if (NULL == pfldctnt) {
		return NULL;
	}
	if (!exmdb_client_get_folder_all_proptags(plogon->get_dir(),
	    folder_id, &tmp_proptags)) {
		return NULL;
	}
	auto pinfo = emsmdb_interface_get_emsmdb_info();
	if (!exmdb_client_get_folder_properties(plogon->get_dir(), pinfo->cpid,
	    folder_id, &tmp_proptags, &tmp_propvals)) {
		return NULL;
	}
	auto pproplist = pfldctnt->get_proplist();
	for (size_t i = 0; i < tmp_propvals.count; ++i) {
		if (!tpropval_array_set_propval(pproplist,
		    tmp_propvals.ppropval + i)) {
			return NULL;
		}
	}
	replid = rop_util_get_replid(folder_id);
	if (1 != replid) {
		if (!exmdb_client_get_mapping_guid(plogon->get_dir(), replid,
		    &b_found, &long_term_id.guid) || !b_found) {
			return NULL;
		}
		rop_util_get_gc_array(folder_id,
			long_term_id.global_counter);
		common_util_domain_to_essdn(plogon->get_account(),
			tmp_essdn, GX_ARRAY_SIZE(tmp_essdn));
		pbin = common_util_to_folder_replica(
				&long_term_id, tmp_essdn);
		if (NULL == pbin) {
			return NULL;
		}
		tmp_propval.proptag = META_TAG_NEWFXFOLDER;
		tmp_propval.pvalue = pbin;
		if (!tpropval_array_set_propval(pproplist,
		    &tmp_propval)) {
			return NULL;
		}
		return pfldctnt;
	}
	if (TRUE == b_fai) {
		pmessage_ids = oxcfxics_load_folder_messages(
					plogon, folder_id, username, TRUE);
		if (NULL == pmessage_ids) {
			return NULL;
		}
		pfldctnt->append_failist_internal(pmessage_ids);
	}
	if (TRUE == b_normal) {
		pmessage_ids = oxcfxics_load_folder_messages(
					plogon, folder_id, username, FALSE);
		if (NULL == pmessage_ids) {
			return NULL;
		}
		pfldctnt->append_normallist_internal(pmessage_ids);
	}
	if (TRUE == b_sub) {
		DCERPC_INFO rpc_info;
		if (plogon->logon_mode != LOGON_MODE_OWNER) {
			rpc_info = get_rpc_info();
			username = rpc_info.username;
		} else {
			username = NULL;
		}
		if (!exmdb_client_load_hierarchy_table(plogon->get_dir(),
		    folder_id, username, TABLE_FLAG_NONOTIFICATIONS, nullptr,
		    &table_id, &row_count)) {
			return NULL;	
		}
		tmp_proptags.count = 1;
		tmp_proptags.pproptag = &tmp_proptag;
		tmp_proptag = PROP_TAG_FOLDERID;
		if (!exmdb_client_query_table(plogon->get_dir(), nullptr, 0,
		    table_id, &tmp_proptags, 0, row_count, &tmp_set)) {
			return NULL;	
		}
		exmdb_client_unload_table(plogon->get_dir(), table_id);
		for (size_t i = 0; i < tmp_set.count; ++i) {
			pfolder_id = static_cast<uint64_t *>(common_util_get_propvals(
			             tmp_set.pparray[i], PROP_TAG_FOLDERID));
			if (NULL == pfolder_id) {
				return NULL;
			}
			auto psubfldctnt = oxcfxics_load_folder_content(
				plogon, *pfolder_id, TRUE, TRUE, TRUE);
			if (NULL == psubfldctnt) {
				return NULL;
			}
			if (!pfldctnt->append_subfolder_internal(std::move(*psubfldctnt)))
				return NULL;
		}
	}
	return pfldctnt;
}

uint32_t rop_fasttransferdestconfigure(
	uint8_t source_operation, uint8_t flags,
	void *plogmap, uint8_t logon_id,
	uint32_t hin, uint32_t *phout)
{
	void *pvalue;
	void *pobject;
	int object_type;
	int root_element;
	uint32_t proptag_buff[4];
	PROPTAG_ARRAY tmp_proptags;
	TPROPVAL_ARRAY tmp_propvals;
	
	if (flags & ~FAST_DEST_CONFIG_FLAG_MOVE) {
		return ecInvalidParam;
	}
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	pobject = rop_processor_get_object(plogmap,
				logon_id, hin, &object_type);
	if (NULL == pobject) {
		return ecNullObject;
	}
	switch (source_operation) {
	case FAST_SOURCE_OPERATION_COPYTO:
	case FAST_SOURCE_OPERATION_COPYPROPERTIES:
		switch (object_type) {
		case OBJECT_TYPE_FOLDER:
			root_element = ROOT_ELEMENT_FOLDERCONTENT;
			break;
		case OBJECT_TYPE_MESSAGE:
			root_element = ROOT_ELEMENT_MESSAGECONTENT;
			break;
		case OBJECT_TYPE_ATTACHMENT:
			root_element = ROOT_ELEMENT_ATTACHMENTCONTENT;
			break;
		default:
			return ecNotSupported;
		}
		break;
	case FAST_SOURCE_OPERATION_COPYMESSAGES:
		if (OBJECT_TYPE_FOLDER != object_type) {
			return ecNotSupported;
		}
		root_element = ROOT_ELEMENT_MESSAGELIST;
		break;
	case FAST_SOURCE_OPERATION_COPYFOLDER:
		if (OBJECT_TYPE_FOLDER != object_type) {
			return ecNotSupported;
		}
		root_element = ROOT_ELEMENT_TOPFOLDER;
		break;
	default:
		return ecInvalidParam;
	}
	if (ROOT_ELEMENT_TOPFOLDER == root_element ||
		ROOT_ELEMENT_MESSAGELIST == root_element ||
		ROOT_ELEMENT_FOLDERCONTENT == root_element) {
		tmp_proptags.count = 4;
		tmp_proptags.pproptag = proptag_buff;
		proptag_buff[0] = PR_MESSAGE_SIZE_EXTENDED;
		proptag_buff[1] = PROP_TAG_STORAGEQUOTALIMIT;
		proptag_buff[2] = PR_ASSOC_CONTENT_COUNT;
		proptag_buff[3] = PROP_TAG_CONTENTCOUNT;
		if (!plogon->get_properties(&tmp_proptags, &tmp_propvals))
			return ecError;
		pvalue = common_util_get_propvals(&tmp_propvals, PROP_TAG_STORAGEQUOTALIMIT);
		uint64_t max_quota = ULLONG_MAX;
		if (pvalue != nullptr) {
			max_quota = *static_cast<uint32_t *>(pvalue);
			max_quota = max_quota >= ULLONG_MAX / 1024 ? ULLONG_MAX : max_quota * 1024ULL;
		}
		pvalue = common_util_get_propvals(&tmp_propvals, PR_MESSAGE_SIZE_EXTENDED);
		uint64_t total_size = pvalue == nullptr ? 0 : *static_cast<uint64_t *>(pvalue);
		if (total_size > max_quota)
			return ecQuotaExceeded;
		pvalue = common_util_get_propvals(&tmp_propvals, PR_ASSOC_CONTENT_COUNT);
		uint32_t total_mail = pvalue != nullptr ? *static_cast<uint32_t *>(pvalue) : 0;
		pvalue = common_util_get_propvals(&tmp_propvals,
								PROP_TAG_CONTENTCOUNT);
		if (NULL != pvalue) {
			total_mail += *(uint32_t*)pvalue;
		}
		if (total_mail > common_util_get_param(
			COMMON_UTIL_MAX_MESSAGE)) {
			return ecQuotaExceeded;
		}
	}
	auto pctx = fastupctx_object_create(plogon, pobject, root_element);
	if (NULL == pctx) {
		return ecError;
	}
	auto hnd = rop_processor_add_object_handle(plogmap,
	           logon_id, hin, OBJECT_TYPE_FASTUPCTX, pctx.get());
	if (hnd < 0) {
		return ecError;
	}
	pctx.release();
	*phout = hnd;
	return ecSuccess;
}

uint32_t rop_fasttransferdestputbuffer(
	const BINARY *ptransfer_data, uint16_t *ptransfer_status,
	uint16_t *pin_progress_count, uint16_t *ptotal_step_count,
	uint8_t *preserved, uint16_t *pused_size, void *plogmap,
	uint8_t logon_id, uint32_t hin)
{
	void *pobject;
	int object_type;
	
	*ptransfer_status = 0;
	*pin_progress_count = 0;
	*ptotal_step_count = 1;
	*preserved = 0;
	*pused_size = 0;
	pobject = rop_processor_get_object(plogmap,
				logon_id, hin, &object_type);
	if (NULL == pobject) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_FASTUPCTX != object_type) {
		return ecNotSupported;
	}
	auto err = static_cast<FASTUPCTX_OBJECT *>(pobject)->write_buffer(ptransfer_data);
	if (err != GXERR_SUCCESS)
		return gxerr_to_hresult(err);
	*pused_size = ptransfer_data->cb;
	return ecSuccess;
}

uint32_t rop_fasttransfersourcegetbuffer(uint16_t buffer_size,
	uint16_t max_buffer_size, uint16_t *ptransfer_status,
	uint16_t *pin_progress_count, uint16_t *ptotal_step_count,
	uint8_t *preserved, BINARY *ptransfer_data,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	BOOL b_last;
	void *pobject;
	int object_type;
	uint16_t max_rop;
	
	*ptransfer_status = TRANSFER_STATUS_ERROR;
	*pin_progress_count = 0;
	*ptotal_step_count = 1;
	*preserved = 0;
	ptransfer_data->cb = 0;
	pobject = rop_processor_get_object(plogmap,
				logon_id, hin, &object_type);
	if (NULL == pobject) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_ICSDOWNCTX != object_type &&
		OBJECT_TYPE_FASTDOWNCTX != object_type) {
		return ecNotSupported;
	}
	emsmdb_interface_get_rop_left(&max_rop);
	max_rop -= 32;
	if (max_rop > 0x7b00) {
		max_rop = 0x7b00;
	}
	uint16_t len = buffer_size == 0xBABE ? max_buffer_size : buffer_size;
	if (len > max_rop) {
		len = max_rop;
	}
	ptransfer_data->pv = common_util_alloc(len);
	if (ptransfer_data->pv == nullptr)
		return ecMAPIOOM;
	if (OBJECT_TYPE_FASTDOWNCTX == object_type) {
		if (!static_cast<FASTDOWNCTX_OBJECT *>(pobject)->get_buffer(
		    ptransfer_data->pv, &len, &b_last, pin_progress_count, ptotal_step_count))
			return ecError;
	} else if (OBJECT_TYPE_ICSDOWNCTX == object_type) {
		auto dobj = static_cast<ICSDOWNCTX_OBJECT *>(pobject);
		if (!dobj->check_started() && !dobj->make_sync())
			return ecError;
		if (!dobj->get_buffer(ptransfer_data->pv, &len, &b_last,
		    pin_progress_count, ptotal_step_count))
			return ecError;
	}
	if (0xBABE != buffer_size && len > max_rop) {
		return ecBufferTooSmall;
	}
	*ptransfer_status = !b_last ? TRANSFER_STATUS_PARTIAL : TRANSFER_STATUS_DONE;
	ptransfer_data->cb = len;
	return ecSuccess;
}

uint32_t rop_fasttransfersourcecopyfolder(uint8_t flags,
	uint8_t send_options, void *plogmap, uint8_t logon_id,
	uint32_t hin, uint32_t *phout)
{
	BOOL b_sub;
	int object_type;
	
	if (send_options & ~(SEND_OPTIONS_UNICODE|
		SEND_OPTIONS_USECPID|SEND_OPTIONS_RECOVERMODE|
		SEND_OPTIONS_FORCEUNICODE|SEND_OPTIONS_PARTIAL|
		SEND_OPTIONS_RESERVED1|SEND_OPTIONS_RESERVED2)) {
		return ecInvalidParam;
	}
	if ((send_options & SEND_OPTIONS_UNICODE) &&
		(send_options & SEND_OPTIONS_USECPID) &&
		(send_options & SEND_OPTIONS_RECOVERMODE)) {
		return ecInvalidParam;
	}
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	auto pfolder = static_cast<FOLDER_OBJECT *>(rop_processor_get_object(plogmap,
	               logon_id, hin, &object_type));
	if (NULL == pfolder) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_FOLDER != object_type) {
		return ecNotSupported;
	}
	b_sub = FALSE;
	if (flags & FAST_COPY_FOLDER_FLAG_MOVE ||
		flags & FAST_COPY_FOLDER_FLAG_COPYSUBFOLDERS) {
		b_sub = TRUE;
	}
	auto pfldctnt = oxcfxics_load_folder_content(plogon, pfolder->folder_id,
	           TRUE, TRUE, b_sub);
	if (NULL == pfldctnt) {
		return ecError;
	}
	auto pctx = fastdownctx_object_create(plogon, send_options & 0x0F);
	if (NULL == pctx) {
		return ecError;
	}
	if (!pctx->make_topfolder(std::move(pfldctnt)))
		return ecError;
	auto hnd = rop_processor_add_object_handle(plogmap,
	           logon_id, hin, OBJECT_TYPE_FASTDOWNCTX, pctx.get());
	if (hnd < 0) {
		return ecError;
	}
	pctx.release();
	*phout = hnd;
	return ecSuccess;
}

uint32_t rop_fasttransfersourcecopymessages(
	const LONGLONG_ARRAY *pmessage_ids, uint8_t flags,
	uint8_t send_options, void *plogmap, uint8_t logon_id,
	uint32_t hin, uint32_t *phout)
{
	BOOL b_owner;
	int object_type;
	EID_ARRAY *pmids;
	uint32_t permission;
	
	if (send_options & ~(SEND_OPTIONS_UNICODE|
		SEND_OPTIONS_USECPID|SEND_OPTIONS_RECOVERMODE|
		SEND_OPTIONS_FORCEUNICODE|SEND_OPTIONS_PARTIAL|
		SEND_OPTIONS_RESERVED1|SEND_OPTIONS_RESERVED2)) {
		return ecInvalidParam;
	}
	if ((send_options & SEND_OPTIONS_UNICODE) &&
		(send_options & SEND_OPTIONS_USECPID) &&
		(send_options & SEND_OPTIONS_RECOVERMODE)) {
		return ecInvalidParam;
	}
	/* we ignore the FAST_COPY_MESSAGE_FLAG_MOVE
	   in flags just like exchange 2010 or later */
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	auto pfolder = static_cast<FOLDER_OBJECT *>(rop_processor_get_object(plogmap,
	               logon_id, hin, &object_type));
	if (NULL == pfolder) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_FOLDER != object_type) {
		return ecNotSupported;
	}
	if (plogon->logon_mode != LOGON_MODE_OWNER) {
		auto rpc_info = get_rpc_info();
		if (!exmdb_client_check_folder_permission(plogon->get_dir(),
		    pfolder->folder_id, rpc_info.username, &permission))
			return ecError;
		if (!(permission & (frightsReadAny | frightsOwner))) {
			for (size_t i = 0; i < pmessage_ids->count; ++i) {
				if (!exmdb_client_check_message_owner(plogon->get_dir(),
				    pmessage_ids->pll[i], rpc_info.username, &b_owner))
					return ecError;
				if (FALSE == b_owner) {
					return ecAccessDenied;
				}
			}
		}
	}
	pmids = eid_array_init();
	if (NULL == pmids) {
		return ecMAPIOOM;
	}
	if (!eid_array_batch_append(pmids, pmessage_ids->count,
	    pmessage_ids->pll)) {
		eid_array_free(pmids);
		return ecMAPIOOM;
	}
	BOOL b_chginfo = (flags & FAST_COPY_MESSAGE_FLAG_SENDENTRYID) ? TRUE : false;
	auto pctx = fastdownctx_object_create(plogon, send_options & 0x0F);
	if (NULL == pctx) {
		eid_array_free(pmids);
		return ecError;
	}
	if (!pctx->make_messagelist(b_chginfo, pmids)) {
		pctx.reset();
		eid_array_free(pmids);
		return ecError;
	}
	auto hnd = rop_processor_add_object_handle(plogmap,
	           logon_id, hin, OBJECT_TYPE_FASTDOWNCTX, pctx.get());
	if (hnd < 0) {
		return ecError;
	}
	pctx.release();
	*phout = hnd;
	return ecSuccess;
}

uint32_t rop_fasttransfersourcecopyto(uint8_t level, uint32_t flags,
	uint8_t send_options, const PROPTAG_ARRAY *pproptags,
	void *plogmap, uint8_t logon_id, uint32_t hin, uint32_t *phout)
{
	int i;
	BOOL b_sub;
	BOOL b_fai;
	BOOL b_normal;
	void *pobject;
	int object_type;
	MESSAGE_CONTENT msgctnt;
	ATTACHMENT_CONTENT attctnt;
	
	if (send_options & ~(SEND_OPTIONS_UNICODE|
		SEND_OPTIONS_USECPID|SEND_OPTIONS_RECOVERMODE|
		SEND_OPTIONS_FORCEUNICODE|SEND_OPTIONS_PARTIAL|
		SEND_OPTIONS_RESERVED1|SEND_OPTIONS_RESERVED2)) {
		return ecInvalidParam;
	}
	if ((send_options & SEND_OPTIONS_UNICODE) &&
		(send_options & SEND_OPTIONS_USECPID) &&
		(send_options & SEND_OPTIONS_RECOVERMODE)) {
		return ecInvalidParam;
	}
	/* just like exchange 2010 or later */
	if (flags & FAST_COPY_TO_FLAG_MOVE) {
		return ecInvalidParam;
	}
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	pobject = rop_processor_get_object(plogmap,
				logon_id, hin, &object_type);
	if (NULL == pobject) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_FOLDER != object_type &&
		OBJECT_TYPE_MESSAGE != object_type &&
		OBJECT_TYPE_ATTACHMENT != object_type) {
		return ecNotSupported;
	}
	auto pctx = fastdownctx_object_create(plogon, send_options & 0x0F);
	if (NULL == pctx) {
		return ecError;
	}
	switch (object_type) {
	case OBJECT_TYPE_FOLDER: {
		if (0 == level) {
			b_sub = TRUE;
			b_fai = TRUE;
			b_normal = TRUE;
			for (i=0; i<pproptags->count; i++) {
				switch (pproptags->pproptag[i]) {
				case PROP_TAG_CONTAINERHIERARCHY:
					b_sub = FALSE;
					break;
				case PROP_TAG_CONTAINERCONTENTS:
					b_normal = FALSE;
					break;
				case PROP_TAG_FOLDERASSOCIATEDCONTENTS:
					b_fai = FALSE;
					break;
				}
			}
		} else {
			b_sub = FALSE;
			b_fai = FALSE;
			b_normal = FALSE;
		}
		auto pfldctnt = oxcfxics_load_folder_content(plogon,
		           static_cast<FOLDER_OBJECT *>(pobject)->folder_id,
		           b_fai, b_normal, b_sub);
		if (NULL == pfldctnt) {
			return ecError;
		}
		auto pproplist = pfldctnt->get_proplist();
		for (i=0; i<pproptags->count; i++) {
			tpropval_array_remove_propval(
				pproplist, pproptags->pproptag[i]);
		}
		if (!pctx->make_foldercontent(b_sub, std::move(pfldctnt)))
			return ecError;
		break;
	}
	case OBJECT_TYPE_MESSAGE:
		if (!static_cast<MESSAGE_OBJECT *>(pobject)->flush_streams())
			return ecError;
		if (!exmdb_client_read_message_instance(plogon->get_dir(),
		    static_cast<MESSAGE_OBJECT *>(pobject)->get_instance_id(), &msgctnt))
			return ecError;
		for (i=0; i<pproptags->count; i++) {
			switch (pproptags->pproptag[i]) {
			case PR_MESSAGE_RECIPIENTS:	
				msgctnt.children.prcpts = NULL;
				break;
			case PR_MESSAGE_ATTACHMENTS:
				msgctnt.children.pattachments = NULL;
				break;
			default:
				common_util_remove_propvals(&msgctnt.proplist,
										pproptags->pproptag[i]);
				break;
			}
		}
		if (0 != level) {
			msgctnt.children.prcpts = NULL;
			msgctnt.children.pattachments = NULL;
		}
		if (!pctx->make_messagecontent(&msgctnt))
			return ecError;
		break;
	case OBJECT_TYPE_ATTACHMENT:
		if (!static_cast<ATTACHMENT_OBJECT *>(pobject)->flush_streams())
			return ecError;
		if (!exmdb_client_read_attachment_instance(plogon->get_dir(),
		    static_cast<ATTACHMENT_OBJECT *>(pobject)->get_instance_id(), &attctnt))
			return ecError;
		for (i=0; i<pproptags->count; i++) {
			switch (pproptags->pproptag[i]) {
			case PR_ATTACH_DATA_OBJ:
				attctnt.pembedded = NULL;
				break;
			default:
				common_util_remove_propvals(&attctnt.proplist,
										pproptags->pproptag[i]);
				break;
			}
		}
		if (!pctx->make_attachmentcontent(&attctnt))
			return ecError;
		break;
	}
	auto hnd = rop_processor_add_object_handle(plogmap,
	           logon_id, hin, OBJECT_TYPE_FASTDOWNCTX, pctx.get());
	if (hnd < 0) {
		return ecError;
	}
	pctx.release();
	*phout = hnd;
	return ecSuccess;
}

uint32_t rop_fasttransfersourcecopyproperties(uint8_t level, uint8_t flags,
	uint8_t send_options, const PROPTAG_ARRAY *pproptags, void *plogmap,
	uint8_t logon_id, uint32_t hin, uint32_t *phout)
{
	int i;
	BOOL b_sub;
	BOOL b_fai;
	BOOL b_normal;
	void *pobject;
	int object_type;
	MESSAGE_CONTENT msgctnt;
	ATTACHMENT_CONTENT attctnt;
	
	if (send_options & ~(SEND_OPTIONS_UNICODE|
		SEND_OPTIONS_USECPID|SEND_OPTIONS_RECOVERMODE|
		SEND_OPTIONS_FORCEUNICODE|SEND_OPTIONS_PARTIAL|
		SEND_OPTIONS_RESERVED1|SEND_OPTIONS_RESERVED2)) {
		return ecInvalidParam;
	}
	if ((send_options & SEND_OPTIONS_UNICODE) &&
		(send_options & SEND_OPTIONS_USECPID) &&
		(send_options & SEND_OPTIONS_RECOVERMODE)) {
		return ecInvalidParam;
	}
	/* just like exchange 2010 or later */
	if (flags & FAST_COPY_PROPERTIES_FLAG_MOVE) {
		return ecInvalidParam;
	}
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	pobject = rop_processor_get_object(plogmap,
				logon_id, hin, &object_type);
	if (NULL == pobject) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_FOLDER != object_type &&
		OBJECT_TYPE_MESSAGE != object_type &&
		OBJECT_TYPE_ATTACHMENT != object_type) {
		return ecNotSupported;
	}
	auto pctx = fastdownctx_object_create(plogon, send_options & 0x0F);
	if (NULL == pctx) {
		return ecError;
	}
	switch (object_type) {
	case OBJECT_TYPE_FOLDER: {
		if (0 == level) {
			b_sub = FALSE;
			b_fai = FALSE;
			b_normal = FALSE;
			for (i=0; i<pproptags->count; i++) {
				switch (pproptags->pproptag[i]) {
				case PROP_TAG_CONTAINERHIERARCHY:
					b_sub = TRUE;
					break;
				case PROP_TAG_CONTAINERCONTENTS:
					b_normal = TRUE;
					break;
				case PROP_TAG_FOLDERASSOCIATEDCONTENTS:
					b_fai = TRUE;
					break;
				}
			}
		} else {
			b_sub = FALSE;
			b_fai = FALSE;
			b_normal = FALSE;
		}
		auto pfldctnt = oxcfxics_load_folder_content(plogon,
		           static_cast<FOLDER_OBJECT *>(pobject)->folder_id,
		           b_fai, b_normal, b_sub);
		if (NULL == pfldctnt) {
			return ecError;
		}
		auto pproplist = pfldctnt->get_proplist();
		i = 0;
		while (i < pproplist->count) {
			if (META_TAG_NEWFXFOLDER != pproplist->ppropval[i].proptag) {
				if (-1 == common_util_index_proptags(pproptags,
					pproplist->ppropval[i].proptag)) {
					tpropval_array_remove_propval(pproplist,
							pproplist->ppropval[i].proptag);
					continue;
				}
			}
			i ++;
		}
		if (!pctx->make_foldercontent(b_sub, std::move(pfldctnt)))
			return ecError;
		break;
	}
	case OBJECT_TYPE_MESSAGE:
		if (!static_cast<MESSAGE_OBJECT *>(pobject)->flush_streams())
			return ecError;
		if (!exmdb_client_read_message_instance(plogon->get_dir(),
		    static_cast<MESSAGE_OBJECT *>(pobject)->get_instance_id(), &msgctnt))
			return ecError;
		i = 0;
		while (i < msgctnt.proplist.count) {
			if (-1 == common_util_index_proptags(pproptags,
				msgctnt.proplist.ppropval[i].proptag)) {
				common_util_remove_propvals(&msgctnt.proplist,
						msgctnt.proplist.ppropval[i].proptag);
				continue;
			}
			i ++;
		}
		if (common_util_index_proptags(pproptags, PR_MESSAGE_RECIPIENTS) == -1)
			msgctnt.children.prcpts = NULL;
		if (common_util_index_proptags(pproptags, PR_MESSAGE_ATTACHMENTS) == -1)
			msgctnt.children.pattachments = NULL;
		if (0 != level) {
			msgctnt.children.prcpts = NULL;
			msgctnt.children.pattachments = NULL;
		}
		if (!pctx->make_messagecontent(&msgctnt))
			return ecError;
		break;
	case OBJECT_TYPE_ATTACHMENT:
		if (!static_cast<ATTACHMENT_OBJECT *>(pobject)->flush_streams())
			return ecError;
		if (!exmdb_client_read_attachment_instance(plogon->get_dir(),
		    static_cast<ATTACHMENT_OBJECT *>(pobject)->get_instance_id(), &attctnt))
			return ecError;
		i = 0;
		while (i < attctnt.proplist.count) {
			if (-1 == common_util_index_proptags(pproptags,
				attctnt.proplist.ppropval[i].proptag)) {
				common_util_remove_propvals(&attctnt.proplist,
						attctnt.proplist.ppropval[i].proptag);
				continue;
			}
			i ++;
		}
		if (common_util_index_proptags(pproptags, PR_ATTACH_DATA_OBJ) == -1)
			attctnt.pembedded = NULL;
		if (!pctx->make_attachmentcontent(&attctnt))
			return ecError;
		break;
	}
	auto hnd = rop_processor_add_object_handle(plogmap,
	           logon_id, hin, OBJECT_TYPE_FASTDOWNCTX, pctx.get());
	if (hnd < 0) {
		return ecError;
	}
	pctx.release();
	*phout = hnd;
	return ecSuccess;
}

uint32_t rop_tellversion(const uint16_t *pversion,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	return ecSuccess;
}

uint32_t rop_syncconfigure(uint8_t sync_type, uint8_t send_options,
	uint16_t sync_flags, const RESTRICTION *pres, uint32_t extra_flags,
	const PROPTAG_ARRAY *pproptags, void *plogmap, uint8_t logon_id,
	uint32_t hin, uint32_t *phout)
{
	int object_type;
	uint32_t permission;
	
	if (SYNC_TYPE_CONTENTS != sync_type &&
		SYNC_TYPE_HIERARCHY != sync_type) {
		return ecInvalidParam;
	}
	if (send_options & ~(SEND_OPTIONS_UNICODE|
		SEND_OPTIONS_USECPID|SEND_OPTIONS_RECOVERMODE|
		SEND_OPTIONS_FORCEUNICODE|SEND_OPTIONS_PARTIAL|
		SEND_OPTIONS_RESERVED1|SEND_OPTIONS_RESERVED2)) {
		return ecInvalidParam;
	}
	if ((send_options & SEND_OPTIONS_UNICODE) &&
		(send_options & SEND_OPTIONS_USECPID) &&
		(send_options & SEND_OPTIONS_RECOVERMODE)) {
		return ecInvalidParam;
	}
	if (SYNC_TYPE_HIERARCHY == sync_type && NULL != pres) {
		return ecInvalidParam;
	}
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	auto pfolder = static_cast<FOLDER_OBJECT *>(rop_processor_get_object(plogmap,
	               logon_id, hin, &object_type));
	if (NULL == pfolder) {
		return ecNullObject;
	}
	if (sync_type == SYNC_TYPE_CONTENTS) {
		if (plogon->logon_mode != LOGON_MODE_OWNER) {
			auto rpc_info = get_rpc_info();
			if (!exmdb_client_check_folder_permission(plogon->get_dir(),
			    pfolder->folder_id, rpc_info.username, &permission))
				return ecError;
			if (!(permission & (frightsOwner | frightsReadAny)))
				return ecAccessDenied;
		}
	}
	if (NULL != pres) {
		if (FALSE == common_util_convert_restriction(
			TRUE, (RESTRICTION*)pres)) {
			return ecError;
		}
	}
	auto pctx = icsdownctx_object_create(plogon, pfolder,
			sync_type, send_options, sync_flags,
			pres, extra_flags, pproptags);
	auto hnd = rop_processor_add_object_handle(plogmap,
	           logon_id, hin, OBJECT_TYPE_ICSDOWNCTX, pctx.get());
	if (hnd < 0) {
		return ecError;
	}
	pctx.release();
	*phout = hnd;
	return ecSuccess;
}

uint32_t rop_syncimportmessagechange(uint8_t import_flags,
	const TPROPVAL_ARRAY *ppropvals, uint64_t *pmessage_id,
	void *plogmap, uint8_t logon_id, uint32_t hin, uint32_t *phout)
{
	BOOL b_new;
	XID tmp_xid;
	BOOL b_exist;
	BOOL b_owner;
	void *pvalue;
	uint32_t result;
	int object_type;
	uint64_t message_id;
	uint32_t permission = rightsNone, tag_access = 0, tmp_proptag;
	PROPTAG_ARRAY proptags;
	PROBLEM_ARRAY tmp_problems;
	TPROPVAL_ARRAY tmp_propvals;
	
	if (import_flags & (~(IMPORT_FLAG_ASSOCIATED|
		IMPORT_FLAG_FAILONCONFLICT))) {
		return ecInvalidParam;
	}
	if (4 != ppropvals->count ||
	    ppropvals->ppropval[0].proptag != PR_SOURCE_KEY ||
	    ppropvals->ppropval[1].proptag != PR_LAST_MODIFICATION_TIME ||
	    ppropvals->ppropval[2].proptag != PR_CHANGE_KEY ||
	    ppropvals->ppropval[3].proptag != PR_PREDECESSOR_CHANGE_LIST)
		return ecInvalidParam;
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	auto pctx = static_cast<ICSUPCTX_OBJECT *>(rop_processor_get_object(plogmap,
	            logon_id, hin, &object_type));
	if (NULL == pctx) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_ICSUPCTX != object_type) {
		return ecNotSupported;
	}
	if (pctx->get_sync_type() != SYNC_TYPE_CONTENTS)
		return ecNotSupported;
	pctx->mark_started();
	auto pfolder = pctx->get_parent_object();
	auto folder_id = pfolder->folder_id;
	auto pbin = static_cast<BINARY *>(ppropvals->ppropval[0].pvalue);
	if (pbin == nullptr || pbin->cb != 22)
		return ecInvalidParam;
	if (FALSE == common_util_binary_to_xid(pbin, &tmp_xid)) {
		return ecError;
	}
	auto tmp_guid = plogon->guid();
	if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
		return ecInvalidParam;
	}
	message_id = rop_util_make_eid(1, tmp_xid.local_id);
	if (!exmdb_client_check_message(plogon->get_dir(), folder_id,
	    message_id, &b_exist))
		return ecError;
	*pmessage_id = message_id;
	if (plogon->logon_mode != LOGON_MODE_OWNER) {
		auto rpc_info = get_rpc_info();
		if (!exmdb_client_check_folder_permission(plogon->get_dir(),
		    folder_id, rpc_info.username, &permission))
			return ecError;
		if (FALSE == b_exist) {
			if (!(permission & frightsCreate))
				return ecAccessDenied;
			tag_access = TAG_ACCESS_READ;
			if (permission & (frightsEditAny | frightsEditOwned))
				tag_access |= TAG_ACCESS_MODIFY;	
			if (permission & (frightsDeleteAny | frightsDeleteOwned))
				tag_access |= TAG_ACCESS_DELETE;	
		} else {
			if (permission & frightsOwner) {
				tag_access = TAG_ACCESS_MODIFY|
					TAG_ACCESS_READ|TAG_ACCESS_DELETE;
			} else {
				if (!exmdb_client_check_message_owner(plogon->get_dir(),
				    message_id, rpc_info.username, &b_owner))
					return ecError;
				if (b_owner || (permission & frightsReadAny))
					tag_access |= TAG_ACCESS_READ;
				if ((permission & frightsEditAny) ||
				    (b_owner && (permission & frightsEditOwned)))
					tag_access |= TAG_ACCESS_MODIFY;	
				if ((permission & frightsDeleteAny) ||
				    (b_owner && (permission & frightsDeleteOwned)))
					tag_access |= TAG_ACCESS_DELETE;	
			}
		}
	} else {
		tag_access = TAG_ACCESS_MODIFY|TAG_ACCESS_READ|TAG_ACCESS_DELETE;
	}
	if (TRUE == b_exist) {
		if (!exmdb_client_get_message_property(plogon->get_dir(),
		    nullptr, 0, message_id, PR_ASSOCIATED, &pvalue))
			return ecError;
		if (IMPORT_FLAG_ASSOCIATED & import_flags) {
			if (NULL == pvalue || 0 == *(uint8_t*)pvalue) {
				return ecInvalidParam;
			}
		} else {
			if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
				return ecInvalidParam;
			}
		}
		b_new = FALSE;
	} else {
		b_new = TRUE;
	}
	auto pinfo = emsmdb_interface_get_emsmdb_info();
	auto pmessage = message_object_create(plogon, b_new,
		pinfo->cpid, message_id, &folder_id, tag_access,
		OPEN_MODE_FLAG_READWRITE, pctx->pstate.get());
	if (NULL == pmessage) {
		return ecError;
	}
	if (TRUE == b_exist) {
		proptags.count = 1;
		proptags.pproptag = &tmp_proptag;
		tmp_proptag = PR_PREDECESSOR_CHANGE_LIST;
		if (!pmessage->get_properties(0, &proptags, &tmp_propvals))
			return ecError;
		pvalue = common_util_get_propvals(&tmp_propvals,  PR_PREDECESSOR_CHANGE_LIST);
		if (NULL == pvalue) {
			return ecError;
		}
		if (!common_util_pcl_compare(static_cast<BINARY *>(pvalue),
		    static_cast<BINARY *>(ppropvals->ppropval[3].pvalue), &result)) {
			return ecError;
		}
		if (PCL_INCLUDE & result) {
			return SYNC_E_IGNORE;
		} else if (PCL_CONFLICT == result) {
			if (IMPORT_FLAG_FAILONCONFLICT & import_flags) {
				return SYNC_E_CONFLICT;
			}
		}
	}
	if (FALSE == b_new) {
		if (!exmdb_client_clear_message_instance(plogon->get_dir(),
		    pmessage->get_instance_id()))
			return ecError;
	} else {
		BOOL b_fai = (import_flags & IMPORT_FLAG_ASSOCIATED) ? TRUE : false;
		if (!pmessage->init_message(b_fai, pinfo->cpid))
			return ecError;
	}
	tmp_propvals.count = 3;
	tmp_propvals.ppropval = ppropvals->ppropval + 1;
	if (!exmdb_client_set_instance_properties(plogon->get_dir(),
	    pmessage->get_instance_id(), &tmp_propvals, &tmp_problems))
		return ecError;
	auto hnd = rop_processor_add_object_handle(plogmap,
	           logon_id, hin, OBJECT_TYPE_MESSAGE, pmessage.get());
	if (hnd < 0) {
		return ecError;
	}
	pmessage.release();
	*phout = hnd;
	return ecSuccess;
}

uint32_t rop_syncimportreadstatechanges(uint16_t count,
	const MESSAGE_READ_STAT *pread_stat,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	int i;
	XID tmp_xid;
	BOOL b_owner;
	void *pvalue;
	int object_type;
	uint64_t read_cn;
	uint64_t folder_id;
	uint64_t message_id;
	uint32_t permission;
	const char *username;
	uint32_t proptag_buff[2];
	PROPTAG_ARRAY tmp_proptags;
	TPROPVAL_ARRAY tmp_propvals;
	
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	auto pctx = static_cast<ICSUPCTX_OBJECT *>(rop_processor_get_object(plogmap,
	            logon_id, hin, &object_type));
	if (NULL == pctx) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_ICSUPCTX != object_type) {
		return ecNotSupported;
	}
	if (pctx->get_sync_type() != SYNC_TYPE_CONTENTS)
		return ecNotSupported;
	pctx->mark_started();
	username = NULL;
	auto rpc_info = get_rpc_info();
	if (plogon->logon_mode != LOGON_MODE_OWNER) {
		auto pfolder = pctx->get_parent_object();
		folder_id = pfolder->folder_id;
		if (!exmdb_client_check_folder_permission(plogon->get_dir(),
		    folder_id, rpc_info.username, &permission))
			return ecError;
		if (!(permission & frightsReadAny))
			username = rpc_info.username;
	}
	for (i=0; i<count; i++) {
		if (FALSE == common_util_binary_to_xid(
			&pread_stat[i].message_xid, &tmp_xid)) {
			return ecError;
		}
		auto tmp_guid = plogon->guid();
		if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
			continue;
		}
		message_id = rop_util_make_eid(1, tmp_xid.local_id);
		if (NULL != username) {
			if (!exmdb_client_check_message_owner(plogon->get_dir(),
			    message_id, username, &b_owner))
				return ecError;
			if (FALSE == b_owner) {
				continue;
			}
		}
		tmp_proptags.count = 2;
		tmp_proptags.pproptag = proptag_buff;
		proptag_buff[0] = PR_ASSOCIATED;
		proptag_buff[1] = PR_READ;
		if (!exmdb_client_get_message_properties(plogon->get_dir(),
		    nullptr, 0, message_id, &tmp_proptags, &tmp_propvals))
			return ecError;
		pvalue = common_util_get_propvals(&tmp_propvals, PR_ASSOCIATED);
		if (NULL != pvalue && 0 != *(uint8_t*)pvalue) {
			continue;
		}
		pvalue = common_util_get_propvals(&tmp_propvals, PR_READ);
		if (NULL == pvalue || 0 == *(uint8_t*)pvalue) {
			if (0 == pread_stat[i].mark_as_read) {
				continue;
			}
		} else {
			if (0 != pread_stat[i].mark_as_read) {
				continue;
			}
		}
		if (plogon->check_private()) {
			if (!exmdb_client_set_message_read_state(plogon->get_dir(),
			    nullptr, message_id, pread_stat[i].mark_as_read, &read_cn))
				return ecError;
		} else {
			if (!exmdb_client_set_message_read_state(plogon->get_dir(),
			    rpc_info.username, message_id,
			    pread_stat[i].mark_as_read, &read_cn))
				return ecError;
		}
		idset_append(pctx->pstate->pread, read_cn);
	}
	return ecSuccess;
}

uint32_t rop_syncimporthierarchychange(const TPROPVAL_ARRAY *phichyvals,
	const TPROPVAL_ARRAY *ppropvals, uint64_t *pfolder_id,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	int i;
	XID tmp_xid;
	BOOL b_exist;
	BINARY *pbin;
	BOOL b_guest;
	BOOL b_found;
	void *pvalue;
	int domain_id;
	BOOL b_partial;
	uint32_t result;
	int object_type;
	uint16_t replid;
	uint64_t tmp_fid;
	uint32_t tmp_type;
	uint64_t folder_id;
	uint64_t parent_id;
	uint64_t parent_id1;
	uint64_t change_num;
	uint32_t permission;
	uint32_t parent_type;
	PROBLEM_ARRAY tmp_problems;
	TPROPVAL_ARRAY tmp_propvals;
	
	if (6 != phichyvals->count ||
	    phichyvals->ppropval[0].proptag != PR_PARENT_SOURCE_KEY ||
	    phichyvals->ppropval[1].proptag != PR_SOURCE_KEY ||
	    phichyvals->ppropval[2].proptag != PR_LAST_MODIFICATION_TIME ||
	    phichyvals->ppropval[3].proptag != PR_CHANGE_KEY ||
	    phichyvals->ppropval[4].proptag != PR_PREDECESSOR_CHANGE_LIST ||
	    phichyvals->ppropval[5].proptag != PR_DISPLAY_NAME)
		return ecInvalidParam;
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	auto pctx = static_cast<ICSUPCTX_OBJECT *>(rop_processor_get_object(plogmap,
	            logon_id, hin, &object_type));
	if (NULL == pctx) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_ICSUPCTX != object_type) {
		return ecNotSupported;
	}
	if (pctx->get_sync_type() != SYNC_TYPE_HIERARCHY)
		return ecNotSupported;
	pctx->mark_started();
	auto pfolder = pctx->get_parent_object();
	auto rpc_info = get_rpc_info();
	if (0 == ((BINARY*)phichyvals->ppropval[0].pvalue)->cb) {
		parent_type = pfolder->type;
		parent_id1 = pfolder->folder_id;
		if (!exmdb_client_check_folder_id(plogon->get_dir(),
		    parent_id1, &b_exist))
			return ecError;
		if (FALSE == b_exist) {
			return SYNC_E_NO_PARENT;
		}
	} else {
		pbin = static_cast<BINARY *>(phichyvals->ppropval[0].pvalue);
		if (pbin == nullptr || pbin->cb != 22)
			return ecInvalidParam;
		if (FALSE == common_util_binary_to_xid(pbin, &tmp_xid)) {
			return ecError;
		}
		if (plogon->check_private()) {
			auto tmp_guid = rop_util_make_user_guid(plogon->account_id);
			if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
				return ecInvalidParam;
			}
		} else {
			auto tmp_guid = rop_util_make_domain_guid(plogon->account_id);
			if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
				return ecAccessDenied;
			}
		}
		parent_id1 = rop_util_make_eid(1, tmp_xid.local_id);
		if (!exmdb_client_get_folder_property(plogon->get_dir(), 0,
		    parent_id1, PR_FOLDER_TYPE, &pvalue))
			return ecError;
		if (NULL == pvalue) {
			return SYNC_E_NO_PARENT;
		}
		parent_type = *(uint32_t*)pvalue;
	}
	if (parent_type == FOLDER_SEARCH)
		return ecNotSupported;
	pbin = static_cast<BINARY *>(phichyvals->ppropval[1].pvalue);
	if (pbin == nullptr || pbin->cb != 22)
		return ecInvalidParam;
	if (FALSE == common_util_binary_to_xid(pbin, &tmp_xid)) {
		return ecError;
	}
	if (plogon->check_private()) {
		auto tmp_guid = rop_util_make_user_guid(plogon->account_id);
		if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
			return ecInvalidParam;
		}
		folder_id = rop_util_make_eid(1, tmp_xid.local_id);
	} else {
		auto tmp_guid = rop_util_make_domain_guid(plogon->account_id);
		if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
			domain_id = rop_util_make_domain_id(tmp_xid.guid);
			if (-1 == domain_id) {
				return ecInvalidParam;
			}
			if (!common_util_check_same_org(domain_id, plogon->account_id))
				return ecInvalidParam;
			if (!exmdb_client_get_mapping_replid(plogon->get_dir(),
			    tmp_xid.guid, &b_found, &replid))
				return ecError;
			if (FALSE == b_found) {
				return ecInvalidParam;
			}
			folder_id = rop_util_make_eid(replid, tmp_xid.local_id);
		} else {
			folder_id = rop_util_make_eid(1, tmp_xid.local_id);
		}
	}
	if (!exmdb_client_check_folder_id(plogon->get_dir(), folder_id, &b_exist))
		return ecError;
	*pfolder_id = 0;
	if (FALSE == b_exist) {
		if (plogon->logon_mode != LOGON_MODE_OWNER) {
			if (!exmdb_client_check_folder_permission(plogon->get_dir(),
			    parent_id1, rpc_info.username, &permission))
				return ecError;
			if (!(permission & frightsCreateSubfolder))
				return ecAccessDenied;
		}
		if (!exmdb_client_get_folder_by_name(plogon->get_dir(), parent_id1,
		    static_cast<char *>(phichyvals->ppropval[5].pvalue), &tmp_fid))
			return ecError;
		if (0 != tmp_fid) {
			return ecDuplicateName;
		}
		if (!exmdb_client_allocate_cn(plogon->get_dir(), &change_num))
			return ecError;
		tmp_propvals.count = 0;
		tmp_propvals.ppropval = cu_alloc<TAGGED_PROPVAL>(8 + ppropvals->count);
		if (NULL == tmp_propvals.ppropval) {
			return ecMAPIOOM;
		}
		tmp_propvals.ppropval[0].proptag = PROP_TAG_FOLDERID;
		tmp_propvals.ppropval[0].pvalue = &folder_id;
		tmp_propvals.ppropval[1].proptag = PROP_TAG_PARENTFOLDERID;
		tmp_propvals.ppropval[1].pvalue = &parent_id1;
		tmp_propvals.ppropval[2].proptag = PR_LAST_MODIFICATION_TIME;
		tmp_propvals.ppropval[2].pvalue = phichyvals->ppropval[2].pvalue;
		tmp_propvals.ppropval[3].proptag = PR_CHANGE_KEY;
		tmp_propvals.ppropval[3].pvalue = phichyvals->ppropval[3].pvalue;
		tmp_propvals.ppropval[4].proptag = PR_PREDECESSOR_CHANGE_LIST;
		tmp_propvals.ppropval[4].pvalue = phichyvals->ppropval[4].pvalue;
		tmp_propvals.ppropval[5].proptag = PR_DISPLAY_NAME;
		tmp_propvals.ppropval[5].pvalue = phichyvals->ppropval[5].pvalue;
		tmp_propvals.ppropval[6].proptag = PROP_TAG_CHANGENUMBER;
		tmp_propvals.ppropval[6].pvalue = &change_num;
		tmp_propvals.count = 7;
		for (i=0; i<ppropvals->count; i++) {
			tmp_propvals.ppropval[tmp_propvals.count] =
								ppropvals->ppropval[i];
			tmp_propvals.count ++;
		}
		if (common_util_get_propvals(&tmp_propvals, PR_FOLDER_TYPE) == nullptr) {
			tmp_type = FOLDER_GENERIC;
			tmp_propvals.ppropval[tmp_propvals.count].proptag = PR_FOLDER_TYPE;
			tmp_propvals.ppropval[tmp_propvals.count].pvalue =
													&tmp_type;
			tmp_propvals.count ++;
		}
		auto pinfo = emsmdb_interface_get_emsmdb_info();
		if (!exmdb_client_create_folder_by_properties(plogon->get_dir(),
		    pinfo->cpid, &tmp_propvals, &tmp_fid) || folder_id != tmp_fid)
			return ecError;
		idset_append(pctx->pstate->pseen, change_num);
		return ecSuccess;
	}
	if (!exmdb_client_get_folder_property(plogon->get_dir(), 0,
	    folder_id, PR_PREDECESSOR_CHANGE_LIST, &pvalue) ||
	    pvalue == nullptr)
		return ecError;
	if (!common_util_pcl_compare(static_cast<BINARY *>(pvalue),
	    static_cast<BINARY *>(phichyvals->ppropval[4].pvalue), &result))
		return ecError;
	if (PCL_INCLUDE & result) {
		return SYNC_E_IGNORE;
	}
	if (plogon->logon_mode != LOGON_MODE_OWNER) {
		if (!exmdb_client_check_folder_permission(plogon->get_dir(),
		    folder_id, rpc_info.username, &permission))
			return ecError;
		if (!(permission & frightsOwner))
			return ecAccessDenied;
	}
	if (!exmdb_client_get_folder_property(plogon->get_dir(), 0, folder_id,
	    PROP_TAG_PARENTFOLDERID, &pvalue) || pvalue == nullptr)
		return ecError;
	parent_id = *(uint64_t*)pvalue;
	if (parent_id != parent_id1) {
		/* MS-OXCFXICS 3.3.5.8.8 move folders
		within public mailbox is not supported */
		if (!plogon->check_private())
			return ecNotSupported;
		if (rop_util_get_gc_value(folder_id) < PRIVATE_FID_CUSTOM) {
			return ecAccessDenied;
		}
		if (plogon->logon_mode != LOGON_MODE_OWNER) {
			if (!exmdb_client_check_folder_permission(plogon->get_dir(),
			    parent_id1, rpc_info.username, &permission))
				return ecError;
			if (!(permission & frightsCreateSubfolder))
				return ecAccessDenied;
			b_guest = TRUE;
		} else {
			b_guest = FALSE;
		}
		auto pinfo = emsmdb_interface_get_emsmdb_info();
		if (!exmdb_client_movecopy_folder(plogon->get_dir(),
		    plogon->account_id, pinfo->cpid, b_guest, rpc_info.username,
		    parent_id, folder_id, parent_id1,
		    static_cast<char *>(phichyvals->ppropval[5].pvalue), false,
			&b_exist, &b_partial)) {
			return ecError;
		}
		if (TRUE == b_exist) {
			return ecDuplicateName;
		}
		if (TRUE == b_partial) {
			return ecError;
		}
	}
	if (!exmdb_client_allocate_cn(plogon->get_dir(), &change_num))
		return ecError;
	tmp_propvals.count = 0;
	tmp_propvals.ppropval = cu_alloc<TAGGED_PROPVAL>(5 + ppropvals->count);
	if (NULL == tmp_propvals.ppropval) {
		return ecMAPIOOM;
	}
	tmp_propvals.ppropval[0].proptag = PR_LAST_MODIFICATION_TIME;
	tmp_propvals.ppropval[0].pvalue = phichyvals->ppropval[2].pvalue;
	tmp_propvals.ppropval[1].proptag = PR_CHANGE_KEY;
	tmp_propvals.ppropval[1].pvalue = phichyvals->ppropval[3].pvalue;
	tmp_propvals.ppropval[2].proptag = PR_PREDECESSOR_CHANGE_LIST;
	tmp_propvals.ppropval[2].pvalue = phichyvals->ppropval[4].pvalue;
	tmp_propvals.ppropval[3].proptag = PR_DISPLAY_NAME;
	tmp_propvals.ppropval[3].pvalue = phichyvals->ppropval[5].pvalue;
	tmp_propvals.ppropval[4].proptag = PROP_TAG_CHANGENUMBER;
	tmp_propvals.ppropval[4].pvalue = &change_num;
	tmp_propvals.count = 5;
	for (i=0; i<ppropvals->count; i++) {
		tmp_propvals.ppropval[tmp_propvals.count] =
							ppropvals->ppropval[i];
		tmp_propvals.count ++;
	}
	auto pinfo = emsmdb_interface_get_emsmdb_info();
	if (!exmdb_client_set_folder_properties(plogon->get_dir(),
	    pinfo->cpid, folder_id, &tmp_propvals, &tmp_problems))
		return ecError;
	idset_append(pctx->pstate->pseen, change_num);
	return ecSuccess;
}

uint32_t rop_syncimportdeletes(
	uint8_t flags, const TPROPVAL_ARRAY *ppropvals,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	XID tmp_xid;
	void *pvalue;
	BOOL b_exist;
	BOOL b_found;
	uint64_t eid;
	BOOL b_owner;
	int domain_id;
	BOOL b_result;
	BOOL b_partial;
	int object_type;
	uint16_t replid;
	uint32_t permission;
	const char *username;
	EID_ARRAY message_ids;
	
	if (ppropvals->count != 1 ||
	    PROP_TYPE(ppropvals->ppropval[0].proptag) != PT_MV_BINARY)
		return ecInvalidParam;
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	auto pctx = static_cast<ICSUPCTX_OBJECT *>(rop_processor_get_object(plogmap,
	            logon_id, hin, &object_type));
	if (NULL == pctx) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_ICSUPCTX != object_type) {
		return ecNotSupported;
	}
	auto sync_type = pctx->get_sync_type();
	BOOL b_hard = (flags & SYNC_DELETES_FLAG_HARDDELETE) ? TRUE : false;
	if (SYNC_DELETES_FLAG_HIERARCHY & flags) {
		if (SYNC_TYPE_CONTENTS == sync_type) {
			return ecNotSupported;
		}
	}
	pctx->mark_started();
	auto pfolder = pctx->get_parent_object();
	auto folder_id = pfolder->folder_id;
	auto rpc_info = get_rpc_info();
	username = rpc_info.username;
	if (plogon->logon_mode == LOGON_MODE_OWNER) {
		username = NULL;
	} else if (sync_type == SYNC_TYPE_CONTENTS &&
	    !exmdb_client_check_folder_permission(plogon->get_dir(),
	    folder_id, rpc_info.username, &permission)) {
		if (permission & (frightsOwner | frightsDeleteAny))
			username = NULL;
		else if (!(permission & frightsDeleteOwned))
			return ecAccessDenied;
	}
	auto pinfo = emsmdb_interface_get_emsmdb_info();
	auto pbins = static_cast<BINARY_ARRAY *>(ppropvals->ppropval[0].pvalue);
	if (SYNC_TYPE_CONTENTS == sync_type) {
		message_ids.count = 0;
		message_ids.pids = cu_alloc<uint64_t>(pbins->count);
		if (NULL == message_ids.pids) {
			return ecMAPIOOM;
		}
	}
	for (size_t i = 0; i < pbins->count; ++i) {
		if (22 != pbins->pbin[i].cb) {
			return ecInvalidParam;
		}
		if (FALSE == common_util_binary_to_xid(
			pbins->pbin + i, &tmp_xid)) {
			return ecError;
		}
		if (plogon->check_private()) {
			auto tmp_guid = rop_util_make_user_guid(plogon->account_id);
			if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
				return ecInvalidParam;
			}
			eid = rop_util_make_eid(1, tmp_xid.local_id);
		} else {
			if (SYNC_TYPE_CONTENTS == sync_type) {
				auto tmp_guid = rop_util_make_domain_guid(plogon->account_id);
				if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
					return ecInvalidParam;
				}
				eid = rop_util_make_eid(1, tmp_xid.local_id);
			} else {
				auto tmp_guid = rop_util_make_domain_guid(plogon->account_id);
				if (0 != guid_compare(&tmp_guid, &tmp_xid.guid)) {
					domain_id = rop_util_make_domain_id(tmp_xid.guid);
					if (-1 == domain_id) {
						return ecInvalidParam;
					}
					if (!common_util_check_same_org(domain_id,
					    plogon->account_id))
						return ecInvalidParam;
					if (!exmdb_client_get_mapping_replid(plogon->get_dir(),
					    tmp_xid.guid, &b_found, &replid))
						return ecError;
					if (FALSE == b_found) {
						return ecInvalidParam;
					}
					eid = rop_util_make_eid(replid, tmp_xid.local_id);
				} else {
					eid = rop_util_make_eid(1, tmp_xid.local_id);
				}
			}
		}
		if (SYNC_TYPE_CONTENTS == sync_type) {
			if (!exmdb_client_check_message(plogon->get_dir(),
			    folder_id, eid, &b_exist))
				return ecError;
		} else if (!exmdb_client_check_folder_id(plogon->get_dir(),
		    eid, &b_exist)) {
			return ecError;
		}
		if (FALSE == b_exist) {
			continue;
		}
		if (NULL != username) {
			if (SYNC_TYPE_CONTENTS == sync_type) {
				if (!exmdb_client_check_message_owner(plogon->get_dir(),
				    eid, username, &b_owner))
					return ecError;
				if (FALSE == b_owner) {
					return ecAccessDenied;
				}
			} else if (!exmdb_client_check_folder_permission(plogon->get_dir(),
			    eid, username, &permission) && !(permission & frightsOwner)) {
				return ecAccessDenied;
			}
		}
		if (SYNC_TYPE_CONTENTS == sync_type) {
			message_ids.pids[message_ids.count] = eid;
			message_ids.count ++;
		} else {
			if (plogon->check_private()) {
				if (!exmdb_client_get_folder_property(plogon->get_dir(),
				    0, eid, PR_FOLDER_TYPE, &pvalue))
					return ecError;
				if (NULL == pvalue) {
					return ecSuccess;
				}
				if (*static_cast<uint32_t *>(pvalue) == FOLDER_SEARCH)
					goto DELETE_FOLDER;
			}
			if (!exmdb_client_empty_folder(plogon->get_dir(),
			    pinfo->cpid, username, eid, b_hard, TRUE, TRUE,
			    TRUE, &b_partial) || b_partial)
				return ecError;
 DELETE_FOLDER:
			if (!exmdb_client_delete_folder(plogon->get_dir(),
			    pinfo->cpid, eid, b_hard, &b_result) || !b_result)
				return ecError;
		}
	}
	if (sync_type == SYNC_TYPE_CONTENTS && message_ids.count > 0 &&
	    (!exmdb_client_delete_messages(plogon->get_dir(),
	    plogon->account_id, pinfo->cpid, nullptr, folder_id,
	    &message_ids, b_hard, &b_partial) || b_partial))
		return ecError;
	return ecSuccess;
}

uint32_t rop_syncimportmessagemove(
	const BINARY *psrc_folder_id, const BINARY *psrc_message_id,
	const BINARY *pchange_list, const BINARY *pdst_message_id,
	const BINARY *pchange_number, uint64_t *pmessage_id,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	XID xid_src;
	XID xid_dst;
	XID xid_fsrc;
	void *pvalue;
	BOOL b_exist;
	BOOL b_owner;
	BOOL b_result;
	uint32_t result;
	int object_type;
	uint64_t src_fid;
	uint64_t src_mid;
	uint64_t dst_mid;
	uint32_t permission;
	TAGGED_PROPVAL tmp_propval;
	
	if (22 != psrc_folder_id->cb ||
		22 != psrc_message_id->cb ||
		22 != pdst_message_id->cb) {
		return ecInvalidParam;
	}
	if (pchange_number->cb < 17 || pchange_number->cb > 24) {
		return ecInvalidParam;
	}
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	auto pctx = static_cast<ICSUPCTX_OBJECT *>(rop_processor_get_object(plogmap,
	            logon_id, hin, &object_type));
	if (NULL == pctx) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_ICSUPCTX != object_type) {
		return ecNotSupported;
	}
	if (pctx->get_sync_type() != SYNC_TYPE_CONTENTS)
		return ecNotSupported;
	pctx->mark_started();
	auto pfolder = pctx->get_parent_object();
	auto folder_id = pfolder->folder_id;
	if (FALSE == common_util_binary_to_xid(
		psrc_folder_id, &xid_fsrc) ||
		FALSE == common_util_binary_to_xid(
		psrc_message_id, &xid_src) ||
		FALSE == common_util_binary_to_xid(
		pdst_message_id, &xid_dst)) {
		return ecError;
	}
	auto tmp_guid = plogon->guid();
	if (0 != guid_compare(&tmp_guid, &xid_fsrc.guid) ||
		0 != guid_compare(&tmp_guid, &xid_src.guid) ||
		0 != guid_compare(&tmp_guid, &xid_dst.guid)) {
		return ecInvalidParam;
	}
	src_fid = rop_util_make_eid(1, xid_fsrc.local_id);
	src_mid = rop_util_make_eid(1, xid_src.local_id);
	dst_mid = rop_util_make_eid(1, xid_dst.local_id);
	if (!exmdb_client_check_message(plogon->get_dir(),
	    src_fid, src_mid, &b_exist))
		return ecError;
	if (FALSE == b_exist) {
		return ecNotFound;
	}
	auto rpc_info = get_rpc_info();
	if (plogon->logon_mode != LOGON_MODE_OWNER) {
		if (!exmdb_client_check_folder_permission(plogon->get_dir(),
		    src_fid, rpc_info.username, &permission))
			return ecError;
		if (permission & frightsDeleteAny) {
			/* do nothing */
		} else if (permission & frightsDeleteOwned) {
			if (!exmdb_client_check_message_owner(plogon->get_dir(),
			    src_mid, rpc_info.username, &b_owner))
				return ecError;
			if (FALSE == b_owner) {
				return ecAccessDenied;
			}
		} else {
			return ecAccessDenied;
		}
		if (!exmdb_client_check_folder_permission(plogon->get_dir(),
		    folder_id, rpc_info.username, &permission))
			return ecError;
		if (!(permission & frightsCreate))
			return ecAccessDenied;
	}
	if (!exmdb_client_get_message_property(plogon->get_dir(), nullptr, 0,
	    src_mid, PR_ASSOCIATED, &pvalue))
		return ecError;
	if (NULL == pvalue) {
		return ecNotFound;
	}
	BOOL b_fai = *static_cast<uint8_t *>(pvalue) != 0 ? TRUE : false;
	if (!exmdb_client_get_message_property(plogon->get_dir(),
	    nullptr, 0, src_mid, PR_PREDECESSOR_CHANGE_LIST, &pvalue))
		return ecError;
	if (NULL == pvalue) {
		return ecError;
	}
	if (!common_util_pcl_compare(static_cast<BINARY *>(pvalue), pchange_list, &result))
		return ecError;
	BOOL b_newer = result == PCL_INCLUDED ? TRUE : false;
	auto pinfo = emsmdb_interface_get_emsmdb_info();
	if (!exmdb_client_movecopy_message(plogon->get_dir(),
	    plogon->account_id, pinfo->cpid, src_mid, folder_id, dst_mid,
	    TRUE, &b_result) || !b_result)
		return ecError;
	if (TRUE == b_newer) {
		uint32_t result_unused;
		tmp_propval.proptag = PR_PREDECESSOR_CHANGE_LIST;
		tmp_propval.pvalue = pvalue;
		exmdb_client_set_message_property(plogon->get_dir(), nullptr,
			0, dst_mid, &tmp_propval, &result_unused);
	}
	if (!exmdb_client_get_message_property(plogon->get_dir(), nullptr, 0,
	    dst_mid, PROP_TAG_CHANGENUMBER, &pvalue) || pvalue == nullptr)
		return ecError;
	idset_append(b_fai ? pctx->pstate->pseen_fai : pctx->pstate->pseen,
	             *static_cast<uint64_t *>(pvalue));
	idset_append(pctx->pstate->pgiven, dst_mid);
	*pmessage_id = 0;
	if (TRUE == b_newer) {
		return SYNC_W_CLIENT_CHANGE_NEWER;
	}
	return ecSuccess;
}

uint32_t rop_syncopencollector(uint8_t is_content_collector,
	void *plogmap, uint8_t logon_id, uint32_t hin, uint32_t *phout)
{
	int object_type;
	
	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	auto pfolder = static_cast<FOLDER_OBJECT *>(rop_processor_get_object(plogmap,
	               logon_id, hin, &object_type));
	if (NULL == pfolder) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_FOLDER != object_type) {
		return ecNotSupported;
	}
	uint8_t sync_type = is_content_collector == 0 ? SYNC_TYPE_HIERARCHY : SYNC_TYPE_CONTENTS;
	auto pctx = icsupctx_object_create(plogon, pfolder, sync_type);
	auto hnd = rop_processor_add_object_handle(plogmap,
	           logon_id, hin, OBJECT_TYPE_ICSUPCTX, pctx.get());
	if (hnd < 0) {
		return ecError;
	}
	pctx.release();
	*phout = hnd;
	return ecSuccess;
}

uint32_t rop_syncgettransferstate(void *plogmap,
	uint8_t logon_id, uint32_t hin, uint32_t *phout)
{
	void *pobject;
	int object_type;
	ICS_STATE *pstate;

	auto plogon = rop_processor_get_logon_object(plogmap, logon_id);
	if (NULL == plogon) {
		return ecError;
	}
	pobject = rop_processor_get_object(plogmap,
					logon_id, hin, &object_type);
	if (NULL == pobject) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_ICSDOWNCTX == object_type) {
		pstate = static_cast<ICSDOWNCTX_OBJECT *>(pobject)->get_state();
	} else if (OBJECT_TYPE_ICSUPCTX == object_type) {
		pstate = static_cast<ICSUPCTX_OBJECT *>(pobject)->get_state();
	} else {
		return ecNotSupported;
	}
	if (NULL == pstate) {
		return ecError;
	}
	auto pctx = fastdownctx_object_create(plogon, 0);
	if (NULL == pctx) {
		return ecError;
	}
	if (!pctx->make_state(pstate))
		return ecError;
	auto hnd = rop_processor_add_object_handle(plogmap,
	           logon_id, hin, OBJECT_TYPE_FASTDOWNCTX, pctx.get());
	if (hnd < 0) {
		return ecError;
	}
	pctx.release();
	*phout = hnd;
	return ecSuccess;
}

uint32_t rop_syncuploadstatestreambegin(uint32_t proptag_state,
	uint32_t buffer_size, void *plogmap, uint8_t logon_id, uint32_t hin)
{
	void *pctx;
	int object_type;
	
	pctx = rop_processor_get_object(plogmap,
				logon_id, hin, &object_type);
	if (NULL == pctx) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_ICSDOWNCTX == object_type) {
		if (!static_cast<ICSDOWNCTX_OBJECT *>(pctx)->begin_state_stream(proptag_state))
			return ecError;
	} else if (OBJECT_TYPE_ICSUPCTX == object_type) {
		if (!static_cast<ICSUPCTX_OBJECT *>(pctx)->begin_state_stream(proptag_state))
			return ecError;
	} else {
		return ecNotSupported;
	}
	return ecSuccess;
}

uint32_t rop_syncuploadstatestreamcontinue(const BINARY *pstream_data,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	void *pctx;
	int object_type;
	
	pctx = rop_processor_get_object(plogmap,
				logon_id, hin, &object_type);
	if (NULL == pctx) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_ICSDOWNCTX == object_type) {
		if (!static_cast<ICSDOWNCTX_OBJECT *>(pctx)->continue_state_stream(pstream_data))
			return ecError;
	} else if (OBJECT_TYPE_ICSUPCTX == object_type) {
		if (!static_cast<ICSUPCTX_OBJECT *>(pctx)->continue_state_stream(pstream_data))
			return ecError;
	} else {
		return ecNotSupported;
	}
	return ecSuccess;
}

uint32_t rop_syncuploadstatestreamend(void *plogmap,
	uint8_t logon_id, uint32_t hin)
{
	void *pctx;
	int object_type;
	
	pctx = rop_processor_get_object(plogmap,
				logon_id, hin, &object_type);
	if (NULL == pctx) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_ICSDOWNCTX == object_type) {
		if (!static_cast<ICSDOWNCTX_OBJECT *>(pctx)->end_state_stream())
			return ecError;
	} else if (OBJECT_TYPE_ICSUPCTX == object_type) {
		if (!static_cast<ICSUPCTX_OBJECT *>(pctx)->end_state_stream())
			return ecError;
	} else {
		return ecNotSupported;
	}
	return ecSuccess;
}

uint32_t rop_setlocalreplicamidsetdeleted(uint32_t count,
	const LONG_TERM_ID_RANGE *prange, void *plogmap,
	uint8_t logon_id, uint32_t hin)
{
	return ecSuccess;
}

uint32_t rop_getlocalreplicaids(uint32_t count,
	GUID *pguid, uint8_t *pglobal_count,
	void *plogmap, uint8_t logon_id, uint32_t hin)
{
	int object_type;
	uint64_t begin_eid;
	
	auto plogon = static_cast<LOGON_OBJECT *>(rop_processor_get_object(plogmap,
	              logon_id, hin, &object_type));
	if (NULL == plogon) {
		return ecNullObject;
	}
	if (OBJECT_TYPE_LOGON != object_type) {
		return ecError;
	}
	if (!exmdb_client_allocate_ids(plogon->get_dir(), count, &begin_eid))
		return ecError;
	/* allocate too many eids within an interval */
	if (0 == begin_eid) {
		return ecError;
	}
	*pguid = plogon->guid();
	rop_util_get_gc_array(begin_eid, pglobal_count);
	return ecSuccess;
}
