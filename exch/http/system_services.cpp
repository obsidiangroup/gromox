// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <gromox/defs.h>
#include "system_services.h"
#include "service.h"
#include <cstdio>

BOOL (*system_services_judge_ip)(const char*);
BOOL (*system_services_judge_user)(const char*);
BOOL (*system_services_container_add_ip)(const char*);
BOOL (*system_services_container_remove_ip)(const char*);
BOOL (*system_services_add_user_into_temp_list)(const char *, int);
BOOL (*system_services_auth_login)(const char*, const char*, char*, char*, char*, int);
const char* (*system_services_extension_to_mime)(const char*);
void (*system_services_log_info)(unsigned int, const char *, ...);

int system_services_run()
{
#define E(f, s) do { \
	(f) = reinterpret_cast<decltype(f)>(service_query((s), "system", typeid(decltype(*(f))))); \
	if ((f) == nullptr) { \
		printf("[%s]: failed to get the \"%s\" service\n", "system_services", (s)); \
		return -1; \
	} \
} while (false)
#define E2(f, s) ((f) = reinterpret_cast<decltype(f)>(service_query((s), "system", typeid(decltype(*(f))))))

	E2(system_services_judge_ip, "ip_filter_judge");
	E2(system_services_container_add_ip, "ip_container_add");
	E2(system_services_container_remove_ip, "ip_container_remove");
	E(system_services_log_info, "log_info");
	E2(system_services_judge_user, "user_filter_judge");
	E2(system_services_add_user_into_temp_list, "user_filter_add");
	E(system_services_auth_login, "auth_login_exch");
	E(system_services_extension_to_mime, "extension_to_mime");
	return 0;
#undef E
#undef E2
}

void system_services_stop()
{
	service_release("ip_filter_judge", "system");
	service_release("ip_container_add", "system");
	service_release("ip_container_remove", "system");
	service_release("log_info", "system");
	service_release("user_filter_judge", "system");
	service_release("user_filter_add", "system");
	service_release("auth_login_exch", "system");
	service_release("extension_to_mime", "system");
}
