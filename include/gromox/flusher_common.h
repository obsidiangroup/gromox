#pragma once
#include <list>
#include <typeinfo>
#include <sys/time.h>
#include <gromox/defs.h>
#include <gromox/common_types.hpp>
#include <gromox/mem_file.hpp>
#include <gromox/plugin.hpp>
#include <gromox/stream.hpp>

enum {
	FLUSH_WHOLE_MAIL = 0,
	FLUSH_PART_MAIL = 1,
};

enum {
	FLUSH_NONE = 0,
	FLUSH_RESULT_OK = 1,
	FLUSH_TEMP_FAIL = 2,
	FLUSH_PERMANENT_FAIL = 3,
};

enum {
	HT_LMTP = 1 << 0,
	HT_SMTP = 1 << 1,
};

struct ENVELOPE_INFO_BASE {
	char parsed_domain[UDOM_SIZE]; /* parsed domain according connection*/
	char hello_domain[UDOM_SIZE]; /* domain name after helo */
	char from[UADDR_SIZE]; /* envelope's from message */
	char username[UADDR_SIZE]; /* user name for login */
	MEM_FILE f_rcpt_to; /* envelope's rcpt to message */
	BOOL        is_login;          /* user is logged in */
    BOOL        is_outbound;       /* in-bound or out-bound */
    BOOL        is_relay;          /* indicate whether this mail is relaying */
};

struct CONNECTION_BASE {
	char client_ip[40]; /* client ip address string */
    int            client_port;        /* value of client port */
	char server_ip[40]; /* server ip address */
    int            server_port;        /* value of server port */
    int            sockd;              /* context's socket file description */
    struct timeval last_timestamp;     /* last time when system got data from */
};

struct FLUSH_INFO {
    int           flush_action; /* indicate flushing whole or part of mail */
    int           flush_result;
    int           flush_ID;
    void          *flush_ptr;     /* extended data pointer */
};

struct SMTP_CONTEXT;
struct FLUSH_ENTITY final {
	STREAM *pstream = nullptr;
	CONNECTION_BASE *pconnection = nullptr;
	FLUSH_INFO *pflusher = nullptr; /* the flusher for saving mail information */
	ENVELOPE_INFO_BASE *penvelope = nullptr;
	BOOL is_spam = false; /* whether the mail is spam */
	int context_ID = 0;
	unsigned int command_protocol = 0;
	SMTP_CONTEXT *pcontext = nullptr;
};

typedef void (*CANCEL_FUNCTION)(FLUSH_ENTITY *);

#define DECLARE_API(x) \
	x void *(*query_serviceF)(const char *, const std::type_info &); \
	x int (*get_queue_length)(); \
	x void (*log_info)(unsigned int, const char *, ...); \
	x BOOL (*feedback_entity)(std::list<FLUSH_ENTITY> &&); \
	x BOOL (*register_cancel)(CANCEL_FUNCTION); \
	x std::list<FLUSH_ENTITY> (*get_from_queue)(); \
	x const char *(*get_host_ID)(); \
	x const char *(*get_plugin_name)(); \
	x const char *(*get_config_path)(); \
	x const char *(*get_data_path)(); \
	x const char *(*get_state_path)(); \
	x int (*get_extra_num)(int); \
	x const char *(*get_extra_tag)(int, int); \
	x const char *(*get_extra_value)(int, int); \
	x BOOL (*set_flush_ID)(int); \
	x int (*inc_flush_ID)(); \
	x BOOL (*check_domain)(const char *); \
	x BOOL (*is_domainlist_valid)();
#define query_service2(n, f) ((f) = reinterpret_cast<decltype(f)>(query_serviceF((n), typeid(decltype(*(f))))))
#define query_service1(n) query_service2(#n, n)
#ifdef DECLARE_API_STATIC
DECLARE_API(static);
#else
DECLARE_API(extern);
#endif

#define LINK_API(param) \
	query_serviceF = reinterpret_cast<decltype(query_serviceF)>(param[0]); \
	query_service1(get_queue_length); \
	query_service1(feedback_entity); \
	query_service1(register_cancel); \
	query_service1(get_from_queue); \
	query_service1(get_host_ID); \
	query_service1(log_info); \
	query_service1(set_flush_ID); \
	query_service1(get_plugin_name); \
	query_service1(get_config_path); \
	query_service1(get_data_path); \
	query_service1(get_state_path); \
	query_service1(inc_flush_ID); \
	query_service1(get_extra_num); \
	query_service1(get_extra_tag); \
	query_service1(get_extra_value); \
	query_service2("domain_list_query", check_domain); \
	query_service1(is_domainlist_valid);
#define FLH_ENTRY(s) BOOL FLH_LibMain(int r, void **p) { return (s)((r), (p)); }

extern "C" { /* dlsym */
extern GX_EXPORT BOOL FLH_LibMain(int reason, void **ptrs);
}
