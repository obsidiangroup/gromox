// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <cerrno>
#include <cstdint>
#include <gromox/database.h>
#include <gromox/fileio.h>
#include <gromox/mapidefs.h>
#include <gromox/proptag_array.hpp>
#include <gromox/scope.hpp>
#include <gromox/sortorder_set.hpp>
#include "exmdb_server.h"
#include <gromox/restriction.hpp>
#include "common_util.h"
#include <gromox/ext_buffer.hpp>
#include "db_engine.h"
#include <gromox/int_hash.hpp>
#include <gromox/rop_util.hpp>
#include <gromox/propval.hpp>
#include <gromox/util.hpp>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <cstring>
#include <cstdlib>
#include <iconv.h>
#include <cstdio>
#define LLU(x) static_cast<unsigned long long>(x)

using namespace gromox;

namespace {

struct CONDITION_NODE {
	DOUBLE_LIST_NODE node;
	uint32_t proptag;
	void *pvalue;
};

struct CONTENT_ROW_PARAM {
	uint32_t cpid;
	sqlite3 *psqlite;
	sqlite3_stmt *pstmt;
	sqlite3_stmt *pstmt1;
	sqlite3_stmt *pstmt2;
	uint64_t folder_id;
	uint64_t inst_id;
	int row_type;
	const SORTORDER_SET *psorts;
	uint32_t instance_tag;
	uint32_t extremum_tag;
};

struct HIERARCHY_ROW_PARAM {
	uint32_t cpid;
	sqlite3 *psqlite;
	sqlite3_stmt *pstmt;
	uint64_t folder_id;
};

}

typedef BOOL (*TABLE_GET_ROW_PROPERTY)(void*, uint32_t, void **);

static BOOL table_sum_table_count(db_item_ptr &pdb,
	uint32_t table_id, uint32_t *prows)
{
	char sql_string[128];
	
	snprintf(sql_string, arsizeof(sql_string), "SELECT "
			"count(idx) FROM t%u", table_id);
	auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW)
		return FALSE;
	*prows = sqlite3_column_int64(pstmt, 0);
	return TRUE;
}

static uint32_t table_sum_hierarchy(sqlite3 *psqlite,
	uint64_t folder_id, const char *username, BOOL b_depth)
{
	uint32_t count;
	uint32_t permission;
	char sql_string[128];
	
	if (FALSE == b_depth) {
		if (NULL == username) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT count(*) FROM"
			          " folders WHERE parent_id=%llu", LLU(folder_id));
			auto pstmt = gx_sql_prep(psqlite, sql_string);
			if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW)
				return 0;
			count = sqlite3_column_int64(pstmt, 0);
		} else {
			count = 0;
			snprintf(sql_string, arsizeof(sql_string), "SELECT folder_id FROM "
			          "folders WHERE parent_id=%llu", LLU(folder_id));
			auto pstmt = gx_sql_prep(psqlite, sql_string);
			if (pstmt == nullptr)
				return 0;
			while (SQLITE_ROW == sqlite3_step(pstmt)) {
				if (FALSE == common_util_check_folder_permission(
					psqlite, sqlite3_column_int64(pstmt, 0),
					username, &permission)) {
					continue;
				}
				if (!(permission & (frightsReadAny | frightsVisible | frightsOwner)))
					continue;
				count ++;
			}
		}
	} else {
		count = 0;
		snprintf(sql_string, arsizeof(sql_string), "SELECT folder_id FROM "
		          "folders WHERE parent_id=%llu", LLU(folder_id));
		auto pstmt = gx_sql_prep(psqlite, sql_string);
		if (pstmt == nullptr)
			return 0;
		while (SQLITE_ROW == sqlite3_step(pstmt)) {
			if (NULL != username) {
				if (FALSE == common_util_check_folder_permission(
					psqlite, sqlite3_column_int64(pstmt, 0),
					username, &permission)) {
					continue;
				}
				if (!(permission & (frightsReadAny | frightsVisible | frightsOwner)))
					continue;
			}
			count += table_sum_hierarchy(psqlite,
				sqlite3_column_int64(pstmt, 0), username, TRUE);
			count ++;
		}
	}
	return count;
}

static BOOL table_load_hierarchy(sqlite3 *psqlite,
	uint64_t folder_id, const char *username, uint8_t table_flags,
	const RESTRICTION *prestriction, sqlite3_stmt *pstmt, int depth,
	uint32_t *prow_count)
{
	uint64_t folder_id1;
	uint32_t permission;
	char sql_string[256];
	
	if (!exmdb_server_check_private()) {
		snprintf(sql_string, GX_ARRAY_SIZE(sql_string), "SELECT folder_id FROM"
		         " folders WHERE parent_id=%llu AND is_deleted=%u",
		         LLU(folder_id), !!(table_flags & TABLE_FLAG_SOFTDELETES));
	} else if (table_flags & TABLE_FLAG_SOFTDELETES) {
		snprintf(sql_string, arsizeof(sql_string), "SELECT"
		        " folder_id FROM folders WHERE 0");
	} else {
		snprintf(sql_string, arsizeof(sql_string), "SELECT folder_id "
		        "FROM folders WHERE parent_id=%llu", LLU(folder_id));
	}
	auto pstmt1 = gx_sql_prep(psqlite, sql_string);
	if (pstmt1 == nullptr)
		return FALSE;
	while (SQLITE_ROW == sqlite3_step(pstmt1)) {
		folder_id1 = sqlite3_column_int64(pstmt1, 0);
		if (NULL != username) {
			if (FALSE == common_util_check_folder_permission(
				psqlite, folder_id1, username, &permission)) {
				continue;
			}
			if (!(permission & (frightsReadAny | frightsVisible | frightsOwner)))
				continue;
		}
		if (prestriction != nullptr &&
		    !common_util_evaluate_folder_restriction(psqlite, folder_id1, prestriction))
			goto LOAD_SUBFOLDER;
		sqlite3_bind_int64(pstmt, 1, folder_id1);
		sqlite3_bind_int64(pstmt, 2, depth);
		if (SQLITE_DONE != sqlite3_step(pstmt)) {
			return FALSE;
		}
		(*prow_count) ++;
		sqlite3_reset(pstmt);
 LOAD_SUBFOLDER:
		if ((table_flags & TABLE_FLAG_DEPTH) &&
		    !table_load_hierarchy(psqlite, folder_id1, username,
		    table_flags, prestriction, pstmt, depth + 1, prow_count)) {
			return FALSE;
		}
	}
	return TRUE;
}

BOOL exmdb_server_sum_hierarchy(const char *dir,
	uint64_t folder_id, const char *username,
	BOOL b_depth, uint32_t *pcount)
{
	uint64_t fid_val;
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	fid_val = rop_util_get_gc_value(folder_id);
	*pcount = table_sum_hierarchy(pdb->psqlite,
					fid_val, username, b_depth);
	return TRUE;
}
	
BOOL exmdb_server_load_hierarchy_table(const char *dir,
	uint64_t folder_id, const char *username, uint8_t table_flags,
	const RESTRICTION *prestriction, uint32_t *ptable_id,
	uint32_t *prow_count)
{
	uint64_t fid_val;
	uint32_t table_id;
	TABLE_NODE *ptnode;
	char sql_string[256];
	const char *remote_id;
	const GUID *phandle_guid;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	if (FALSE == exmdb_server_check_private()) {
		exmdb_server_set_public_username(username);
	}
	fid_val = rop_util_get_gc_value(folder_id);
	if (NULL == pdb->tables.psqlite) {
		if (SQLITE_OK != sqlite3_open_v2(":memory:", &pdb->tables.psqlite,
			SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, NULL)) {
			return FALSE;
		}
	}
	pdb->tables.last_id ++;
	table_id = pdb->tables.last_id;
	/* begin the transaction */
	sqlite3_exec(pdb->tables.psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	snprintf(sql_string, arsizeof(sql_string), "CREATE TABLE t%u "
		"(idx INTEGER PRIMARY KEY AUTOINCREMENT, "
		"folder_id INTEGER UNIQUE NOT NULL, "
		"depth INTEGER NOT NULL)", table_id);
	if (SQLITE_OK != sqlite3_exec(pdb->tables.psqlite,
		sql_string, NULL, NULL, NULL)) {
		sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	ptnode = me_alloc<TABLE_NODE>();
	if (NULL == ptnode) {
		sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	memset(ptnode, 0, sizeof(TABLE_NODE));
	ptnode->node.pdata = ptnode;
	ptnode->table_id = table_id;
	remote_id = exmdb_server_get_remote_id();
	if (NULL != remote_id) {
		ptnode->remote_id = strdup(remote_id);
		if (NULL == ptnode->remote_id) {
			free(ptnode);
			sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
	}
	ptnode->type = TABLE_TYPE_HIERARCHY;
	ptnode->folder_id = fid_val;
	ptnode->table_flags = table_flags;
	if (table_flags & TABLE_FLAG_SUPPRESSNOTIFICATIONS) {
		phandle_guid = exmdb_server_get_handle();
		if (NULL == phandle_guid) {
			memset(&ptnode->handle_guid, 0, sizeof(GUID));
		} else {
			ptnode->handle_guid = *phandle_guid;
		}
	}
	if (NULL != prestriction) {
		ptnode->prestriction = restriction_dup(prestriction);
		if (NULL == ptnode->prestriction) {
			if (NULL != ptnode->remote_id) {
				free(ptnode->remote_id);
			}
			free(ptnode);
			sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
	}
	snprintf(sql_string, arsizeof(sql_string), "INSERT INTO t%u (folder_id,"
					" depth) VALUES (?, ?)", ptnode->table_id);
	auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt == nullptr) {
		if (NULL != ptnode->prestriction) {
			restriction_free(ptnode->prestriction);
		}
		if (NULL != ptnode->remote_id) {
			free(ptnode->remote_id);
		}
		free(ptnode);
		sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	*prow_count = 0;
	if (FALSE == table_load_hierarchy(pdb->psqlite,
		fid_val, username, table_flags, prestriction,
		pstmt, 1, prow_count)) {
		pstmt.finalize();
		if (NULL != ptnode->prestriction) {
			restriction_free(ptnode->prestriction);
		}
		if (NULL != ptnode->remote_id) {
			free(ptnode->remote_id);
		}
		free(ptnode);
		sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	pstmt.finalize();
	sqlite3_exec(pdb->tables.psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	double_list_append_as_tail(&pdb->tables.table_list, &ptnode->node);
	*ptable_id = ptnode->table_id;
	return TRUE;
}

BOOL exmdb_server_sum_content(const char *dir, uint64_t folder_id,
	BOOL b_fai, BOOL b_deleted, uint32_t *pcount)
{
	uint64_t fid_val;
	char sql_string[256];
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	fid_val = rop_util_get_gc_value(folder_id);
	if (TRUE == exmdb_server_check_private()) {
		snprintf(sql_string, GX_ARRAY_SIZE(sql_string), "SELECT count(*)"
			" FROM messages WHERE parent_fid=%llu AND "
			"is_associated=%u", LLU(fid_val), !!b_fai);
	} else {
		snprintf(sql_string, GX_ARRAY_SIZE(sql_string), "SELECT count(*)"
			" FROM messages WHERE parent_fid=%llu AND "
			"(is_associated=%u AND is_deleted=%u)",
			LLU(fid_val), !!b_fai, !!b_deleted);
	}
	auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr || sqlite3_step(pstmt) != SQLITE_ROW)
		return FALSE;
	*pcount = sqlite3_column_int64(pstmt, 0);
	return TRUE;
}

static void table_condition_list_to_where_clause(
	DOUBLE_LIST *pcondition_list, char *where_clause, int length)
{
	int offset;
	CONDITION_NODE *pcnode;
	DOUBLE_LIST_NODE *pnode;
	
	offset = 0;
	for (pnode=double_list_get_head(pcondition_list); NULL!=pnode;
		pnode=double_list_get_after(pcondition_list, pnode)) {
		pcnode = (CONDITION_NODE*)pnode->pdata;
		if (0 == offset) {
			offset = gx_snprintf(where_clause, length, "WHERE ");
		} else {
			offset += gx_snprintf(where_clause + offset,
						length - offset, " AND ");
		}
		if (NULL == pcnode->pvalue) {
			offset += gx_snprintf(where_clause + offset,
					length - offset, "v%x IS NULL",
					pcnode->proptag);
		} else {
			offset += gx_snprintf(where_clause + offset,
				length - offset, "v%x=?", pcnode->proptag);
		}
	}
	where_clause[offset] = '\0';
}

static BOOL table_load_content(db_item_ptr &pdb, sqlite3 *psqlite,
	const SORTORDER_SET *psorts, int depth, uint64_t parent_id,
	DOUBLE_LIST *pcondition_list, sqlite3_stmt *pstmt_insert,
	uint32_t *pheader_id, sqlite3_stmt *pstmt_update,
	uint32_t *punread_count)
{
	int i;
	int sql_len;
	void *pvalue;
	uint16_t type;
	BOOL b_orderby;
	int bind_index;
	int multi_index;
	BOOL b_extremum;
	uint64_t header_id;
	uint32_t tmp_proptag;
	uint32_t tmp_proptag1;
	char sql_string[1024];
	uint32_t unread_count;
	char where_clause[1024];
	DOUBLE_LIST_NODE *pnode;
	CONDITION_NODE tmp_cnode;
	
	int64_t prev_id = -parent_id;
	table_condition_list_to_where_clause(pcondition_list,
					where_clause, sizeof(where_clause));
	if (depth == psorts->ccategories) {
		multi_index = -1;
		for (i=0; i<psorts->ccategories; i++) {
			if ((psorts->psort[i].type & MVI_FLAG) == MVI_FLAG) {
				tmp_proptag = PROP_TAG(psorts->psort[i].type, psorts->psort[i].propid);
				multi_index = i;
				break;
			}
		}
		if (-1 == multi_index) {
			if (psorts->ccategories > 0) {
				sql_len = gx_snprintf(sql_string, GX_ARRAY_SIZE(sql_string),
					"SELECT message_id, read_state FROM stbl %s",
					where_clause);
			} else {
				sql_len = gx_snprintf(sql_string, GX_ARRAY_SIZE(sql_string),
					"SELECT message_id FROM stbl %s", where_clause);
			}
		} else {
			sql_len = gx_snprintf(sql_string, GX_ARRAY_SIZE(sql_string),
				"SELECT message_id, read_state, inst_num, v%x"
				" FROM stbl %s", tmp_proptag, where_clause);
		}
		b_orderby = FALSE;
		for (i=psorts->ccategories; i<psorts->count; i++) {
			tmp_proptag = PROP_TAG(psorts->psort[i].type, psorts->psort[i].propid);
			if (TABLE_SORT_MAXIMUM_CATEGORY ==
				psorts->psort[i].table_sort ||
				TABLE_SORT_MINIMUM_CATEGORY ==
				psorts->psort[i].table_sort) {
				continue;
			}
			if (FALSE == b_orderby) {
				sql_len += gx_snprintf(sql_string + sql_len,
				           GX_ARRAY_SIZE(sql_string) - sql_len,
							" ORDER BY v%x ", tmp_proptag);
				b_orderby = TRUE;
			} else {
				sql_len += gx_snprintf(sql_string + sql_len,
				           GX_ARRAY_SIZE(sql_string) - sql_len,
							", v%x ", tmp_proptag);
			}
			if (TABLE_SORT_ASCEND ==
				psorts->psort[i].table_sort) {
				sql_len += gx_snprintf(sql_string + sql_len,
				           GX_ARRAY_SIZE(sql_string) - sql_len, " ASC");
			} else {
				sql_len += gx_snprintf(sql_string + sql_len,
				           GX_ARRAY_SIZE(sql_string) - sql_len, " DESC");
			}
		}
		auto pstmt = gx_sql_prep(psqlite, sql_string);
		if (pstmt == nullptr)
			return FALSE;
		bind_index = 1;
		for (i=0,pnode=double_list_get_head(pcondition_list); NULL!=pnode;
			pnode=double_list_get_after(pcondition_list, pnode),i++) {
			if (NULL != ((CONDITION_NODE*)pnode->pdata)->pvalue) {
				type = psorts->psort[i].type;
				if ((psorts->psort[i].type & MVI_FLAG) == MVI_FLAG)
					type &= ~MVI_FLAG;
				if (FALSE == common_util_bind_sqlite_statement(
					pstmt, bind_index, type,
					((CONDITION_NODE*)pnode->pdata)->pvalue)) {
					return FALSE;
				}
				bind_index ++;
			}
		}
		while (SQLITE_ROW == sqlite3_step(pstmt)) {
			if (psorts->ccategories > 0) {
				if (0 == sqlite3_column_int64(pstmt, 1)) {
					(*punread_count) ++;
					/* unread(0) in extremum for message row */
					sqlite3_bind_int64(pstmt_insert, 9, 0);
				} else {
					/* read(1) in extremum for message row */
					sqlite3_bind_int64(pstmt_insert, 9, 1);
				}
			} else {
				sqlite3_bind_null(pstmt_insert, 9);
			}
			sqlite3_bind_int64(pstmt_insert, 1,
				sqlite3_column_int64(pstmt, 0));
			sqlite3_bind_int64(pstmt_insert, 2, CONTENT_ROW_MESSAGE);
			sqlite3_bind_null(pstmt_insert, 3);
			sqlite3_bind_int64(pstmt_insert, 4, parent_id);
			sqlite3_bind_int64(pstmt_insert, 5, depth);
			sqlite3_bind_null(pstmt_insert, 6);
			if (-1 != multi_index) {
				sqlite3_bind_int64(pstmt_insert, 7,
					sqlite3_column_int64(pstmt, 2));
				type = psorts->psort[multi_index].type & ~MVI_FLAG;
				pvalue = common_util_column_sqlite_statement(pstmt, 3, type);
				if (NULL == pvalue) {
					sqlite3_bind_null(pstmt_insert, 8);
				} else {
					if (FALSE == common_util_bind_sqlite_statement(
						pstmt_insert, 8, type, pvalue)) {
						return FALSE;
					}
				}
			} else {
				sqlite3_bind_int64(pstmt_insert, 7, 0);
				sqlite3_bind_null(pstmt_insert, 8);
			}
			sqlite3_bind_int64(pstmt_insert, 10, prev_id);
			if (SQLITE_DONE != sqlite3_step(pstmt_insert)) {
				return FALSE;
			}
			prev_id = sqlite3_last_insert_rowid(pdb->tables.psqlite);
			sqlite3_reset(pstmt_insert);
		}
		return TRUE;
	}
	tmp_proptag = PROP_TAG(psorts->psort[depth].type, psorts->psort[depth].propid);
	if (depth == psorts->ccategories - 1 &&
		psorts->count > psorts->ccategories
		&& (TABLE_SORT_MAXIMUM_CATEGORY ==
		psorts->psort[depth + 1].table_sort ||
		TABLE_SORT_MINIMUM_CATEGORY ==
		psorts->psort[depth + 1].table_sort)) {
		b_extremum = TRUE;
		tmp_proptag1 = PROP_TAG(psorts->psort[depth+1].type, psorts->psort[depth+1].propid);
		if (TABLE_SORT_MAXIMUM_CATEGORY ==
			psorts->psort[depth + 1].table_sort) {
			sql_len = gx_snprintf(sql_string, GX_ARRAY_SIZE(sql_string),
					"SELECT v%x, count(*), max(v%x) AS max_field "
					"FROM stbl %s GROUP BY v%x ORDER BY max_field",
					tmp_proptag, tmp_proptag1, where_clause,
					tmp_proptag);
		} else {
			sql_len = gx_snprintf(sql_string, GX_ARRAY_SIZE(sql_string),
					"SELECT v%x, count(*), min(v%x) AS max_field "
					"FROM stbl %s GROUP BY v%x ORDER BY max_field",
					tmp_proptag, tmp_proptag1, where_clause,
					tmp_proptag);	
		}
	} else {
		b_extremum = FALSE;
		sql_len = gx_snprintf(sql_string, GX_ARRAY_SIZE(sql_string),
				"SELECT v%x, count(*) FROM stbl %s GROUP"
				" BY v%x ORDER BY v%x", tmp_proptag,
				where_clause, tmp_proptag, tmp_proptag);
	}
	if (TABLE_SORT_ASCEND == psorts->psort[depth].table_sort) {
		gx_snprintf(sql_string + sql_len,
		           GX_ARRAY_SIZE(sql_string) - sql_len, " ASC");
	} else {
		gx_snprintf(sql_string + sql_len,
		           GX_ARRAY_SIZE(sql_string) - sql_len, " DESC");
	}
	auto pstmt = gx_sql_prep(psqlite, sql_string);
	if (pstmt == nullptr)
		return FALSE;
	bind_index = 1;
	for (i=0,pnode=double_list_get_head(pcondition_list); NULL!=pnode;
		pnode=double_list_get_after(pcondition_list, pnode),i++) {
		if (NULL != ((CONDITION_NODE*)pnode->pdata)->pvalue) {
			type = psorts->psort[i].type;
			if ((psorts->psort[i].type & MVI_FLAG) == MVI_FLAG)
				type &= ~MVI_FLAG;
			if (FALSE == common_util_bind_sqlite_statement(
				pstmt, bind_index, type,
				((CONDITION_NODE*)pnode->pdata)->pvalue)) {
				return FALSE;
			}
			bind_index ++;
		}
	}
	tmp_cnode.node.pdata = &tmp_cnode;
	double_list_append_as_tail(pcondition_list, &tmp_cnode.node);
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		(*pheader_id) ++;
		header_id = *pheader_id | 0x100000000000000ULL;
		sqlite3_bind_int64(pstmt_insert, 1, header_id);
		sqlite3_bind_int64(pstmt_insert, 2, CONTENT_ROW_HEADER);
		sqlite3_bind_int64(pstmt_insert, 3, depth < psorts->cexpanded);
		sqlite3_bind_int64(pstmt_insert, 4, parent_id);
		sqlite3_bind_int64(pstmt_insert, 5, depth);
		/* total messages */
		sqlite3_bind_int64(pstmt_insert, 6,
			sqlite3_column_int64(pstmt, 1));
		sqlite3_bind_int64(pstmt_insert, 7, 0);
		type = psorts->psort[depth].type;
		if ((type & MVI_FLAG) == MVI_FLAG)
			type &= ~MVI_FLAG;
		if (TRUE == b_extremum && NULL != 
			(pvalue = common_util_column_sqlite_statement(
			pstmt, 2, psorts->psort[depth + 1].type))) {
			if (FALSE == common_util_bind_sqlite_statement(pstmt_insert,
				9, psorts->psort[depth + 1].type, pvalue)) {
				return FALSE;
			}
		} else {
			sqlite3_bind_null(pstmt_insert, 9);
		}
		/* pvalue will be recorded in condition list */
		pvalue = common_util_column_sqlite_statement(pstmt, 0, type);
		if (NULL == pvalue) {
			sqlite3_bind_null(pstmt_insert, 8);
		} else {
			if (FALSE == common_util_bind_sqlite_statement(
				pstmt_insert, 8, type, pvalue)) {
				return FALSE;
			}
		}
		sqlite3_bind_int64(pstmt_insert, 10, prev_id);
		if (SQLITE_DONE != sqlite3_step(pstmt_insert)) {
			return FALSE;
		}
		prev_id = sqlite3_last_insert_rowid(pdb->tables.psqlite);
		sqlite3_reset(pstmt_insert);
		tmp_cnode.proptag = tmp_proptag;
		unread_count = 0;
		tmp_cnode.pvalue = pvalue;
		if (FALSE == table_load_content(pdb, psqlite, psorts,
			depth + 1, prev_id, pcondition_list, pstmt_insert,
			pheader_id, pstmt_update, &unread_count)) {
			return FALSE;
		}
		sqlite3_bind_int64(pstmt_update, 1, unread_count);
		sqlite3_bind_int64(pstmt_update, 2, prev_id);
		if (SQLITE_DONE != sqlite3_step(pstmt_update)) {
			return FALSE;
		}
		sqlite3_reset(pstmt_update);
		*punread_count += unread_count;
	}
	double_list_remove(pcondition_list, &tmp_cnode.node);
	return TRUE;
}

/* under public mode username always available for read state */
static BOOL table_load_content_table(db_item_ptr &pdb, uint32_t cpid,
	uint64_t fid_val, const char *username, uint8_t table_flags,
	const RESTRICTION *prestriction, const SORTORDER_SET *psorts,
	uint32_t *ptable_id, uint32_t *prow_count)
{
	int depth;
	int sql_len, multi_index = 0;
	size_t tag_count = 0;
	void *pvalue;
	uint16_t type;
	BOOL b_search;
	uint64_t row_id;
	uint64_t prev_id;
	sqlite3 *psqlite;
	uint64_t mid_val;
	uint32_t table_id;
	TABLE_NODE *ptnode;
	BOOL b_conversation;
	uint64_t parent_fid;
	uint64_t last_row_id;
	uint32_t tmp_proptag;
	char tmp_string[128];
	char sql_string[1024];
	const char *remote_id;
	DOUBLE_LIST value_list;
	uint32_t tmp_proptags[16];
	RESTRICTION_PROPERTY *pres = nullptr;
	
	b_conversation = FALSE;
	if ((table_flags & TABLE_FLAG_CONVERSATIONMEMBERS) &&
	    prestriction != nullptr &&
	    prestriction->rt == RES_PROPERTY &&
	    (pres = prestriction->prop) != nullptr &&
	    pres->relop == RELOP_EQ &&
	    pres->proptag == PROP_TAG_CONVERSATIONID &&
	    static_cast<BINARY *>(pres->propval.pvalue)->cb == 16)
		b_conversation = TRUE;
	if (NULL != psorts && psorts->count >
		sizeof(tmp_proptags)/sizeof(uint32_t)) {
		return FALSE;	
	}
	b_search = FALSE;
	if (FALSE == exmdb_server_check_private()) {
		exmdb_server_set_public_username(username);
	} else {
		snprintf(sql_string, GX_ARRAY_SIZE(sql_string), "SELECT is_search FROM"
		          " folders WHERE folder_id=%llu", LLU(fid_val));
		auto pstmt = gx_sql_prep(pdb->psqlite, sql_string);
		if (pstmt == nullptr)
			return FALSE;
		if (SQLITE_ROW != sqlite3_step(pstmt)) {
			return TRUE;
		}
		if (0 != sqlite3_column_int64(pstmt, 0)) {
			b_search = TRUE;
		}
	}
	if (pdb->tables.psqlite == nullptr &&
	    sqlite3_open_v2(":memory:", &pdb->tables.psqlite,
	    SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr) != SQLITE_OK)
		return FALSE;
	if (0 == *ptable_id) {
		pdb->tables.last_id ++;
		table_id = pdb->tables.last_id;
	} else {
		table_id = *ptable_id;
	}
	sqlite3_exec(pdb->tables.psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	snprintf(sql_string, arsizeof(sql_string), "CREATE TABLE t%u "
		"(row_id INTEGER PRIMARY KEY AUTOINCREMENT, "
		"idx INTEGER UNIQUE DEFAULT NULL, "
		"prev_id INTEGER UNIQUE DEFAULT NULL, "
		"inst_id INTEGER NOT NULL, "
		"row_type INTEGER NOT NULL, "
		"row_stat INTEGER DEFAULT NULL, "	/* expanded(1) or collapsed(0) */
		"parent_id INTEGER DEFAULT NULL, "
		"depth INTEGER NOT NULL, "
		"count INTEGER DEFAULT NULL, "
		"unread INTEGER DEFAULT NULL, "
		"inst_num INTEGER NOT NULL, "
		"value NONE DEFAULT NULL, "
		"extremum NONE DEFAULT NULL)",		/* read(unread) for message row */
		table_id);
	if (SQLITE_OK != sqlite3_exec(pdb->tables.psqlite,
		sql_string, NULL, NULL, NULL)) {
		sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	if (NULL != psorts && psorts->ccategories > 0) {
		snprintf(sql_string, arsizeof(sql_string), "CREATE UNIQUE INDEX t%u_1 ON "
			"t%u (inst_id, inst_num)", table_id, table_id);
		if (SQLITE_OK != sqlite3_exec(pdb->tables.psqlite,
			sql_string, NULL, NULL, NULL)) {
			sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
		snprintf(sql_string, arsizeof(sql_string), "CREATE INDEX t%u_2 ON"
			" t%u (parent_id)", table_id, table_id);
		if (SQLITE_OK != sqlite3_exec(pdb->tables.psqlite,
			sql_string, NULL, NULL, NULL)) {
			sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
		snprintf(sql_string, arsizeof(sql_string), "CREATE INDEX t%u_3 ON t%u"
			" (parent_id, value)", table_id, table_id);
		if (SQLITE_OK != sqlite3_exec(pdb->tables.psqlite,
			sql_string, NULL, NULL, NULL)) {
			sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
	}
	ptnode = me_alloc<TABLE_NODE>();
	if (NULL == ptnode) {
		sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	xstmt pstmt, pstmt1;
	psqlite = NULL;
	memset(ptnode, 0, sizeof(TABLE_NODE));
	ptnode->node.pdata = ptnode;
	ptnode->table_id = table_id;
	remote_id = exmdb_server_get_remote_id();
	bool all_ok = false;
	auto cl_0 = make_scope_exit([&]() {
		if (all_ok)
			return;
		pstmt.finalize();
		pstmt1.finalize();
		if (psqlite != nullptr) {
			sqlite3_exec(psqlite, "ROLLBACK", nullptr, nullptr, nullptr);
			sqlite3_close(psqlite);
		}
		if (ptnode->psorts != nullptr)
			sortorder_set_free(ptnode->psorts);
		if (ptnode->prestriction != nullptr)
			restriction_free(ptnode->prestriction);
		if (ptnode->username != nullptr)
			free(ptnode->username);
		if (ptnode->remote_id != nullptr)
			free(ptnode->remote_id);
		free(ptnode);
		sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", nullptr, nullptr, nullptr);
	});
	if (NULL != remote_id) {
		ptnode->remote_id = strdup(remote_id);
		if (NULL == ptnode->remote_id) {
			return false;
		}
	}
	ptnode->type = TABLE_TYPE_CONTENT;
	ptnode->folder_id = fid_val;
	ptnode->table_flags = table_flags;
	ptnode->b_search = b_search;
	ptnode->cpid = cpid;
	if (FALSE == exmdb_server_check_private()) {
		ptnode->username = strdup(username);
		if (NULL == ptnode->username) {
			return false;
		}
	}
	if (NULL != prestriction) {
		ptnode->prestriction = restriction_dup(prestriction);
		if (NULL == ptnode->prestriction) {
			return false;
		}
	}
	if (NULL != psorts) {
		ptnode->psorts = sortorder_set_dup(psorts);
		if (NULL == ptnode->psorts) {
			return false;
		}
		if (SQLITE_OK != sqlite3_open_v2(":memory:", &psqlite,
			SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, NULL)) {
			return false;
		}
		sqlite3_exec(psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
		sql_len = snprintf(sql_string, arsizeof(sql_string), "CREATE"
			" TABLE stbl (message_id INTEGER");
		tag_count = 0;
		for (size_t i = 0; i < psorts->count; ++i) {
			tmp_proptag = PROP_TAG(psorts->psort[i].type, psorts->psort[i].propid);
			if (TABLE_SORT_MAXIMUM_CATEGORY ==
				psorts->psort[i].table_sort ||
				TABLE_SORT_MINIMUM_CATEGORY ==
				psorts->psort[i].table_sort) {
				ptnode->extremum_tag = tmp_proptag;
			}
			tmp_proptags[tag_count] = tmp_proptag;
			/* check if proptag is already in the field list */
			if (i >= psorts->ccategories) {
				size_t j;
				for (j = 0; j < tag_count; ++j)
					if (tmp_proptags[j] == tmp_proptag) {
						break;
					}
				if (j < tag_count)
					continue;
			}
			tag_count ++;
			type = psorts->psort[i].type;
			if ((type & MVI_FLAG) == MVI_FLAG) {
				type &= ~MVI_FLAG;
				ptnode->instance_tag = tmp_proptag;
				multi_index = i + 2;
			}
			switch (type) {
			case PT_STRING8:
			case PT_UNICODE:
				sql_len += gx_snprintf(sql_string + sql_len,
				           GX_ARRAY_SIZE(sql_string) - sql_len,
							", v%x TEXT COLLATE NOCASE", tmp_proptag);
				break;
			case PT_FLOAT:
			case PT_DOUBLE:
			case PT_APPTIME:
				sql_len += gx_snprintf(sql_string + sql_len,
				           GX_ARRAY_SIZE(sql_string) - sql_len,
							", v%x REAL", tmp_proptag);
				break;
			case PT_CURRENCY:
			case PT_I8:
			case PT_SYSTIME:
			case PT_SHORT:
			case PT_LONG:
			case PT_BOOLEAN:
				sql_len += gx_snprintf(sql_string + sql_len,
				           GX_ARRAY_SIZE(sql_string) - sql_len,
							", v%x INTEGER", tmp_proptag);
				break;
			case PT_CLSID:
			case PT_SVREID:
			case PT_OBJECT:
			case PT_BINARY:
				sql_len += gx_snprintf(sql_string + sql_len,
				           GX_ARRAY_SIZE(sql_string) - sql_len,
							", v%x BLOB", tmp_proptag);
				break;
			default:
				return false;
			}
		}
		if (psorts->ccategories > 0) {
			sql_len += gx_snprintf(sql_string + sql_len,
			           GX_ARRAY_SIZE(sql_string) - sql_len,
						", read_state INTEGER");
		}
		if (0 != ptnode->instance_tag) {
			sql_len += gx_snprintf(sql_string + sql_len,
			           GX_ARRAY_SIZE(sql_string) - sql_len,
						", inst_num INTEGER");
		}
		sql_string[sql_len] = ')';
		sql_len ++;
		sql_string[sql_len] = '\0';
		if (SQLITE_OK != sqlite3_exec(psqlite,
			sql_string, NULL, NULL, NULL)) {
			return false;
		}
		for (size_t i = 0; i < tag_count; ++i) {
			tmp_proptag = tmp_proptags[i];
			snprintf(sql_string, GX_ARRAY_SIZE(sql_string),
			         "CREATE INDEX stbl_%zu ON stbl (v%x)",
			         i, tmp_proptag);
			if (SQLITE_OK != sqlite3_exec(psqlite,
				sql_string, NULL, NULL, NULL)) {
				return false;
			}
		}
		if (0 == ptnode->instance_tag) {
			snprintf(sql_string, arsizeof(sql_string), "CREATE UNIQUE INDEX t%u_4 "
					"ON t%u (inst_id)", table_id, table_id);
		} else {
			snprintf(sql_string, arsizeof(sql_string), "CREATE INDEX t%u_4 "
				"ON t%u (inst_id)", table_id, table_id);
		}
		if (SQLITE_OK != sqlite3_exec(pdb->tables.psqlite,
			sql_string, NULL, NULL, NULL)) {
			return false;
		}
		sql_len = snprintf(sql_string, arsizeof(sql_string), "INSERT INTO stbl VALUES (?");
		for (size_t i = 0; i < tag_count; ++i)
			sql_len += gx_snprintf(sql_string + sql_len,
			           GX_ARRAY_SIZE(sql_string) - sql_len, ", ?");
		if (psorts->ccategories > 0) {
			sql_len += gx_snprintf(sql_string + sql_len,
			           GX_ARRAY_SIZE(sql_string) - sql_len, ", ?");
		}
		if (0 != ptnode->instance_tag) {
			sql_len += gx_snprintf(sql_string + sql_len,
			           GX_ARRAY_SIZE(sql_string) - sql_len, ", ?");
		}
		sql_string[sql_len] = ')';
		sql_len ++;
		sql_string[sql_len] = '\0';
		pstmt1 = gx_sql_prep(psqlite, sql_string);
		if (pstmt1 == nullptr)
			return false;
	} else {
		snprintf(sql_string, arsizeof(sql_string), "INSERT INTO t%u (inst_id,"
			" prev_id, row_type, depth, inst_num, idx) VALUES "
			"(?, ?, %u, 0, 0, ?)", table_id, CONTENT_ROW_MESSAGE);
		pstmt1 = gx_sql_prep(pdb->tables.psqlite, sql_string);
		if (pstmt1 == nullptr)
			return false;
	}
	if (TRUE == exmdb_server_check_private()) {
		if (table_flags & TABLE_FLAG_SOFTDELETES) {
			strcpy(sql_string, "SELECT message_id FROM messages WHERE 0");
		} else if (table_flags & TABLE_FLAG_ASSOCIATED) {
			if (FALSE == b_search) {
				snprintf(sql_string, arsizeof(sql_string), "SELECT message_id "
				        "FROM messages WHERE parent_fid=%llu "
				        "AND is_associated=1", LLU(fid_val));
			} else {
				snprintf(sql_string, arsizeof(sql_string), "SELECT "
				        "messages.message_id FROM messages"
				        " JOIN search_result ON "
				        "search_result.folder_id=%llu AND "
				        "search_result.message_id=messages.message_id"
				        " AND messages.is_associated=1", LLU(fid_val));
			}
		} else if (table_flags & TABLE_FLAG_CONVERSATIONMEMBERS) {
			if (TRUE == b_conversation) {
				encode_hex_binary(((BINARY *)pres->propval.pvalue)->pb,
					16, tmp_string, sizeof(tmp_string));
				snprintf(sql_string, arsizeof(sql_string), "SELECT message_id "
				        "FROM message_properties WHERE proptag=%u AND"
				        " propval=x'%s'", PROP_TAG_CONVERSATIONID,
				        tmp_string);
			} else {
				strcpy(sql_string, "SELECT message_id"
				       " FROM messages WHERE parent_fid IS NOT NULL"
				       " AND is_associated=0");
			}
		} else if (!b_search) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT message_id "
			        "FROM messages WHERE parent_fid=%llu "
			        "AND is_associated=0", LLU(fid_val));
		} else {
			snprintf(sql_string, arsizeof(sql_string), "SELECT "
			        "messages.message_id FROM messages"
			        " JOIN search_result ON "
			        "search_result.folder_id=%llu AND "
			        "search_result.message_id=messages.message_id"
			        " AND messages.is_associated=0", LLU(fid_val));
		}
	} else if (!(table_flags & TABLE_FLAG_CONVERSATIONMEMBERS)) {
		gx_snprintf(sql_string, GX_ARRAY_SIZE(sql_string),
		            "SELECT message_id "
		            "FROM messages WHERE parent_fid=%llu "
		            " AND is_deleted=%u AND is_associated=%u",
		            LLU(fid_val),
		            !!(table_flags & TABLE_FLAG_SOFTDELETES),
		            !!(table_flags & TABLE_FLAG_ASSOCIATED));
	} else if (b_conversation) {
		encode_hex_binary(((BINARY *)pres->propval.pvalue)->pb,
			16, tmp_string, sizeof(tmp_string));
		gx_snprintf(sql_string, GX_ARRAY_SIZE(sql_string),
		            "SELECT message_properties.message_id "
		            "FROM message_properties JOIN messages ON "
		            "messages.message_id=message_properties.message_id"
		            " WHERE message_properties.proptag=%u AND"
		            " message_properties.propval=x'%s' AND "
		            "messages.is_deleted=%u", PROP_TAG_CONVERSATIONID,
		            tmp_string, !!(table_flags & TABLE_FLAG_SOFTDELETES));
	} else {
		gx_snprintf(sql_string, GX_ARRAY_SIZE(sql_string),
		            "SELECT message_id"
		            " FROM messages WHERE parent_fid IS NOT NULL"
		            " AND is_associated=0 AND is_deleted=%u",
		            !!(table_flags & TABLE_FLAG_SOFTDELETES));
	}
	pstmt = gx_sql_prep(pdb->psqlite, sql_string);
	if (pstmt == nullptr)
		return false;
	last_row_id = 0;
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		mid_val = sqlite3_column_int64(pstmt, 0);
		if (TRUE == b_conversation) {
			if (TRUE == common_util_check_message_associated(
				pdb->psqlite, mid_val)) {
				continue;
			}
			if (FALSE == common_util_get_message_parent_folder(
				pdb->psqlite, mid_val, &parent_fid)) {
				return false;
			}
			if (0 == parent_fid) {
				continue;
			}
		} else if (prestriction != nullptr &&
		    !common_util_evaluate_message_restriction(pdb->psqlite, cpid, mid_val, prestriction)) {
			continue;
		}
		sqlite3_bind_int64(pstmt1, 1, mid_val);
		if (NULL != psorts) {
			for (size_t i = 0; i < tag_count; ++i) {
				tmp_proptag = tmp_proptags[i];
				if (tmp_proptag == ptnode->instance_tag) {
					continue;
				}
				if (FALSE == common_util_get_property(
					MESSAGE_PROPERTIES_TABLE, mid_val,
					cpid, pdb->psqlite, tmp_proptag,
					&pvalue)) {
					return false;
				}
				if (NULL == pvalue) {
					sqlite3_bind_null(pstmt1, i + 2);
				} else if (!common_util_bind_sqlite_statement(pstmt1, i + 2, PROP_TYPE(tmp_proptag), pvalue)) {
					return false;
				}
			}
			if (psorts->ccategories > 0) {
				if (!common_util_get_property(MESSAGE_PROPERTIES_TABLE,
				    mid_val, 0, pdb->psqlite, PR_READ, &pvalue))
					return false;
				sqlite3_bind_int64(pstmt1, tag_count + 2,
					pvalue == nullptr || *static_cast<uint8_t *>(pvalue) == 0 ? 0 : 1);
			}
			/* inssert all instances into stbl */
			if (0 != ptnode->instance_tag) {
				type = PROP_TYPE(ptnode->instance_tag);
				type &= ~MV_INSTANCE;
				if (!common_util_get_property(MESSAGE_PROPERTIES_TABLE,
				    mid_val, cpid, pdb->psqlite,
				    ptnode->instance_tag & ~MV_INSTANCE, &pvalue))
					return false;
				if (NULL == pvalue) {
 BIND_NULL_INSTANCE:
					sqlite3_bind_null(pstmt1, multi_index);
					sqlite3_bind_int64(pstmt1, tag_count + 3, 0);
					if (SQLITE_DONE != sqlite3_step(pstmt1)) {
						return false;
					}
					sqlite3_reset(pstmt1);
					continue;
				}
				switch (type) {
				case PT_MV_SHORT: {
					auto sa = static_cast<SHORT_ARRAY *>(pvalue);
					if (sa->count == 0)
						goto BIND_NULL_INSTANCE;
					for (size_t i = 0; i < sa->count; ++i) {
						if (FALSE == common_util_bind_sqlite_statement(
						    pstmt1, multi_index, PT_SHORT, &sa->ps[i]))
							return false;
						sqlite3_bind_int64(pstmt1,
							tag_count + 3, i + 1);
						if (SQLITE_DONE != sqlite3_step(pstmt1)) {
							return false;
						}
						sqlite3_reset(pstmt1);
					}
					break;
				}
				case PT_MV_LONG: {
					auto la = static_cast<LONG_ARRAY *>(pvalue);
					if (la->count == 0)
						goto BIND_NULL_INSTANCE;
					for (size_t i = 0; i < la->count; ++i) {
						if (FALSE == common_util_bind_sqlite_statement(
						    pstmt1, multi_index, PT_LONG, &la->pl[i]))
							return false;
						sqlite3_bind_int64(pstmt1,
							tag_count + 3, i + 1);
						if (SQLITE_DONE != sqlite3_step(pstmt1)) {
							return false;
						}
						sqlite3_reset(pstmt1);
					}
					break;
				}
				case PT_MV_I8: {
					auto la = static_cast<LONGLONG_ARRAY *>(pvalue);
					if (la->count == 0)
						goto BIND_NULL_INSTANCE;
					for (size_t i = 0; i < la->count; ++i) {
						if (FALSE == common_util_bind_sqlite_statement(
						    pstmt1, multi_index, PT_I8, &la->pll[i]))
							return false;
						sqlite3_bind_int64(pstmt1,
							tag_count + 3, i + 1);
						if (SQLITE_DONE != sqlite3_step(pstmt1)) {
							return false;
						}
						sqlite3_reset(pstmt1);
					}
					break;
				}
				case PT_MV_STRING8:
				case PT_MV_UNICODE: {
					auto sa = static_cast<STRING_ARRAY *>(pvalue);
					if (sa->count == 0)
						goto BIND_NULL_INSTANCE;
					for (size_t i = 0; i < sa->count; ++i) {
						if (FALSE == common_util_bind_sqlite_statement(
						    pstmt1, multi_index, PT_STRING8, &sa->ppstr[i]))
							return false;
						sqlite3_bind_int64(pstmt1,
							tag_count + 3, i + 1);
						if (SQLITE_DONE != sqlite3_step(pstmt1)) {
							return false;
						}
						sqlite3_reset(pstmt1);
					}
					break;
				}
				case PT_MV_CLSID: {
					auto ga = static_cast<GUID_ARRAY *>(pvalue);
					if (ga->count == 0)
						goto BIND_NULL_INSTANCE;
					for (size_t i = 0; i < ga->count; ++i) {
						if (FALSE == common_util_bind_sqlite_statement(
						    pstmt1, multi_index, PT_CLSID, &ga->pguid[i]))
							return false;
						sqlite3_bind_int64(pstmt1,
							tag_count + 3, i + 1);
						if (SQLITE_DONE != sqlite3_step(pstmt1)) {
							return false;
						}
						sqlite3_reset(pstmt1);
					}
					break;
				}
				case PT_MV_BINARY: {
					auto ba = static_cast<BINARY_ARRAY *>(pvalue);
					if (ba->count == 0)
						goto BIND_NULL_INSTANCE;
					for (size_t i = 0; i < ba->count; ++i) {
						if (FALSE == common_util_bind_sqlite_statement(
						    pstmt1, multi_index, PT_BINARY, ba->pbin + i))
							return false;
						sqlite3_bind_int64(pstmt1,
							tag_count + 3, i + 1);
						if (SQLITE_DONE != sqlite3_step(pstmt1)) {
							return false;
						}
						sqlite3_reset(pstmt1);
					}
					break;
				}
				default:
					return false;
				}
				continue;
			}
		} else {
			sqlite3_bind_int64(pstmt1, 2, last_row_id);
			sqlite3_bind_int64(pstmt1, 3, last_row_id + 1);
		}
		if (SQLITE_DONE != sqlite3_step(pstmt1)) {
			return false;
		}
		if (NULL == psorts) {
			last_row_id = sqlite3_last_insert_rowid(pdb->tables.psqlite);
		}
		sqlite3_reset(pstmt1);
	}
	if (NULL != psorts) {
		sqlite3_exec(psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
		sqlite3_exec(psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	}
	pstmt.finalize();
	pstmt1.finalize();
	if (NULL != psorts) {
		snprintf(sql_string, arsizeof(sql_string), "INSERT INTO t%u "
			    "(inst_id, row_type, row_stat, parent_id, depth, "
			    "count, inst_num, value, extremum, prev_id) VALUES"
			    " (?, ?, ?, ?, ?, ?, ?, ?, ?, ?)", table_id);
		pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
		if (pstmt == nullptr)
			return false;
		snprintf(sql_string, arsizeof(sql_string), "UPDATE t%u SET"
		        " unread=? WHERE row_id=?", table_id);
		pstmt1 = gx_sql_prep(pdb->tables.psqlite, sql_string);
		if (pstmt1 == nullptr)
			return false;
		double_list_init(&value_list);
		uint32_t unread_count = 0;
		if (FALSE == table_load_content(pdb,
			psqlite, psorts, 0, 0, &value_list, pstmt,
			&ptnode->header_id, pstmt1, &unread_count)) {
			return false;
		}
		pstmt.finalize();
		pstmt1.finalize();
		sqlite3_exec(psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
		sqlite3_close(psqlite);
		psqlite = NULL;
		/* index the content table */
		if (psorts->ccategories > 0) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT row_id,"
			        " row_type, row_stat, depth, prev_id FROM"
			        " t%u ORDER BY row_id", table_id);
			pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
			if (pstmt == nullptr)
				return false;
			snprintf(sql_string, arsizeof(sql_string), "UPDATE t%u SET "
			        "idx=? WHERE row_id=?", table_id);
			pstmt1 = gx_sql_prep(pdb->tables.psqlite, sql_string);
			if (pstmt1 == nullptr)
				return false;
			size_t i = 1;
			prev_id = 0;
			while (SQLITE_ROW == sqlite3_step(pstmt)) {
				if (0 != prev_id &&
					depth < sqlite3_column_int64(pstmt, 3) &&
				    gx_sql_col_uint64(pstmt, 4) != prev_id)
					continue;
				row_id = sqlite3_column_int64(pstmt, 0);
				if (CONTENT_ROW_HEADER == sqlite3_column_int64(pstmt, 1)) {
					if (0 == sqlite3_column_int64(pstmt, 2)) {
						prev_id = row_id;
						depth = sqlite3_column_int64(pstmt, 3);
					} else {
						prev_id = 0;
					}
				}
				sqlite3_bind_int64(pstmt1, 1, i);
				sqlite3_bind_int64(pstmt1, 2, row_id);
				if (SQLITE_DONE != sqlite3_step(pstmt1)) {
					return false;
				}
				sqlite3_reset(pstmt1);
				i ++;
			}
			pstmt.finalize();
			pstmt1.finalize();
		} else {
			snprintf(sql_string, arsizeof(sql_string), "UPDATE t%u SET idx=row_id", table_id);
			if (SQLITE_OK != sqlite3_exec(pdb->tables.psqlite,
				sql_string, NULL, NULL, NULL)) {
				return false;
			}
		}
	}
	all_ok = true;
	sqlite3_exec(pdb->tables.psqlite,
		"COMMIT TRANSACTION", NULL, NULL, NULL);
	double_list_append_as_tail(&pdb->tables.table_list, &ptnode->node);
	if (0 == *ptable_id) {
		*ptable_id = table_id;
	}
	*prow_count = 0;
	table_sum_table_count(pdb, table_id, prow_count); 
	return TRUE;
}

BOOL exmdb_server_load_content_table(const char *dir, uint32_t cpid,
	uint64_t folder_id, const char *username, uint8_t table_flags,
	const RESTRICTION *prestriction, const SORTORDER_SET *psorts,
	uint32_t *ptable_id, uint32_t *prow_count)
{
	uint64_t fid_val;
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	*ptable_id = 0;
	fid_val = rop_util_get_gc_value(folder_id);
	if (FALSE == table_load_content_table(pdb, cpid,
		fid_val, username, table_flags, prestriction,
		psorts, ptable_id, prow_count)) {
		return FALSE;	
	}
	return TRUE;
}

BOOL exmdb_server_reload_content_table(const char *dir, uint32_t table_id)
{
	BOOL b_result;
	uint32_t row_count;
	TABLE_NODE *ptnode;
	char sql_string[128];
	DOUBLE_LIST_NODE *pnode;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	for (pnode=double_list_get_head(&pdb->tables.table_list); NULL!=pnode;
		pnode=double_list_get_after(&pdb->tables.table_list, pnode)) {
		if (TABLE_TYPE_CONTENT == ((TABLE_NODE*)pnode->pdata)->type
			&& table_id == ((TABLE_NODE*)pnode->pdata)->table_id) {
			double_list_remove(&pdb->tables.table_list, pnode);
			break;
		}
	}
	if (NULL == pnode) {
		return TRUE;
	}
	ptnode = (TABLE_NODE*)pnode->pdata;
	snprintf(sql_string, arsizeof(sql_string), "DROP TABLE t%u", table_id);
	sqlite3_exec(pdb->tables.psqlite, sql_string, NULL, NULL, NULL);
	b_result = table_load_content_table(pdb, ptnode->cpid,
			ptnode->folder_id, ptnode->username, ptnode->table_flags,
			ptnode->prestriction, ptnode->psorts, &table_id,
			&row_count);
	if (NULL != ptnode->remote_id) {
		free(ptnode->remote_id);
	}
	if (NULL != ptnode->username) {
		free(ptnode->username);
	}
	if (NULL != ptnode->prestriction) {
		restriction_free(ptnode->prestriction);
	}
	if (NULL != ptnode->psorts) {
		sortorder_set_free(ptnode->psorts);
	}
	free(ptnode);
	db_engine_notify_content_table_reload(pdb, table_id);
	return b_result;
}

static BOOL table_load_permissions(sqlite3 *psqlite,
	uint64_t folder_id, sqlite3_stmt *pstmt, uint32_t *prow_count)
{
	BOOL b_default;
	BOOL b_anonymous;
	uint64_t member_id;
	char sql_string[256];
	const char *pusername;
	
	snprintf(sql_string, arsizeof(sql_string), "SELECT member_id, username"
	          " FROM permissions WHERE folder_id=%llu", LLU(folder_id));
	auto pstmt1 = gx_sql_prep(psqlite, sql_string);
	if (pstmt1 == nullptr)
		return FALSE;
	b_default = FALSE;
	b_anonymous = FALSE;
	while (SQLITE_ROW == sqlite3_step(pstmt1)) {
		member_id = sqlite3_column_int64(pstmt1, 0);
		sqlite3_bind_int64(pstmt, 1, member_id);
		if (SQLITE_DONE != sqlite3_step(pstmt)) {
			return FALSE;
		}
		(*prow_count) ++;
		sqlite3_reset(pstmt);
		if (SQLITE_NULL == sqlite3_column_type(pstmt1, 1)) {
			return FALSE;
		}
		pusername = reinterpret_cast<const char *>(sqlite3_column_text(pstmt1, 1));
		if ('\0' == pusername[0]) {
			b_anonymous = TRUE;
		} else if (0 == strcasecmp("default", pusername)) {
			b_default = TRUE;
		}
	}
	if (FALSE == b_default) {
		sqlite3_bind_int64(pstmt, 1, 0);
		if (SQLITE_DONE != sqlite3_step(pstmt)) {
			return FALSE;
		}
		(*prow_count) ++;
		sqlite3_reset(pstmt);
	}
	if (FALSE == b_anonymous) {
		sqlite3_bind_int64(pstmt, 1, -1);
		if (SQLITE_DONE != sqlite3_step(pstmt)) {
			return FALSE;
		}
		(*prow_count) ++;
		sqlite3_reset(pstmt);
	}
	return TRUE;
}

BOOL exmdb_server_load_permission_table(const char *dir,
	uint64_t folder_id, uint8_t table_flags,
	uint32_t *ptable_id, uint32_t *prow_count)
{
	uint64_t fid_val;
	uint32_t table_id;
	TABLE_NODE *ptnode;
	char sql_string[256];
	const char *remote_id;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	fid_val = rop_util_get_gc_value(folder_id);
	if (NULL == pdb->tables.psqlite) {
		if (SQLITE_OK != sqlite3_open_v2(":memory:", &pdb->tables.psqlite,
			SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, NULL)) {
			return FALSE;
		}
	}
	pdb->tables.last_id ++;
	table_id = pdb->tables.last_id;
	/* begin the transaction */
	sqlite3_exec(pdb->tables.psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	snprintf(sql_string, arsizeof(sql_string), "CREATE TABLE t%u (idx INTEGER PRIMARY KEY "
		"AUTOINCREMENT, member_id INTEGER UNIQUE NOT NULL)", table_id);
	if (SQLITE_OK != sqlite3_exec(pdb->tables.psqlite,
		sql_string, NULL, NULL, NULL)) {
		sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	ptnode = me_alloc<TABLE_NODE>();
	if (NULL == ptnode) {
		sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	memset(ptnode, 0, sizeof(TABLE_NODE));
	ptnode->node.pdata = ptnode;
	ptnode->table_id = table_id;
	remote_id = exmdb_server_get_remote_id();
	if (NULL != remote_id) {
		ptnode->remote_id = strdup(remote_id);
		if (NULL == ptnode->remote_id) {
			free(ptnode);
			sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
	}
	ptnode->type = TABLE_TYPE_PERMISSION;
	ptnode->folder_id = fid_val;
	ptnode->table_flags = table_flags;
	snprintf(sql_string, arsizeof(sql_string), "INSERT INTO t%u "
		"(member_id) VALUES (?)", ptnode->table_id);
	auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt == nullptr) {
		if (NULL != ptnode->remote_id) {
			free(ptnode->remote_id);
		}
		free(ptnode);
		sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	*prow_count = 0;
	if (FALSE == table_load_permissions(pdb->psqlite,
		fid_val, pstmt, prow_count)) {
		pstmt.finalize();
		if (NULL != ptnode->remote_id) {
			free(ptnode->remote_id);
		}
		free(ptnode);
		sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	pstmt.finalize();
	sqlite3_exec(pdb->tables.psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	double_list_append_as_tail(&pdb->tables.table_list, &ptnode->node);
	*ptable_id = ptnode->table_id;
	return TRUE;
}

static BOOL table_evaluate_rule_restriction(sqlite3 *psqlite,
	uint64_t rule_id, const RESTRICTION *pres)
{
	void *pvalue;
	void *pvalue1;
	uint32_t val_size;
	
	switch (pres->rt) {
	case RES_OR:
	case RES_AND:
		for (size_t i = 0; i < pres->andor->count; ++i)
			if (!table_evaluate_rule_restriction(psqlite,
			    rule_id, &pres->andor->pres[i]))
				return FALSE;
		return TRUE;
	case RES_NOT:
		if (table_evaluate_rule_restriction(psqlite,
		    rule_id, &pres->xnot->res))
			return FALSE;
		return TRUE;
	case RES_CONTENT: {
		auto rcon = pres->cont;
		if (PROP_TYPE(rcon->proptag) != PT_UNICODE)
			return FALSE;
		if (PROP_TYPE(rcon->proptag) != PROP_TYPE(rcon->propval.proptag))
			return FALSE;
		if (!common_util_get_rule_property(rule_id, psqlite,
		    rcon->proptag, &pvalue) || pvalue == nullptr)
			return FALSE;
		switch (rcon->fuzzy_level & 0xFFFF) {
		case FL_FULLSTRING:
			if (rcon->fuzzy_level & (FL_IGNORECASE | FL_LOOSE)) {
				if (strcasecmp(static_cast<char *>(rcon->propval.pvalue),
				    static_cast<char *>(pvalue)) == 0)
					return TRUE;
				return FALSE;
			} else {
				if (strcmp(static_cast<char *>(rcon->propval.pvalue),
				    static_cast<char *>(pvalue)) == 0)
					return TRUE;
				return FALSE;
			}
			return FALSE;
		case FL_SUBSTRING:
			if (rcon->fuzzy_level & (FL_IGNORECASE|FL_LOOSE)) {
				if (strcasestr(static_cast<char *>(pvalue),
				    static_cast<char *>(rcon->propval.pvalue)) != nullptr)
					return TRUE;
				return FALSE;
			} else {
				if (strstr(static_cast<char *>(pvalue),
				    static_cast<char *>(rcon->propval.pvalue)) != nullptr)
					return TRUE;
			}
			return FALSE;
		case FL_PREFIX: {
			auto len = strlen(static_cast<char *>(rcon->propval.pvalue));
			if (rcon->fuzzy_level & (FL_IGNORECASE | FL_LOOSE)) {
				if (strncasecmp(static_cast<char *>(pvalue),
				    static_cast<char *>(rcon->propval.pvalue),
				    len) == 0)
					return TRUE;
				return FALSE;
			} else {
				if (strncmp(static_cast<char *>(pvalue),
				    static_cast<char *>(rcon->propval.pvalue),
				    len) == 0)
					return TRUE;
				return FALSE;
			}
			return FALSE;
		}
		}
		return FALSE;
	}
	case RES_PROPERTY: {
		auto rprop = pres->prop;
		if (!common_util_get_rule_property(rule_id, psqlite,
		    rprop->proptag, &pvalue) || pvalue == nullptr)
			return FALSE;
		if (rprop->proptag == PROP_TAG_ANR) {
			if (PROP_TYPE(rprop->propval.proptag) != PT_UNICODE)
				return FALSE;
			if (strcasestr(static_cast<char *>(pvalue),
			    static_cast<char *>(rprop->propval.pvalue)) != nullptr)
				return TRUE;
			return FALSE;
		}
		return propval_compare_relop(rprop->relop,
		       PROP_TYPE(rprop->proptag), pvalue, rprop->propval.pvalue);
	}
	case RES_PROPCOMPARE: {
		auto rprop = pres->pcmp;
		if (PROP_TYPE(rprop->proptag1) != PROP_TYPE(rprop->proptag2))
			return FALSE;
		if (!common_util_get_rule_property(rule_id, psqlite,
		    rprop->proptag1, &pvalue) || pvalue == nullptr)
			return FALSE;
		if (!common_util_get_rule_property(rule_id, psqlite,
		    rprop->proptag2, &pvalue1) || pvalue1 == nullptr)
			return FALSE;
		return propval_compare_relop(rprop->relop,
		       PROP_TYPE(rprop->proptag1), pvalue, pvalue1);
	}
	case RES_BITMASK: {
		auto rbm = pres->bm;
		if (PROP_TYPE(rbm->proptag) != PT_LONG)
			return FALSE;
		if (!common_util_get_rule_property(rule_id, psqlite,
		    rbm->proptag, &pvalue) || pvalue == nullptr)
			return FALSE;
		switch (rbm->bitmask_relop) {
		case BMR_EQZ:
			if ((*static_cast<uint32_t *>(pvalue) & rbm->mask) == 0)
				return TRUE;
			break;
		case BMR_NEZ:
			if (*static_cast<uint32_t *>(pvalue) & rbm->mask)
				return TRUE;
			break;
		}	
		return FALSE;
	}
	case RES_SIZE: {
		auto rsize = pres->size;
		if (!common_util_get_rule_property(rule_id, psqlite,
		    rsize->proptag, &pvalue) || pvalue == nullptr)
			return FALSE;
		val_size = propval_size(rsize->proptag, pvalue);
		return propval_compare_relop(rsize->relop, PT_LONG,
		       &val_size, &rsize->size);
	}
	case RES_EXIST:
		if (!common_util_get_rule_property(rule_id, psqlite,
		    pres->exist->proptag, &pvalue) || pvalue == nullptr)
			return FALSE;
		return TRUE;
	case RES_COMMENT:
		if (pres->comment->pres == nullptr)
			return TRUE;
		return table_evaluate_rule_restriction(psqlite, rule_id,
		       pres->comment->pres);
	default:
		return FALSE;
	}	
	return FALSE;
}

static BOOL table_load_rules(sqlite3 *psqlite, uint64_t folder_id,
	uint8_t table_flags, const RESTRICTION *prestriction,
	sqlite3_stmt *pstmt, uint32_t *prow_count)
{
	uint64_t rule_id;
	char sql_string[80];
	
	snprintf(sql_string, arsizeof(sql_string), "SELECT rule_id FROM "
	          "rules WHERE folder_id=%llu", LLU(folder_id));
	auto pstmt1 = gx_sql_prep(psqlite, sql_string);
	if (pstmt1 == nullptr)
		return FALSE;
	while (SQLITE_ROW == sqlite3_step(pstmt1)) {
		rule_id = sqlite3_column_int64(pstmt1, 0);
		if (NULL != prestriction) {
			if (FALSE == table_evaluate_rule_restriction(
				psqlite, rule_id, prestriction)) {
				continue;
			}
		}
		sqlite3_bind_int64(pstmt, 1, rule_id);
		if (SQLITE_DONE != sqlite3_step(pstmt)) {
			return FALSE;
		}
		(*prow_count) ++;
		sqlite3_reset(pstmt);
	}
	return TRUE;
}

BOOL exmdb_server_load_rule_table(const char *dir,
	uint64_t folder_id, uint8_t table_flags,
	const RESTRICTION *prestriction,
	uint32_t *ptable_id, uint32_t *prow_count)
{
	uint64_t fid_val;
	uint32_t table_id;
	TABLE_NODE *ptnode;
	char sql_string[256];
	const char *remote_id;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	fid_val = rop_util_get_gc_value(folder_id);
	if (NULL == pdb->tables.psqlite) {
		if (SQLITE_OK != sqlite3_open_v2(":memory:", &pdb->tables.psqlite,
			SQLITE_OPEN_READWRITE|SQLITE_OPEN_CREATE, NULL)) {
			return FALSE;
		}
	}
	pdb->tables.last_id ++;
	table_id = pdb->tables.last_id;
	/* begin the transaction */
	sqlite3_exec(pdb->tables.psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	snprintf(sql_string, arsizeof(sql_string), "CREATE TABLE t%u (idx INTEGER PRIMARY KEY "
		"AUTOINCREMENT, rule_id INTEGER UNIQUE NOT NULL)", table_id);
	if (SQLITE_OK != sqlite3_exec(pdb->tables.psqlite,
		sql_string, NULL, NULL, NULL)) {
		sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	ptnode = me_alloc<TABLE_NODE>();
	if (NULL == ptnode) {
		sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	memset(ptnode, 0, sizeof(TABLE_NODE));
	ptnode->node.pdata = ptnode;
	ptnode->table_id = table_id;
	remote_id = exmdb_server_get_remote_id();
	if (NULL != remote_id) {
		ptnode->remote_id = strdup(remote_id);
		if (NULL == ptnode->remote_id) {
			free(ptnode);
			sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
	}
	ptnode->type = TABLE_TYPE_RULE;
	ptnode->folder_id = fid_val;
	if (NULL != prestriction) {
		ptnode->prestriction = restriction_dup(prestriction);
		if (NULL == ptnode->prestriction) {
			if (NULL != ptnode->remote_id) {
				free(ptnode->remote_id);
			}
			free(ptnode);
			sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
	}
	snprintf(sql_string, arsizeof(sql_string), "INSERT INTO t%u "
		"(rule_id) VALUES (?)", ptnode->table_id);
	auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt == nullptr) {
		if (NULL != ptnode->prestriction) {
			restriction_free(ptnode->prestriction);
		}
		if (NULL != ptnode->remote_id) {
			free(ptnode->remote_id);
		}
		free(ptnode);
		sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	*prow_count = 0;
	if (FALSE == table_load_rules(pdb->psqlite,
		fid_val, table_flags, prestriction,
		pstmt, prow_count)) {
		pstmt.finalize();
		if (NULL != ptnode->prestriction) {
			restriction_free(ptnode->prestriction);
		}
		if (NULL != ptnode->remote_id) {
			free(ptnode->remote_id);
		}
		free(ptnode);
		sqlite3_exec(pdb->tables.psqlite, "ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	pstmt.finalize();
	sqlite3_exec(pdb->tables.psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	double_list_append_as_tail(&pdb->tables.table_list, &ptnode->node);
	*ptable_id = ptnode->table_id;
	return TRUE;
}

BOOL exmdb_server_unload_table(const char *dir, uint32_t table_id)
{
	TABLE_NODE *ptnode;
	char sql_string[128];
	DOUBLE_LIST_NODE *pnode;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	for (pnode=double_list_get_head(&pdb->tables.table_list); NULL!=pnode;
		pnode=double_list_get_after(&pdb->tables.table_list, pnode)) {
		if (table_id == ((TABLE_NODE*)pnode->pdata)->table_id) {
			double_list_remove(&pdb->tables.table_list, pnode);
			break;
		}
	}
	if (NULL == pnode) {
		return TRUE;
	}
	ptnode = (TABLE_NODE*)pnode->pdata;
	snprintf(sql_string, arsizeof(sql_string), "DROP TABLE t%u", table_id);
	sqlite3_exec(pdb->tables.psqlite, sql_string, NULL, NULL, NULL);
	if (NULL != ptnode->remote_id) {
		free(ptnode->remote_id);
	}
	if (NULL != ptnode->username) {
		free(ptnode->username);
	}
	if (NULL != ptnode->prestriction) {
		restriction_free(ptnode->prestriction);
	}
	if (NULL != ptnode->psorts) {
		sortorder_set_free(ptnode->psorts);
	}
	free(ptnode);
	return TRUE;
}

BOOL exmdb_server_sum_table(const char *dir,
	uint32_t table_id, uint32_t *prows)
{
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	if (FALSE == table_sum_table_count(pdb, table_id, prows)) {
		return FALSE;
	}
	return TRUE;
}

static BOOL table_column_content_tmptbl(
	sqlite3_stmt *pstmt, sqlite3_stmt *pstmt1, sqlite3_stmt *pstmt2,
	const SORTORDER_SET *psorts, uint64_t folder_id, int row_type,
	uint32_t proptag, uint32_t instance_tag, uint32_t extremum_tag,
	void **ppvalue)
{
	int i;
	int depth;
	uint64_t row_id;
	uint32_t tmp_proptag;
	
	switch (proptag) {
	case PROP_TAG_FOLDERID:
		if (CONTENT_ROW_HEADER == row_type) {
			*ppvalue = cu_alloc<uint64_t>();
			if (NULL != *ppvalue) {
				*(uint64_t*)*ppvalue =
					rop_util_make_eid_ex(1, folder_id);
			}
			return TRUE;
		}
		break;
	case PROP_TAG_INSTID:
		*ppvalue = common_util_column_sqlite_statement(pstmt, 3, PT_I8);
		if (*ppvalue != NULL) {
			*static_cast<uint64_t *>(*ppvalue) = row_type == CONTENT_ROW_MESSAGE ?
				rop_util_make_eid_ex(1, *static_cast<uint64_t *>(*ppvalue)) :
				rop_util_make_eid_ex(2, *static_cast<uint64_t *>(*ppvalue) & 0x00FFFFFFFFFFFFFFULL);
		}
		return TRUE;
	case PROP_TAG_INSTANCENUM:
		*ppvalue = common_util_column_sqlite_statement(pstmt, 10, PT_LONG);
		return TRUE;
	case PROP_TAG_ROWTYPE:
		if (NULL == psorts || 0 == psorts->ccategories) {
			*ppvalue = NULL;
			return TRUE;
		}
		*ppvalue = cu_alloc<uint32_t>();
		if (NULL == *ppvalue) {
			return TRUE;
		}
		*static_cast<uint32_t *>(*ppvalue) =
			row_type == CONTENT_ROW_MESSAGE ? ROW_TYPE_LEAF_ROW :
			sqlite3_column_int64(pstmt, 8) == 0 ? ROW_TYPE_EMPTY_CATEGORY :
			sqlite3_column_int64(pstmt, 5) == 0 ? ROW_TYPE_COLLAPSED_CATEGORY :
			ROW_TYPE_EXPANDED_CATEGORY;
		return TRUE;
	case PROP_TAG_DEPTH:
		if (NULL == psorts || 0 == psorts->ccategories) {
			*ppvalue = NULL;
			return TRUE;
		}
		*ppvalue = common_util_column_sqlite_statement(pstmt, 7, PT_LONG);
		return TRUE;
	case PROP_TAG_CONTENTCOUNT:
		if (CONTENT_ROW_MESSAGE == row_type) {
			*ppvalue = cu_alloc<uint32_t>();
			if (NULL != *ppvalue) {
				*(uint32_t*)*ppvalue = 0;
			}
		} else {
			*ppvalue = common_util_column_sqlite_statement(pstmt, 8, PT_LONG);
		}
		return TRUE;
	case PROP_TAG_CONTENTUNREADCOUNT:
		if (CONTENT_ROW_MESSAGE == row_type) {
			*ppvalue = cu_alloc<uint32_t>();
			if (NULL != *ppvalue) {
				*(uint32_t*)*ppvalue = 0;
			}
		} else {
			*ppvalue = common_util_column_sqlite_statement(pstmt, 9, PT_LONG);
		}
		return TRUE;
	}
	if (CONTENT_ROW_MESSAGE == row_type) {
		if (0 != instance_tag && instance_tag == proptag) {
			*ppvalue = common_util_column_sqlite_statement(pstmt,
			           11, PROP_TYPE(instance_tag) & ~MVI_FLAG);
			return TRUE;
		}
		return FALSE;
	}
	if (NULL == psorts || 0 == psorts->ccategories) {
		return FALSE;
	}
	if (extremum_tag == proptag) {
		*ppvalue = common_util_column_sqlite_statement(pstmt, 12, PROP_TYPE(proptag));
		return TRUE;
	}
	for (i=psorts->ccategories-1; i>=0; i--) {
		tmp_proptag = PROP_TAG(psorts->psort[i].type, psorts->psort[i].propid);
		if (proptag == tmp_proptag) {
			break;
		}
	}
	if (i < 0) {
		return FALSE;
	}
	depth = sqlite3_column_int64(pstmt, 7);
	if (i > depth) {
		return FALSE;
	}
	row_id = sqlite3_column_int64(pstmt, 0);
	for (; depth>i; depth--) {
		sqlite3_bind_int64(pstmt1, 1, row_id);
		if (SQLITE_ROW != sqlite3_step(pstmt1)) {
			return FALSE;
		}
		row_id = sqlite3_column_int64(pstmt1, 0);
		sqlite3_reset(pstmt1);
	}
	sqlite3_bind_int64(pstmt2, 1, row_id);
	if (SQLITE_ROW != sqlite3_step(pstmt2)) {
		return FALSE;
	}
	if ((proptag & MVI_FLAG) == MVI_FLAG)
		*ppvalue = common_util_column_sqlite_statement(pstmt2, 0,
		           PROP_TYPE(proptag) & ~MVI_FLAG);
	else
		*ppvalue = common_util_column_sqlite_statement(pstmt2, 0,
		           PROP_TYPE(proptag));
	sqlite3_reset(pstmt2);
	return TRUE;
}

static void table_truncate_string(uint32_t cpid, char *pstring)
{
	size_t in_len;
	size_t out_len;
	int string_len;
	iconv_t conv_id;
	char *pin, *pout;
	char tmp_buff[512];
	const char *charset;
	char tmp_charset[256];
	
	string_len = strlen(pstring);
	if (string_len <= 510) {
		return;
	}
	string_len ++;
	pstring[510] = '\0';
	charset = common_util_cpid_to_charset(cpid);
	if (NULL == charset) {
		return;
	}
	in_len = string_len;
	out_len = sizeof(tmp_buff);
	pin = pstring;
	pout = tmp_buff;
	memset(tmp_buff, 0, sizeof(tmp_buff));
	snprintf(tmp_charset, arsizeof(tmp_charset), "%s//IGNORE", charset);
	conv_id = iconv_open(tmp_charset, charset);
	iconv(conv_id, &pin, &in_len, &pout, &out_len);
	iconv_close(conv_id);
	if (out_len < sizeof(tmp_buff)) {
		strcpy(pstring, tmp_buff);
	}
}

/* every property value returned in a row MUST
be less than or equal to 510 bytes in size. */
BOOL exmdb_server_query_table(const char *dir, const char *username,
	uint32_t cpid, uint32_t table_id, const PROPTAG_ARRAY *pproptags,
	uint32_t start_pos, int32_t row_needed, TARRAY_SET *pset)
{
	int i;
	int count;
	void *pvalue;
	int row_type;
	int32_t end_pos;
	uint32_t proptag;
	uint64_t rule_id;
	uint64_t inst_id;
	uint64_t member_id;
	uint64_t folder_id;
	TABLE_NODE *ptnode;
	xstmt pstmt1, pstmt2;
	char sql_string[1024];
	DOUBLE_LIST_NODE *pnode;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	pset->count = 0;
	pset->pparray = NULL;
	for (pnode=double_list_get_head(&pdb->tables.table_list); NULL!=pnode;
		pnode=double_list_get_after(&pdb->tables.table_list, pnode)) {
		if (table_id == ((TABLE_NODE*)pnode->pdata)->table_id) {
			break;
		}
	}
	if (NULL == pnode) {
		return TRUE;
	}
	if (FALSE == exmdb_server_check_private()) {
		exmdb_server_set_public_username(username);
	}
	ptnode = (TABLE_NODE*)pnode->pdata;
	switch (ptnode->type) {
	case TABLE_TYPE_HIERARCHY: {
		if (row_needed > 0) {
			end_pos = start_pos + row_needed;
			snprintf(sql_string, arsizeof(sql_string), "SELECT folder_id, depth FROM"
						" t%u WHERE idx>=%u AND idx<%u ORDER BY idx ASC",
						table_id, start_pos + 1, end_pos + 1);
			pset->pparray = cu_alloc<TPROPVAL_ARRAY *>(row_needed);
		} else {
			end_pos = start_pos + row_needed;
			if (end_pos < 0) {
				end_pos = 0;
			}
			snprintf(sql_string, arsizeof(sql_string), "SELECT folder_id, depth FROM "
						"t%u WHERE idx>%u AND idx<=%u ORDER BY idx DESC",
						table_id, end_pos + 1, start_pos + 1);
			pset->pparray = cu_alloc<TPROPVAL_ARRAY *>(start_pos - end_pos);
		}
		if (NULL == pset->pparray) {
			return FALSE;
		}
		auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
		if (pstmt == nullptr) {
			return FALSE;
		}
		sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
		while (SQLITE_ROW == sqlite3_step(pstmt)) {
			folder_id = sqlite3_column_int64(pstmt, 0);
			pset->pparray[pset->count] = cu_alloc<TPROPVAL_ARRAY>();
			if (NULL == pset->pparray[pset->count]) {
				pstmt.finalize();
				sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
				return FALSE;
			}
			pset->pparray[pset->count]->count = 0;
			pset->pparray[pset->count]->ppropval = cu_alloc<TAGGED_PROPVAL>(pproptags->count);
			if (NULL == pset->pparray[pset->count]->ppropval) {
				pstmt.finalize();
				sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
				return FALSE;
			}
			count = 0;
			for (i=0; i<pproptags->count; i++) {
				if (PROP_TAG_DEPTH == pproptags->pproptag[i]) {
					pvalue = cu_alloc<uint32_t>();
					if (NULL == pvalue) {
						pstmt.finalize();
						sqlite3_exec(pdb->psqlite,
							"ROLLBACK", NULL, NULL, NULL);
						return FALSE;
					}
					*(uint32_t*)pvalue = sqlite3_column_int64(pstmt, 1);
				} else {
					if (FALSE == common_util_get_property(
						FOLDER_PROPERTIES_TABLE, folder_id, cpid,
						pdb->psqlite, pproptags->pproptag[i], &pvalue)) {
						pstmt.finalize();
						sqlite3_exec(pdb->psqlite,
							"ROLLBACK", NULL, NULL, NULL);
						return FALSE;
					}
					if (NULL == pvalue) {
						continue;
					}
					switch (PROP_TYPE(pproptags->pproptag[i])) {
					case PT_UNICODE:
						utf8_truncate(static_cast<char *>(pvalue), 255);
						break;
					case PT_STRING8:
						table_truncate_string(cpid, static_cast<char *>(pvalue));
						break;
					case PT_BINARY:
						if (((BINARY*)pvalue)->cb > 510) {
							((BINARY*)pvalue)->cb = 510;
						}
						break;
					}
				}
				pset->pparray[pset->count]->ppropval[count].proptag =
													pproptags->pproptag[i];
				pset->pparray[pset->count]->ppropval[count].pvalue = pvalue;
				count ++;
			}
			pset->pparray[pset->count]->count = count;
			pset->count ++;
		}
		pstmt.finalize();
		sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
		break;
	}
	case TABLE_TYPE_CONTENT: {
		if (row_needed > 0) {
			end_pos = start_pos + row_needed;
			snprintf(sql_string, arsizeof(sql_string), "SELECT * FROM t%u"
				" WHERE idx>=%u AND idx<%u ORDER BY idx ASC",
				table_id, start_pos + 1, end_pos + 1);
			pset->pparray = cu_alloc<TPROPVAL_ARRAY *>(row_needed);
		} else {
			end_pos = start_pos + row_needed;
			if (end_pos < 0) {
				end_pos = 0;
			}
			snprintf(sql_string, arsizeof(sql_string), "SELECT * FROM t%u"
				" WHERE idx>=%u AND idx<%u ORDER BY idx DESC",
				table_id, end_pos + 1, start_pos + 1);
			pset->pparray = cu_alloc<TPROPVAL_ARRAY *>(start_pos - end_pos);
		}
		if (NULL == pset->pparray) {
			return FALSE;
		}
		auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
		if (pstmt == nullptr) {
			return FALSE;
		}
		if (NULL != ptnode->psorts && ptnode->psorts->ccategories > 0) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT parent_id FROM"
					" t%u WHERE row_id=?", ptnode->table_id);
			pstmt1 = gx_sql_prep(pdb->tables.psqlite, sql_string);
			if (pstmt1 == nullptr) {
				return FALSE;
			}
			snprintf(sql_string, arsizeof(sql_string), "SELECT value FROM"
					" t%u WHERE row_id=?", ptnode->table_id);
			pstmt2 = gx_sql_prep(pdb->tables.psqlite, sql_string);
			if (pstmt2 == nullptr) {
				return FALSE;
			}
		} else {
			pstmt1 = NULL;
			pstmt2 = NULL;
		}
		sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
		if (FALSE == common_util_begin_message_optimize(pdb->psqlite)) {
			sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
		while (SQLITE_ROW == sqlite3_step(pstmt)) {
			inst_id = sqlite3_column_int64(pstmt, 3);
			row_type = sqlite3_column_int64(pstmt, 4);
			pset->pparray[pset->count] = cu_alloc<TPROPVAL_ARRAY>();
			if (NULL == pset->pparray[pset->count]) {
				pstmt.finalize();
				pstmt1.finalize();
				pstmt2.finalize();
				common_util_end_message_optimize();
				sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
				return FALSE;
			}
			pset->pparray[pset->count]->count = 0;
			pset->pparray[pset->count]->ppropval = cu_alloc<TAGGED_PROPVAL>(pproptags->count);
			if (NULL == pset->pparray[pset->count]->ppropval) {
				pstmt.finalize();
				pstmt1.finalize();
				pstmt2.finalize();
				common_util_end_message_optimize();
				sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
				return FALSE;
			}
			count = 0;
			for (i=0; i<pproptags->count; i++) {
				if (FALSE == table_column_content_tmptbl(pstmt, pstmt1,
					pstmt2, ptnode->psorts, ptnode->folder_id, row_type,
					pproptags->pproptag[i], ptnode->instance_tag,
					ptnode->extremum_tag, &pvalue)) {
					if (CONTENT_ROW_HEADER == row_type) {
						continue;
					}
					if (FALSE == common_util_get_property(
						MESSAGE_PROPERTIES_TABLE, inst_id, cpid,
						pdb->psqlite, pproptags->pproptag[i], &pvalue)) {
						pstmt.finalize();
						pstmt1.finalize();
						pstmt2.finalize();
						common_util_end_message_optimize();
						sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
						return FALSE;
					}
				}
				if (NULL == pvalue) {
					continue;
				}
				switch (PROP_TYPE(pproptags->pproptag[i])) {
				case PT_UNICODE:
					utf8_truncate(static_cast<char *>(pvalue), 255);
					break;
				case PT_STRING8:
					table_truncate_string(cpid, static_cast<char *>(pvalue));
					break;
				case PT_BINARY:
					if (((BINARY*)pvalue)->cb > 510) {
						((BINARY*)pvalue)->cb = 510;
					}
					break;
				}
				pset->pparray[pset->count]->ppropval[count].proptag =
													pproptags->pproptag[i];
				pset->pparray[pset->count]->ppropval[count].pvalue = pvalue;
				count ++;
			}
			pset->pparray[pset->count]->count = count;
			pset->count ++;
		}
		pstmt.finalize();
		pstmt1.finalize();
		pstmt2.finalize();
		common_util_end_message_optimize();
		sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
		break;
	}
	case TABLE_TYPE_PERMISSION: {
		if (row_needed > 0) {
			end_pos = start_pos + row_needed;
			snprintf(sql_string, arsizeof(sql_string), "SELECT member_id FROM t%u "
						"WHERE idx>=%u AND idx<%u ORDER BY idx ASC",
						table_id, start_pos + 1, end_pos + 1);
			pset->pparray = cu_alloc<TPROPVAL_ARRAY *>(row_needed);
		} else {
			end_pos = start_pos + row_needed;
			if (end_pos < 0) {
				end_pos = 0;
			}
			snprintf(sql_string, arsizeof(sql_string), "SELECT member_id FROM t%u "
						"WHERE idx>%u AND idx<=%u ORDER BY idx DESC",
						table_id, end_pos + 1, start_pos + 1);
			pset->pparray = cu_alloc<TPROPVAL_ARRAY *>(start_pos - end_pos);
		}
		if (NULL == pset->pparray) {
			return FALSE;
		}
		auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
		if (pstmt == nullptr) {
			return FALSE;
		}
		while (SQLITE_ROW == sqlite3_step(pstmt)) {
			member_id = sqlite3_column_int64(pstmt, 0);
			pset->pparray[pset->count] = cu_alloc<TPROPVAL_ARRAY>();
			if (NULL == pset->pparray[pset->count]) {
				return FALSE;
			}
			pset->pparray[pset->count]->count = 0;
			pset->pparray[pset->count]->ppropval = cu_alloc<TAGGED_PROPVAL>(pproptags->count);
			if (NULL == pset->pparray[pset->count]->ppropval) {
				return FALSE;
			}
			count = 0;
			for (i=0; i<pproptags->count; i++) {
				proptag = pproptags->pproptag[i];
				if (PROP_TAG_MEMBERNAME_STRING8 == proptag) {
					proptag = PROP_TAG_MEMBERNAME;
				}
				if (FALSE == common_util_get_permission_property(member_id,
				    pdb->psqlite, proptag, &pvalue))
					return FALSE;
				if (PROP_TAG_MEMBERRIGHTS == pproptags->pproptag[i]
					&& 0 == (ptnode->table_flags &
					PERMISSIONS_TABLE_FLAG_INCLUDEFREEBUSY)) {
					*static_cast<uint32_t *>(pvalue) &= ~(frightsFreeBusySimple | frightsFreeBusyDetailed);
				}
				if (NULL == pvalue) {
					continue;
				}
				pset->pparray[pset->count]->ppropval[count].proptag =
													pproptags->pproptag[i];
				if (PROP_TAG_MEMBERNAME_STRING8 == pproptags->pproptag[i]) {
					pset->pparray[pset->count]->ppropval[count].pvalue =
						common_util_convert_copy(FALSE, cpid, static_cast<char *>(pvalue));
				} else {
					pset->pparray[pset->count]->ppropval[count].pvalue =
																	pvalue;
				}
				count ++;
			}
			pset->pparray[pset->count]->count = count;
			pset->count ++;
		}
		break;
	}
	case TABLE_TYPE_RULE: {
		if (row_needed > 0) {
			end_pos = start_pos + row_needed;
			snprintf(sql_string, arsizeof(sql_string), "SELECT rule_id FROM t%u "
						"WHERE idx>=%u AND idx<%u ORDER BY idx ASC",
						table_id, start_pos + 1, end_pos + 1);
			pset->pparray = cu_alloc<TPROPVAL_ARRAY *>(row_needed);
		} else {
			end_pos = start_pos + row_needed;
			if (end_pos < 0) {
				end_pos = 0;
			}
			snprintf(sql_string, arsizeof(sql_string), "SELECT rule_id FROM t%u "
						"WHERE idx>%u AND idx<=%u ORDER BY idx DESC",
						table_id, end_pos + 1, start_pos + 1);
			pset->pparray = cu_alloc<TPROPVAL_ARRAY *>(start_pos - end_pos);
		}
		if (NULL == pset->pparray) {
			return FALSE;
		}
		auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
		if (pstmt == nullptr) {
			return FALSE;
		}
		while (SQLITE_ROW == sqlite3_step(pstmt)) {
			rule_id = sqlite3_column_int64(pstmt, 0);
			pset->pparray[pset->count] = cu_alloc<TPROPVAL_ARRAY>();
			if (NULL == pset->pparray[pset->count]) {
				return FALSE;
			}
			pset->pparray[pset->count]->count = 0;
			pset->pparray[pset->count]->ppropval = cu_alloc<TAGGED_PROPVAL>(pproptags->count);
			if (NULL == pset->pparray[pset->count]->ppropval) {
				return FALSE;
			}
			count = 0;
			for (i=0; i<pproptags->count; i++) {
				proptag = pproptags->pproptag[i];
				if (proptag == PR_RULE_NAME_A)
					proptag = PR_RULE_NAME;
				else if (proptag == PR_RULE_PROVIDER_A)
					proptag = PR_RULE_PROVIDER;
				if (FALSE == common_util_get_rule_property(
					rule_id, pdb->psqlite, proptag, &pvalue)) {
					return FALSE;
				}
				if (NULL == pvalue) {
					continue;
				}
				pset->pparray[pset->count]->ppropval[count].proptag =
													pproptags->pproptag[i];
				if (pproptags->pproptag[i] == PR_RULE_NAME_A ||
				    pproptags->pproptag[i] == PR_RULE_PROVIDER_A)
					pset->pparray[pset->count]->ppropval[count].pvalue =
						common_util_convert_copy(FALSE, cpid, static_cast<char *>(pvalue));
				else
					pset->pparray[pset->count]->ppropval[count].pvalue =
																	pvalue;
				count ++;
			}
			pset->pparray[pset->count]->count = count;
			pset->count ++;
		}
		break;
	}
	}
	return TRUE;
}

static BOOL table_get_content_row_property(
	 void *pparam, uint32_t proptag, void **ppvalue)
{
	uint32_t *pinst_num;
	uint64_t parent_fid;
	CONTENT_ROW_PARAM *prow_param;
	
	prow_param = (CONTENT_ROW_PARAM*)pparam;
	if (proptag == PROP_TAG_INSTANCESVREID) {
		auto eid = cu_alloc<SVREID>();
		if (eid == nullptr)
			return FALSE;
		*ppvalue = eid;
		eid->pbin = nullptr;
		if (CONTENT_ROW_HEADER == prow_param->row_type) {
			eid->folder_id = rop_util_make_eid_ex(1, prow_param->folder_id);
			eid->message_id = rop_util_make_eid_ex(2, prow_param->inst_id & 0x00FFFFFFFFFFFFFFULL);
			eid->instance = 0;
		} else {
			if (FALSE == common_util_get_message_parent_folder(
				prow_param->psqlite, prow_param->inst_id,
				&parent_fid)) {
				return FALSE;	
			}
			eid->folder_id = rop_util_make_eid_ex(1, parent_fid);
			eid->message_id = rop_util_make_eid_ex(1, prow_param->inst_id);
			pinst_num = static_cast<uint32_t *>(common_util_column_sqlite_statement(
			            prow_param->pstmt, 10, PT_LONG));
			if (NULL == pinst_num) {
				return FALSE;
			}
			eid->instance = *pinst_num;
		}
		return TRUE;
	}
	if (FALSE == table_column_content_tmptbl(
		prow_param->pstmt, prow_param->pstmt1,
		prow_param->pstmt2, prow_param->psorts,
		prow_param->folder_id, prow_param->row_type,
		proptag, prow_param->instance_tag,
		prow_param->extremum_tag, ppvalue)) {
		if (CONTENT_ROW_HEADER == prow_param->row_type) {
			*ppvalue = NULL;
			return TRUE;
		}
		if (FALSE == common_util_get_property(
			MESSAGE_PROPERTIES_TABLE, prow_param->inst_id,
			prow_param->cpid, prow_param->psqlite, proptag,
			ppvalue)) {
			return FALSE;	
		}
	}
	return TRUE;
}

static BOOL table_get_hierarchy_row_property(
	 void *pparam, uint32_t proptag, void **ppvalue)
{
	HIERARCHY_ROW_PARAM *prow_param;
	
	prow_param = (HIERARCHY_ROW_PARAM*)pparam;
	if (PROP_TAG_DEPTH == proptag) {
		*ppvalue = cu_alloc<uint32_t>();
		if (NULL == *ppvalue) {
			return FALSE;
		}
		*(uint32_t*)*ppvalue = sqlite3_column_int64(prow_param->pstmt, 2);
	} else {
		if (FALSE == common_util_get_property(
			FOLDER_PROPERTIES_TABLE, prow_param->folder_id,
			prow_param->cpid, prow_param->psqlite, proptag,
			ppvalue)) {
			return FALSE;
		}
	}
	return TRUE;
}

static BOOL table_evaluate_row_restriction(
	const RESTRICTION *pres, void *pparam,
	TABLE_GET_ROW_PROPERTY get_property)
{
	void *pvalue;
	void *pvalue1;
	uint32_t val_size;
	
	switch (pres->rt) {
	case RES_AND:
		for (size_t i = 0; i < pres->andor->count; ++i)
			if (!table_evaluate_row_restriction(&pres->andor->pres[i],
			    pparam, get_property))
				return FALSE;
		return TRUE;
	case RES_OR:
		for (size_t i = 0; i < pres->andor->count; ++i)
			if (table_evaluate_row_restriction(&pres->andor->pres[i],
			    pparam, get_property))
				return TRUE;
		return FALSE;
	case RES_NOT:
		if (table_evaluate_row_restriction(&pres->xnot->res,
		    pparam, get_property))
			return FALSE;
		return TRUE;
	case RES_CONTENT: {
		auto rcon = pres->cont;
		if (PROP_TYPE(rcon->proptag) != PT_UNICODE)
			return FALSE;
		if (PROP_TYPE(rcon->proptag) != PROP_TYPE(rcon->propval.proptag))
			return FALSE;
		if (!get_property(pparam, rcon->proptag, &pvalue) ||
		    pvalue == nullptr)
			return FALSE;
		switch (rcon->fuzzy_level & 0xFFFF) {
		case FL_FULLSTRING:
			if (rcon->fuzzy_level & (FL_IGNORECASE | FL_LOOSE)) {
				if (strcasecmp(static_cast<char *>(rcon->propval.pvalue),
				    static_cast<char *>(pvalue)) == 0)
					return TRUE;
				return FALSE;
			} else {
				if (strcmp(static_cast<char *>(rcon->propval.pvalue),
				    static_cast<char *>(pvalue)) == 0)
					return TRUE;
				return FALSE;
			}
			return FALSE;
		case FL_SUBSTRING:
			if (rcon->fuzzy_level & (FL_IGNORECASE | FL_LOOSE)) {
				if (strcasestr(static_cast<char *>(pvalue),
				    static_cast<char *>(rcon->propval.pvalue)) != nullptr)
					return TRUE;
				return FALSE;
			} else {
				if (strstr(static_cast<char *>(pvalue),
				    static_cast<char *>(rcon->propval.pvalue)) != nullptr)
					return TRUE;
			}
			return FALSE;
		case FL_PREFIX: {
			auto len = strlen(static_cast<char *>(rcon->propval.pvalue));
			if (rcon->fuzzy_level & (FL_IGNORECASE | FL_LOOSE)) {
				if (strncasecmp(static_cast<char *>(pvalue),
				    static_cast<char *>(rcon->propval.pvalue),
				    len) == 0)
					return TRUE;
				return FALSE;
			} else {
				if (strncmp(static_cast<char *>(pvalue),
				    static_cast<char *>(rcon->propval.pvalue),
				    len) == 0)
					return TRUE;
				return FALSE;
			}
			return FALSE;
		}
		}
		return FALSE;
	}
	case RES_PROPERTY: {
		auto rprop = pres->prop;
		if (!get_property(pparam, rprop->proptag, &pvalue) ||
		    pvalue == nullptr)
			return FALSE;
		if (rprop->proptag == PROP_TAG_ANR) {
			if (PROP_TYPE(rprop->propval.proptag) != PT_UNICODE)
				return FALSE;
			if (strcasestr(static_cast<char *>(pvalue),
			    static_cast<char *>(rprop->propval.pvalue)) != nullptr)
				return TRUE;
			return FALSE;
		}
		return propval_compare_relop(rprop->relop,
		       PROP_TYPE(rprop->proptag), pvalue, rprop->propval.pvalue);
	}
	case RES_PROPCOMPARE: {
		auto rprop = pres->pcmp;
		if (PROP_TYPE(rprop->proptag1) != PROP_TYPE(rprop->proptag2))
			return FALSE;
		if (!get_property(pparam, rprop->proptag1, &pvalue) ||
		    pvalue == nullptr)
			return FALSE;
		if (!get_property(pparam, rprop->proptag2, &pvalue1) ||
		    pvalue1 == nullptr)
			return FALSE;
		return propval_compare_relop(rprop->relop,
		       PROP_TYPE(rprop->proptag1), pvalue, pvalue1);
	}
	case RES_BITMASK: {
		auto rbm = pres->bm;
		if (PROP_TYPE(rbm->proptag) != PT_LONG)
			return FALSE;
		if (!get_property(pparam, rbm->proptag, &pvalue) ||
		    pvalue == nullptr)
			return FALSE;
		switch (rbm->bitmask_relop) {
		case BMR_EQZ:
			if ((*static_cast<uint32_t *>(pvalue) & rbm->mask) == 0)
				return TRUE;
			break;
		case BMR_NEZ:
			if (*static_cast<uint32_t *>(pvalue) & rbm->mask)
				return TRUE;
			break;
		}	
		return FALSE;
	}
	case RES_SIZE: {
		auto rsize = pres->size;
		if (!get_property(pparam, rsize->proptag, &pvalue) ||
		    pvalue == nullptr)
			return FALSE;
		val_size = propval_size(rsize->proptag, pvalue);
		return propval_compare_relop(rsize->relop, PT_LONG,
		       &val_size, &rsize->size);
	}
	case RES_EXIST:
		if (!get_property(pparam, pres->exist->proptag, &pvalue) ||
		    pvalue == nullptr)
			return FALSE;
		return TRUE;
	case RES_COMMENT:
		if (pres->comment->pres == nullptr)
			return TRUE;
		return table_evaluate_row_restriction(pres->comment->pres,
		       pparam, get_property);
	default:
		return FALSE;
	}	
	return FALSE;
}

BOOL exmdb_server_match_table(const char *dir, const char *username,
	uint32_t cpid, uint32_t table_id, BOOL b_forward, uint32_t start_pos,
	const RESTRICTION *pres, const PROPTAG_ARRAY *pproptags,
	int32_t *pposition, TPROPVAL_ARRAY *ppropvals)
{
	int i;
	int idx;
	int count;
	void *pvalue;
	int row_type;
	uint32_t proptag;
	uint64_t rule_id;
	uint64_t inst_id;
	uint64_t folder_id;
	TABLE_NODE *ptnode;
	char sql_string[1024];
	DOUBLE_LIST_NODE *pnode;
	CONTENT_ROW_PARAM content_param;
	HIERARCHY_ROW_PARAM hierarchy_param;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	for (pnode=double_list_get_head(&pdb->tables.table_list); NULL!=pnode;
		pnode=double_list_get_after(&pdb->tables.table_list, pnode)) {
		if (table_id == ((TABLE_NODE*)pnode->pdata)->table_id) {
			break;
		}
	}
	if (NULL == pnode) {
		*pposition = -1;
		return TRUE;
	}
	ptnode = (TABLE_NODE*)pnode->pdata;
	if (FALSE == exmdb_server_check_private()) {
		exmdb_server_set_public_username(username);
	}
	idx = 0;
	ppropvals->count = 0;
	ppropvals->ppropval = NULL;
	if (TABLE_TYPE_HIERARCHY == ptnode->type) {
		if (TRUE == b_forward) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT folder_id,"
				" idx, depth FROM t%u WHERE idx>=%u ORDER BY"
				" idx ASC", table_id, start_pos + 1);
		} else {
			snprintf(sql_string, arsizeof(sql_string), "SELECT folder_id,"
				" idx, depth FROM t%u WHERE idx<=%u ORDER BY"
				" idx DESC", table_id, start_pos + 1);
		}
		auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
		if (pstmt == nullptr) {
			return FALSE;
		}
		sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
		while (SQLITE_ROW == sqlite3_step(pstmt)) {
			folder_id = sqlite3_column_int64(pstmt, 0);
			hierarchy_param.cpid = cpid;
			hierarchy_param.psqlite = pdb->psqlite;
			hierarchy_param.pstmt = pstmt;
			hierarchy_param.folder_id = folder_id;
			if (TRUE == table_evaluate_row_restriction(pres,
				&hierarchy_param, table_get_hierarchy_row_property)) {
				idx = sqlite3_column_int64(pstmt, 1);
				count = 0;
				ppropvals->ppropval = cu_alloc<TAGGED_PROPVAL>(pproptags->count);
				if (NULL == ppropvals->ppropval) {
					pstmt.finalize();
					sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
					return FALSE;
				}
				for (i=0; i<pproptags->count; i++) {
					if (PROP_TAG_DEPTH == pproptags->pproptag[i]) {
						pvalue = cu_alloc<uint32_t>();
						if (NULL == pvalue) {
							pstmt.finalize();
							sqlite3_exec(pdb->psqlite,
								"ROLLBACK", NULL, NULL, NULL);
							return FALSE;
						}
						*(uint32_t*)pvalue = sqlite3_column_int64(pstmt, 2);
					} else {
						if (FALSE == common_util_get_property(
							FOLDER_PROPERTIES_TABLE, folder_id, cpid,
							pdb->psqlite, pproptags->pproptag[i], &pvalue)) {
							pstmt.finalize();
							sqlite3_exec(pdb->psqlite,
								"ROLLBACK", NULL, NULL, NULL);
							return FALSE;
						}
						if (NULL == pvalue) {
							continue;
						}
						switch (PROP_TYPE(pproptags->pproptag[i])) {
						case PT_UNICODE:
							utf8_truncate(static_cast<char *>(pvalue), 255);
							break;
						case PT_STRING8:
							table_truncate_string(cpid, static_cast<char *>(pvalue));
							break;
						case PT_BINARY:
							if (((BINARY*)pvalue)->cb > 510) {
								((BINARY*)pvalue)->cb = 510;
							}
							break;
						}
					}
					ppropvals->ppropval[count].proptag = pproptags->pproptag[i];
					ppropvals->ppropval[count].pvalue = pvalue;
					count ++;
				}
				ppropvals->count = count;
				break;
			}
		}
		pstmt.finalize();
		sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
		*pposition = idx - 1;
	} else if (TABLE_TYPE_CONTENT == ptnode->type) {
		if (TRUE == b_forward) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT * FROM t%u"
				" WHERE idx>=%u ORDER BY idx ASC", table_id,
				start_pos + 1);
		} else {
			snprintf(sql_string, arsizeof(sql_string), "SELECT * FROM t%u"
				" WHERE idx<=%u ORDER BY idx DESC", table_id,
				start_pos + 1);
		}
		auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
		if (pstmt == nullptr) {
			return FALSE;
		}
		xstmt pstmt1, pstmt2;
		if (NULL != ptnode->psorts && ptnode->psorts->ccategories > 0) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT parent_id FROM"
					" t%u WHERE row_id=?", ptnode->table_id);
			pstmt1 = gx_sql_prep(pdb->tables.psqlite, sql_string);
			if (pstmt1 == nullptr) {
				return FALSE;
			}
			snprintf(sql_string, arsizeof(sql_string), "SELECT value FROM"
					" t%u WHERE row_id=?", ptnode->table_id);
			pstmt2 = gx_sql_prep(pdb->tables.psqlite, sql_string);
			if (pstmt2 == nullptr) {
				return FALSE;
			}
		} else {
			pstmt1 = NULL;
			pstmt2 = NULL;
		}
		sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
		if (FALSE == common_util_begin_message_optimize(pdb->psqlite)) {
			sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
		while (SQLITE_ROW == sqlite3_step(pstmt)) {
			inst_id = sqlite3_column_int64(pstmt, 3);
			row_type = sqlite3_column_int64(pstmt, 4);
			content_param.cpid = cpid;
			content_param.psqlite = pdb->psqlite;
			content_param.pstmt = pstmt;
			content_param.pstmt1 = pstmt1;
			content_param.pstmt2 = pstmt2;
			content_param.folder_id = ptnode->folder_id;
			content_param.inst_id = inst_id;
			content_param.row_type = row_type;
			content_param.psorts = ptnode->psorts;
			content_param.instance_tag = ptnode->instance_tag;
			content_param.extremum_tag = ptnode->extremum_tag;
			if (TRUE == table_evaluate_row_restriction(pres,
				&content_param, table_get_content_row_property)) {
				idx = sqlite3_column_int64(pstmt, 1);
				count = 0;
				ppropvals->ppropval = cu_alloc<TAGGED_PROPVAL>(pproptags->count);
				if (NULL == ppropvals->ppropval) {
					pstmt.finalize();
					pstmt1.finalize();
					pstmt2.finalize();
					common_util_end_message_optimize();
					sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
					return FALSE;
				}
				for (i=0; i<pproptags->count; i++) {
					if (FALSE == table_column_content_tmptbl(pstmt, pstmt1,
						pstmt2, ptnode->psorts, ptnode->folder_id, row_type,
						pproptags->pproptag[i], ptnode->instance_tag,
						ptnode->extremum_tag, &pvalue)) {
						if (CONTENT_ROW_HEADER == row_type) {
							continue;
						}
						if (FALSE == common_util_get_property(
							MESSAGE_PROPERTIES_TABLE, inst_id, cpid,
							pdb->psqlite, pproptags->pproptag[i], &pvalue)) {
							pstmt.finalize();
							pstmt1.finalize();
							pstmt2.finalize();
							common_util_end_message_optimize();
							sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
							return FALSE;
						}
					}
					if (NULL == pvalue) {
						continue;
					}
					switch (PROP_TYPE(pproptags->pproptag[i])) {
					case PT_UNICODE:
						utf8_truncate(static_cast<char *>(pvalue), 255);
						break;
					case PT_STRING8:
						table_truncate_string(cpid, static_cast<char *>(pvalue));
						break;
					case PT_BINARY:
						if (((BINARY*)pvalue)->cb > 510) {
							((BINARY*)pvalue)->cb = 510;
						}
						break;
					}
					ppropvals->ppropval[count].proptag = pproptags->pproptag[i];
					ppropvals->ppropval[count].pvalue = pvalue;
					count ++;
				}
				ppropvals->count = count;
				break;
			}
		}
		pstmt.finalize();
		pstmt1.finalize();
		pstmt2.finalize();
		common_util_end_message_optimize();
		sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
		*pposition = idx - 1;
	} else if (TABLE_TYPE_RULE == ptnode->type) {
		if (TRUE == b_forward) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT rule_id"
					" idx FROM t%u WHERE idx>=%u ORDER BY"
					" idx ASC", table_id, start_pos + 1);
		} else {
			snprintf(sql_string, arsizeof(sql_string), "SELECT rule_id,"
					" idx FROM t%u WHERE idx<=%u ORDER BY"
					" idx DESC", table_id, start_pos + 1);
		}
		auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
		if (pstmt == nullptr) {
			return FALSE;
		}
		while (SQLITE_ROW == sqlite3_step(pstmt)) {
			rule_id = sqlite3_column_int64(pstmt, 0);
			if (TRUE == table_evaluate_rule_restriction(
				pdb->psqlite, rule_id, pres)) {
				ppropvals->count = 0;
				ppropvals->ppropval = cu_alloc<TAGGED_PROPVAL>(pproptags->count);
				if (NULL == ppropvals->ppropval) {
					return FALSE;
				}
				count = 0;
				for (i=0; i<pproptags->count; i++) {
					proptag = pproptags->pproptag[i];
					if (proptag == PR_RULE_NAME_A)
						proptag = PR_RULE_NAME;
					else if (proptag == PR_RULE_PROVIDER_A)
						proptag = PR_RULE_PROVIDER;
					if (FALSE == common_util_get_rule_property(
						rule_id, pdb->psqlite, proptag, &pvalue)) {
						return FALSE;
					}
					if (NULL == pvalue) {
						continue;
					}
					ppropvals->ppropval[count].proptag = pproptags->pproptag[i];
					if (pproptags->pproptag[i] == PR_RULE_NAME_A ||
					    pproptags->pproptag[i] == PR_RULE_PROVIDER_A)
						ppropvals->ppropval[count].pvalue =
							common_util_convert_copy(FALSE, cpid, static_cast<char *>(pvalue));
					else
						ppropvals->ppropval[count].pvalue = pvalue;
					count ++;
				}
				ppropvals->count = count;
				break;
			}
		}
		*pposition = idx - 1;
	} else {
		*pposition = -1;
	}
	return TRUE;
}

BOOL exmdb_server_locate_table(const char *dir,
	uint32_t table_id, uint64_t inst_id, uint32_t inst_num,
	int32_t *pposition, uint32_t *prow_type)
{
	int idx;
	TABLE_NODE *ptnode;
	char sql_string[256];
	DOUBLE_LIST_NODE *pnode;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	for (pnode=double_list_get_head(&pdb->tables.table_list); NULL!=pnode;
		pnode=double_list_get_after(&pdb->tables.table_list, pnode)) {
		if (table_id == ((TABLE_NODE*)pnode->pdata)->table_id) {
			break;
		}
	}
	if (NULL == pnode) {
		*pposition = -1;
		return TRUE;
	}
	ptnode = (TABLE_NODE*)pnode->pdata;
	switch (ptnode->type) {
	case TABLE_TYPE_HIERARCHY:
		if (1 == rop_util_get_replid(inst_id)) {
			inst_id = rop_util_get_gc_value(inst_id);
		} else {
			inst_id = rop_util_get_replid(inst_id);
			inst_id <<= 48;
			inst_id |= rop_util_get_gc_value(inst_id);
		}
		snprintf(sql_string, arsizeof(sql_string), "SELECT idx FROM t%u "
		          "WHERE folder_id=%llu", ptnode->table_id, LLU(inst_id));
		break;
	case TABLE_TYPE_CONTENT:
		inst_id = rop_util_get_replid(inst_id) == 1 ?
		          rop_util_get_gc_value(inst_id) :
		          rop_util_get_gc_value(inst_id) | 0x100000000000000ULL;
		snprintf(sql_string, arsizeof(sql_string), "SELECT idx, row_type "
				"FROM t%u WHERE inst_id=%llu AND inst_num=%u",
				ptnode->table_id, LLU(inst_id), inst_num);
		break;
	case TABLE_TYPE_PERMISSION:
		snprintf(sql_string, arsizeof(sql_string), "SELECT idx FROM t%u "
			"WHERE member_id=%llu", ptnode->table_id, LLU(inst_id));
		break;
	case TABLE_TYPE_RULE:
		inst_id = rop_util_get_gc_value(inst_id);
		snprintf(sql_string, arsizeof(sql_string), "SELECT idx FROM t%u "
		          "WHERE rule_id=%llu", ptnode->table_id, LLU(inst_id));
		break;
	default:
		return FALSE;
	}
	auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	*prow_type = 0;
	if (SQLITE_ROW == sqlite3_step(pstmt)) {
		idx = sqlite3_column_int64(pstmt, 0);
		if (TABLE_TYPE_CONTENT == ptnode->type) {
			*prow_type = sqlite3_column_int64(pstmt, 1);
		}
	} else {
		idx = 0;
	}
	*pposition = idx - 1;
	return TRUE;
}

BOOL exmdb_server_read_table_row(const char *dir, const char *username,
	uint32_t cpid, uint32_t table_id, const PROPTAG_ARRAY *pproptags,
	uint64_t inst_id, uint32_t inst_num, TPROPVAL_ARRAY *ppropvals)
{
	int i;
	int count;
	void *pvalue;
	int row_type;
	uint32_t depth;
	TABLE_NODE *ptnode;
	uint64_t folder_id;
	char sql_string[1024];
	DOUBLE_LIST_NODE *pnode;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	for (pnode=double_list_get_head(&pdb->tables.table_list); NULL!=pnode;
		pnode=double_list_get_after(&pdb->tables.table_list, pnode)) {
		if (table_id == ((TABLE_NODE*)pnode->pdata)->table_id) {
			break;
		}
	}
	if (NULL == pnode) {
		ppropvals->count = 0;
		return TRUE;
	}
	ptnode = (TABLE_NODE*)pnode->pdata;
	if (FALSE == exmdb_server_check_private()) {
		exmdb_server_set_public_username(username);
	}
	if (TABLE_TYPE_HIERARCHY == ptnode->type) {
		if (1 == rop_util_get_replid(inst_id)) {
			folder_id = rop_util_get_gc_value(inst_id);
		} else {
			folder_id = rop_util_get_replid(inst_id);
			folder_id <<= 48;
			folder_id |= rop_util_get_gc_value(inst_id);
		}
		snprintf(sql_string, arsizeof(sql_string), "SELECT depth FROM t%u"
		          " WHERE folder_id=%llu", table_id, LLU(folder_id));
		auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
		if (pstmt == nullptr) {
			return FALSE;
		}
		if (SQLITE_ROW != sqlite3_step(pstmt)) {
			ppropvals->count = 0;
			return TRUE;
		}
		depth = sqlite3_column_int64(pstmt, 0);
		pstmt.finalize();
		sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
		count = 0;
		ppropvals->ppropval = cu_alloc<TAGGED_PROPVAL>(pproptags->count);
		if (NULL == ppropvals->ppropval) {
			sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
		for (i=0; i<pproptags->count; i++) {
			if (PROP_TAG_DEPTH == pproptags->pproptag[i]) {
				pvalue = cu_alloc<uint32_t>();
				if (NULL == pvalue) {
					sqlite3_exec(pdb->psqlite,
						"ROLLBACK", NULL, NULL, NULL);
					return FALSE;
				}
				*(uint32_t*)pvalue = depth;
			} else {
				if (FALSE == common_util_get_property(
					FOLDER_PROPERTIES_TABLE, folder_id, cpid,
					pdb->psqlite, pproptags->pproptag[i], &pvalue)) {
					sqlite3_exec(pdb->psqlite,
						"ROLLBACK", NULL, NULL, NULL);
					return FALSE;
				}
				if (NULL == pvalue) {
					continue;
				}
				switch (PROP_TYPE(pproptags->pproptag[i])) {
				case PT_UNICODE:
					utf8_truncate(static_cast<char *>(pvalue), 255);
					break;
				case PT_STRING8:
					table_truncate_string(cpid, static_cast<char *>(pvalue));
					break;
				case PT_BINARY:
					if (((BINARY*)pvalue)->cb > 510) {
						((BINARY*)pvalue)->cb = 510;
					}
					break;
				}
			}
			ppropvals->ppropval[count].proptag = pproptags->pproptag[i];
			ppropvals->ppropval[count].pvalue = pvalue;
			count ++;
		}
		ppropvals->count = count;
		sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	} else if (TABLE_TYPE_CONTENT == ptnode->type) {
		inst_id = rop_util_get_replid(inst_id) == 1 ?
		          rop_util_get_gc_value(inst_id) :
		          rop_util_get_gc_value(inst_id) | 0x100000000000000ULL;
		snprintf(sql_string, arsizeof(sql_string), "SELECT * FROM t%u"
					" WHERE inst_id=%llu AND inst_num=%u",
					table_id, LLU(inst_id), inst_num);
		auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
		if (pstmt == nullptr) {
			return FALSE;
		}
		if (SQLITE_ROW != sqlite3_step(pstmt)) {
			ppropvals->count = 0;
			return TRUE;
		}
		row_type = sqlite3_column_int64(pstmt, 4);
		xstmt pstmt1, pstmt2;
		if (NULL != ptnode->psorts && ptnode->psorts->ccategories > 0) {
			snprintf(sql_string, arsizeof(sql_string), "SELECT parent_id FROM"
					" t%u WHERE row_id=?", ptnode->table_id);
			pstmt1 = gx_sql_prep(pdb->tables.psqlite, sql_string);
			if (pstmt1 == nullptr) {
				return FALSE;
			}
			snprintf(sql_string, arsizeof(sql_string), "SELECT value FROM"
					" t%u WHERE row_id=?", ptnode->table_id);
			pstmt2 = gx_sql_prep(pdb->tables.psqlite, sql_string);
			if (pstmt2 == nullptr) {
				return FALSE;
			}
		} else {
			pstmt1 = NULL;
			pstmt2 = NULL;
		}
		sqlite3_exec(pdb->psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);			
		count = 0;
		ppropvals->ppropval = cu_alloc<TAGGED_PROPVAL>(pproptags->count);
		if (NULL == ppropvals->ppropval) {
			pstmt.finalize();
			pstmt1.finalize();
			pstmt2.finalize();
			sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
		for (i=0; i<pproptags->count; i++) {
			if (FALSE == table_column_content_tmptbl(pstmt, pstmt1,
				pstmt2, ptnode->psorts, ptnode->folder_id, row_type,
				pproptags->pproptag[i], ptnode->instance_tag,
				ptnode->extremum_tag, &pvalue)) {
				if (CONTENT_ROW_HEADER == row_type) {
					continue;
				}
				if (FALSE == common_util_get_property(
					MESSAGE_PROPERTIES_TABLE, inst_id, cpid,
					pdb->psqlite, pproptags->pproptag[i], &pvalue)) {
					pstmt.finalize();
					pstmt1.finalize();
					pstmt2.finalize();
					sqlite3_exec(pdb->psqlite, "ROLLBACK", NULL, NULL, NULL);
					return FALSE;
				}
			}
			if (NULL == pvalue) {
				continue;
			}
			switch (PROP_TYPE(pproptags->pproptag[i])) {
			case PT_UNICODE:
				utf8_truncate(static_cast<char *>(pvalue), 255);
				break;
			case PT_STRING8:
				table_truncate_string(cpid, static_cast<char *>(pvalue));
				break;
			case PT_BINARY:
				if (((BINARY*)pvalue)->cb > 510) {
					((BINARY*)pvalue)->cb = 510;
				}
				break;
			}
			ppropvals->ppropval[count].proptag = pproptags->pproptag[i];
			ppropvals->ppropval[count].pvalue = pvalue;
			count ++;
		}
		ppropvals->count = count;
		pstmt.finalize();
		pstmt1.finalize();
		pstmt2.finalize();
		sqlite3_exec(pdb->psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	} else {
		ppropvals->count = 0;
	}
	return TRUE;
}
	
BOOL exmdb_server_mark_table(const char *dir,
	uint32_t table_id, uint32_t position, uint64_t *pinst_id,
	uint32_t *pinst_num, uint32_t *prow_type)
{
	TABLE_NODE *ptnode;
	char sql_string[256];
	DOUBLE_LIST_NODE *pnode;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	*pinst_id = 0;
	*pinst_num = 0;
	*prow_type = 0;
	for (pnode=double_list_get_head(&pdb->tables.table_list); NULL!=pnode;
		pnode=double_list_get_after(&pdb->tables.table_list, pnode)) {
		if (table_id == ((TABLE_NODE*)pnode->pdata)->table_id) {
			break;
		}
	}
	if (NULL == pnode) {
		return TRUE;
	}
	ptnode = (TABLE_NODE*)pnode->pdata;
	switch (ptnode->type) {
	case TABLE_TYPE_HIERARCHY:
		snprintf(sql_string, arsizeof(sql_string), "SELECT folder_id FROM t%u"
				" WHERE idx=%u", ptnode->table_id, position + 1);
		break;
	case TABLE_TYPE_CONTENT:
		snprintf(sql_string, arsizeof(sql_string), "SELECT inst_id,"
			" inst_num, row_type FROM t%u WHERE idx=%u",
			ptnode->table_id, position + 1);
		break;
	case TABLE_TYPE_PERMISSION:
		snprintf(sql_string, arsizeof(sql_string), "SELECT member_id FROM t%u "
			"WHERE idx=%u", ptnode->table_id, position + 1);
		break;
	case TABLE_TYPE_RULE:
		snprintf(sql_string, arsizeof(sql_string), "SELECT rule_id FROM t%u "
			"WHERE idx=%u", ptnode->table_id, position + 1);
		break;
	default:
		return FALSE;
	}
	auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	if (SQLITE_ROW == sqlite3_step(pstmt)) {
		*pinst_id = sqlite3_column_int64(pstmt, 0);
		switch (ptnode->type) {
		case TABLE_TYPE_HIERARCHY:
			*pinst_id = (*pinst_id & 0xFF00000000000000ULL) == 0 ?
			            rop_util_make_eid_ex(1, *pinst_id) :
			            rop_util_make_eid_ex(*pinst_id >> 48, *pinst_id & 0x00FFFFFFFFFFFFFFULL);
			break;
		case TABLE_TYPE_CONTENT:
			*pinst_id = (*pinst_id & 0xFF00000000000000ULL) == 0 ?
			            rop_util_make_eid_ex(1, *pinst_id) :
			            rop_util_make_eid_ex(2, *pinst_id & 0x00FFFFFFFFFFFFFFULL);
			*pinst_num = sqlite3_column_int64(pstmt, 1);
			*prow_type = sqlite3_column_int64(pstmt, 2);
			break;
		case TABLE_TYPE_RULE:
			*pinst_id = rop_util_make_eid_ex(1, *pinst_id);
			break;
		}
	}
	return TRUE;
}

BOOL exmdb_server_get_table_all_proptags(const char *dir,
	uint32_t table_id, PROPTAG_ARRAY *pproptags)
{
	uint32_t proptag;
	TABLE_NODE *ptnode;
	INT_HASH_ITER *iter;
	char sql_string[256];
	INT_HASH_TABLE *phash;
	DOUBLE_LIST_NODE *pnode;
	uint32_t tmp_proptags[0x1000];
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	for (pnode=double_list_get_head(&pdb->tables.table_list); NULL!=pnode;
		pnode=double_list_get_after(&pdb->tables.table_list, pnode)) {
		if (table_id == ((TABLE_NODE*)pnode->pdata)->table_id) {
			break;
		}
	}
	if (NULL == pnode) {
		pproptags->count = 0;
		pproptags->pproptag = NULL;
		return TRUE;
	}
	ptnode = (TABLE_NODE*)pnode->pdata;
	switch (ptnode->type) {
	case TABLE_TYPE_HIERARCHY: {
		phash = int_hash_init(0x1000, sizeof(int));
		if (NULL == phash) {
			return FALSE;
		}
		snprintf(sql_string, arsizeof(sql_string), "SELECT "
			"folder_id FROM t%u", ptnode->table_id);
		auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
		if (pstmt == nullptr) {
			int_hash_free(phash);
			return FALSE;
		}
		snprintf(sql_string, arsizeof(sql_string), "SELECT proptag "
			"FROM folder_properties WHERE folder_id=?");
		auto pstmt1 = gx_sql_prep(pdb->psqlite, sql_string);
		if (pstmt1 == nullptr) {
			int_hash_free(phash);
			return FALSE;
		}
		while (SQLITE_ROW == sqlite3_step(pstmt)) {
			sqlite3_bind_int64(pstmt1, 1,
				sqlite3_column_int64(pstmt, 0));
			while (SQLITE_ROW == sqlite3_step(pstmt1)) {
				proptag = sqlite3_column_int64(pstmt1, 0);
				if (NULL != int_hash_query(phash, proptag)) {
					continue;	
				}
				int_hash_add(phash, proptag, &proptag);
			}
			sqlite3_reset(pstmt1);
		}
		pstmt.finalize();
		pstmt1.finalize();
		iter = int_hash_iter_init(phash);
		if (NULL == iter) {
			int_hash_free(phash);
			return FALSE;
		}
		pproptags->count = 0;
		 for (int_hash_iter_begin(iter); !int_hash_iter_done(iter);
			int_hash_iter_forward(iter)) {
			int_hash_iter_get_value(iter, reinterpret_cast<int *>(&proptag));
			tmp_proptags[pproptags->count] = proptag;
			pproptags->count ++;
		}
		int_hash_iter_free(iter);
		int_hash_free(phash);
		tmp_proptags[pproptags->count++] = PROP_TAG_DEPTH;
		pproptags->pproptag = cu_alloc<uint32_t>(pproptags->count);
		if (NULL == pproptags->pproptag) {
			pproptags->count = 0;
			return FALSE;
		}
		memcpy(pproptags->pproptag, tmp_proptags,
			sizeof(uint32_t)*pproptags->count);
		return TRUE;
	}
	case TABLE_TYPE_CONTENT:	 {
		phash = int_hash_init(0x1000, sizeof(int));
		if (NULL == phash) {
			return FALSE;
		}
		snprintf(sql_string, arsizeof(sql_string), "SELECT inst_id,"
				" row_type FROM t%u", ptnode->table_id);
		auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
		if (pstmt == nullptr) {
			int_hash_free(phash);
			return FALSE;
		}
		snprintf(sql_string, arsizeof(sql_string), "SELECT proptag "
			"FROM message_properties WHERE message_id=?");
		auto pstmt1 = gx_sql_prep(pdb->psqlite, sql_string);
		if (pstmt1 == nullptr) {
			int_hash_free(phash);
			return FALSE;
		}
		while (SQLITE_ROW == sqlite3_step(pstmt)) {
			if (sqlite3_column_int64(pstmt, 1)
				!= CONTENT_ROW_MESSAGE) {
				continue;
			}
			sqlite3_bind_int64(pstmt1, 1,
				sqlite3_column_int64(pstmt, 0));
			while (SQLITE_ROW == sqlite3_step(pstmt1)) {
				proptag = sqlite3_column_int64(pstmt1, 0);
				if (NULL != int_hash_query(phash, proptag)) {
					continue;	
				}
				int_hash_add(phash, proptag, &proptag);
			}
			sqlite3_reset(pstmt1);
		}
		pstmt.finalize();
		pstmt1.finalize();
		iter = int_hash_iter_init(phash);
		if (NULL == iter) {
			int_hash_free(phash);
			return FALSE;
		}
		pproptags->count = 0;
		 for (int_hash_iter_begin(iter); !int_hash_iter_done(iter);
			int_hash_iter_forward(iter)) {
			int_hash_iter_get_value(iter, reinterpret_cast<int *>(&proptag));
			tmp_proptags[pproptags->count] = proptag;
			pproptags->count ++;
		}
		int_hash_iter_free(iter);
		int_hash_free(phash);
		tmp_proptags[pproptags->count++] = PROP_TAG_MID;
		tmp_proptags[pproptags->count++] = PR_MESSAGE_SIZE;
		tmp_proptags[pproptags->count++] = PR_ASSOCIATED;
		tmp_proptags[pproptags->count++] = PROP_TAG_CHANGENUMBER;
		tmp_proptags[pproptags->count++] = PR_READ;
		tmp_proptags[pproptags->count++] = PROP_TAG_HASATTACHMENTS;
		tmp_proptags[pproptags->count++] = PR_MESSAGE_FLAGS;
		tmp_proptags[pproptags->count++] = PR_DISPLAY_TO;
		tmp_proptags[pproptags->count++] = PR_DISPLAY_CC;
		tmp_proptags[pproptags->count++] = PR_DISPLAY_BCC;
		tmp_proptags[pproptags->count++] = PROP_TAG_INSTID;
		tmp_proptags[pproptags->count++] = PROP_TAG_INSTANCENUM;
		tmp_proptags[pproptags->count++] = PROP_TAG_ROWTYPE;
		tmp_proptags[pproptags->count++] = PROP_TAG_DEPTH;
		tmp_proptags[pproptags->count++] = PROP_TAG_CONTENTCOUNT;
		tmp_proptags[pproptags->count++] = PROP_TAG_CONTENTUNREADCOUNT;
		pproptags->pproptag = cu_alloc<uint32_t>(pproptags->count);
		if (NULL == pproptags->pproptag) {
			pproptags->count = 0;
			return FALSE;
		}
		memcpy(pproptags->pproptag, tmp_proptags,
			sizeof(uint32_t)*pproptags->count);
		return TRUE;
	}
	case TABLE_TYPE_PERMISSION:
		pproptags->count = 4;
		pproptags->pproptag = cu_alloc<uint32_t>(4);
		if (NULL == pproptags->pproptag) {
			return FALSE;
		}
		pproptags->pproptag[0] = PR_ENTRYID;
		pproptags->pproptag[1] = PROP_TAG_MEMBERID;
		pproptags->pproptag[2] = PROP_TAG_MEMBERNAME;
		pproptags->pproptag[3] = PROP_TAG_MEMBERRIGHTS;
		return TRUE;
	case TABLE_TYPE_RULE:
		pproptags->count = 10;
		pproptags->pproptag = cu_alloc<uint32_t>(10);
		if (NULL == pproptags->pproptag) {
			return FALSE;
		}
		pproptags->pproptag[0] = PR_RULE_ID;
		pproptags->pproptag[1] = PR_RULE_SEQUENCE;
		pproptags->pproptag[2] = PR_RULE_STATE;
		pproptags->pproptag[3] = PR_RULE_NAME;
		pproptags->pproptag[4] = PR_RULE_PROVIDER;
		pproptags->pproptag[5] = PR_RULE_LEVEL;
		pproptags->pproptag[6] = PR_RULE_USER_FLAGS;
		pproptags->pproptag[7] = PR_RULE_PROVIDER_DATA;
		pproptags->pproptag[8] = PR_RULE_CONDITION;
		pproptags->pproptag[9] = PR_RULE_ACTIONS;
		return TRUE;
	}
	return FALSE;
}

static BOOL table_traverse_sub_contents(uint32_t step,
	uint64_t parent_id, sqlite3_stmt *pstmt,
	sqlite3_stmt *pstmt1, uint32_t *pcount)
{
	uint64_t row_id;
	DOUBLE_LIST tmp_list;
	DOUBLE_LIST_NODE *pnode;
	
	double_list_init(&tmp_list);
	sqlite3_bind_int64(pstmt1, 1, parent_id);
	while (SQLITE_ROW == sqlite3_step(pstmt1)) {
		(*pcount) ++;
		if (0 == sqlite3_column_int64(pstmt1, 1)) {
			continue;
		}
		row_id = sqlite3_column_int64(pstmt1, 0);
		if (1 == step) {
			sqlite3_bind_int64(pstmt, 1, row_id);
			if (SQLITE_ROW != sqlite3_step(pstmt)) {
				return FALSE;
			}
			*pcount += sqlite3_column_int64(pstmt, 0);
			sqlite3_reset(pstmt);
		} else {
			pnode = cu_alloc<DOUBLE_LIST_NODE>();
			if (NULL == pnode) {
				return FALSE;
			}
			pnode->pdata = cu_alloc<uint64_t>();
			if (NULL == pnode->pdata) {
				return FALSE;
			}
			*(uint64_t*)pnode->pdata = row_id;
			double_list_append_as_tail(&tmp_list, pnode);
		}
	}
	sqlite3_reset(pstmt1);
	if (1 == step) {
		return TRUE;
	}
	while ((pnode = double_list_pop_front(&tmp_list)) != nullptr) {
		if (FALSE == table_traverse_sub_contents(step - 1,
			*(uint64_t*)pnode->pdata, pstmt, pstmt1, pcount)) {
			return FALSE;	
		}
	}
	return TRUE;
}

static BOOL table_expand_sub_contents(int depth,
	uint64_t parent_id, sqlite3_stmt *pstmt,
	sqlite3_stmt *pstmt1, uint32_t *pidx)
{
	uint64_t row_id;
	uint8_t row_stat;
	
	sqlite3_bind_int64(pstmt, 1, -parent_id);
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		sqlite3_reset(pstmt);
		return TRUE;
	}
	do {
		row_id = sqlite3_column_int64(pstmt, 0);
		row_stat = sqlite3_column_int64(pstmt, 1);
		sqlite3_reset(pstmt);
		(*pidx) ++;
		sqlite3_bind_int64(pstmt1, 1, *pidx);
		sqlite3_bind_int64(pstmt1, 2, row_id);
		if (SQLITE_DONE != sqlite3_step(pstmt1)) {
			return FALSE;
		}
		sqlite3_reset(pstmt1);
		if (depth > 0 && 0 != row_stat) {
			if (FALSE == table_expand_sub_contents(
				depth - 1, row_id, pstmt, pstmt1, pidx)) {
				return FALSE;
			}
		}
		sqlite3_bind_int64(pstmt, 1, row_id);
	} while (SQLITE_ROW == sqlite3_step(pstmt));
	sqlite3_reset(pstmt);
	return TRUE;
}

BOOL exmdb_server_expand_table(const char *dir,
	uint32_t table_id, uint64_t inst_id, BOOL *pb_found,
	int32_t *pposition, uint32_t *prow_count)
{
	int depth;
	uint32_t idx;
	uint64_t row_id;
	TABLE_NODE *ptnode;
	char sql_string[256];
	DOUBLE_LIST_NODE *pnode;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	for (pnode=double_list_get_head(&pdb->tables.table_list); NULL!=pnode;
		pnode=double_list_get_after(&pdb->tables.table_list, pnode)) {
		if (table_id == ((TABLE_NODE*)pnode->pdata)->table_id) {
			break;
		}
	}
	if (NULL == pnode) {
		*pb_found = FALSE;
		return TRUE;
	}
	ptnode = (TABLE_NODE*)pnode->pdata;
	if (TABLE_TYPE_CONTENT != ptnode->type ||
		2 != rop_util_get_replid(inst_id)) {
		*pb_found = FALSE;
		return TRUE;
	}
	inst_id = rop_util_get_gc_value(inst_id) | 0x100000000000000ULL;
	snprintf(sql_string, arsizeof(sql_string), "SELECT row_id, row_type, "
			"row_stat, depth, idx FROM t%u WHERE inst_id=%llu"
			" AND inst_num=0", ptnode->table_id, LLU(inst_id));
	auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	if (SQLITE_ROW != sqlite3_step(pstmt) ||
		CONTENT_ROW_HEADER != sqlite3_column_int64(pstmt, 1)) {
		*pb_found = FALSE;
		return TRUE;
	}
	*pb_found = TRUE;
	if (0 != sqlite3_column_int64(pstmt, 2)) {
		*pposition = -1;
		return TRUE;
	}
	row_id = sqlite3_column_int64(pstmt, 0);
	depth = sqlite3_column_int64(pstmt, 3);
	idx = sqlite3_column_int64(pstmt, 4);
	*pposition = idx - 1;
	pstmt.finalize();
	snprintf(sql_string, arsizeof(sql_string), "SELECT count(*) FROM"
			" t%u WHERE parent_id=?", ptnode->table_id);
	pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	if (ptnode->psorts->ccategories == depth + 1) {
		sqlite3_bind_int64(pstmt, 1, row_id);
		if (SQLITE_ROW != sqlite3_step(pstmt)) {
			return FALSE;
		}
		*prow_count = sqlite3_column_int64(pstmt, 0);
	} else {
		snprintf(sql_string, arsizeof(sql_string), "SELECT row_id, row_stat "
				"FROM t%u WHERE parent_id=?", ptnode->table_id);
		auto pstmt1 = gx_sql_prep(pdb->tables.psqlite, sql_string);
		if (pstmt1 == nullptr) {
			return FALSE;
		}
		*prow_count = 0;
		if (FALSE == table_traverse_sub_contents(
			ptnode->psorts->ccategories - depth - 1,
			row_id, pstmt, pstmt1, prow_count)) {
			return FALSE;
		}
	}
	pstmt.finalize();
	snprintf(sql_string, arsizeof(sql_string), "UPDATE t%u SET row_stat=1 "
	        "WHERE row_id=%llu", ptnode->table_id, LLU(row_id));
	if (SQLITE_OK != sqlite3_exec(pdb->tables.psqlite,
		sql_string, NULL, NULL, NULL)) {
		return FALSE;
	}
	if (0 == *prow_count) {
		return TRUE;
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT row_id "
		"FROM t%u WHERE idx>%u ORDER BY idx DESC",
		ptnode->table_id, idx);
	pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "UPDATE t%u SET idx=idx+%u"
			" WHERE row_id=?", ptnode->table_id, *prow_count);
	auto pstmt1 = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt1 == nullptr) {
		return FALSE;
	}
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		sqlite3_bind_int64(pstmt1, 1,
			sqlite3_column_int64(pstmt, 0));
		if (SQLITE_DONE != sqlite3_step(pstmt1)) {
			return FALSE;
		}
		sqlite3_reset(pstmt1);
	}
	pstmt.finalize();
	pstmt1.finalize();
	snprintf(sql_string, arsizeof(sql_string), "SELECT row_id, row_stat"
			" FROM t%u WHERE prev_id=?", ptnode->table_id);
	pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "UPDATE t%u SET"
		" idx=? WHERE row_id=?", ptnode->table_id);
	pstmt1 = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt1 == nullptr) {
		return FALSE;
	}
	if (FALSE == table_expand_sub_contents(
		ptnode->psorts->ccategories - depth - 1,
		row_id, pstmt, pstmt1, &idx)) {
		return false;
	}
	return TRUE;
}

BOOL exmdb_server_collapse_table(const char *dir,
	uint32_t table_id, uint64_t inst_id, BOOL *pb_found,
	int32_t *pposition, uint32_t *prow_count)
{
	int depth;
	uint32_t idx;
	uint64_t row_id;
	uint64_t prev_id;
	TABLE_NODE *ptnode;
	char sql_string[256];
	DOUBLE_LIST_NODE *pnode;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	for (pnode=double_list_get_head(&pdb->tables.table_list); NULL!=pnode;
		pnode=double_list_get_after(&pdb->tables.table_list, pnode)) {
		if (table_id == ((TABLE_NODE*)pnode->pdata)->table_id) {
			break;
		}
	}
	if (NULL == pnode) {
		*pb_found = FALSE;
		return TRUE;
	}
	ptnode = (TABLE_NODE*)pnode->pdata;
	if (TABLE_TYPE_CONTENT != ptnode->type ||
		2 != rop_util_get_replid(inst_id)) {
		*pb_found = FALSE;
		return TRUE;
	}
	inst_id = rop_util_get_gc_value(inst_id) | 0x100000000000000ULL;
	snprintf(sql_string, arsizeof(sql_string), "SELECT row_id, row_type, "
		"row_stat, depth, idx FROM t%u WHERE inst_id=%llu AND"
		" inst_num=0", ptnode->table_id, LLU(inst_id));
	auto pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	if (SQLITE_ROW != sqlite3_step(pstmt) ||
		CONTENT_ROW_HEADER != sqlite3_column_int64(pstmt, 1)) {
		*pb_found = FALSE;
		return TRUE;
	}
	*pb_found = TRUE;
	if (0 == sqlite3_column_int64(pstmt, 2)) {
		*pposition = -1;
		return TRUE;
	}
	row_id = sqlite3_column_int64(pstmt, 0);
	depth = sqlite3_column_int64(pstmt, 3);
	idx = sqlite3_column_int64(pstmt, 4);
	*pposition = idx - 1;
	pstmt.finalize();
	snprintf(sql_string, arsizeof(sql_string), "UPDATE t%u SET row_stat=0 "
	        "WHERE row_id=%llu", ptnode->table_id, LLU(row_id));
	if (SQLITE_OK != sqlite3_exec(pdb->tables.psqlite,
		sql_string, NULL, NULL, NULL)) {
		return FALSE;
	}
	*prow_count = 0;
	prev_id = row_id;
	snprintf(sql_string, arsizeof(sql_string), "SELECT row_id, "
			"depth, prev_id FROM t%u WHERE idx>%u "
			"ORDER BY idx ASC", ptnode->table_id, idx);
	pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt == nullptr) {
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "UPDATE t%u SET"
		" idx=? WHERE row_id=?", ptnode->table_id);
	auto pstmt1 = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt1 == nullptr) {
		return FALSE;
	}
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		row_id = sqlite3_column_int64(pstmt, 0);
		if (0 != prev_id &&
			(depth > sqlite3_column_int64(pstmt, 1) ||
		    gx_sql_col_uint64(pstmt, 2) == prev_id)) {
			if (0 == *prow_count) {
				break;
			}
			prev_id = 0;
		}
		if (0 != prev_id) {
			(*prow_count) ++;
			sqlite3_bind_null(pstmt1, 1);
		} else {
			idx ++;
			sqlite3_bind_int64(pstmt1, 1, idx);
		}
		sqlite3_bind_int64(pstmt1, 2, row_id);
		if (SQLITE_DONE != sqlite3_step(pstmt1)) {
			return FALSE;
		}
		sqlite3_reset(pstmt1);
	}
	return TRUE;
}

BOOL exmdb_server_store_table_state(const char *dir,
	uint32_t table_id, uint64_t inst_id,
	uint32_t inst_num, uint32_t *pstate_id)
{
	int i;
	int depth;
	void *pvalue;
	uint16_t type;
	uint64_t row_id;
	uint64_t last_id;
	sqlite3 *psqlite;
	EXT_PUSH ext_push;
	char tmp_path[256];
	TABLE_NODE *ptnode;
	char tmp_buff[1024];
	uint32_t tmp_proptag;
	char sql_string[1024];
	struct stat node_stat;
	DOUBLE_LIST_NODE *pnode;
	
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	for (pnode=double_list_get_head(&pdb->tables.table_list); NULL!=pnode;
		pnode=double_list_get_after(&pdb->tables.table_list, pnode)) {
		if (table_id == ((TABLE_NODE*)pnode->pdata)->table_id) {
			break;
		}
	}
	*pstate_id = 0;
	if (NULL == pnode) {
		return TRUE;
	}
	ptnode = (TABLE_NODE*)pnode->pdata;
	if (TABLE_TYPE_CONTENT != ptnode->type) {
		return TRUE;
	}
	snprintf(tmp_path, arsizeof(tmp_path), "%s/tmp/state.sqlite3", exmdb_server_get_dir());
	if (0 != stat(tmp_path, &node_stat)) {
		auto ret = sqlite3_open_v2(tmp_path, &psqlite, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, nullptr);
		if (ret != SQLITE_OK) {
			fprintf(stderr, "E-1435: sqlite3_open %s: %s\n", tmp_path, sqlite3_errstr(ret));
			return FALSE;
		}
		sqlite3_exec(psqlite, "PRAGMA journal_mode=OFF", NULL, NULL, NULL);
		sqlite3_exec(psqlite, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
		sprintf(sql_string,
			"CREATE TABLE state_info "
			"(state_id INTEGER PRIMARY KEY AUTOINCREMENT, "
			"folder_id INTEGER NOT NULL, "
			"table_flags INTEGER NOT NULL, "
			"sorts BLOB, "
			"message_id INTEGER DEFAULT NULL, "
			"inst_num INTEGER DEFAULT NULL, "
			"header_id INTEGER DEFAULT NULL, "
			"header_stat INTEGER DEFAULT NULL)");
		if (SQLITE_OK != sqlite3_exec(psqlite,
			sql_string, NULL, NULL, NULL)) {
			sqlite3_close(psqlite);
			if (remove(tmp_path) < 0 && errno != ENOENT)
				fprintf(stderr, "W-1348: remove %s: %s\n", tmp_path, strerror(errno));
			return FALSE;
		}
		snprintf(sql_string, arsizeof(sql_string), "CREATE UNIQUE INDEX state_index"
			" ON state_info (folder_id, table_flags, sorts)");
		if (SQLITE_OK != sqlite3_exec(psqlite,
			sql_string, NULL, NULL, NULL)) {
			sqlite3_close(psqlite);
			remove(tmp_path);
			return FALSE;
		}
	} else {
		auto ret = sqlite3_open_v2(tmp_path, &psqlite, SQLITE_OPEN_READWRITE, nullptr);
		if (ret != SQLITE_OK) {
			fprintf(stderr, "E-1436: sqlite3_open %s: %s\n", tmp_path, sqlite3_errstr(ret));
			return FALSE;
		}
		sqlite3_exec(psqlite, "PRAGMA journal_mode=OFF", NULL, NULL, NULL);
		sqlite3_exec(psqlite, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
	}
	if (NULL != ptnode->psorts && 0 != ptnode->psorts->ccategories) {
		strcpy(sql_string, "SELECT state_id FROM "
			"state_info WHERE folder_id=? AND table_flags=? "
			"AND sorts=?");
	} else {
		strcpy(sql_string, "SELECT state_id FROM "
			"state_info WHERE folder_id=? AND table_flags=? "
			"AND sorts IS NULL");
	}
	auto pstmt = gx_sql_prep(psqlite, sql_string);
	if (pstmt == nullptr) {
		sqlite3_close(psqlite);
		return FALSE;
	}
	sqlite3_bind_int64(pstmt, 1, ptnode->folder_id);
	sqlite3_bind_int64(pstmt, 2, ptnode->table_flags);
	if (NULL != ptnode->psorts && 0 != ptnode->psorts->ccategories) {
		if (!ext_push.init(tmp_buff, sizeof(tmp_buff), 0) ||
		    ext_push.p_sortorder_set(ptnode->psorts) != EXT_ERR_SUCCESS) {
			pstmt.finalize();
			sqlite3_close(psqlite);
			return FALSE;
		}
		sqlite3_bind_blob(pstmt, 3, ext_push.m_udata, ext_push.m_offset, SQLITE_STATIC);
	}
	if (SQLITE_ROW == sqlite3_step(pstmt)) {
		*pstate_id = sqlite3_column_int64(pstmt, 0);
	}
	pstmt.finalize();
	sqlite3_exec(psqlite, "BEGIN TRANSACTION", NULL, NULL, NULL);
	if (0 == *pstate_id) {
		strcpy(sql_string, "INSERT INTO state_info"
			"(folder_id, table_flags, sorts) VALUES (?, ?, ?)");
		pstmt = gx_sql_prep(psqlite, sql_string);
		if (pstmt == nullptr) {
			sqlite3_exec(psqlite, "ROLLBACK", NULL, NULL, NULL);
			sqlite3_close(psqlite);
			return FALSE;
		}
		sqlite3_bind_int64(pstmt, 1, ptnode->folder_id);
		sqlite3_bind_int64(pstmt, 2, ptnode->table_flags);
		if (NULL == ptnode->psorts || 0 == ptnode->psorts->ccategories) {
			sqlite3_bind_null(pstmt, 3);
		} else {
			sqlite3_bind_blob(pstmt, 3, ext_push.m_udata, ext_push.m_offset, SQLITE_STATIC);
		}
		if (SQLITE_DONE != sqlite3_step(pstmt)) {
			pstmt.finalize();
			sqlite3_exec(psqlite, "ROLLBACK", NULL, NULL, NULL);
			sqlite3_close(psqlite);
			return FALSE;
		}
		*pstate_id = sqlite3_last_insert_rowid(psqlite);
		pstmt.finalize();
	} else {
		if (NULL != ptnode->psorts && 0 != ptnode->psorts->ccategories) {
			snprintf(sql_string, arsizeof(sql_string), "DROP TABLE s%u", *pstate_id);
			if (SQLITE_OK != sqlite3_exec(psqlite,
				sql_string, NULL, NULL, NULL)) {
				sqlite3_exec(psqlite, "ROLLBACK", NULL, NULL, NULL);
				sqlite3_close(psqlite);
				return FALSE;
			}
		}
		if (1 != rop_util_get_replid(inst_id)) {
			snprintf(sql_string, arsizeof(sql_string), "UPDATE "
				"state_info SET message_id=NULL, "
				"inst_num=NULL WHERE state_id=%u",
				*pstate_id);
			if (SQLITE_OK != sqlite3_exec(psqlite,
				sql_string, NULL, NULL, NULL)) {
				sqlite3_exec(psqlite, "ROLLBACK", NULL, NULL, NULL);
				sqlite3_close(psqlite);
				return FALSE;
			}
		}
	}
	if (1 == rop_util_get_replid(inst_id)) {
		snprintf(sql_string, arsizeof(sql_string), "UPDATE "
			"state_info SET message_id=%llu, "
			"inst_num=%u WHERE state_id=%u",
			LLU(rop_util_get_gc_value(inst_id)),
			inst_num, *pstate_id);
		if (SQLITE_OK != sqlite3_exec(psqlite,
			sql_string, NULL, NULL, NULL)) {
			sqlite3_exec(psqlite, "ROLLBACK", NULL, NULL, NULL);
			sqlite3_close(psqlite);
			return FALSE;
		}
	}
	if (NULL == ptnode->psorts || 0 == ptnode->psorts->ccategories) {
		sqlite3_exec(psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
		sqlite3_close(psqlite);
		return TRUE;
	}
	auto sql_len = snprintf(sql_string, arsizeof(sql_string), "CREATE TABLE s%u "
			"(depth INTEGER NOT NULL ", *pstate_id);
	for (i=0; i<ptnode->psorts->ccategories; i++) {
		tmp_proptag = PROP_TAG(ptnode->psorts->psort[i].type, ptnode->psorts->psort[i].propid);
		type = ptnode->psorts->psort[i].type;
		if (ptnode->instance_tag == tmp_proptag)
			type &= ~MVI_FLAG;
		switch (type) {
		case PT_STRING8:
		case PT_UNICODE:
			sql_len += gx_snprintf(sql_string + sql_len,
			           GX_ARRAY_SIZE(sql_string) - sql_len,
						", v%x TEXT", tmp_proptag);
			break;
		case PT_FLOAT:
		case PT_DOUBLE:
		case PT_APPTIME:
			sql_len += gx_snprintf(sql_string + sql_len,
			           GX_ARRAY_SIZE(sql_string) - sql_len,
						", v%x REAL", tmp_proptag);
			break;
		case PT_CURRENCY:
		case PT_I8:
		case PT_SYSTIME:
		case PT_SHORT:
		case PT_LONG:
		case PT_BOOLEAN:
			sql_len += gx_snprintf(sql_string + sql_len,
			           GX_ARRAY_SIZE(sql_string) - sql_len,
						", v%x INTEGER", tmp_proptag);
			break;
		case PT_CLSID:
		case PT_SVREID:
		case PT_OBJECT:
		case PT_BINARY:
			sql_len += gx_snprintf(sql_string + sql_len,
			           GX_ARRAY_SIZE(sql_string) - sql_len,
						", v%x BLOB", tmp_proptag);
			break;
		default:
			sqlite3_exec(psqlite, "ROLLBACK", NULL, NULL, NULL);
			sqlite3_close(psqlite);
			return FALSE;
		}
	}
	sql_string[sql_len] = ')';
	sql_len ++;
	sql_string[sql_len] = '\0';
	if (SQLITE_OK != sqlite3_exec(psqlite,
		sql_string, NULL, NULL, NULL)) {
		sqlite3_exec(psqlite, "ROLLBACK", NULL, NULL, NULL);
		sqlite3_close(psqlite);
		return FALSE;
	}
	snprintf(sql_string, GX_ARRAY_SIZE(sql_string), "SELECT row_id, inst_id,"
			" row_stat, depth FROM t%u", ptnode->table_id);
	pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt == nullptr) {
		sqlite3_exec(psqlite, "ROLLBACK", NULL, NULL, NULL);
		sqlite3_close(psqlite);
		return FALSE;
	}
	sql_len = snprintf(sql_string, arsizeof(sql_string), "INSERT"
		" INTO s%u VALUES (?", *pstate_id);
	for (i=0; i<ptnode->psorts->ccategories; i++) {
		sql_len += gx_snprintf(sql_string + sql_len,
		           GX_ARRAY_SIZE(sql_string) - sql_len, ", ?");
	}
	sql_string[sql_len] = ')';
	sql_len ++;
	sql_string[sql_len] = '\0';
	auto pstmt1 = gx_sql_prep(psqlite, sql_string);
	if (pstmt1 == nullptr) {
		pstmt.finalize();
		sqlite3_exec(psqlite, "ROLLBACK", NULL, NULL, NULL);
		sqlite3_close(psqlite);
		return FALSE;
	}
	snprintf(sql_string, GX_ARRAY_SIZE(sql_string), "SELECT parent_id FROM"
			" t%u WHERE row_id=?", ptnode->table_id);
	auto pstmt2 = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt2 == nullptr) {
		pstmt.finalize();
		pstmt1.finalize();
		sqlite3_exec(psqlite, "ROLLBACK", NULL, NULL, NULL);
		sqlite3_close(psqlite);
		return FALSE;
	}
	snprintf(sql_string, GX_ARRAY_SIZE(sql_string), "SELECT value FROM"
			" t%u WHERE row_id=?", ptnode->table_id);
	auto pstmt3 = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt3 == nullptr) {
		pstmt.finalize();
		pstmt1.finalize();
		pstmt2.finalize();
		sqlite3_close(psqlite);
		return FALSE;
	}
	uint64_t inst_id1 = rop_util_get_replid(inst_id) == 2 ?
	                    rop_util_get_gc_value(inst_id) | 0x100000000000000ULL : 0;
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		depth = sqlite3_column_int64(pstmt, 3);
		if (ptnode->psorts->ccategories == depth) {
			continue;	
		}
		if (gx_sql_col_uint64(pstmt, 1) == inst_id1) {
			last_id = sqlite3_last_insert_rowid(psqlite);
			snprintf(sql_string, arsizeof(sql_string), "UPDATE state_info SET header_id=%llu,"
				" header_stat=%llu WHERE state_id=%u", LLU(last_id + 1),
				LLU(sqlite3_column_int64(pstmt, 2)), *pstate_id);
			if (SQLITE_OK != sqlite3_exec(psqlite,
				sql_string, NULL, NULL, NULL)) {
				pstmt.finalize();
				pstmt1.finalize();
				pstmt2.finalize();
				pstmt3.finalize();
				sqlite3_exec(psqlite, "ROLLBACK", NULL, NULL, NULL);
				sqlite3_close(psqlite);
				return FALSE;
			}
		} else {
			if (0 == sqlite3_column_int64(pstmt, 2)) {
				if (depth >= ptnode->psorts->cexpanded) {
					continue;
				}
			} else {
				if (depth < ptnode->psorts->cexpanded) {
					continue;
				}
			}
		}
		sqlite3_bind_int64(pstmt1, 1, depth);
		row_id = sqlite3_column_int64(pstmt, 0);
		i = depth;
		while (true) {
			sqlite3_bind_int64(pstmt3, 1, row_id);
			type = ptnode->psorts->psort[i].type;
			if ((type & MVI_FLAG) == MVI_FLAG)
				type &= ~MVI_FLAG;
			if (SQLITE_ROW != sqlite3_step(pstmt3)) {
				pstmt.finalize();
				pstmt1.finalize();
				pstmt2.finalize();
				pstmt3.finalize();
				sqlite3_exec(psqlite, "ROLLBACK", NULL, NULL, NULL);
				sqlite3_close(psqlite);
				return FALSE;
			}
			pvalue = common_util_column_sqlite_statement(pstmt3, 0, type);
			sqlite3_reset(pstmt3);
			if (NULL == pvalue) {
				sqlite3_bind_null(pstmt1, i + 2);
			} else {
				if (FALSE == common_util_bind_sqlite_statement(
					pstmt1, i + 2, type, pvalue)) {
					pstmt.finalize();
					pstmt1.finalize();
					pstmt2.finalize();
					pstmt3.finalize();
					sqlite3_exec(psqlite, "ROLLBACK", NULL, NULL, NULL);
					sqlite3_close(psqlite);
					return FALSE;	
				}
			}
			if (0 == i) {
				break;
			}
			i --;
			sqlite3_bind_int64(pstmt2, 1, row_id);
			if (SQLITE_ROW != sqlite3_step(pstmt2)) {
				pstmt.finalize();
				pstmt1.finalize();
				pstmt2.finalize();
				pstmt3.finalize();
				sqlite3_exec(psqlite, "ROLLBACK", NULL, NULL, NULL);
				sqlite3_close(psqlite);
				return FALSE;
			}
			row_id = sqlite3_column_int64(pstmt2, 0);
			sqlite3_reset(pstmt2);
		}
		for (i=depth+1; i<ptnode->psorts->ccategories; i++) {
			sqlite3_bind_null(pstmt1, i + 2);
		}
		if (SQLITE_DONE != sqlite3_step(pstmt1)) {
			pstmt.finalize();
			pstmt1.finalize();
			pstmt2.finalize();
			pstmt3.finalize();
			sqlite3_exec(psqlite, "ROLLBACK", NULL, NULL, NULL);
			sqlite3_close(psqlite);
			return FALSE;
		}
		sqlite3_reset(pstmt1);
	}
	pstmt.finalize();
	pstmt1.finalize();
	pstmt2.finalize();
	pstmt3.finalize();
	sqlite3_exec(psqlite, "COMMIT TRANSACTION", NULL, NULL, NULL);
	sqlite3_close(psqlite);
	return TRUE;
}

BOOL exmdb_server_restore_table_state(const char *dir,
	uint32_t table_id, uint32_t state_id, int32_t *pposition)
{
	int i;
	int depth;
	void *pvalue;
	uint32_t idx;
	uint16_t type;
	uint64_t row_id;
	uint64_t row_id1;
	uint8_t row_stat;
	sqlite3 *psqlite;
	uint64_t inst_num;
	EXT_PUSH ext_push;
	char tmp_path[256];
	TABLE_NODE *ptnode;
	uint64_t header_id;
	uint64_t message_id;
	uint8_t header_stat;
	uint64_t current_id;
	xstmt pstmt1, pstmt2, pstmt3;
	char tmp_buff[1024];
	char sql_string[1024];
	struct stat node_stat;
	DOUBLE_LIST_NODE *pnode;
	
	row_id1 = 0;
	*pposition = -1;
	if (0 == state_id) {
		return TRUE;
	}
	auto pdb = db_engine_get_db(dir);
	if (pdb == nullptr || pdb->psqlite == nullptr)
		return FALSE;
	for (pnode=double_list_get_head(&pdb->tables.table_list); NULL!=pnode;
		pnode=double_list_get_after(&pdb->tables.table_list, pnode)) {
		if (table_id == ((TABLE_NODE*)pnode->pdata)->table_id) {
			break;
		}
	}
	if (NULL == pnode) {
		return TRUE;
	}
	ptnode = (TABLE_NODE*)pnode->pdata;
	if (TABLE_TYPE_CONTENT != ptnode->type) {
		return TRUE;
	}
	snprintf(tmp_path, arsizeof(tmp_path), "%s/tmp/state.sqlite3", exmdb_server_get_dir());
	if (0 != stat(tmp_path, &node_stat)) {
		return TRUE;
	}
	auto ret = sqlite3_open_v2(tmp_path, &psqlite, SQLITE_OPEN_READWRITE, nullptr);
	if (ret != SQLITE_OK) {
		fprintf(stderr, "E-1437: sqlite3_open %s: %s\n", tmp_path, sqlite3_errstr(ret));
		return FALSE;
	}
	sqlite3_exec(psqlite, "PRAGMA journal_mode=OFF", NULL, NULL, NULL);
	sqlite3_exec(psqlite, "PRAGMA synchronous=OFF", NULL, NULL, NULL);
	snprintf(sql_string, arsizeof(sql_string), "SELECT folder_id, table_flags,"
			" sorts, message_id, inst_num, header_id, header_stat"
			" FROM state_info WHERE state_id=%u", state_id);
	auto pstmt = gx_sql_prep(psqlite, sql_string);
	if (pstmt == nullptr) {
		sqlite3_close(psqlite);
		return FALSE;	
	}
	if (SQLITE_ROW != sqlite3_step(pstmt)) {
		pstmt.finalize();
		sqlite3_close(psqlite);
		return TRUE;
	}
	message_id = sqlite3_column_int64(pstmt, 3);
	inst_num = sqlite3_column_int64(pstmt, 4);
	if (gx_sql_col_uint64(pstmt, 0) != ptnode->folder_id ||
		ptnode->table_flags != sqlite3_column_int64(pstmt, 1)) {
		pstmt.finalize();
		goto RESTORE_POSITION;
	}
	if (NULL == ptnode->psorts || 0 == ptnode->psorts->ccategories) {
		if (SQLITE_NULL != sqlite3_column_type(pstmt, 2)) {
			pstmt.finalize();
			goto RESTORE_POSITION;
		}
	} else {
		if (!ext_push.init(tmp_buff, sizeof(tmp_buff), 0) ||
		    ext_push.p_sortorder_set(ptnode->psorts) != EXT_ERR_SUCCESS) {
			pstmt.finalize();
			goto RESTORE_POSITION;
		}
		if (static_cast<unsigned int>(sqlite3_column_bytes(pstmt, 2)) != ext_push.m_offset ||
		    memcmp(sqlite3_column_blob(pstmt, 2), ext_push.m_udata, ext_push.m_offset) != 0) {
			pstmt.finalize();
			goto RESTORE_POSITION;
		}
	}
	header_id = sqlite3_column_int64(pstmt, 5);
	header_stat = sqlite3_column_int64(pstmt, 6);
	pstmt.finalize();
	if (NULL == ptnode->psorts || 0 == ptnode->psorts->ccategories) {
		sqlite3_close(psqlite);
		goto RESTORE_POSITION;
	}
	sqlite3_exec(pdb->tables.psqlite,
		"BEGIN TRANSACTION", NULL, NULL, NULL);
	/* reset table into initial state */
	snprintf(sql_string, arsizeof(sql_string), "SELECT row_id, "
		"row_stat, depth FROM t%u WHERE row_type=%u",
		ptnode->table_id, CONTENT_ROW_HEADER);
	pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt == nullptr) {
		sqlite3_close(psqlite);
		sqlite3_exec(pdb->tables.psqlite,
			"ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "UPDATE t%u SET "
		"row_stat=? WHERE row_id=?", ptnode->table_id);
	pstmt1 = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt1 == nullptr) {
		pstmt1.finalize();
		sqlite3_close(psqlite);
		sqlite3_exec(pdb->tables.psqlite,
			"ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		row_id = sqlite3_column_int64(pstmt, 0);
		row_stat = sqlite3_column_int64(pstmt, 1);
		depth = sqlite3_column_int64(pstmt, 2);
		if (depth >= ptnode->psorts->cexpanded) {
			if (0 == row_stat) {
				continue;
			}
			row_stat = 0;
		} else {
			if (0 != row_stat) {
				continue;
			}
			row_stat = 1;
		}
		sqlite3_bind_int64(pstmt1, 1, row_stat);
		sqlite3_bind_int64(pstmt1, 2, row_id);
		if (SQLITE_DONE != sqlite3_step(pstmt)) {
			pstmt.finalize();
			pstmt1.finalize();
			sqlite3_close(psqlite);
			sqlite3_exec(pdb->tables.psqlite,
				"ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
		sqlite3_reset(pstmt1);
	}
	pstmt.finalize();
	pstmt1.finalize();
	/* end of resetting table */
	snprintf(sql_string, arsizeof(sql_string), "SELECT * FROM"
			" s%u ORDER BY ROWID ASC", state_id);
	pstmt = gx_sql_prep(psqlite, sql_string);
	if (pstmt == nullptr) {
		sqlite3_close(psqlite);
		sqlite3_exec(pdb->tables.psqlite,
			"ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT row_id FROM t%u WHERE"
			" parent_id=? AND value IS NULL", ptnode->table_id);
	pstmt1 = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt1 == nullptr) {
		pstmt.finalize();
		sqlite3_close(psqlite);
		sqlite3_exec(pdb->tables.psqlite,
			"ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT row_id FROM t%u WHERE"
				" parent_id=? AND value=?", ptnode->table_id);
	pstmt2 = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt2 == nullptr) {
		pstmt.finalize();
		pstmt1.finalize();
		sqlite3_close(psqlite);
		sqlite3_exec(pdb->tables.psqlite,
			"ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "UPDATE t%u SET "
		"row_stat=? WHERE row_id=?", ptnode->table_id);
	pstmt3 = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt3 == nullptr) {
		pstmt.finalize();
		pstmt1.finalize();
		pstmt2.finalize();
		sqlite3_close(psqlite);
		sqlite3_exec(pdb->tables.psqlite,
			"ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	current_id = 0;
	while (SQLITE_ROW == sqlite3_step(pstmt)) {
		current_id ++;
		depth = sqlite3_column_int64(pstmt, 0);
		row_id = 0;
		for (i=0; i<=depth; i++) {
			type = ptnode->psorts->psort[i].type;
			if ((type & MVI_FLAG) == MVI_FLAG)
				type &= ~MVI_FLAG;
			pvalue = common_util_column_sqlite_statement(pstmt, i + 1, type);
			if (NULL == pvalue) {
				sqlite3_bind_int64(pstmt1, 1, row_id);
				if (SQLITE_ROW != sqlite3_step(pstmt1)) {
					sqlite3_reset(pstmt1);
					break;
				}
				row_id = sqlite3_column_int64(pstmt1, 0);
				sqlite3_reset(pstmt1);
			} else {
				sqlite3_bind_int64(pstmt2, 1, row_id);
				if (FALSE == common_util_bind_sqlite_statement(
					pstmt2, 2, type, pvalue)) {
					pstmt.finalize();
					pstmt1.finalize();
					pstmt2.finalize();
					pstmt3.finalize();
					sqlite3_close(psqlite);
					sqlite3_exec(pdb->tables.psqlite,
						"ROLLBACK", NULL, NULL, NULL);
					return FALSE;
				}
				if (SQLITE_ROW != sqlite3_step(pstmt2)) {
					sqlite3_reset(pstmt2);
					break;
				}
				row_id = sqlite3_column_int64(pstmt2, 0);
				sqlite3_reset(pstmt2);
			}
		}
		if (i <= depth) {
			continue;
		}
		if (header_id == current_id) {
			row_stat = header_stat;
			row_id1 = row_id;
		} else {
			row_stat = depth >= ptnode->psorts->cexpanded;
		}
		sqlite3_bind_int64(pstmt3, 1, row_stat);
		sqlite3_bind_int64(pstmt3, 2, row_id);
		if (SQLITE_DONE != sqlite3_step(pstmt3)) {
			pstmt.finalize();
			pstmt1.finalize();
			pstmt2.finalize();
			pstmt3.finalize();
			sqlite3_close(psqlite);
			sqlite3_exec(pdb->tables.psqlite,
				"ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
		sqlite3_reset(pstmt3);
	}
	pstmt.finalize();
	pstmt1.finalize();
	pstmt2.finalize();
	pstmt3.finalize();
	sqlite3_close(psqlite);
	snprintf(sql_string, arsizeof(sql_string), "UPDATE t%u SET idx=NULL", ptnode->table_id);
	if (SQLITE_OK != sqlite3_exec(pdb->tables.psqlite,
		sql_string, NULL, NULL, NULL)) {
		sqlite3_exec(pdb->tables.psqlite,
			"ROLLBACK", NULL, NULL, NULL);
		return FALSE;
	}
	snprintf(sql_string, arsizeof(sql_string), "SELECT row_id, row_stat"
			" FROM t%u WHERE prev_id=?", ptnode->table_id);
	pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt == nullptr) {
		sqlite3_exec(pdb->tables.psqlite,
			"ROLLBACK", NULL, NULL, NULL);
		return false;
	}
	snprintf(sql_string, arsizeof(sql_string), "UPDATE t%u SET"
		" idx=? WHERE row_id=?", ptnode->table_id);
	pstmt1 = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt1 == nullptr) {
		pstmt.finalize();
		sqlite3_exec(pdb->tables.psqlite,
			"ROLLBACK", NULL, NULL, NULL);
		return false;
	}
	idx = 0;
	sqlite3_bind_int64(pstmt, 1, 0);
	if (SQLITE_ROW == sqlite3_step(pstmt)) {
		if (FALSE == common_util_indexing_sub_contents(
			ptnode->psorts->ccategories, pstmt,
			pstmt1, &idx)) {
			pstmt.finalize();
			pstmt1.finalize();
			sqlite3_exec(pdb->tables.psqlite,
				"ROLLBACK", NULL, NULL, NULL);
			return FALSE;
		}
	}
	pstmt.finalize();
	pstmt1.finalize();
	sqlite3_exec(pdb->tables.psqlite,
		"COMMIT TRANSACTION", NULL, NULL, NULL);
 RESTORE_POSITION:
	if (0 != message_id) {
		snprintf(sql_string, arsizeof(sql_string), "SELECT idx FROM t%u WHERE "
				"inst_id=%llu AND inst_num=%llu", ptnode->table_id,
				LLU(message_id), LLU(inst_num));
	} else {
		snprintf(sql_string, arsizeof(sql_string), "SELECT idx FROM t%u WHERE"
		          " row_id=%llu", ptnode->table_id, LLU(row_id1));
	}
	pstmt = gx_sql_prep(pdb->tables.psqlite, sql_string);
	if (pstmt == nullptr) {
		return false;
	}
	*pposition = sqlite3_step(pstmt) == SQLITE_ROW ? sqlite3_column_int64(pstmt, 0) - 1 : -1;
	return TRUE;
}
