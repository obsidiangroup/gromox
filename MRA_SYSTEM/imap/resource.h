#ifndef _H_RESOURCE_
#define _H_RESOURCE_
#include "common_types.h"

typedef struct _IMAP_RETURN_CODE {
    int     code;
    char    comment[512];
} IMAP_RETURN_CODE;

enum {
	IMAP_CODE_2160001=0,
	IMAP_CODE_2160002,
	IMAP_CODE_2160003,
	IMAP_CODE_2160004,

	IMAP_CODE_2170000,
	IMAP_CODE_2170001,
	IMAP_CODE_2170002,
	IMAP_CODE_2170003,
	IMAP_CODE_2170004,
	IMAP_CODE_2170005,
	IMAP_CODE_2170006,
	IMAP_CODE_2170007,
	IMAP_CODE_2170008,
	IMAP_CODE_2170009,
	IMAP_CODE_2170010,
	IMAP_CODE_2170011,
	IMAP_CODE_2170012,
	IMAP_CODE_2170013,
	IMAP_CODE_2170014,
	IMAP_CODE_2170015,
	IMAP_CODE_2170016,
	IMAP_CODE_2170017,
	IMAP_CODE_2170018,
	IMAP_CODE_2170019,
	IMAP_CODE_2170020,
	IMAP_CODE_2170021,
	IMAP_CODE_2170022,
	IMAP_CODE_2170023,
	IMAP_CODE_2170024,
	IMAP_CODE_2170025,
	IMAP_CODE_2170026,
	IMAP_CODE_2170027,
	IMAP_CODE_2170028,
	IMAP_CODE_2170029,
	IMAP_CODE_2170030,

	IMAP_CODE_2180000,
	IMAP_CODE_2180001,
	IMAP_CODE_2180002,
	IMAP_CODE_2180003,
	IMAP_CODE_2180004,
	IMAP_CODE_2180005,
	IMAP_CODE_2180006,
	IMAP_CODE_2180007,
	IMAP_CODE_2180008,
	IMAP_CODE_2180009,
	IMAP_CODE_2180010,
	IMAP_CODE_2180011,
	IMAP_CODE_2180012,
	IMAP_CODE_2180013,
	IMAP_CODE_2180014,
	IMAP_CODE_2180015,
	IMAP_CODE_2180016,
	IMAP_CODE_2180017,
	IMAP_CODE_2180018,
	IMAP_CODE_2180019,
	IMAP_CODE_2180020,

	IMAP_CODE_2190001,
	IMAP_CODE_2190002,
	IMAP_CODE_2190003,
	IMAP_CODE_2190004,
	IMAP_CODE_2190005,
	IMAP_CODE_2190006,
	IMAP_CODE_2190007,
	IMAP_CODE_2190008,
	IMAP_CODE_2190009,
	IMAP_CODE_2190010,
	IMAP_CODE_2190011,
	IMAP_CODE_2190012,
	IMAP_CODE_2190013,
	IMAP_CODE_2190014,
	IMAP_CODE_2190015,
	IMAP_CODE_2190016,
	IMAP_CODE_2190017,
	
	IMAP_CODE_2200000,
	IMAP_CODE_2200001,
	IMAP_CODE_2200002,
	IMAP_CODE_2200003,
	IMAP_CODE_2200004,
	IMAP_CODE_2200005,
	IMAP_CODE_2200006,
	IMAP_CODE_2200007,
	IMAP_CODE_2200008,
	IMAP_CODE_2200009,
	IMAP_CODE_2200010
};

extern void resource_init(const char *cfg_filename);
extern void resource_free(void);
extern int resource_run(void);
extern int resource_stop(void);
extern BOOL resource_save(void);
extern BOOL resource_get_integer(const char *key, int *value);
const char *resource_get_string(const char *key);
extern BOOL resource_set_integer(const char *key, int value);
extern BOOL resource_set_string(const char *key, const char *value);
extern const char *resource_get_imap_code(int code_type, int n, int *len);
extern BOOL resource_refresh_imap_code_table(void);
char** resource_get_folder_strings(const char*lang);

const char* resource_get_default_charset(const char *lang);
extern BOOL resource_get_digest_string(const char *src, const char *tag, char *buff, int buff_len);
extern BOOL resource_get_digest_integer(const char *src, const char *tag, long *pinteger);
extern void resource_set_digest_string(char *src, int length, const char *tag, const char *value);
extern void resource_set_digest_integer(char *src, int length, const char *tag, long value);
extern const char *resource_get_error_string(int errno);

#endif /* _H_RESOURCE_ */
