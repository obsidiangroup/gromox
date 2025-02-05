#pragma once
#include <cstdint>
#include <memory>
#include "store_object.h"
#include <gromox/mapi_types.hpp>

enum zcore_table_type {
	STORE_TABLE = 1,
	HIERARCHY_TABLE = 2,
	CONTENT_TABLE = 3,
	RULE_TABLE = 4,
	ATTACHMENT_TABLE = 5,
	RECIPIENT_TABLE = 6,
	CONTAINER_TABLE = 7,
	USER_TABLE = 8,
};

struct TABLE_OBJECT {
	~TABLE_OBJECT();
	const PROPTAG_ARRAY *get_columns() const { return pcolumns; }
	BOOL set_columns(const PROPTAG_ARRAY *);
	BOOL set_sorts(const SORTORDER_SET *);
	BOOL check_to_load();
	void unload();
	BOOL query_rows(const PROPTAG_ARRAY *cols, uint32_t row_count, TARRAY_SET *);
	BOOL set_restriction(const RESTRICTION *);
	void seek_current(BOOL forward, uint32_t row_count);
	uint8_t get_table_type() const { return table_type; }
	uint32_t get_position() const { return position; }
	void set_position(uint32_t pos);
	void clear_position() { position = 0; }
	uint32_t get_total();
	BOOL create_bookmark(uint32_t *index);
	void remove_bookmark(uint32_t index);
	void clear_bookmarks();
	BOOL retrieve_bookmark(uint32_t index, BOOL *exist);
	BOOL filter_rows(uint32_t count, const RESTRICTION *, const PROPTAG_ARRAY *cols, TARRAY_SET *);
	BOOL match_row(BOOL forward, const RESTRICTION *, int32_t *pos);

	STORE_OBJECT *pstore = nullptr;
	uint32_t handle = 0;
	void *pparent_obj = nullptr;
	enum zcore_table_type table_type{};
	uint32_t table_flags = 0;
	PROPTAG_ARRAY *pcolumns = nullptr;
	SORTORDER_SET *psorts = nullptr;
	RESTRICTION *prestriction = nullptr;
	uint32_t position = 0, table_id = 0, bookmark_index = 0;
	DOUBLE_LIST bookmark_list{};
};

extern std::unique_ptr<TABLE_OBJECT> table_object_create(STORE_OBJECT *, void *parent, uint8_t table_type, uint32_t table_flags);
