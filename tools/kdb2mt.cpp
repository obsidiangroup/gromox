// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2021 grommunio GmbH
// This file is part of Gromox.
#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>
#include <mysql.h>
#include <unistd.h>
#include <zlib.h>
#include <libHX/option.h>
#include <gromox/database_mysql.hpp>
#include <gromox/defs.h>
#include <gromox/ext_buffer.hpp>
#include <gromox/fileio.h>
#include <gromox/scope.hpp>
#include <gromox/tarray_set.hpp>
#include <gromox/tpropval_array.hpp>
#include <gromox/util.hpp>
#include "genimport.hpp"

using namespace std::string_literals;
using namespace gromox;

namespace {

union UPV {
	uint16_t i;
	uint32_t l;
	uint64_t ll;
	float flt;
	double dbl;
	bool b;
	char *str;
	void *ptr;
	BINARY bin;
};

enum {
	/* KC does not have MSGFLAG_READ really */
	KC_MSGFLAG_EVERREAD = 1U << 1,
	KC_MSGFLAG_DELETED = 1U << 10,
};

enum propcol {
	PCOL_TAG, PCOL_TYPE, PCOL_ULONG, PCOL_STRING, PCOL_BINARY, PCOL_DOUBLE,
	PCOL_LONGINT, PCOL_HI, PCOL_LO,
};

struct kdb_item;

struct driver final {
	driver() = default;
	driver(driver &&o) = delete;
	~driver();
	void operator=(driver &&) = delete;
	DB_RESULT query(const char *);
	uint32_t hid_from_mst(kdb_item &, uint32_t);
	std::unique_ptr<kdb_item> get_store_item();
	std::unique_ptr<kdb_item> get_root_folder();
	void fmap_setup_standard(const char *title);
	void fmap_setup_splice();

	void do_database(const char *title);

	MYSQL *m_conn = nullptr;
	uint32_t m_user_id = 0, m_store_hid = 0, m_root_hid = 0;
	gi_folder_map_t m_folder_map;
	unsigned int schema_vers = 0;
};

struct kdb_item final {
	kdb_item(driver &drv) : m_drv(drv) {}
	static std::unique_ptr<kdb_item> load_hid_base(driver &, uint32_t hid);
	TPROPVAL_ARRAY *get_props();
	size_t get_sub_count() { return m_sub_hids.size(); }
	std::unique_ptr<kdb_item> get_sub_item(size_t idx);

	using hidxtype = std::pair<uint32_t, unsigned int>;

	driver &m_drv;
	uint32_t m_hid = 0;
	enum mapi_object_type m_mapitype{};
	tpropval_array_ptr m_props;
	std::vector<hidxtype> m_sub_hids;
};

struct sql_login_param {
	std::string host, user, pass, dbname;
	uint16_t port = 0;
};

}

static int do_item(driver &, unsigned int, const parent_desc &, kdb_item &);

static char *g_sqlhost, *g_sqlport, *g_sqldb, *g_sqluser, *g_atxdir;
static char *g_srcguid, *g_srcmbox;
static unsigned int g_splice, g_level1_fan = 10, g_level2_fan = 20, g_verbose;
static std::vector<uint32_t> g_only_objs;

static void cb_only_obj(const HXoptcb *cb) {
		g_only_objs.push_back(cb->data_long);
}

static const struct HXoption g_options_table[] = {
	{nullptr, 'p', HXTYPE_NONE, &g_show_props, nullptr, nullptr, 0, "Show properties in detail (if -t)"},
	{nullptr, 's', HXTYPE_NONE, &g_splice, nullptr, nullptr, 0, "Splice source mail objects into existing destination mailbox hierarchy"},
	{nullptr, 't', HXTYPE_NONE, &g_show_tree, nullptr, nullptr, 0, "Show tree-based analysis of the source archive"},
	{nullptr, 'v', HXTYPE_NONE | HXOPT_INC, &g_verbose, nullptr, nullptr, 0, "More detailed progress reports"},
	{"l1", 0, HXTYPE_UINT, &g_level1_fan, nullptr, nullptr, 0, "L1 fan number for attachment directories of type files_v1 (default: 10)", "N"},
	{"l2", 0, HXTYPE_UINT, &g_level1_fan, nullptr, nullptr, 0, "L2 fan number for attachment directories of type files_v1 (default: 20)", "N"},
	{"src-host", 0, HXTYPE_STRING, &g_sqlhost, nullptr, nullptr, 0, "Hostname for SQL connection (default: localhost)", "HOST"},
	{"src-port", 0, HXTYPE_STRING, &g_sqlport, nullptr, nullptr, 0, "Port for SQL connection (default: auto)", "PORT"},
	{"src-db", 0, HXTYPE_STRING, &g_sqldb, nullptr, nullptr, 0, "Database name (default: kopano)", "NAME"},
	{"src-user", 0, HXTYPE_STRING, &g_sqluser, nullptr, nullptr, 0, "Username for SQL connection (default: root)", "USER"},
	{"src-at", 0, HXTYPE_STRING, &g_atxdir, nullptr, nullptr, 0, "Attachment directory", "DIR"},
	{"src-guid", 0, HXTYPE_STRING, &g_srcguid, nullptr, nullptr, 0, "Mailbox to extract from SQL", "GUID"},
	{"src-mbox", 0, HXTYPE_STRING, &g_srcmbox, nullptr, nullptr, 0, "Mailbox to extract from SQL", "USERNAME"},
	{"only-obj", 0, HXTYPE_ULONG, nullptr, nullptr, cb_only_obj, 0, "Extract specific object only", "OBJID"},
	HXOPT_AUTOHELP,
	HXOPT_TABLEEND,
};

static const char *snul(const std::string &s) { return s.size() != 0 ? s.c_str() : nullptr; }
static const char *znul(const char *s) { return s != nullptr ? s : ""; }

static std::string sql_escape(MYSQL *sqh, const char *in)
{
	std::string out;
	out.resize(strlen(in) * 2 + 1);
	auto ret = mysql_real_escape_string(sqh, out.data(), in, strlen(in));
	out.resize(ret);
	return out;
}

static void hid_to_tpropval_1(driver &drv, const char *qstr, TPROPVAL_ARRAY *ar)
{
	auto res = drv.query(qstr);
	DB_ROW row;
	while ((row = res.fetch_row()) != nullptr) {
		auto xtag = strtoul(znul(row[PCOL_TAG]), nullptr, 0);
		auto xtype = strtoul(znul(row[PCOL_TYPE]), nullptr, 0);
		auto rowlen = res.row_lengths();
		UPV upv{};
		TAGGED_PROPVAL pv{};
		pv.pvalue = &upv;

		switch (xtype) {
		case PT_SHORT: upv.i = strtoul(znul(row[PCOL_ULONG]), nullptr, 0); break;
		case PT_LONG: [[fallthrough]];
		case PT_ERROR: upv.l = strtoul(znul(row[PCOL_ULONG]), nullptr, 0); break;
		case PT_FLOAT: upv.flt = strtod(znul(row[PCOL_DOUBLE]), nullptr); break;
		case PT_DOUBLE: upv.dbl = strtod(znul(row[PCOL_DOUBLE]), nullptr); break;
		case PT_BOOLEAN: upv.b = strtoul(znul(row[PCOL_ULONG]), nullptr, 0); break;
		case PT_I8: upv.ll = strtoll(znul(row[PCOL_LONGINT]), nullptr, 0); break;
		case PT_SYSTIME:
			upv.ll = (static_cast<uint64_t>(strtol(znul(row[PCOL_HI]), nullptr, 0)) << 32) |
			         strtoul(znul(row[PCOL_LO]), nullptr, 0);
			break;
		case PT_STRING8:
			xtype = PT_UNICODE;
			[[fallthrough]];
		case PT_UNICODE: pv.pvalue = row[PCOL_STRING]; break;
		case PT_CLSID: [[fallthrough]];
		case PT_BINARY:
			upv.bin.cb = rowlen[PCOL_BINARY];
			upv.bin.pv = row[PCOL_BINARY];
			pv.pvalue = &upv.bin;
			break;
		default:
			throw YError("PK-1007: proptype %xh not supported. Implement me!", pv.proptag);
		}
		pv.proptag = PROP_TAG(xtype, xtag);
		if (!tpropval_array_set_propval(ar, &pv))
			throw std::bad_alloc();
	}
}

static void hid_to_tpropval_mv(driver &drv, const char *qstr, TPROPVAL_ARRAY *ar)
{
	auto res = drv.query(qstr);
	using UPW = std::pair<std::vector<uint32_t>, std::vector<std::string>>;
	std::unordered_map<uint32_t, UPW> collect;
	DB_ROW row;
	while ((row = res.fetch_row()) != nullptr) {
		if (row[PCOL_TAG] == nullptr || row[PCOL_TYPE] == nullptr)
			continue;
		auto xtag  = strtoul(row[PCOL_TAG], nullptr, 0);
		auto xtype = strtoul(row[PCOL_TYPE], nullptr, 0);
		auto proptag = PROP_TAG(xtype, xtag);
		auto colen = res.row_lengths();
		switch (xtype) {
		case PT_MV_LONG:
			if (row[PCOL_ULONG] == nullptr)
				continue;
			collect[proptag].first.emplace_back(strtoul(row[PCOL_ULONG], nullptr, 0));
			break;
		case PT_MV_STRING8:
		case PT_MV_UNICODE:
			if (row[PCOL_STRING] == nullptr)
				continue;
			collect[proptag].second.emplace_back(row[PCOL_STRING]);
			break;
		case PT_MV_BINARY:
			if (row[PCOL_BINARY] == nullptr)
				continue;
			collect[proptag].second.emplace_back(std::string(row[PCOL_BINARY], colen[PCOL_BINARY]));
			break;
		default:
			throw YError("PK-1010: Proptype %lxh not supported. Implement me!", static_cast<unsigned long>(proptag));
		}
	}

	TAGGED_PROPVAL pv;
	for (auto &&[proptag, xpair] : collect) {
		pv.proptag = proptag;
		switch (PROP_TYPE(proptag)) {
		case PT_MV_LONG: {
			LONG_ARRAY la;
			la.count = xpair.first.size();
			la.pl = xpair.first.data();
			pv.pvalue = &la;
			if (!tpropval_array_set_propval(ar, &pv))
				throw std::bad_alloc();
			break;
		}
		case PT_MV_STRING8:
		case PT_MV_UNICODE: {
			std::vector<char *> ptrs(xpair.second.size());
			STRING_ARRAY sa;
			sa.count = xpair.second.size();
			for (size_t i = 0; i < sa.count; ++i)
				ptrs[i] = xpair.second[i].data();
			sa.ppstr = ptrs.data();
			pv.proptag = CHANGE_PROP_TYPE(proptag, PT_MV_UNICODE);
			pv.pvalue = &sa;
			if (!tpropval_array_set_propval(ar, &pv))
				throw std::bad_alloc();
			break;
		}
		case PT_MV_BINARY: {
			std::vector<BINARY> bins(xpair.second.size());
			BINARY_ARRAY ba;
			ba.count = xpair.second.size();
			for (size_t i = 0; i < ba.count; ++i) {
				bins[i].pv = xpair.second[i].data();
				bins[i].cb = xpair.second[i].size();
			}
			ba.pbin = bins.data();
			pv.pvalue = &ba;
			if (!tpropval_array_set_propval(ar, &pv))
				throw std::bad_alloc();
			break;
		}
		}
	}
}

static tpropval_array_ptr hid_to_propval_a(driver &drv, uint32_t hid)
{
	tpropval_array_ptr props(tpropval_array_init());
	if (props == nullptr)
		throw std::bad_alloc();
	char qstr[256];
	snprintf(qstr, arsizeof(qstr),
		"SELECT tag, type, val_ulong, val_string, val_binary, val_double, val_longint, val_hi, val_lo "
		"FROM properties WHERE hierarchyid=%u", hid);
	hid_to_tpropval_1(drv, qstr, props.get());
	snprintf(qstr, arsizeof(qstr),
		"SELECT tag, type, val_ulong, val_string, val_binary, val_double, val_longint, val_hi, val_lo "
		"FROM mvproperties WHERE hierarchyid=%u ORDER BY tag, type, orderid", hid);
	hid_to_tpropval_mv(drv, qstr, props.get());
	return props;
}

static const char *kp_item_type_to_str(enum mapi_object_type t)
{
	thread_local char buf[32];
	switch (t) {
	case MAPI_STORE: return "store";
	case MAPI_FOLDER: return "folder";
	case MAPI_MESSAGE: return "message";
	case MAPI_MAILUSER: return "mailuser";
	case MAPI_ATTACH: return "attach";
	case MAPI_DISTLIST: return "distlist";
	default: snprintf(buf, arsizeof(buf), "other-%u", t); return buf;
	}
}

static void do_print(unsigned int depth, kdb_item &item)
{
	tree(depth);
	tlog("[hid=%lu type=%s]\n", static_cast<unsigned long>(item.m_hid),
	     kp_item_type_to_str(item.m_mapitype));
}

static std::unique_ptr<driver>
kdb_open_by_guid_1(std::unique_ptr<driver> &&drv, const char *guid)
{
	if (hex2bin(guid).size() != 16)
		throw YError("PK-1011: invalid GUID passed");

	char qstr[96];
	DB_RESULT res;
	DB_ROW row;
	snprintf(qstr, arsizeof(qstr), "SELECT MAX(databaserevision) FROM versions");
	try {
		res = drv->query(qstr);
		row = res.fetch_row();
		if (row == nullptr || row[0] == nullptr)
			throw YError("PK-1002: Database has no version information and is too old");
	} catch (const YError &e) {
		fprintf(stderr, "PK-1003: Database has no version information and is too old.\n");
		throw;
	}
	drv->schema_vers = strtoul(row[0], nullptr, 0);
	if (drv->schema_vers < 61)
		throw YError("PK-1004: Database schema n%u is not supported.\n", drv->schema_vers);
	fprintf(stderr, "Database schema n%u\n", drv->schema_vers);

	/* user_id available from n61 */
	snprintf(qstr, arsizeof(qstr), "SELECT hierarchy_id, user_id FROM stores WHERE guid=0x%.32s", guid);
	res = drv->query(qstr);
	row = res.fetch_row();
	if (row == nullptr || row[0] == nullptr || row[1] == nullptr)
		throw YError("PK-1014: no store by that GUID");
	drv->m_user_id = strtoul(row[1], nullptr, 0);
	drv->m_store_hid = strtoul(row[0], nullptr, 0);
	return std::move(drv);
}

static std::unique_ptr<driver>
kdb_open_by_guid(const char *guid, const sql_login_param &sqp)
{
	auto drv = std::make_unique<driver>();
	drv->m_conn = mysql_init(nullptr);
	if (drv->m_conn == nullptr)
		throw std::bad_alloc();
	mysql_options(drv->m_conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
	if (mysql_real_connect(drv->m_conn, snul(sqp.host), sqp.user.c_str(),
	    sqp.pass.c_str(), sqp.dbname.c_str(), sqp.port, nullptr, 0) == nullptr)
		throw YError("PK-1018: mysql_connect %s@%s: %s",
		      sqp.user.c_str(), sqp.host.c_str(), mysql_error(drv->m_conn));
	return kdb_open_by_guid_1(std::move(drv), guid);
}

static void present_stores(const char *storeuser, DB_RESULT &res)
{
	DB_ROW row;
	fprintf(stderr, "PK-1008: This utility only does a heuristic search, "
	        "lest it would require the full original user database, "
	        "a requirement this tool does not want to impose. "
	        "The search for \"%s\" has turned up multiple candidate stores:\n\n", storeuser);
	fprintf(stderr, "GUID                              user_id  most_recent_owner\n");
	fprintf(stderr, "============================================================\n");
	while ((row = res.fetch_row()) != nullptr) {
		auto colen = res.row_lengths();
		fprintf(stderr, "%s  %7lu  %s\n", bin2hex(row[0], colen[0]).c_str(),
		        strtoul(row[1], nullptr, 0), storeuser);
	}
	fprintf(stderr, "============================================================\n");
}

static std::unique_ptr<driver>
kdb_open_by_user(const char *storeuser, const sql_login_param &sqp)
{
	auto drv = std::make_unique<driver>();
	drv->m_conn = mysql_init(nullptr);
	if (drv->m_conn == nullptr)
		throw std::bad_alloc();
	mysql_options(drv->m_conn, MYSQL_SET_CHARSET_NAME, "utf8mb4");
	if (mysql_real_connect(drv->m_conn, snul(sqp.host), sqp.user.c_str(),
	    sqp.pass.c_str(), sqp.dbname.c_str(), sqp.port, nullptr, 0) == nullptr)
		throw YError("PK-1019: mysql_connect %s@%s: %s",
		      sqp.user.c_str(), sqp.host.c_str(), mysql_error(drv->m_conn));

	auto qstr = "SELECT stores.guid, stores.user_id FROM stores INNER JOIN users ON stores.user_id=users.id WHERE stores.user_name='" + sql_escape(drv->m_conn, storeuser) + "'";
	auto res = drv->query(qstr.c_str());
	if (mysql_num_rows(res.get()) > 1) {
		present_stores(storeuser, res);
		throw YError("PK-1013: \"%s\" was ambiguous.\n", storeuser);
	}
	auto row = res.fetch_row();
	if (row == nullptr || row[0] == nullptr)
		throw YError("PK-1022: no store for that user");
	auto rowlen = res.row_lengths();
	return kdb_open_by_guid_1(std::move(drv), bin2hex(row[0], rowlen[0]).c_str());
}

driver::~driver()
{
	if (m_conn != nullptr)
		mysql_close(m_conn);
}

DB_RESULT driver::query(const char *qstr)
{
	auto ret = mysql_query(m_conn, qstr);
	if (ret != 0)
		throw YError("PK-1000: mysql_query \"%s\": %s", qstr, mysql_error(m_conn));
	DB_RESULT res = mysql_store_result(m_conn);
	if (res == nullptr)
		throw YError("PK-1001: mysql_store: %s", mysql_error(m_conn));
	return res;
}

uint32_t driver::hid_from_mst(kdb_item &item, uint32_t proptag)
{
	auto props = item.get_props();
	auto eid = static_cast<BINARY *>(tpropval_array_get_propval(props, proptag));
	if (eid == nullptr)
		return 0;
	char qstr[184];
	snprintf(qstr, arsizeof(qstr), "SELECT hierarchyid FROM indexedproperties "
		"WHERE tag=0x0FFF AND val_binary=0x%.96s LIMIT 1", bin2hex(eid->pv, eid->cb).c_str());
	auto res = query(qstr);
	auto row = res.fetch_row();
	if (row == nullptr || row[0] == nullptr)
		return 0;
	return strtoul(row[0], nullptr, 0);
}

void driver::fmap_setup_splice()
{
	m_folder_map.clear();
	auto store = get_store_item();
	auto root = get_root_folder();
	m_folder_map.emplace(root->m_hid, tgt_folder{false, rop_util_make_eid_ex(1, PRIVATE_FID_ROOT), "FID_ROOT"});
	auto nid = hid_from_mst(*store, PR_IPM_SUBTREE_ENTRYID);
	if (nid != 0)
		m_folder_map.emplace(nid, tgt_folder{false, rop_util_make_eid_ex(1, PRIVATE_FID_IPMSUBTREE), "FID_IPMSUBTREE"});
	nid = hid_from_mst(*store, PR_IPM_OUTBOX_ENTRYID);
	if (nid != 0)
		m_folder_map.emplace(nid, tgt_folder{false, rop_util_make_eid_ex(1, PRIVATE_FID_OUTBOX), "FID_OUTBOX"});
	nid = hid_from_mst(*store, PR_IPM_WASTEBASKET_ENTRYID);
	if (nid != 0)
		m_folder_map.emplace(nid, tgt_folder{false, rop_util_make_eid_ex(1, PRIVATE_FID_DELETED_ITEMS), "FID_DELETED_ITEMS"});
	nid = hid_from_mst(*store, PR_IPM_SENTMAIL_ENTRYID);
	if (nid != 0)
		m_folder_map.emplace(nid, tgt_folder{false, rop_util_make_eid_ex(1, PRIVATE_FID_SENT_ITEMS), "FID_SENT_ITEMS"});
	nid = hid_from_mst(*store, PR_COMMON_VIEWS_ENTRYID);
	if (nid != 0)
		m_folder_map.emplace(nid, tgt_folder{false, rop_util_make_eid_ex(1, PRIVATE_FID_COMMON_VIEWS), "FID_COMMON_VIEWS"});
	nid = hid_from_mst(*store, PR_VIEWS_ENTRYID);
	if (nid != 0)
		m_folder_map.emplace(nid, tgt_folder{false, rop_util_make_eid_ex(1, PRIVATE_FID_VIEWS), "FID_VIEWS"});
	nid = hid_from_mst(*store, PR_FINDER_ENTRYID);
	if (nid != 0)
		m_folder_map.emplace(nid, tgt_folder{false, rop_util_make_eid_ex(1, PRIVATE_FID_FINDER), "FID_FINDER"});
	nid = hid_from_mst(*store, PR_SCHEDULE_FOLDER_ENTRYID);
	if (nid != 0)
		m_folder_map.emplace(nid, tgt_folder{false, rop_util_make_eid_ex(1, PRIVATE_FID_SCHEDULE), "FID_SCHEDULE"});

	nid = hid_from_mst(*root, PR_IPM_APPOINTMENT_ENTRYID);
	if (nid != 0)
		m_folder_map.emplace(nid, tgt_folder{false, rop_util_make_eid_ex(1, PRIVATE_FID_CALENDAR), "FID_CALENDAR"});
	nid = hid_from_mst(*root, PR_IPM_CONTACT_ENTRYID);
	if (nid != 0)
		m_folder_map.emplace(nid, tgt_folder{false, rop_util_make_eid_ex(1, PRIVATE_FID_CONTACTS), "FID_CONTACTS"});
	nid = hid_from_mst(*root, PR_IPM_JOURNAL_ENTRYID);
	if (nid != 0)
		m_folder_map.emplace(nid, tgt_folder{false, rop_util_make_eid_ex(1, PRIVATE_FID_JOURNAL), "FID_JOURNAL"});
	nid = hid_from_mst(*root, PR_IPM_NOTE_ENTRYID);
	if (nid != 0)
		m_folder_map.emplace(nid, tgt_folder{false, rop_util_make_eid_ex(1, PRIVATE_FID_NOTES), "FID_NOTES"});
	nid = hid_from_mst(*root, PR_IPM_TASK_ENTRYID);
	if (nid != 0)
		m_folder_map.emplace(nid, tgt_folder{false, rop_util_make_eid_ex(1, PRIVATE_FID_TASKS), "FID_TASKS"});
	nid = hid_from_mst(*root, PR_IPM_DRAFTS_ENTRYID);
	if (nid != 0)
		m_folder_map.emplace(nid, tgt_folder{false, rop_util_make_eid_ex(1, PRIVATE_FID_DRAFT), "FID_DRAFTS"});
}

void driver::fmap_setup_standard(const char *title)
{
	char timebuf[64];
	time_t now = time(nullptr);
	auto tm = localtime(&now);
	strftime(timebuf, arsizeof(timebuf), " @%FT%T", tm);
	m_folder_map.clear();
	auto root = get_root_folder();
	m_folder_map.emplace(root->m_hid, tgt_folder{true,
		rop_util_make_eid_ex(1, PRIVATE_FID_IPMSUBTREE),
		"Import of "s + title + timebuf});
}

std::unique_ptr<kdb_item> driver::get_store_item()
{
	return kdb_item::load_hid_base(*this, m_store_hid);
}

std::unique_ptr<kdb_item> driver::get_root_folder()
{
	if (m_root_hid == 0) {
		char qstr[80];
		snprintf(qstr, arsizeof(qstr), "SELECT id FROM hierarchy WHERE parent=%u AND type=3 LIMIT 1", m_store_hid);
		auto res = query(qstr);
		auto row = res.fetch_row();
		if (row == nullptr || row[0] == nullptr)
			throw YError("PK-1017: no root folder for store");
		m_root_hid = strtoul(row[0], nullptr, 0);
	}
	return kdb_item::load_hid_base(*this, m_root_hid);
}

std::unique_ptr<kdb_item> kdb_item::load_hid_base(driver &drv, uint32_t hid)
{
	char qstr[84];
	snprintf(qstr, arsizeof(qstr), "SELECT id, type, flags FROM hierarchy WHERE (id=%u OR parent=%u)", hid, hid);
	auto res = drv.query(qstr);
	auto yi = std::make_unique<kdb_item>(drv);
	DB_ROW row;
	while ((row = res.fetch_row()) != nullptr) {
		auto xid   = strtoul(row[0], nullptr, 0);
		auto xtype = strtoul(row[1], nullptr, 0);
		auto xflag = strtoul(row[2], nullptr, 0);
		if (xid == hid) {
			/* Own existence validated */
			yi->m_hid = xid;
			yi->m_mapitype = static_cast<enum mapi_object_type>(xtype);
			continue;
		}
		if (xtype == MAPI_FOLDER && xflag == FOLDER_SEARCH)
			/* Skip over search folders */
			continue;
		if (xtype == MAPI_MESSAGE && (xflag & KC_MSGFLAG_DELETED))
			/* Skip over softdeletes */
			continue;
		yi->m_sub_hids.push_back({xid, xtype});
	}
	/*
	 * Put messages before folders, so genimport processes a folder's
	 * message before the folder's subfolders. (Harmonizes better with
	 * genimport's status printouts.)
	 */
	std::sort(yi->m_sub_hids.begin(), yi->m_sub_hids.end(),
		[](const hidxtype &a, const hidxtype &b) /* operator< */
		{
			if (a.second == MAPI_MESSAGE && b.second == MAPI_FOLDER)
				return true;
			if (a.second == MAPI_FOLDER && b.second == MAPI_MESSAGE)
				return false;
			return a < b;
		});
	if (yi->m_hid != hid)
		return nullptr;
	return yi;
}

TPROPVAL_ARRAY *kdb_item::get_props()
{
	if (m_props == nullptr)
		m_props = hid_to_propval_a(m_drv, m_hid);
	return m_props.get();
}

std::unique_ptr<kdb_item> kdb_item::get_sub_item(size_t idx)
{
	if (idx >= m_sub_hids.size())
		return nullptr;
	return load_hid_base(m_drv, m_sub_hids[idx].first);
}

static void do_namemap_table(driver &drv, gi_name_map &map)
{
	auto res = drv.query("SELECT id, guid, nameid, namestring FROM names");
	DB_ROW row;
	while ((row = res.fetch_row()) != nullptr) {
		auto rowlen = res.row_lengths();
		std::unique_ptr<char[]> pnstr;
		PROPERTY_NAME pn_req{};

		if (rowlen[1] != sizeof(GUID))
			continue;
		memcpy(&pn_req.guid, row[1], sizeof(GUID));
		if (row[2] != nullptr) {
			pn_req.kind = MNID_ID;
			pn_req.lid  = strtoul(row[2], nullptr, 0);
		} else {
			pn_req.kind = MNID_STRING;
			pnstr.reset(static_cast<char *>(malloc(rowlen[3] + 1)));
			memcpy(pnstr.get(), row[3], rowlen[3] + 1);
			pn_req.pname = pnstr.get();
		}
		map.emplace(PROP_TAG(PT_UNSPECIFIED, 0x8501 + strtoul(row[0], nullptr, 0)), pn_req);
		pnstr.release();
		pn_req.pname = nullptr;
	}
}

static gi_name_map do_namemap(driver &drv)
{
	gi_name_map map;
	static constexpr struct {
		unsigned int psetid, lid_min, lid_max, base;
	} hardmapped_nprops[] = {
		{PSETID_ADDRESS,          0x8000, 0x80EF, 0x80B0},
		{PSETID_TASK,             0x8100, 0x813F, 0x8070},
		{PSETID_APPOINTMENT,      0x8200, 0x826F, 0x8000},
		{PSETID_COMMON,           0x8500, 0x85FF, 0x81A0},
		{PSETID_LOG,              0x8700, 0x871F, 0x82A0},
		{PSETID_BUSINESSCARDVIEW, 0x8800, 0x881F, 0x82C0},
		{PSETID_NOTE,             0x8B00, 0x8B1F, 0x82E0},
		{PSETID_REPORT,           0x8D00, 0x8D1F, 0x8300},
		{PSETID_REMOTE,           0x8F00, 0x8F1F, 0x8320},
		{PSETID_MEETING,          0x0000, 0x003F, 0x8340},
		{PSETID_KC,               0x0002, 0x0002, 0x8380},
	};
	PROPERTY_NAME pn;
	pn.kind = MNID_ID;

	for (const auto &row : hardmapped_nprops) {
		rop_util_get_common_pset(row.psetid, &pn.guid);
		for (pn.lid = row.lid_min; pn.lid < row.lid_max; ++pn.lid)
			map.emplace(PROP_TAG(PT_UNSPECIFIED, pn.lid - row.lid_min + row.base), pn);
	}
	do_namemap_table(drv, map);
	return map;
}

static int do_folder(driver &drv, unsigned int depth, const parent_desc &parent, kdb_item &item)
{
	auto props = item.get_props();
	if (g_show_tree) {
		gi_dump_tpropval_a(depth, *props);
	} else {
		auto dn = static_cast<const char *>(tpropval_array_get_propval(props, PR_DISPLAY_NAME));
		fprintf(stderr, "Processing folder \"%s\" (%zu elements)...\n",
		        dn != nullptr ? dn : "", item.m_sub_hids.size());
	}

	bool b_create = false;
	auto iter = drv.m_folder_map.find(item.m_hid);
	if (iter == drv.m_folder_map.end() && parent.type == MAPI_FOLDER) {
		/* PST folder with name -> new folder in store. Create. */
		b_create = true;
	} else if (iter == drv.m_folder_map.end()) {
		/* No @parent for writing the item anywhere, and no hints in map => do not create. */
	} else if (!iter->second.create) {
		/* Splice request (e.g. PST wastebox -> Store wastebox) */
		b_create = true;
	} else {
		/* Create request (e.g. PST root without name -> new folder in store with name) */
		b_create = true;
	}

	if (!b_create)
		return 0;
	EXT_PUSH ep;
	if (!ep.init(nullptr, 0, EXT_FLAG_WCOUNT))
		throw std::bad_alloc();
	ep.p_uint32(MAPI_FOLDER);
	ep.p_uint32(item.m_hid);
	ep.p_uint32(parent.type);
	ep.p_uint64(parent.folder_id);
	ep.p_tpropval_a(props);
	uint64_t xsize = cpu_to_le64(ep.m_offset);
	write(STDOUT_FILENO, &xsize, sizeof(xsize));
	write(STDOUT_FILENO, ep.m_vdata, ep.m_offset);
	return 0;
}

static message_content_ptr build_message(driver &drv, unsigned int depth,
    kdb_item &item)
{
	auto props = item.get_props();
	message_content_ptr ctnt(message_content_init());
	if (ctnt == nullptr)
		throw std::bad_alloc();
	ctnt->children.pattachments = attachment_list_init();
	if (ctnt->children.pattachments == nullptr)
		throw std::bad_alloc();
	ctnt->children.prcpts = tarray_set_init();
	if (ctnt->children.prcpts == nullptr)
		throw std::bad_alloc();
	std::swap(ctnt->proplist, *props);

	/* Subitems can be recipients, attachments... */
	auto parent = parent_desc::as_msg(ctnt.get());
	for (size_t i = 0; i < item.m_sub_hids.size(); ++i) {
		auto subitem = item.get_sub_item(i);
		auto ret = do_item(drv, depth, parent, *subitem);
		if (ret < 0)
			throw YError("PK-1015: %s", strerror(-ret));
	}
	return ctnt;
}

static int do_message(driver &drv, unsigned int depth, const parent_desc &parent, kdb_item &item)
{
	auto ctnt = build_message(drv, depth, item);
	if (parent.type == MAPI_ATTACH)
		attachment_content_set_embedded_internal(parent.attach, ctnt.release());
	if (parent.type != MAPI_FOLDER)
		return 0;

	if (g_show_tree)
		gi_dump_msgctnt(depth, *ctnt);
	EXT_PUSH ep;
	if (!ep.init(nullptr, 0, EXT_FLAG_WCOUNT))
		throw std::bad_alloc();
	if (ep.p_uint32(MAPI_MESSAGE) != EXT_ERR_SUCCESS ||
	    ep.p_uint32(item.m_hid) != EXT_ERR_SUCCESS ||
	    ep.p_uint32(parent.type) != EXT_ERR_SUCCESS ||
	    ep.p_uint64(parent.folder_id) != EXT_ERR_SUCCESS ||
	    ep.p_msgctnt(ctnt.get()) != EXT_ERR_SUCCESS)
		throw YError("PF-1058");
	uint64_t xsize = cpu_to_le64(ep.m_offset);
	write(STDOUT_FILENO, &xsize, sizeof(xsize));
	write(STDOUT_FILENO, ep.m_vdata, ep.m_offset);
	return 0;
}

static int do_recip(driver &drv, unsigned int depth, const parent_desc &parent, kdb_item &item)
{
	auto props = item.get_props();
	if (!tarray_set_append_internal(parent.message->children.prcpts, props))
		throw std::bad_alloc();
	item.m_props.release();
	return 0;
}

static std::string slurp_file_gz(const char *file)
{
	std::string file_gz, outstr;
	gzFile fp = gzopen(file, "rb");
	if (fp == nullptr && errno == ENOENT) {
		file_gz = file + ".gz"s;
		file = file_gz.c_str();
		fp = gzopen(file, "rb");
	}
	if (fp == nullptr) {
		fprintf(stderr, "gzopen %s: %s\n", file, strerror(errno));
		return outstr;
	}
	auto cl_0 = make_scope_exit([&]() { gzclose(fp); });
	char buf[4096];
	while (!gzeof(fp)) {
		auto rd = gzread(fp, buf, arsizeof(buf));
		/* save errno because gzread might just fail save-restoring it */
		int saved_errno = errno, zerror;
		const char *zerrstr = gzerror(fp, &zerror);
		if (rd < 0 && zerror == Z_ERRNO) {
			fprintf(stderr, "gzread %s: %s (%d): %s\n", file, zerrstr, zerror, strerror(saved_errno));
			break;
		} else if (rd < 0) {
			fprintf(stderr, "gzread %s: %s (%d)\n", file, zerrstr, zerror);
			break;
		}
		if (rd == 0)
			break;
		outstr.append(buf, rd);
	}
	return outstr;
}

static void do_attach_byval(driver &drv, unsigned int depth, unsigned int hid,
    TPROPVAL_ARRAY *props)
{
	char qstr[96];
	snprintf(qstr, arsizeof(qstr), drv.schema_vers >= 71 ?
	         "SELECT instanceid, filename FROM singleinstances WHERE hierarchyid=%u LIMIT 1" :
	         "SELECT instanceid FROM singleinstances WHERE hierarchyid=%u LIMIT 1", hid);
	auto res = drv.query(qstr);
	auto row = res.fetch_row();
	if (row == nullptr || row[0] == nullptr) {
		fprintf(stderr, "PK-1012: attachment %u is missing from \"singleinstances\" table and is lost\n", hid);
		return;
	}
	std::string filename;
	auto siid = strtoul(row[0], nullptr, 0);
	if (drv.schema_vers >= 71 && row[1] != nullptr && row[1][0] != '\0')
		filename = g_atxdir + "/"s + row[1] + "/content";
	else
		filename = g_atxdir + "/"s + std::to_string(siid % g_level1_fan) +
		           "/" + std::to_string(siid / g_level1_fan % g_level2_fan) +
		           "/" + std::to_string(siid);
	if (g_show_tree) {
		tree(depth);
		fprintf(stderr, "Attachment source: %s\n", filename.c_str());
	}
	std::string contents = slurp_file_gz(filename.c_str());
	BINARY bin;
	bin.cb = contents.size();
	bin.pv = contents.data();
	TAGGED_PROPVAL pv;
	pv.proptag = PR_ATTACH_DATA_BIN;
	pv.pvalue = &bin;
	if (!tpropval_array_set_propval(props, &pv))
		throw std::bad_alloc();
}

static int do_attach(driver &drv, unsigned int depth, const parent_desc &parent, kdb_item &item)
{
	attachment_content_ptr atc(attachment_content_init());
	if (atc == nullptr)
		throw std::bad_alloc();
	auto props = item.get_props();
	auto mode = static_cast<uint32_t *>(tpropval_array_get_propval(props, PR_ATTACH_METHOD));

	if (mode == nullptr)
		fprintf(stderr, "PK-1005: Attachment %u without PR_ATTACH_METHOD.\n",
		        static_cast<unsigned int>(item.m_hid));
	else if (*mode == ATTACH_BY_VALUE && *g_atxdir != '\0')
		do_attach_byval(drv, depth, item.m_hid, props);

	auto saved_show_tree = g_show_tree;
	g_show_tree = false;
	auto new_parent = parent_desc::as_attach(atc.get());
	for (size_t i = 0; i < item.m_sub_hids.size(); ++i) {
		auto subitem = item.get_sub_item(i);
		auto ret = do_item(drv, depth + 1, new_parent, *subitem);
		if (ret < 0) {
			g_show_tree = saved_show_tree;
			return ret;
		}
	}
	g_show_tree = saved_show_tree;

	std::swap(atc->proplist, *props);
	if (parent.type == MAPI_MESSAGE) {
		if (!attachment_list_append_internal(parent.message->children.pattachments, atc.get()))
			throw std::bad_alloc();
		atc.release();
	}
	return 0;
}

static int do_item(driver &drv, unsigned int depth, const parent_desc &parent, kdb_item &item)
{
	auto new_parent = parent;
	int ret = 0;
	if (g_show_tree)
		do_print(depth++, item);
	if (item.m_mapitype == MAPI_FOLDER) {
		ret = do_folder(drv, depth, parent, item);
		new_parent.type = MAPI_FOLDER;
		new_parent.folder_id = item.m_hid;
	} else if (item.m_mapitype == MAPI_MESSAGE) {
		return do_message(drv, depth, parent, item);
	} else if (item.m_mapitype == MAPI_MAILUSER) {
		ret = do_recip(drv, depth, parent, item);
	} else if (item.m_mapitype == MAPI_ATTACH) {
		ret = do_attach(drv, depth, parent, item);
	}
	if (ret < 0)
		return ret;

	auto istty = isatty(STDERR_FILENO);
	auto last_ts = std::chrono::steady_clock::now();
	unsigned int verb = (new_parent.type == MAPI_STORE ||
	                    new_parent.type == MAPI_FOLDER) &&
	                    !g_show_tree && g_verbose;

	for (size_t i = 0; i < item.m_sub_hids.size(); ++i) {
		auto subitem = item.get_sub_item(i);
		ret = do_item(drv, depth, new_parent, *subitem);
		if (ret < 0)
			return ret;
		auto now_ts = decltype(last_ts)::clock::now();
		auto tsdiff = now_ts - last_ts;
		if (verb > 0 && tsdiff > std::chrono::seconds(1)) {
			fprintf(stderr, " %zu/%zu (%.0f%%)%c", i, item.m_sub_hids.size(),
			        i * 100.0 / item.m_sub_hids.size(), istty ? '\r' : '\n');
			last_ts = now_ts;
			verb = 2;
		}
	}
	if (verb > 0 && istty)
		fprintf(stderr, "\e[2K");
	return 0;
}

static int do_database(std::unique_ptr<driver> &&drv, const char *title)
{
	write(STDOUT_FILENO, "GXMT0000", 8);
	uint8_t xsplice = g_splice;
	write(STDOUT_FILENO, &xsplice, sizeof(xsplice));
	if (g_splice)
		drv->fmap_setup_splice();
	else
		drv->fmap_setup_standard(title);
	gi_dump_folder_map(drv->m_folder_map);
	gi_folder_map_write(drv->m_folder_map);

	auto name_map = do_namemap(*drv);
	gi_dump_name_map(name_map);
	gi_name_map_write(name_map);

	if (g_show_tree)
		fprintf(stderr, "Object tree:\n");
	if (g_only_objs.size() == 0)
		return do_item(*drv, 0, {}, *drv->get_store_item());

	auto pd = parent_desc::as_folder(~0ULL);
	for (const auto hid : g_only_objs) {
		auto item = kdb_item::load_hid_base(*drv, hid);
		auto ret = do_item(*drv, 0, pd, *item);
		if (ret < 0)
			throw YError("PK-1015: %s", strerror(-ret));
	}
	return 0;
}

int main(int argc, const char **argv)
{
	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (HX_getopt(g_options_table, &argc, &argv, HXOPT_USAGEONERR) != HXOPT_ERR_SUCCESS)
		return EXIT_FAILURE;
	if ((g_srcguid != nullptr) == (g_srcmbox != nullptr)) {
		fprintf(stderr, "Exactly one of --src-guid or --src-mbox must be specified.\n");
		return EXIT_FAILURE;
	} else if (g_atxdir == nullptr) {
		fprintf(stderr, "You need to specify the --src-at option.\n");
		fprintf(stderr, "(To skip importing file-based attachments, use --src-at \"\".)\n");
		return EXIT_FAILURE;
	}
	if (argc != 1) {
		fprintf(stderr, "Usage: SRCPASS=sqlpass gromox-kdb2mt --src-sql kdb.lan "
		        "--src-attach /tmp/at --src-mbox jdoe\n");
		return EXIT_FAILURE;
	}

	int ret = EXIT_SUCCESS;
	sql_login_param sqp;
	if (g_sqlhost != nullptr)
		sqp.host = g_sqlhost;
	if (g_sqlport != nullptr)
		sqp.port = strtoul(g_sqlport, nullptr, 0);
	sqp.dbname = g_sqldb != nullptr ? g_sqldb : "kopano";
	sqp.user = g_sqluser != nullptr ? g_sqluser : "root";
	auto s = getenv("SRCPASS");
	if (s != nullptr)
		sqp.pass = s;

	try {
		std::unique_ptr<driver> drv;
		if (g_srcguid != nullptr)
			drv = kdb_open_by_guid(g_srcguid, sqp);
		else if (g_srcmbox != nullptr)
			drv = kdb_open_by_user(g_srcmbox, sqp);
		if (drv == nullptr) {
			fprintf(stderr, "Problem?!\n");
			return EXIT_FAILURE;
		}
		ret = do_database(std::move(drv), g_srcguid != nullptr ? g_srcguid : g_srcmbox);
	} catch (const char *e) {
		fprintf(stderr, "Exception: %s\n", e);
		return -ECANCELED;
	} catch (const std::string &e) {
		fprintf(stderr, "Exception: %s\n", e.c_str());
		return -ECANCELED;
	} catch (const std::exception &e) {
		fprintf(stderr, "Exception: %s\n", e.what());
		return -ECANCELED;
	}
	return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}
