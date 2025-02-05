#pragma once
#include <cstdint>
#include <memory>
#include <gromox/mapi_types.hpp>

#define MAX_LENGTH_FOR_FOLDER						64*1024

struct STREAM_OBJECT {
	~STREAM_OBJECT();
	BOOL check() const { return content_bin.pb != nullptr ? TRUE : false; }
	uint32_t get_max_length() const { return max_length; }
	uint32_t read(void *buf, uint32_t len);
	uint16_t write(void *buf, uint16_t len);
	uint8_t get_open_flags() const { return open_flags; }
	int get_parent_type() const { return object_type; }
	uint32_t get_proptag() const { return proptag; }
	void* get_content();
	uint32_t get_length() const { return content_bin.cb; }
	BOOL set_length(uint32_t len);
	BOOL seek(uint8_t opt, int64_t offset);
	uint32_t get_seek_position() const { return seek_ptr; }
	BOOL copy(STREAM_OBJECT *src, uint32_t *len);
	BOOL commit();

	void *pparent = nullptr;
	int object_type = 0;
	uint8_t open_flags = 0;
	uint32_t proptag = 0, seek_ptr = 0;
	BINARY content_bin{};
	BOOL b_touched = false;
	uint32_t max_length = 0;
};

extern std::unique_ptr<STREAM_OBJECT> stream_object_create(void *parent, int object_type, uint32_t open_flags, uint32_t proptag, uint32_t max_length);
