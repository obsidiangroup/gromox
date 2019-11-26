#ifndef _H_RESOURCE_
#define _H_RESOURCE_
#include "common_types.h"

typedef struct _SMTP_ERROR_CODE {
    int     code;
    char    comment[512];
} SMTP_ERROR_CODE;

enum {
    SMTP_CODE_2172001 = 0,
    SMTP_CODE_2172002,
    SMTP_CODE_2172003,
    SMTP_CODE_2172004,
    SMTP_CODE_2172005,
    SMTP_CODE_2172006,
    SMTP_CODE_2172007,
    SMTP_CODE_2172008,
    SMTP_CODE_2172009,
    SMTP_CODE_2172010,

    SMTP_CODE_2173001,
    SMTP_CODE_2173002,
    SMTP_CODE_2173003,
    SMTP_CODE_2173004,

    SMTP_CODE_2174001,
    SMTP_CODE_2174002,
    SMTP_CODE_2174003,
    SMTP_CODE_2174004,
    SMTP_CODE_2174005, 
    SMTP_CODE_2174006,
    SMTP_CODE_2174007, 
    SMTP_CODE_2174008, 
    SMTP_CODE_2174009, 
    SMTP_CODE_2174010, 
    SMTP_CODE_2174011, 
    SMTP_CODE_2174012, 
    SMTP_CODE_2174013, 
    SMTP_CODE_2174014, 
    SMTP_CODE_2174015, 
    SMTP_CODE_2174016, 
    SMTP_CODE_2174017, 
    SMTP_CODE_2174018, 
    SMTP_CODE_2174019, 
    SMTP_CODE_2174020,

    SMTP_CODE_2175001,
    SMTP_CODE_2175002,
    SMTP_CODE_2175003,
    SMTP_CODE_2175004,
    SMTP_CODE_2175005,
    SMTP_CODE_2175006,
    SMTP_CODE_2175007,
    SMTP_CODE_2175008,
    SMTP_CODE_2175009,
    SMTP_CODE_2175010,
    SMTP_CODE_2175011,
    SMTP_CODE_2175012,
    SMTP_CODE_2175013,
    SMTP_CODE_2175014,
    SMTP_CODE_2175015,
    SMTP_CODE_2175016,
    SMTP_CODE_2175017,
    SMTP_CODE_2175018,
    SMTP_CODE_2175019,
    SMTP_CODE_2175020,
    SMTP_CODE_2175021,
    SMTP_CODE_2175022,
    SMTP_CODE_2175023,
    SMTP_CODE_2175024,
    SMTP_CODE_2175025,
    SMTP_CODE_2175026,
    SMTP_CODE_2175027,
    SMTP_CODE_2175028,
    SMTP_CODE_2175029,
    SMTP_CODE_2175030,
    SMTP_CODE_2175031,
    SMTP_CODE_2175032,
    SMTP_CODE_2175033, 
    SMTP_CODE_2175034, 
    SMTP_CODE_2175035,
    SMTP_CODE_2175036,
    SMTP_CODE_COUNT
};

extern void resource_init(const char *cfg_filename);
extern void resource_free(void);
extern int resource_run(void);
extern int resource_stop(void);
extern BOOL resource_save(void);
extern BOOL resource_get_integer(const char *key, int *value);
extern const char *resource_get_string(const char *key);
extern BOOL resource_set_integer(const char *key, int value);
extern BOOL resource_set_string(const char *key, const char *value);
char* resource_get_smtp_code(int code_type, int n, int *len);
extern BOOL resource_refresh_smtp_code_table(void);

#endif /* _H_RESOURCE_ */
