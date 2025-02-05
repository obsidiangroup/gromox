// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cstdint>
#include <memory>
#include "icsdownctx_object.h"
#include "emsmdb_interface.h"
#include <gromox/tpropval_array.hpp>
#include <gromox/proptag_array.hpp>
#include "exmdb_client.h"
#include <gromox/proc_common.h>
#include "common_util.h"
#include <gromox/restriction.hpp>
#include <gromox/ext_buffer.hpp>
#include "ics_state.h"
#include <gromox/eid_array.hpp>
#include <gromox/rop_util.hpp>
#include <gromox/idset.hpp>
#include <cstdlib>
#include <cstring>

enum {
	FUNC_ID_UINT32,
	FUNC_ID_PROGRESSTOTAL,
	FUNC_ID_UPDATED_MESSAGE,
	FUNC_ID_NEW_MESSAGE,
	FUNC_ID_DELETIONS,
	FUNC_ID_READSTATECHANGES,
	FUNC_ID_STATE
};

namespace {

struct ICS_FLOW_NODE {
	DOUBLE_LIST_NODE node;
	uint8_t func_id;
	void *pparam;
};

struct ics_group_node {
	DOUBLE_LIST_NODE node;
	uint32_t group_id;
};

}

#define MAX_PARTIAL_ON_ROP		100	/* for limit of memory accumulation */

static BOOL icsdownctx_object_record_flow_node(
	DOUBLE_LIST *pflow_list, int func_id, void *pparam)
{
	auto pflow = me_alloc<ICS_FLOW_NODE>();
	if (NULL == pflow) {
		return FALSE;
	}
	pflow->node.pdata = pflow;
	pflow->func_id = func_id;
	pflow->pparam = pparam;
	double_list_append_as_tail(pflow_list, &pflow->node);
	return TRUE;
}

std::unique_ptr<ICSDOWNCTX_OBJECT> icsdownctx_object_create(LOGON_OBJECT *plogon,
	FOLDER_OBJECT *pfolder, uint8_t sync_type, uint8_t send_options,
	uint16_t sync_flags, const RESTRICTION *prestriction,
	uint32_t extra_flags, const PROPTAG_ARRAY *pproptags)
{
	int state_type = sync_type == SYNC_TYPE_CONTENTS ? ICS_STATE_CONTENTS_DOWN : ICS_STATE_HIERARCHY_DOWN;
	std::unique_ptr<ICSDOWNCTX_OBJECT> pctx;
	try {
		pctx = std::make_unique<ICSDOWNCTX_OBJECT>();
	} catch (const std::bad_alloc &) {
		fprintf(stderr, "E-1454: ENOMEM\n");
		return NULL;
	}
	pctx->pstate = ics_state_create(plogon, state_type);
	if (NULL == pctx->pstate) {
		return NULL;
	}
	pctx->pfolder = pfolder;
	pctx->sync_type = sync_type;
	pctx->send_options = send_options;
	pctx->sync_flags = sync_flags;
	pctx->extra_flags = extra_flags;
	pctx->pproptags = proptag_array_dup(pproptags);
	if (NULL == pctx->pproptags) {
		return NULL;
	}
	if (NULL != prestriction) {
		pctx->prestriction = restriction_dup(prestriction);
		if (NULL == pctx->prestriction) {
			return NULL;
		}
	} else {
		pctx->prestriction = NULL;
	}
	pctx->pstream = ftstream_producer_create(
				plogon, send_options & 0x0F);
	if (NULL == pctx->pstream) {
		return NULL;
	}
	double_list_init(&pctx->flow_list);
	double_list_init(&pctx->group_list);
	pctx->pprogtotal = NULL;
	pctx->pmessages = NULL;
	pctx->pread_messags = NULL;
	pctx->punread_messags = NULL;
	pctx->pdeleted_messages = NULL;
	pctx->pnolonger_messages = NULL;
	pctx->state_property = 0;
	pctx->b_started = FALSE;
	return pctx;
}

static BOOL icsdownctx_object_make_content(ICSDOWNCTX_OBJECT *pctx)
{
	uint32_t count_fai;
	uint64_t total_fai;
	DCERPC_INFO rpc_info;
	const char *username;
	uint64_t total_normal;
	uint32_t count_normal;
	EID_ARRAY chg_messages;
	EID_ARRAY read_messags;
	EID_ARRAY given_messages;
	EID_ARRAY unread_messags;
	EID_ARRAY updated_messages;
	EID_ARRAY deleted_messages;
	EID_ARRAY nolonger_messages;
	
	
	if (SYNC_TYPE_CONTENTS != pctx->sync_type) {
		return FALSE;
	}
	
	if (pctx->sync_flags & SYNC_FLAG_PROGRESS) {
		pctx->pprogtotal = me_alloc<PROGRESS_INFORMATION>();
		if (NULL == pctx->pprogtotal) {
			return FALSE;
		}
	}
	auto pread     = (pctx->sync_flags & SYNC_FLAG_READSTATE) ? pctx->pstate->pread : nullptr;
	auto pseen_fai = (pctx->sync_flags & SYNC_FLAG_FAI) ? pctx->pstate->pseen_fai : nullptr;
	auto pseen     = (pctx->sync_flags & SYNC_FLAG_NORMAL) ? pctx->pstate->pseen : nullptr;
	BOOL b_ordered = (pctx->extra_flags & SYNC_EXTRA_FLAG_ORDERBYDELIVERYTIME) ? TRUE : false;
	if (!pctx->pstream->plogon->check_private()) {
		rpc_info = get_rpc_info();
		username = rpc_info.username;
	} else {
		username = NULL;
	}
	auto pinfo = emsmdb_interface_get_emsmdb_info();
	if (!exmdb_client_get_content_sync(pctx->pstream->plogon->get_dir(),
	    pctx->pfolder->folder_id, username,
	    pctx->pstate->pgiven, pseen, pseen_fai, pread,
	    pinfo->cpid, pctx->prestriction, b_ordered,
	    &count_fai, &total_fai, &count_normal, &total_normal,
	    &updated_messages, &chg_messages, &pctx->last_changenum,
	    &given_messages, &deleted_messages, &nolonger_messages,
	    &read_messags, &unread_messags, &pctx->last_readcn))
		return FALSE;
	
	idset_clear(pctx->pstate->pgiven);
	for (size_t i = 0; i < given_messages.count; ++i) {
		if (FALSE == idset_append(pctx->pstate->pgiven,
			given_messages.pids[i])) {
			return FALSE;	
		}
	}
	
	if ((pctx->sync_flags & SYNC_FLAG_FAI) ||
		(pctx->sync_flags & SYNC_FLAG_NORMAL)) {
		pctx->pmessages = eid_array_dup(&chg_messages);
		if (NULL == pctx->pmessages) {
			return FALSE;
		}
	}
	
	if (SYNC_FLAG_PROGRESS & pctx->sync_flags) {
		pctx->pprogtotal->version = 0;
		pctx->pprogtotal->padding1 = 0;
		pctx->pprogtotal->fai_count = count_fai;
		pctx->pprogtotal->fai_size = total_fai;
		pctx->pprogtotal->normal_count = count_normal;
		pctx->pprogtotal->padding2 = 0;
		pctx->pprogtotal->normal_size = total_normal;
	}
	
	if (0 == (pctx->sync_flags & SYNC_FLAG_NODELETIONS)) {
		pctx->pdeleted_messages = eid_array_dup(&deleted_messages);
		if (NULL == pctx->pdeleted_messages) {
			return FALSE;
		}
		pctx->pnolonger_messages = eid_array_dup(&nolonger_messages);
		if (NULL == pctx->pnolonger_messages) {
			return FALSE;
		}
	}
	
	if (pctx->sync_flags & SYNC_FLAG_READSTATE) {
		pctx->pread_messags = eid_array_dup(&read_messags);
		if (NULL == pctx->pread_messags) {
			return FALSE;
		}
		pctx->punread_messags = eid_array_dup(&unread_messags);
		if (NULL == pctx->punread_messags) {
			return FALSE;
		}
	}
	
	pctx->fake_gpinfo.group_id = 0xFFFFFFFF;
	pctx->fake_gpinfo.reserved = 0;
	pctx->fake_gpinfo.count = 0;
	pctx->fake_gpinfo.pgroups = NULL;
	
	if (SYNC_FLAG_PROGRESS & pctx->sync_flags) {
		if (FALSE == icsdownctx_object_record_flow_node(
			&pctx->flow_list, FUNC_ID_PROGRESSTOTAL, NULL)) {
			return FALSE;
		}
	}
	
	if ((pctx->sync_flags & SYNC_FLAG_FAI) ||
		(pctx->sync_flags & SYNC_FLAG_NORMAL)) {
		for (size_t i = 0; i < pctx->pmessages->count; ++i) {
			size_t j;
			for (j=0; j<updated_messages.count; j++) {
				if (updated_messages.pids[j] == pctx->pmessages->pids[i]) {
					break;
				}
			}
			if (j < updated_messages.count) {
				if (FALSE == icsdownctx_object_record_flow_node(
					&pctx->flow_list, FUNC_ID_UPDATED_MESSAGE,
					pctx->pmessages->pids + i)) {
					return FALSE;	
				}
			} else {
				if (FALSE == icsdownctx_object_record_flow_node(
					&pctx->flow_list, FUNC_ID_NEW_MESSAGE,
					pctx->pmessages->pids + i)) {
					return FALSE;	
				}
			}
		}
	}
	
	if (0 == (pctx->sync_flags & SYNC_FLAG_NODELETIONS)) {
		if (FALSE == icsdownctx_object_record_flow_node(
			&pctx->flow_list, FUNC_ID_DELETIONS, NULL)) {
			return FALSE;
		}
	}
	
	if (pctx->sync_flags & SYNC_FLAG_READSTATE) {
		if (FALSE == icsdownctx_object_record_flow_node(
			&pctx->flow_list, FUNC_ID_READSTATECHANGES, NULL)) {
			return FALSE;
		}
	}
	
	if (FALSE == icsdownctx_object_record_flow_node(
		&pctx->flow_list, FUNC_ID_STATE, NULL)) {
		return FALSE;
	}
	
	if (FALSE == icsdownctx_object_record_flow_node(
		&pctx->flow_list, FUNC_ID_UINT32, (void*)INCRSYNCEND)) {
		return FALSE;	
	}
	
	pctx->progress_steps = 0;
	pctx->next_progress_steps = 0;
	pctx->total_steps = total_normal + total_fai;
	size_t i;
	for (i=0; i<64; i++) {
		if ((pctx->total_steps >> i) <= 0xFFFF) {
			break;
		}
	}
	pctx->ratio = 1ULL << i;
	return TRUE;
}

static void icsdownctx_object_adjust_fldchgs(FOLDER_CHANGES *pfldchgs,
	const PROPTAG_ARRAY *pproptags, BOOL b_exclude)
{
	if (TRUE == b_exclude) {
		for (size_t i = 0; i < pfldchgs->count; ++i) {
			for (size_t j = 0; j < pproptags->count; ++j) {
				common_util_remove_propvals(
					pfldchgs->pfldchgs + i,
					pproptags->pproptag[j]);
			}
		}
	} else {
		for (size_t i = 0; i < pfldchgs->count; ++i) {
			size_t j = 0;
			while (j < pfldchgs->pfldchgs[i].count) {
				if (!proptag_array_check(pproptags,
				    pfldchgs->pfldchgs[i].ppropval[j].proptag)) {
					common_util_remove_propvals(pfldchgs->pfldchgs + i,
							pfldchgs->pfldchgs[i].ppropval[j].proptag);
					continue;
				}
				j ++;
			}
		}
	}
}

static BOOL icsdownctx_object_make_hierarchy(ICSDOWNCTX_OBJECT *pctx)
{
	BINARY *pbin = nullptr;
	IDSET *pidset;
	BINARY tmp_bin;
	EXT_PUSH ext_push;
	uint64_t folder_id;
	uint64_t parent_fid;
	const char *username;
	DCERPC_INFO rpc_info;
	char temp_buff[1024];
	FOLDER_CHANGES fldchgs;
	EID_ARRAY given_folders;
	uint64_t last_changenum;
	TAGGED_PROPVAL *ppropval;
	EID_ARRAY deleted_folders;
	PERSISTDATA *ppersistdata;
	TAGGED_PROPVAL tmp_propval;
	TPROPVAL_ARRAY tmp_proplist;
	static constexpr uint8_t fake_byte = 0;
	PERSISTDATA_ARRAY persistdatas;
	TPROPVAL_ARRAY *pproplist_deletions;
	
	if (SYNC_TYPE_HIERARCHY != pctx->sync_type) {
		return FALSE;
	}
	if (pctx->pstream->plogon->logon_mode == LOGON_MODE_OWNER) {
		username = NULL;
	} else {
		rpc_info = get_rpc_info();
		username = rpc_info.username;
	}
	if (!exmdb_client_get_hierarchy_sync(pctx->pstream->plogon->get_dir(),
	    pctx->pfolder->folder_id, username, pctx->pstate->pgiven,
	    pctx->pstate->pseen, &fldchgs, &last_changenum, &given_folders,
	    &deleted_folders))
		return FALSE;
	idset_clear(pctx->pstate->pgiven);
	for (size_t i = 0; i < given_folders.count; ++i) {
		if (FALSE == idset_append(pctx->pstate->pgiven,
			given_folders.pids[i])) {
			return FALSE;	
		}
	}
	for (size_t i = 0; i < fldchgs.count; ++i) {
		static constexpr uint32_t tags[] = {
			PROP_TAG_FOLDERPATHNAME, PR_NORMAL_MESSAGE_SIZE,
			PROP_TAG_NORMALMESSAGESIZEEXTENDED, PR_MESSAGE_SIZE_EXTENDED,
			PR_ASSOC_MESSAGE_SIZE, PR_ASSOC_MESSAGE_SIZE_EXTENDED,
			PROP_TAG_FOLDERCHILDCOUNT, PR_DELETED_FOLDER_COUNT,
			PROP_TAG_ARTICLENUMBERNEXT, PROP_TAG_FOLDERFLAGS,
		};
		for (auto t : tags)
			common_util_remove_propvals(&fldchgs.pfldchgs[i], t);
		if (NULL == common_util_get_propvals(
			fldchgs.pfldchgs + i, PROP_TAG_ATTRIBUTEHIDDEN)) {
			tmp_propval.proptag = PROP_TAG_ATTRIBUTEHIDDEN;
			tmp_propval.pvalue = deconst(&fake_byte);
			common_util_set_propvals(fldchgs.pfldchgs + i, &tmp_propval);
		}
		if (NULL == common_util_get_propvals(
			fldchgs.pfldchgs + i, PROP_TAG_ATTRIBUTESYSTEM)) {
			tmp_propval.proptag = PROP_TAG_ATTRIBUTESYSTEM;
			tmp_propval.pvalue = deconst(&fake_byte);
			common_util_set_propvals(fldchgs.pfldchgs + i, &tmp_propval);
		}
		if (NULL == common_util_get_propvals(
			fldchgs.pfldchgs + i, PROP_TAG_ATTRIBUTEREADONLY)) {
			tmp_propval.proptag = PROP_TAG_ATTRIBUTEREADONLY;
			tmp_propval.pvalue = deconst(&fake_byte);
			common_util_set_propvals(fldchgs.pfldchgs + i, &tmp_propval);
		}
		if (NULL == common_util_get_propvals(
			fldchgs.pfldchgs + i, PROP_TAG_CREATORSID)) {
			tmp_propval.proptag = PROP_TAG_CREATORSID;
			tmp_propval.pvalue = &tmp_bin;
			tmp_bin.cb = 0;
			tmp_bin.pb = NULL;
			common_util_set_propvals(fldchgs.pfldchgs + i, &tmp_propval);
		}
		auto pvalue = common_util_get_propvals(&fldchgs.pfldchgs[i], PROP_TAG_FOLDERID);
		if (NULL == pvalue) {
			return FALSE;
		}
		folder_id = *(uint64_t*)pvalue;
		if (0 == (SYNC_EXTRA_FLAG_EID & pctx->extra_flags)) {
			common_util_remove_propvals(
				fldchgs.pfldchgs + i,
				PROP_TAG_FOLDERID);
		}
		pvalue = common_util_get_propvals(
					fldchgs.pfldchgs + i,
					PROP_TAG_PARENTFOLDERID);
		if (NULL == pvalue) {
			return FALSE;
		}
		parent_fid = *(uint64_t*)pvalue;
		if (SYNC_FLAG_NOFOREIGNIDENTIFIERS & pctx->sync_flags) {
			common_util_remove_propvals(&fldchgs.pfldchgs[i], PR_SOURCE_KEY);
			tmp_propval.proptag = PR_SOURCE_KEY;
			tmp_propval.pvalue =
				common_util_calculate_folder_sourcekey(
				pctx->pstream->plogon, folder_id);
			if (NULL == tmp_propval.pvalue) {
				return FALSE;
			}
			common_util_set_propvals(fldchgs.pfldchgs + i, &tmp_propval);
			if (pctx->pfolder->folder_id == parent_fid) {
				tmp_propval.proptag = PR_PARENT_SOURCE_KEY;
				tmp_propval.pvalue = &tmp_bin;
				tmp_bin.cb = 0;
				tmp_bin.pb = NULL;
			} else {
				tmp_propval.proptag = PR_PARENT_SOURCE_KEY;
				tmp_propval.pvalue =
					common_util_calculate_folder_sourcekey(
					pctx->pstream->plogon, parent_fid);
				if (NULL == tmp_propval.pvalue) {
					return FALSE;
				}
			}
			common_util_set_propvals(fldchgs.pfldchgs + i, &tmp_propval);
		} else {
			if (common_util_get_propvals(&fldchgs.pfldchgs[i], PR_SOURCE_KEY) == nullptr) {
				tmp_propval.proptag = PR_SOURCE_KEY;
				tmp_propval.pvalue =
					common_util_calculate_folder_sourcekey(
					pctx->pstream->plogon, folder_id);
				if (NULL == tmp_propval.pvalue) {
					return FALSE;
				}
				common_util_set_propvals(fldchgs.pfldchgs + i, &tmp_propval);
			}
			if (pctx->pfolder->folder_id == parent_fid) {
				tmp_propval.proptag = PR_PARENT_SOURCE_KEY;
				tmp_propval.pvalue = &tmp_bin;
				tmp_bin.cb = 0;
				tmp_bin.pb = NULL;
			} else {
				if (!exmdb_client_get_folder_property(pctx->pstream->plogon->get_dir(),
				    0, parent_fid, PR_SOURCE_KEY, &pvalue))
					return FALSE;	
				if (NULL == pvalue) {
					tmp_propval.proptag = PR_PARENT_SOURCE_KEY;
					tmp_propval.pvalue =
						common_util_calculate_folder_sourcekey(
						pctx->pstream->plogon, parent_fid);
					if (NULL == tmp_propval.pvalue) {
						return FALSE;
					}
				} else {
					tmp_propval.proptag = PR_PARENT_SOURCE_KEY;
					tmp_propval.pvalue = pvalue;
				}
			}
			common_util_set_propvals(fldchgs.pfldchgs + i, &tmp_propval);
		}
		if (pctx->pstream->plogon->check_private() &&
		    (folder_id == rop_util_make_eid_ex(1, PRIVATE_FID_ROOT) ||
		    folder_id == rop_util_make_eid_ex(1, PRIVATE_FID_INBOX))) {
			ppropval = cu_alloc<TAGGED_PROPVAL>(fldchgs.pfldchgs[i].count + 10);
			if (NULL == ppropval) {
				return FALSE;
			}
			memcpy(ppropval, fldchgs.pfldchgs[i].ppropval,
				sizeof(TAGGED_PROPVAL)*fldchgs.pfldchgs[i].count);
			fldchgs.pfldchgs[i].ppropval = ppropval;
			tmp_propval.proptag = PR_IPM_DRAFTS_ENTRYID;
			tmp_propval.pvalue = common_util_to_folder_entryid(
				pctx->pstream->plogon, rop_util_make_eid_ex(1,
				PRIVATE_FID_DRAFT));
			if (NULL == tmp_propval.pvalue) {
				return FALSE;
			}
			common_util_set_propvals(fldchgs.pfldchgs + i, &tmp_propval);
			tmp_propval.proptag = PR_IPM_CONTACT_ENTRYID;
			tmp_propval.pvalue = common_util_to_folder_entryid(
				pctx->pstream->plogon, rop_util_make_eid_ex(1,
				PRIVATE_FID_CONTACTS));
			if (NULL == tmp_propval.pvalue) {
				return FALSE;
			}
			common_util_set_propvals(fldchgs.pfldchgs + i, &tmp_propval);
			tmp_propval.proptag = PR_IPM_APPOINTMENT_ENTRYID;
			tmp_propval.pvalue = common_util_to_folder_entryid(
				pctx->pstream->plogon, rop_util_make_eid_ex(1,
				PRIVATE_FID_CALENDAR));
			if (NULL == tmp_propval.pvalue) {
				return FALSE;
			}
			common_util_set_propvals(fldchgs.pfldchgs + i, &tmp_propval);
			tmp_propval.proptag = PR_IPM_JOURNAL_ENTRYID;
			tmp_propval.pvalue = common_util_to_folder_entryid(
				pctx->pstream->plogon, rop_util_make_eid_ex(1,
				PRIVATE_FID_JOURNAL));
			if (NULL == tmp_propval.pvalue) {
				return FALSE;
			}
			common_util_set_propvals(fldchgs.pfldchgs + i, &tmp_propval);
			tmp_propval.proptag = PR_IPM_NOTE_ENTRYID;
			tmp_propval.pvalue = common_util_to_folder_entryid(
				pctx->pstream->plogon, rop_util_make_eid_ex(1,
				PRIVATE_FID_NOTES));
			if (NULL == tmp_propval.pvalue) {
				return FALSE;
			}
			common_util_set_propvals(fldchgs.pfldchgs + i, &tmp_propval);
			tmp_propval.proptag = PR_IPM_TASK_ENTRYID;
			tmp_propval.pvalue = common_util_to_folder_entryid(
				pctx->pstream->plogon, rop_util_make_eid_ex(1,
				PRIVATE_FID_TASKS));
			if (NULL == tmp_propval.pvalue) {
				return FALSE;
			}
			common_util_set_propvals(fldchgs.pfldchgs + i, &tmp_propval);
			if (common_util_get_propvals(&fldchgs.pfldchgs[i], PR_ADDITIONAL_REN_ENTRYIDS) == nullptr) {
				tmp_propval.proptag = PR_ADDITIONAL_REN_ENTRYIDS;
				pvalue = cu_alloc<BINARY_ARRAY>();
				auto ba = static_cast<BINARY_ARRAY *>(pvalue);
				if (NULL == pvalue) {
					return FALSE;
				}
				tmp_propval.pvalue = pvalue;
				ba->count = 5;
				ba->pbin = cu_alloc<BINARY>(ba->count);
				if (ba->pbin == nullptr) {
					return FALSE;
				}
				pbin = common_util_to_folder_entryid(pctx->pstream->plogon,
							rop_util_make_eid_ex(1, PRIVATE_FID_CONFLICTS));
				if (NULL == pbin) {
					return FALSE;
				}
				ba->pbin[0] = *pbin;
				pbin = common_util_to_folder_entryid(pctx->pstream->plogon,
						rop_util_make_eid_ex(1, PRIVATE_FID_SYNC_ISSUES));
				if (NULL == pbin) {
					return FALSE;
				}
				ba->pbin[1] = *pbin;
				pbin = common_util_to_folder_entryid(pctx->pstream->plogon,
						rop_util_make_eid_ex(1, PRIVATE_FID_LOCAL_FAILURES));
				if (NULL == pbin) {
					return FALSE;
				}
				ba->pbin[2] = *pbin;
				pbin = common_util_to_folder_entryid(pctx->pstream->plogon,
						rop_util_make_eid_ex(1, PRIVATE_FID_SERVER_FAILURES));
				if (NULL == pbin) {
					return FALSE;
				}
				ba->pbin[3] = *pbin;
				pbin = common_util_to_folder_entryid(pctx->pstream->plogon,
								rop_util_make_eid_ex(1, PRIVATE_FID_JUNK));
				if (NULL == pbin) {
					return FALSE;
				}
				ba->pbin[4] = *pbin;
				common_util_set_propvals(
					fldchgs.pfldchgs + i, &tmp_propval);
			}
			if (common_util_get_propvals(&fldchgs.pfldchgs[i], PR_ADDITIONAL_REN_ENTRYIDS_EX) == nullptr) {
				tmp_propval.proptag = PR_ADDITIONAL_REN_ENTRYIDS_EX;
				pvalue = cu_alloc<BINARY>();
				auto bv = static_cast<BINARY *>(pvalue);
				if (NULL == pvalue) {
					return FALSE;
				}
				tmp_propval.pvalue = pvalue;
				persistdatas.count = 3;
				persistdatas.ppitems = cu_alloc<PERSISTDATA *>(persistdatas.count);
				if (NULL == persistdatas.ppitems) {
					return FALSE;
				}
				ppersistdata = cu_alloc<PERSISTDATA>(persistdatas.count);
				if (NULL == ppersistdata) {
					return FALSE;
				}
				persistdatas.ppitems[0] = ppersistdata;
				persistdatas.ppitems[0]->persist_id = RSF_PID_CONV_ACTIONS;
				persistdatas.ppitems[0]->element.element_id = RSF_ELID_ENTRYID;
				persistdatas.ppitems[0]->element.pentry_id =
					common_util_to_folder_entryid(pctx->pstream->plogon,
					rop_util_make_eid_ex(1, PRIVATE_FID_CONVERSATION_ACTION_SETTINGS));
				persistdatas.ppitems[1] = ppersistdata + 1;
				persistdatas.ppitems[1]->persist_id = RSF_PID_BUDDYLIST_PDLS;
				persistdatas.ppitems[1]->element.element_id = RSF_ELID_ENTRYID;
				persistdatas.ppitems[1]->element.pentry_id =
					common_util_to_folder_entryid(pctx->pstream->plogon,
					rop_util_make_eid_ex(1, PRIVATE_FID_IMCONTACTLIST));
				persistdatas.ppitems[2] = ppersistdata + 2;
				persistdatas.ppitems[2]->persist_id = RSF_PID_BUDDYLIST_CONTACTS;
				persistdatas.ppitems[2]->element.element_id = RSF_ELID_ENTRYID;
				persistdatas.ppitems[2]->element.pentry_id =
					common_util_to_folder_entryid(pctx->pstream->plogon,
					rop_util_make_eid_ex(1, PRIVATE_FID_QUICKCONTACTS));
				if (!ext_push.init(temp_buff, sizeof(temp_buff), 0) ||
				    ext_push.p_persistdata_a(&persistdatas) != EXT_ERR_SUCCESS)
					return false;
				bv->cb = ext_push.m_offset;
				bv->pv = common_util_alloc(bv->cb);
				if (bv->pv == nullptr)
					return FALSE;
				memcpy(bv->pv, ext_push.m_udata, bv->cb);
				common_util_set_propvals(
					fldchgs.pfldchgs + i, &tmp_propval);
			}
			if (common_util_get_propvals(&fldchgs.pfldchgs[i], PR_FREEBUSY_ENTRYIDS) == nullptr) {
				tmp_propval.proptag = PR_FREEBUSY_ENTRYIDS;
				pvalue = cu_alloc<BINARY_ARRAY>();
				auto ba = static_cast<BINARY_ARRAY *>(pvalue);
				if (NULL == pvalue) {
					return FALSE;
				}
				tmp_propval.pvalue = pvalue;
				ba->count = 4;
				ba->pbin = cu_alloc<BINARY>(ba->count);
				if (ba->pbin == nullptr)
					return FALSE;
				ba->pbin[0].cb = 0;
				ba->pbin[0].pb = nullptr;
				ba->pbin[1].cb = 0;
				ba->pbin[1].pb = nullptr;
				ba->pbin[2].cb = 0;
				ba->pbin[2].pb = nullptr;
				pbin = common_util_to_folder_entryid(pctx->pstream->plogon,
						rop_util_make_eid_ex(1, PRIVATE_FID_LOCAL_FREEBUSY));
				if (NULL == pbin) {
					return FALSE;
				}
				ba->pbin[3] = *pbin;
				common_util_set_propvals(
					fldchgs.pfldchgs + i, &tmp_propval);
			}
		}
	}
	if (pctx->sync_flags & SYNC_FLAG_ONLYSPECIFIEDPROPERTIES) {
		icsdownctx_object_adjust_fldchgs(
			&fldchgs, pctx->pproptags, FALSE);
	} else {
		icsdownctx_object_adjust_fldchgs(
			&fldchgs, pctx->pproptags, TRUE);
	}
	if (0 == (pctx->sync_flags & SYNC_FLAG_NODELETIONS)) {
		if (0 == deleted_folders.count) {
			pproplist_deletions = NULL;
		} else {
			pidset = idset_init(TRUE, REPL_TYPE_ID);
			if (NULL == pidset) {
				return FALSE;
			}
			for (size_t i = 0; i < deleted_folders.count; ++i) {
				if (FALSE == idset_append(pidset,
					deleted_folders.pids[i])) {
					idset_free(pidset);
					return FALSE;
				}
			}
			pbin = idset_serialize(pidset);
			idset_free(pidset);
			if (pbin == nullptr)
				return false;
			pproplist_deletions = &tmp_proplist;
			pproplist_deletions->count = 1;
			pproplist_deletions->ppropval = &tmp_propval;
			tmp_propval.proptag = META_TAG_IDSETDELETED;
			tmp_propval.pvalue = pbin;
		}
	} else {
		pproplist_deletions = NULL;
	}
	if (0 != last_changenum) {
		idset_clear(pctx->pstate->pseen);
		if (FALSE == idset_append_range(
			pctx->pstate->pseen, 1, 1,
			rop_util_get_gc_value(last_changenum))) {
			if (NULL != pproplist_deletions) {
				rop_util_free_binary(pbin);
			}
			return FALSE;
		}
	}
	auto pproplist_state = pctx->pstate->serialize();
	if (NULL == pproplist_state) {
		if (NULL != pproplist_deletions) {
			rop_util_free_binary(pbin);
		}
		return FALSE;
	}
	if (!pctx->pstream->write_hierarchysync(&fldchgs,
	    pproplist_deletions, pproplist_state)) {
		tpropval_array_free(pproplist_state);
		if (NULL != pproplist_deletions) {
			rop_util_free_binary(pbin);
		}
		return FALSE;	
	}
	tpropval_array_free(pproplist_state);
	if (NULL != pproplist_deletions) {
		rop_util_free_binary(pbin);
	}
	pctx->progress_steps = 0;
	pctx->total_steps = pctx->pstream->total_length();
	pctx->ratio = 1;
	return TRUE;
}

BOOL ICSDOWNCTX_OBJECT::make_sync()
{
	auto pctx = this;
	if (TRUE == pctx->b_started) {
		return FALSE;
	}
	if (SYNC_TYPE_CONTENTS == pctx->sync_type) {
		if (FALSE == icsdownctx_object_make_content(pctx)) {
			return FALSE;
		}
	} else {
		if (FALSE == icsdownctx_object_make_hierarchy(pctx)) {
			return FALSE;
		}
	}
	pctx->b_started = TRUE;
	return TRUE;
}

static BOOL icsdownctx_object_extract_msgctntinfo(
	MESSAGE_CONTENT *pmsgctnt, uint8_t extra_flags,
	TPROPVAL_ARRAY *pchgheader, PROGRESS_MESSAGE *pprogmsg)
{
	pchgheader->ppropval = cu_alloc<TAGGED_PROPVAL>(8);
	if (NULL == pchgheader->ppropval) {
		return FALSE;
	}
	pchgheader->count = 0;
	auto pvalue = common_util_get_propvals(&pmsgctnt->proplist, PR_SOURCE_KEY);
	if (NULL == pvalue) {
		return FALSE;
	}
	pchgheader->ppropval[pchgheader->count].proptag = PR_SOURCE_KEY;
	pchgheader->ppropval[pchgheader->count].pvalue = pvalue;
	pchgheader->count ++;
	common_util_remove_propvals(&pmsgctnt->proplist, PR_SOURCE_KEY);
	
	pvalue = common_util_get_propvals(&pmsgctnt->proplist, PR_LAST_MODIFICATION_TIME);
	if (NULL == pvalue) {
		return FALSE;
	}
	pchgheader->ppropval[pchgheader->count].proptag = PR_LAST_MODIFICATION_TIME;
	pchgheader->ppropval[pchgheader->count].pvalue = pvalue;
	pchgheader->count ++;
	
	pvalue = common_util_get_propvals(&pmsgctnt->proplist, PR_CHANGE_KEY);
	if (NULL == pvalue) {
		return FALSE;
	}
	pchgheader->ppropval[pchgheader->count].proptag = PR_CHANGE_KEY;
	pchgheader->ppropval[pchgheader->count].pvalue = pvalue;
	pchgheader->count ++;
	
	pvalue = common_util_get_propvals(&pmsgctnt->proplist, PR_PREDECESSOR_CHANGE_LIST);
	if (NULL == pvalue) {
		return FALSE;
	}
	pchgheader->ppropval[pchgheader->count].proptag = PR_PREDECESSOR_CHANGE_LIST;
	pchgheader->ppropval[pchgheader->count].pvalue = pvalue;
	pchgheader->count ++;
	common_util_remove_propvals(&pmsgctnt->proplist, PR_PREDECESSOR_CHANGE_LIST);
	
	pvalue = common_util_get_propvals(&pmsgctnt->proplist, PR_ASSOCIATED);
	if (NULL == pvalue) {
		return FALSE;
	}
	pprogmsg->b_fai = *static_cast<uint8_t *>(pvalue) == 0 ? false : TRUE;
	pchgheader->ppropval[pchgheader->count].proptag = PR_ASSOCIATED;
	pchgheader->ppropval[pchgheader->count].pvalue = pvalue;
	pchgheader->count ++;
	common_util_remove_propvals(&pmsgctnt->proplist, PR_ASSOCIATED);
	
	if (SYNC_EXTRA_FLAG_EID & extra_flags) {
		pvalue = common_util_get_propvals(
			&pmsgctnt->proplist, PROP_TAG_MID);
		if (NULL == pvalue) {
			return FALSE;
		}
		pchgheader->ppropval[pchgheader->count].proptag =
												PROP_TAG_MID;
		pchgheader->ppropval[pchgheader->count].pvalue = pvalue;
		pchgheader->count ++;
	}
	common_util_remove_propvals(
			&pmsgctnt->proplist, PROP_TAG_MID);
	
	pvalue = common_util_get_propvals(&pmsgctnt->proplist, PR_MESSAGE_SIZE);
	if (NULL == pvalue) {
		return FALSE;
	}
	pprogmsg->message_size = *(uint32_t*)pvalue;
	if (SYNC_EXTRA_FLAG_MESSAGESIZE & extra_flags) {
		pchgheader->ppropval[pchgheader->count].proptag = PR_MESSAGE_SIZE;
		pchgheader->ppropval[pchgheader->count].pvalue = pvalue;
		pchgheader->count ++;
	}
	common_util_remove_propvals(&pmsgctnt->proplist, PR_MESSAGE_SIZE);
	
	if (SYNC_EXTRA_FLAG_CN & extra_flags) {
		pvalue = common_util_get_propvals(
			&pmsgctnt->proplist, PROP_TAG_CHANGENUMBER);
		if (NULL == pvalue) {
			return FALSE;
		}
		pchgheader->ppropval[pchgheader->count].proptag =
										PROP_TAG_CHANGENUMBER;
		pchgheader->ppropval[pchgheader->count].pvalue = pvalue;
		pchgheader->count ++;
	}
	common_util_remove_propvals(
		&pmsgctnt->proplist, PROP_TAG_CHANGENUMBER);
	return TRUE;
}

static void icsdownctx_object_adjust_msgctnt(MESSAGE_CONTENT *pmsgctnt,
	const PROPTAG_ARRAY *pproptags, BOOL b_exclude)
{
	int i;
	
	if (TRUE == b_exclude) {
		for (i=0; i<pproptags->count; i++) {
			switch (pproptags->pproptag[i]) {
			case PR_MESSAGE_RECIPIENTS:
				pmsgctnt->children.prcpts = NULL;
				break;
			case PR_MESSAGE_ATTACHMENTS:
				pmsgctnt->children.pattachments = NULL;
				break;
			default:
				common_util_remove_propvals(&pmsgctnt->proplist,
										pproptags->pproptag[i]);
				break;
			}
		}
	} else {
		i = 0;
		while (i < pmsgctnt->proplist.count) {
			if (!proptag_array_check(pproptags,
			    pmsgctnt->proplist.ppropval[i].proptag)) {
				common_util_remove_propvals(&pmsgctnt->proplist,
						pmsgctnt->proplist.ppropval[i].proptag);
				continue;
			}
			i ++;
		}
		if (!proptag_array_check(pproptags, PR_MESSAGE_RECIPIENTS))
			pmsgctnt->children.prcpts = NULL;
		if (!proptag_array_check(pproptags, PR_MESSAGE_ATTACHMENTS))
			pmsgctnt->children.pattachments = NULL;
	}
}

static BOOL icsdownctx_object_get_changepartial(
	ICSDOWNCTX_OBJECT *pctx, MESSAGE_CONTENT *pmsgctnt,
	uint32_t group_id, const INDEX_ARRAY *pindices,
	const PROPTAG_ARRAY *pproptags, MSGCHG_PARTIAL *pmsg)
{
	int i, j;
	uint16_t count;
	uint32_t index;
	BOOL b_written;
	uint32_t proptag;
	ics_group_node *pgpnode = nullptr;
	DOUBLE_LIST_NODE *pnode;
	PROPTAG_ARRAY *pchangetags;
	static constexpr BINARY fake_bin{};
	
	auto pgpinfo = pctx->pstream->plogon->get_property_groupinfo(group_id);
	if (NULL == pgpinfo) {
		return FALSE;
	}
	b_written = FALSE;
	for (pnode=double_list_get_head(&pctx->group_list); NULL!=pnode;
		pnode=double_list_get_after(&pctx->group_list, pnode)) {
		if (static_cast<ics_group_node *>(pnode->pdata)->group_id == group_id) {
			b_written = TRUE;
			break;
		}
	}
	pmsg->group_id = group_id;
	if (TRUE == b_written) {
		pmsg->pgpinfo= &pctx->fake_gpinfo;
	} else {
		pmsg->pgpinfo = pgpinfo;
		pgpnode = me_alloc<ics_group_node>();
		if (NULL == pgpnode) {
			return FALSE;
		}
		pgpnode->node.pdata = pgpnode;
		pgpnode->group_id = group_id;
		double_list_append_as_tail(&pctx->group_list, &pgpnode->node);
	}
	pmsg->count = pindices->count;
	if (pproptags->count != 0)
		++pmsg->count;
	pmsg->pchanges = cu_alloc<CHANGE_PART>(pmsg->count);
	if (NULL == pmsg->pchanges) {
		pmsg->count = 0;
		return FALSE;
	}
	for (i=0; i<pindices->count; i++) {
		index = pindices->pproptag[i];
		pmsg->pchanges[i].index = index;
		pchangetags = pgpinfo->pgroups + index;
		pmsg->pchanges[i].proplist.ppropval = cu_alloc<TAGGED_PROPVAL>(pchangetags->count);
		count = 0;
		for (j=0; j<pchangetags->count; j++) {
			proptag = pchangetags->pproptag[j];
			switch (proptag) {
			case PR_MESSAGE_RECIPIENTS:
				pmsg->pchanges[i].proplist.ppropval[count].proptag = PR_MESSAGE_RECIPIENTS;
				pmsg->pchanges[i].proplist.ppropval[count].pvalue = deconst(&fake_bin);
				count ++;
				pmsg->children.prcpts = pmsgctnt->children.prcpts;
				break;
			case PR_MESSAGE_ATTACHMENTS:
				pmsg->pchanges[i].proplist.ppropval[count].proptag = PR_MESSAGE_ATTACHMENTS;
				pmsg->pchanges[i].proplist.ppropval[count].pvalue = deconst(&fake_bin);
				count ++;
				pmsg->children.pattachments =
					pmsgctnt->children.pattachments;
				break;
			default: {
				auto pvalue = common_util_get_propvals(&pmsgctnt->proplist, proptag);
				if (NULL != pvalue) {
					pmsg->pchanges[i].proplist.ppropval[
								count].proptag = proptag;
					pmsg->pchanges[i].proplist.ppropval[
								count].pvalue = pvalue;
					count ++;
				}
				break;
			}
			}
		}
		pmsg->pchanges[i].proplist.count = count;
	}
	if (0 == pproptags->count) {
		return TRUE;
	}
	pmsg->pchanges[i].index = 0xFFFFFFFF;
	pmsg->pchanges[i].proplist.ppropval = cu_alloc<TAGGED_PROPVAL>(pproptags->count);
	count = 0;
	for (j=0; j<pproptags->count; j++) {
		proptag = pproptags->pproptag[j];
		switch (proptag) {
		case PR_MESSAGE_RECIPIENTS:
			pmsg->pchanges[i].proplist.ppropval[count].proptag = PR_MESSAGE_RECIPIENTS;
			pmsg->pchanges[i].proplist.ppropval[count].pvalue = deconst(&fake_bin);
			count ++;
			pmsg->children.prcpts = pmsgctnt->children.prcpts;
			break;
		case PR_MESSAGE_ATTACHMENTS:
			pmsg->pchanges[i].proplist.ppropval[count].proptag = PR_MESSAGE_ATTACHMENTS;
			pmsg->pchanges[i].proplist.ppropval[count].pvalue = deconst(&fake_bin);
			count ++;
			pmsg->children.pattachments =
				pmsgctnt->children.pattachments;
			break;
		default: {
			auto pvalue = common_util_get_propvals(&pmsgctnt->proplist, proptag);
			if (NULL != pvalue) {
				pmsg->pchanges[i].proplist.ppropval[
							count].proptag = proptag;
				pmsg->pchanges[i].proplist.ppropval[
							count].pvalue = pvalue;
				count ++;
			}
			break;
		}
		}
	}
	pmsg->pchanges[i].proplist.count = count;
	return TRUE;
}

static void icsdownctx_object_trim_embedded(
	MESSAGE_CONTENT *pmsgctnt)
{
	int i, j;
	MESSAGE_CONTENT *pembedded;
	ATTACHMENT_CONTENT *pattachment;
	
	if (NULL == pmsgctnt->children.pattachments) {
		return;
	}
	for (i=0; i<pmsgctnt->children.pattachments->count; i++) {
		pattachment = pmsgctnt->children.pattachments->pplist[i];
		if (NULL == pattachment->pembedded) {
			continue;
		}
		pembedded = pattachment->pembedded;
		for (j=0; j<pembedded->proplist.count; j++) {
			if (PROP_TAG_MID == pembedded->proplist.ppropval[j].proptag) {
				*(uint64_t*)pembedded->proplist.ppropval[j].pvalue = 0;
				break;
			}
		}
		common_util_remove_propvals(
			&pembedded->proplist, PROP_TAG_CHANGENUMBER);
		common_util_remove_propvals(
			&pembedded->proplist, PROP_TAG_MESSAGESTATUS);
		icsdownctx_object_trim_embedded(pembedded);
	}
}

/* Outlook 2016 does not accept recipients
	of report messages, get rid of them */
static void icsdownctx_object_trim_report_recipients(
	MESSAGE_CONTENT *pmsgctnt)
{
	int i;
	ATTACHMENT_CONTENT *pattachment;
	
	auto pvalue = static_cast<const char *>(common_util_get_propvals(
	              &pmsgctnt->proplist, PROP_TAG_MESSAGECLASS));
	if (NULL != pvalue && 0 == strncasecmp(
		pvalue, "REPORT.IPM.Note.", 16)) {
		pmsgctnt->children.prcpts = NULL;
	}
	if (NULL == pmsgctnt->children.pattachments) {
		return;
	}
	for (i=0; i<pmsgctnt->children.pattachments->count; i++) {
		pattachment = pmsgctnt->children.pattachments->pplist[i];
		if (NULL != pattachment->pembedded) {
			icsdownctx_object_trim_report_recipients(
							pattachment->pembedded);
		}
	}
}

static BOOL icsdownctx_object_write_message_change(ICSDOWNCTX_OBJECT *pctx,
	uint64_t message_id, BOOL b_downloaded, int *ppartial_count)
{
	int i;
	BOOL b_full;
	void *pvalue;
	uint64_t last_cn;
	uint32_t *pstatus;
	INDEX_ARRAY indices;
	uint32_t *pgroup_id;
	PROPTAG_ARRAY proptags;
	TAGGED_PROPVAL *ppropval;
	PROGRESS_MESSAGE progmsg;
	TPROPVAL_ARRAY chgheader;
	MESSAGE_CONTENT *pmsgctnt;
	MESSAGE_CONTENT *pembedded;
	MSGCHG_PARTIAL msg_partial;
	TAGGED_PROPVAL tmp_propval;
	static constexpr uint8_t fake_true = 1;
	static constexpr uint8_t fake_false = 0;
	
	auto pinfo = emsmdb_interface_get_emsmdb_info();
	if (pctx->pstream->plogon->check_private()) {
		if (!exmdb_client_read_message(pctx->pstream->plogon->get_dir(),
		    nullptr, pinfo->cpid, message_id, &pmsgctnt))
			return FALSE;
	} else {
		auto rpc_info = get_rpc_info();
		if (!exmdb_client_read_message(pctx->pstream->plogon->get_dir(),
		    rpc_info.username, pinfo->cpid, message_id, &pmsgctnt))
			return FALSE;
	}
	if (NULL == pmsgctnt) {
		idset_remove(pctx->pstate->pgiven, message_id);
		if (TRUE == b_downloaded) {
			if (0 == (SYNC_FLAG_NODELETIONS & pctx->sync_flags)) {
				if (!eid_array_append(pctx->pdeleted_messages, message_id))
					return FALSE;	
			}
			if (SYNC_FLAG_READSTATE & pctx->sync_flags) {
				eid_array_remove(pctx->pread_messags, message_id);
				eid_array_remove(pctx->punread_messags, message_id);
			}
		}
		return TRUE;
	}
	icsdownctx_object_trim_report_recipients(pmsgctnt);
	auto folder_id = pctx->pfolder->folder_id;
	pstatus = static_cast<uint32_t *>(common_util_get_propvals(
	          &pmsgctnt->proplist, PROP_TAG_MESSAGESTATUS));
	if (NULL == pstatus) {
		return FALSE;
	}
	if (*pstatus & MESSAGE_STATUS_IN_CONFLICT) {
		if (0 == (pctx->sync_flags & SYNC_FLAG_NOFOREIGNIDENTIFIERS)) {
			if (!exmdb_client_get_folder_property(pctx->pstream->plogon->get_dir(),
			    0, folder_id, PR_SOURCE_KEY, &pvalue))
				return FALSE;	
			if (NULL == pvalue) {
				pvalue = common_util_calculate_folder_sourcekey(
								pctx->pstream->plogon, folder_id);
			}
		} else {
			pvalue = common_util_calculate_folder_sourcekey(
							pctx->pstream->plogon, folder_id);
		}
		if (NULL == pvalue) {
			return FALSE;
		}
		for (i=0; i<pmsgctnt->children.pattachments->count; i++) {
			if (NULL == common_util_get_propvals(
				&pmsgctnt->children.pattachments->pplist[i]->proplist,
				PROP_TAG_INCONFLICT)) {
				continue;
			}
			pembedded = pmsgctnt->children.pattachments->pplist[i]->pembedded;
			if (NULL == pembedded) {
				return FALSE;
			}
			icsdownctx_object_trim_embedded(pembedded);
			ppropval = cu_alloc<TAGGED_PROPVAL>(pembedded->proplist.count + 2);
			if (NULL == ppropval) {
				return FALSE;
			}
			memcpy(ppropval, pembedded->proplist.ppropval,
				sizeof(TAGGED_PROPVAL)*pembedded->proplist.count);
			pembedded->proplist.ppropval = ppropval;
			tmp_propval.proptag = PROP_TAG_MID;
			tmp_propval.pvalue = &message_id;
			common_util_set_propvals(&pembedded->proplist, &tmp_propval);
			tmp_propval.proptag = PR_PARENT_SOURCE_KEY;
			tmp_propval.pvalue = pvalue;
			common_util_set_propvals(&pembedded->proplist, &tmp_propval);
			if (common_util_get_propvals(&pembedded->proplist, PR_SOURCE_KEY) == nullptr) {
				tmp_propval.proptag = PR_SOURCE_KEY;
				tmp_propval.pvalue = common_util_calculate_message_sourcekey(
										pctx->pstream->plogon, message_id);
				if (NULL == tmp_propval.pvalue) {
					return FALSE;
				}
				common_util_set_propvals(&pembedded->proplist, &tmp_propval);	
			}
			if (FALSE == icsdownctx_object_extract_msgctntinfo(
				pembedded, pctx->extra_flags, &chgheader, &progmsg)) {
				return FALSE;
			}
			if (pctx->sync_flags & SYNC_FLAG_ONLYSPECIFIEDPROPERTIES) {
				icsdownctx_object_adjust_msgctnt(
					pembedded, pctx->pproptags, FALSE);
			} else {
				icsdownctx_object_adjust_msgctnt(
					pembedded, pctx->pproptags, TRUE);
			}
			if (pctx->sync_flags & SYNC_FLAG_PROGRESS &&
			    !pctx->pstream->write_progresspermessage(&progmsg))
				return FALSE;
			common_util_remove_propvals(&pembedded->proplist, PR_READ);
			common_util_remove_propvals(&pembedded->proplist, PR_CHANGE_KEY);
			common_util_remove_propvals(
				&pembedded->proplist, PROP_TAG_MESSAGESTATUS);
			pvalue = common_util_get_propvals(&pembedded->proplist, PR_MESSAGE_FLAGS);
			tmp_propval.proptag = PR_READ_RECEIPT_REQUESTED;
			tmp_propval.pvalue = pvalue != nullptr &&
			                     (*static_cast<uint32_t *>(pvalue) & MSGFLAG_RN_PENDING) ?
			                     deconst(&fake_true) : deconst(&fake_false);
			common_util_set_propvals(&pembedded->proplist, &tmp_propval);
			tmp_propval.proptag = PR_NON_RECEIPT_NOTIFICATION_REQUESTED;
			tmp_propval.pvalue = pvalue != nullptr &&
			                     (*static_cast<uint32_t *>(pvalue) & MSGFLAG_NRN_PENDING) ?
			                     deconst(&fake_true) : deconst(&fake_false);
			common_util_set_propvals(&pembedded->proplist, &tmp_propval);
			if (!pctx->pstream->write_messagechangefull(&chgheader, pembedded))
				return FALSE;
		}
		return TRUE;
	}
	icsdownctx_object_trim_embedded(pmsgctnt);
	ppropval = cu_alloc<TAGGED_PROPVAL>(pmsgctnt->proplist.count + 10);
	if (NULL == ppropval) {
		return FALSE;
	}
	memcpy(ppropval, pmsgctnt->proplist.ppropval,
		sizeof(TAGGED_PROPVAL)*pmsgctnt->proplist.count);
	pmsgctnt->proplist.ppropval = ppropval;
	if (0 == (pctx->sync_flags & SYNC_FLAG_NOFOREIGNIDENTIFIERS)) {
		if (!exmdb_client_get_folder_property(pctx->pstream->plogon->get_dir(),
		    0, folder_id, PR_SOURCE_KEY, &pvalue))
			return FALSE;	
		if (NULL == pvalue) {
			tmp_propval.proptag = PR_PARENT_SOURCE_KEY;
			tmp_propval.pvalue = common_util_calculate_folder_sourcekey(
									pctx->pstream->plogon, folder_id);
			if (NULL == tmp_propval.pvalue) {
				return FALSE;
			}
		} else {
			tmp_propval.proptag = PR_PARENT_SOURCE_KEY;
			tmp_propval.pvalue = pvalue;
		}
		common_util_set_propvals(&pmsgctnt->proplist, &tmp_propval);
		if (common_util_get_propvals(&pmsgctnt->proplist, PR_SOURCE_KEY) == nullptr) {
			tmp_propval.proptag = PR_SOURCE_KEY;
			tmp_propval.pvalue =
				common_util_calculate_message_sourcekey(
					pctx->pstream->plogon, message_id);
			if (NULL == tmp_propval.pvalue) {
				return FALSE;
			}
			common_util_set_propvals(&pmsgctnt->proplist, &tmp_propval);	
		}
	} else {
		tmp_propval.proptag = PR_PARENT_SOURCE_KEY;
		tmp_propval.pvalue = common_util_calculate_folder_sourcekey(
								pctx->pstream->plogon, folder_id);
		if (NULL == tmp_propval.pvalue) {
			return FALSE;
		}
		common_util_set_propvals(&pmsgctnt->proplist, &tmp_propval);
		tmp_propval.proptag = PR_SOURCE_KEY;
		tmp_propval.pvalue = common_util_calculate_message_sourcekey(
								pctx->pstream->plogon, message_id);
		if (NULL == tmp_propval.pvalue) {
			return FALSE;
		}
		common_util_set_propvals(&pmsgctnt->proplist, &tmp_propval);
	}
	if (FALSE == icsdownctx_object_extract_msgctntinfo(
		pmsgctnt, pctx->extra_flags, &chgheader, &progmsg)) {
		return FALSE;
	}
	if (pctx->sync_flags & SYNC_FLAG_ONLYSPECIFIEDPROPERTIES) {
		if (pctx->sync_flags & SYNC_FLAG_IGNORESPECIFIEDONFAI) {
			if (FALSE == progmsg.b_fai) {
				icsdownctx_object_adjust_msgctnt(
					pmsgctnt, pctx->pproptags, FALSE);
			}
		} else {
			icsdownctx_object_adjust_msgctnt(
				pmsgctnt, pctx->pproptags, FALSE);
		}
	} else {
		icsdownctx_object_adjust_msgctnt(
			pmsgctnt, pctx->pproptags, TRUE);
	}
	if (FALSE == b_downloaded || TRUE == progmsg.b_fai) {
		b_full = TRUE;
	} else {
		if (!exmdb_client_get_message_group_id(pctx->pstream->plogon->get_dir(),
		    message_id, &pgroup_id))
			return FALSE;
		if (0 == (pctx->send_options & SEND_OPTIONS_PARTIAL) ||
			 TRUE == progmsg.b_fai || NULL == pgroup_id ||
			 *ppartial_count > MAX_PARTIAL_ON_ROP) {
			b_full = TRUE;
		} else {
			if (FALSE == progmsg.b_fai) {
				if (FALSE == idset_get_repl_first_max(
					pctx->pstate->pseen, 1, &last_cn)) {
					return FALSE;	
				}
			} else {
				if (FALSE == idset_get_repl_first_max(
					pctx->pstate->pseen_fai, 1, &last_cn)) {
					return FALSE;	
				}
			}
			if (!exmdb_client_get_change_indices(pctx->pstream->plogon->get_dir(),
			    message_id, last_cn, &indices, &proptags))
				return FALSE;	
			if (0 == indices.count && 0 == proptags.count) {
				b_full = TRUE;
			} else {
				b_full = FALSE;
				(*ppartial_count) ++;
			}
		}
		if (FALSE == b_full) {
			if (FALSE == icsdownctx_object_get_changepartial(
				pctx, pmsgctnt, *pgroup_id, &indices,
				&proptags, &msg_partial)) {
				return FALSE;
			}
		}
	}
	if (pctx->sync_flags & SYNC_FLAG_PROGRESS &&
	    !pctx->pstream->write_progresspermessage(&progmsg))
		return FALSE;
	pctx->next_progress_steps += progmsg.message_size;
	if (TRUE == b_full) {
		common_util_remove_propvals(&pmsgctnt->proplist, PR_READ);
		common_util_remove_propvals(&pmsgctnt->proplist, PR_CHANGE_KEY);
		common_util_remove_propvals(
			&pmsgctnt->proplist, PROP_TAG_MESSAGESTATUS);
		pvalue = common_util_get_propvals(&pmsgctnt->proplist, PR_MESSAGE_FLAGS);
		tmp_propval.proptag = PR_READ_RECEIPT_REQUESTED;
		tmp_propval.pvalue = pvalue != nullptr &&
		                     (*static_cast<uint32_t *>(pvalue) & MSGFLAG_RN_PENDING) ?
		                     deconst(&fake_true) : deconst(&fake_false);
		common_util_set_propvals(&pmsgctnt->proplist, &tmp_propval);
		tmp_propval.proptag = PR_NON_RECEIPT_NOTIFICATION_REQUESTED;
		tmp_propval.pvalue = pvalue != nullptr &&
		                     (*static_cast<uint32_t *>(pvalue) & MSGFLAG_NRN_PENDING) ?
		                     deconst(&fake_true) : deconst(&fake_false);
		common_util_set_propvals(&pmsgctnt->proplist, &tmp_propval);
		if (!pctx->pstream->write_messagechangefull(&chgheader, pmsgctnt))
			return FALSE;
	} else {
		if (!pctx->pstream->write_messagechangepartial(&chgheader, &msg_partial))
			return FALSE;
	}
	return TRUE;
}

/* only be called under content sync */
static BOOL icsdownctx_object_write_deletions(ICSDOWNCTX_OBJECT *pctx)
{
	BINARY *pbin1;
	BINARY *pbin2;
	IDSET *pidset;
	TPROPVAL_ARRAY proplist;
	TAGGED_PROPVAL tmp_propvals[2];
	
	proplist.count = 0;
	proplist.ppropval = tmp_propvals;
	pbin1 = NULL;
	pbin2 = NULL;
	if (pctx->pdeleted_messages->count > 0) {
		pidset = idset_init(TRUE, REPL_TYPE_ID);
		if (NULL == pidset) {
			return FALSE;
		}
		for (size_t i = 0; i < pctx->pdeleted_messages->count; ++i) {
			if (FALSE == idset_append(pidset,
				pctx->pdeleted_messages->pids[i])) {
				idset_free(pidset);
				return FALSE;
			}
		}
		pbin1 = idset_serialize(pidset);
		idset_free(pidset);
		if (NULL == pbin1) {
			return FALSE;
		}
		proplist.ppropval[proplist.count].proptag =
								META_TAG_IDSETDELETED;
		proplist.ppropval[proplist.count].pvalue = pbin1;
		proplist.count ++;
	}
	if (0 == (SYNC_FLAG_IGNORENOLONGERINSCOPE & pctx->sync_flags)
		&& pctx->pnolonger_messages->count > 0) {
		pidset = idset_init(TRUE, REPL_TYPE_ID);
		if (NULL == pidset) {
			if (NULL != pbin1) {
				rop_util_free_binary(pbin1);
			}
			return FALSE;
		}
		for (size_t i = 0; i < pctx->pnolonger_messages->count; ++i) {
			if (FALSE == idset_append(pidset,
				pctx->pnolonger_messages->pids[i])) {
				idset_free(pidset);
				if (NULL != pbin1) {
					rop_util_free_binary(pbin1);
				}
				return FALSE;
			}
		}
		pbin2 = idset_serialize(pidset);
		idset_free(pidset);
		if (NULL == pbin2) {
			if (NULL != pbin1) {
				rop_util_free_binary(pbin1);
			}
			return FALSE;
		}
		proplist.ppropval[proplist.count].proptag =
						META_TAG_IDSETNOLONGERINSCOPE;
		proplist.ppropval[proplist.count].pvalue = pbin2;
		proplist.count ++;
	}
	if (0 == proplist.count) {
		return TRUE;
	}
	if (!pctx->pstream->write_deletions(&proplist)) {
		if (NULL != pbin1) {
			rop_util_free_binary(pbin1);
		}
		if (NULL != pbin2) {
			rop_util_free_binary(pbin2);
		}
		return FALSE;
	}
	if (NULL != pbin1) {
		rop_util_free_binary(pbin1);
	}
	if (NULL != pbin2) {
		rop_util_free_binary(pbin2);
	}
	return TRUE;
}

/* only be called under content sync */
static BOOL icsdownctx_object_write_readstate_changes(
	ICSDOWNCTX_OBJECT *pctx)
{
	BINARY *pbin1;
	BINARY *pbin2;
	IDSET *pidset;
	TPROPVAL_ARRAY proplist;
	TAGGED_PROPVAL tmp_propvals[2];
	
	pbin1 = NULL;
	pbin2 = NULL;
	proplist.count = 0;
	proplist.ppropval = tmp_propvals;
	if (pctx->pread_messags->count > 0) {
		pidset = idset_init(TRUE, REPL_TYPE_ID);
		if (NULL == pidset) {
			return FALSE;
		}
		for (size_t i = 0; i < pctx->pread_messags->count; ++i) {
			if (FALSE == idset_append(pidset,
				pctx->pread_messags->pids[i])) {
				idset_free(pidset);
				return FALSE;
			}
		}
		pbin1 = idset_serialize(pidset);
		idset_free(pidset);
		if (NULL == pbin1) {
			return FALSE;
		}
		proplist.ppropval[proplist.count].proptag =
								META_TAG_IDSETREAD;
		proplist.ppropval[proplist.count].pvalue = pbin1;
		proplist.count ++;
	}
	if (pctx->punread_messags->count > 0) {
		pidset = idset_init(TRUE, REPL_TYPE_ID);
		if (NULL == pidset) {
			if (NULL != pbin1) {
				rop_util_free_binary(pbin1);
			}
			return FALSE;
		}
		for (size_t i = 0; i < pctx->punread_messags->count; ++i) {
			if (FALSE == idset_append(pidset,
				pctx->punread_messags->pids[i])) {
				idset_free(pidset);
				if (NULL != pbin1) {
					rop_util_free_binary(pbin1);
				}
				return FALSE;
			}
		}
		pbin2 = idset_serialize(pidset);
		idset_free(pidset);
		if (NULL == pbin2) {
			if (NULL != pbin1) {
				rop_util_free_binary(pbin1);
			}
			return FALSE;
		}
		proplist.ppropval[proplist.count].proptag =
								META_TAG_IDSETUNREAD;
		proplist.ppropval[proplist.count].pvalue = pbin2;
		proplist.count ++;
	}
	if (0 == proplist.count) {
		return TRUE;
	}
	if (!pctx->pstream->write_readstatechanges(&proplist)) {
		if (NULL != pbin1) {
			rop_util_free_binary(pbin1);
		}
		if (NULL != pbin2) {
			rop_util_free_binary(pbin2);
		}
		return FALSE;
	}
	if (NULL != pbin1) {
		rop_util_free_binary(pbin1);
	}
	if (NULL != pbin2) {
		rop_util_free_binary(pbin2);
	}
	return TRUE;
}

/* only be called under content sync */
static BOOL icsdownctx_object_write_state(ICSDOWNCTX_OBJECT *pctx)
{
	idset_clear(pctx->pstate->pseen);
	if (pctx->sync_flags & SYNC_FLAG_NORMAL) {
		if (0 != pctx->last_changenum) {
			if (FALSE == idset_append_range(pctx->pstate->pseen, 1,
				1, rop_util_get_gc_value(pctx->last_changenum))) {
				return FALSE;
			}
		}
	}
	idset_clear(pctx->pstate->pseen_fai);
	if (pctx->sync_flags & SYNC_FLAG_FAI) {
		if (0 != pctx->last_changenum) {
			if (FALSE == idset_append_range(pctx->pstate->pseen_fai,
				1, 1, rop_util_get_gc_value(pctx->last_changenum))) {
				return FALSE;
			}
		}
	}
	idset_clear(pctx->pstate->pread);
	if (pctx->sync_flags & SYNC_FLAG_READSTATE) {
		if (0 == pctx->last_readcn) {
			if (0 != pctx->last_changenum) {
				if (FALSE == idset_append_range(
					pctx->pstate->pread, 1, 1,
					CHANGE_NUMBER_BEGIN - 1)) {
					return FALSE;	
				}
			}
		} else {
			if (FALSE == idset_append_range(pctx->pstate->pread,
				1, 1, rop_util_get_gc_value(pctx->last_readcn))) {
				return FALSE;
			}
		}
	}
	auto pproplist = pctx->pstate->serialize();
	if (NULL == pproplist) {
		return FALSE;
	}
	if (!pctx->pstream->write_state(pproplist)) {
		tpropval_array_free(pproplist);
		return FALSE;
	}
	tpropval_array_free(pproplist);
	return TRUE;
}

static BOOL icsdownctx_object_get_buffer_internal(
	ICSDOWNCTX_OBJECT *pctx, void *pbuff,
	uint16_t *plen, BOOL *pb_last)
{
	BOOL b_last;
	uint16_t len;
	uint16_t len1;
	int partial_count;
	uint64_t message_id;
	ICS_FLOW_NODE *pflow;
	DOUBLE_LIST_NODE *pnode;
	
	if (0 == double_list_get_nodes_num(&pctx->flow_list)) {
		if (!pctx->pstream->read_buffer(pbuff, plen, pb_last))
			return FALSE;	
		if (SYNC_TYPE_HIERARCHY == pctx->sync_type) {
			pctx->progress_steps += *plen;
		}
		return TRUE;
	}
	len = 0;
	if (pctx->pstream->total_length() > 0) {
		len = *plen;
		if (!pctx->pstream->read_buffer(pbuff, &len, &b_last))
			return FALSE;	
		if (FALSE == b_last || *plen - len <
			2*FTSTREAM_PRODUCER_POINT_LENGTH) {
			*plen = len;
			*pb_last = FALSE;
			return TRUE;
		}
	}
	partial_count = 0;
	len1 = *plen - len;
	while ((pnode = double_list_pop_front(&pctx->flow_list)) != nullptr) {
		pctx->progress_steps = pctx->next_progress_steps;
		pflow = (ICS_FLOW_NODE*)pnode->pdata;
		switch (pflow->func_id) {
		case FUNC_ID_UINT32:
			if (!pctx->pstream->write_uint32(reinterpret_cast<uintptr_t>(pflow->pparam))) {
				free(pnode->pdata);
				return FALSE;
			}
			break;
		case FUNC_ID_PROGRESSTOTAL:
			if (!pctx->pstream->write_progresstotal(pctx->pprogtotal)) {
				free(pnode->pdata);
				return FALSE;
			}
			break;
		case FUNC_ID_UPDATED_MESSAGE:
			message_id = *(uint64_t*)pflow->pparam;
			if (FALSE == icsdownctx_object_write_message_change(
				pctx, message_id, TRUE, &partial_count)) {
				free(pnode->pdata);
				return FALSE;
			}
			break;
		case FUNC_ID_NEW_MESSAGE:
			message_id = *(uint64_t*)pflow->pparam;
			if (FALSE == icsdownctx_object_write_message_change(
				pctx, message_id, FALSE, &partial_count)) {
				free(pnode->pdata);
				return FALSE;
			}
			break;
		case FUNC_ID_DELETIONS:
			if (FALSE == icsdownctx_object_write_deletions(pctx)) {
				free(pnode->pdata);
				return FALSE;
			}
			break;
		case FUNC_ID_READSTATECHANGES:
			if (FALSE == icsdownctx_object_write_readstate_changes(pctx)) {
				free(pnode->pdata);
				return FALSE;
			}
			break;
		case FUNC_ID_STATE:
			if (FALSE == icsdownctx_object_write_state(pctx)) {
				free(pnode->pdata);
				return FALSE;
			}
			break;
		default:
			free(pnode->pdata);
			return FALSE;
		}
		free(pnode->pdata);
		if (pctx->pstream->total_length() > len1)
			break;
	}
	if (!pctx->pstream->read_buffer(static_cast<char *>(pbuff) + len, &len1, &b_last))
		return FALSE;	
	*plen = len + len1;
	*pb_last = double_list_get_nodes_num(&pctx->flow_list) == 0 && b_last ? TRUE : false;
	return TRUE;
}

BOOL ICSDOWNCTX_OBJECT::get_buffer(void *pbuff, uint16_t *plen, BOOL *pb_last,
	uint16_t *pprogress, uint16_t *ptotal)
{
	auto pctx = this;
	*pprogress = pctx->progress_steps / pctx->ratio;
	*ptotal = pctx->total_steps / pctx->ratio;
	if (0 == *ptotal) {
		*ptotal = 1;
	}
	if (FALSE == icsdownctx_object_get_buffer_internal(
		pctx, pbuff, plen, pb_last)) {
		return FALSE;	
	}
	if (TRUE == *pb_last) {
		*pprogress = *ptotal;
	}
	return TRUE;
}

ICSDOWNCTX_OBJECT::~ICSDOWNCTX_OBJECT()
{
	auto pctx = this;
	DOUBLE_LIST_NODE *pnode;
	
	while ((pnode = double_list_pop_front(&pctx->flow_list)) != nullptr)
		free(pnode->pdata);
	double_list_free(&pctx->flow_list);
	while ((pnode = double_list_pop_front(&pctx->group_list)) != nullptr)
		free(pnode->pdata);
	double_list_free(&pctx->group_list);
	if (NULL != pctx->pprogtotal) {
		free(pctx->pprogtotal);
	}
	if (NULL != pctx->pmessages) {
		eid_array_free(pctx->pmessages);
	}
	if (NULL != pctx->pdeleted_messages) {
		eid_array_free(pctx->pdeleted_messages);
	}
	if (NULL != pctx->pnolonger_messages) {
		eid_array_free(pctx->pnolonger_messages);
	}
	if (NULL != pctx->pread_messags) {
		eid_array_free(pctx->pread_messags);
	}
	if (NULL != pctx->punread_messags) {
		eid_array_free(pctx->punread_messags);
	}
	if (0 != pctx->state_property) {
		mem_file_free(&pctx->f_state_stream);
	}
	proptag_array_free(pctx->pproptags);
	if (NULL != pctx->prestriction) {
		restriction_free(pctx->prestriction);
	}
}

BOOL ICSDOWNCTX_OBJECT::begin_state_stream(uint32_t new_state_prop)
{
	auto pctx = this;
	if (TRUE == pctx->b_started) {
		return FALSE;
	}
	if (0 != pctx->state_property) {
		return FALSE;
	}
	switch (new_state_prop) {
	case META_TAG_IDSETGIVEN:
	case META_TAG_IDSETGIVEN1:
	case META_TAG_CNSETSEEN:
		break;
	case META_TAG_CNSETSEENFAI:
	case META_TAG_CNSETREAD:
		if (SYNC_TYPE_CONTENTS != pctx->sync_type) {
			return FALSE;
		}
		break;
	default:
		return FALSE;
	}
	pctx->state_property = new_state_prop;
	mem_file_init(&pctx->f_state_stream, common_util_get_allocator());
	return TRUE;
}

BOOL ICSDOWNCTX_OBJECT::continue_state_stream(const BINARY *pstream_data)
{
	auto pctx = this;
	if (TRUE == pctx->b_started) {
		return FALSE;
	}
	if (0 == pctx->state_property) {
		return FALSE;
	}
	if (pstream_data->cb != mem_file_write(&pctx->f_state_stream,
		pstream_data->pb, pstream_data->cb)) {
		return FALSE;	
	}
	return TRUE;
}

BOOL ICSDOWNCTX_OBJECT::end_state_stream()
{
	auto pctx = this;
	IDSET *pset;
	BINARY tmp_bin;
	
	if (TRUE == pctx->b_started) {
		return FALSE;
	}
	if (0 == pctx->state_property) {
		return FALSE;
	}
	pset = idset_init(FALSE, REPL_TYPE_GUID);
	if (NULL == pset) {
		return FALSE;
	}
	tmp_bin.cb = mem_file_get_total_length(&pctx->f_state_stream);
	tmp_bin.pv = common_util_alloc(tmp_bin.cb);
	if (tmp_bin.pv == nullptr) {
		idset_free(pset);
		return FALSE;
	}
	mem_file_read(&pctx->f_state_stream, tmp_bin.pv, tmp_bin.cb);
	mem_file_free(&pctx->f_state_stream);
	auto saved_state_property = pctx->state_property;
	pctx->state_property = 0;
	if (FALSE == idset_deserialize(pset, &tmp_bin)) {
		idset_free(pset);
		return FALSE;
	}
	tmp_bin.cb = sizeof(void*);
	tmp_bin.pv = &pctx->pstream->plogon;
	if (FALSE == idset_register_mapping(pset,
		&tmp_bin, common_util_mapping_replica)) {
		idset_free(pset);
		return FALSE;
	}
	if (FALSE == idset_convert(pset)) {
		idset_free(pset);
		return FALSE;
	}
	if (!pctx->pstate->append_idset(saved_state_property, pset)) {
		idset_free(pset);
		return FALSE;
	}
	return TRUE;
}
