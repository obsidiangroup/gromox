// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <libHX/option.h>
#include <gromox/fileio.h>
#include <gromox/paths.h>
#include <gromox/scope.hpp>
#include <gromox/config_file.hpp>
#include "message_dequeue.h" 
#include "console_cmd_handler.h"
#include <gromox/console_server.hpp>
#include "system_services.h"
#include "transporter.h" 
#include <gromox/lib_buffer.hpp>
#include "resource.h" 
#include "service.h" 
#include <gromox/util.hpp>
#include <sys/types.h>
#include <csignal>
#include <unistd.h>
#include <cstdio>
#include <pwd.h>

using namespace gromox;

std::atomic<bool> g_notify_stop{false};
std::shared_ptr<CONFIG_FILE> g_config_file;
static char *opt_config_file;
static std::atomic<bool> g_hup_signalled{false};
static struct HXoption g_options_table[] = {
	{nullptr, 'c', HXTYPE_STRING, &opt_config_file, nullptr, nullptr, 0, "Config file to read", "FILE"},
	HXOPT_AUTOHELP,
	HXOPT_TABLEEND,
};

static const char *const g_dfl_mpc_plugins[] = {
	"libgxm_alias_translator.so",
	"libgxm_exmdb_local.so",
	"libgxm_mlist_expand.so",
	NULL,
};

static const char *const g_dfl_svc_plugins[] = {
	"libgxs_domain_list.so",
	"libgxs_logthru.so",
	"libgxs_ldap_adaptor.so",
	"libgxs_mysql_adaptor.so",
	"libgxs_textmaps.so",
	"libgxs_authmgr.so",
	NULL,
};

typedef void (*STOP_FUNC)();

static void term_handler(int signo);

int main(int argc, const char **argv) try
{ 
	int retcode = EXIT_FAILURE;
	char temp_buff[256];
    struct passwd *puser_pass;

	setvbuf(stdout, nullptr, _IOLBF, 0);
	if (HX_getopt(g_options_table, &argc, &argv, HXOPT_USAGEONERR) != HXOPT_ERR_SUCCESS)
		return EXIT_FAILURE;
	struct sigaction sact{};
	sigemptyset(&sact.sa_mask);
	sact.sa_handler = [](int) {};
	sigaction(SIGALRM, &sact, nullptr);
	sact.sa_handler = [](int) { g_hup_signalled = true; };
	sigaction(SIGHUP, &sact, nullptr);
	sact.sa_handler = SIG_IGN;
	sact.sa_flags   = SA_RESTART;
	sigaction(SIGPIPE, &sact, nullptr);
	sact.sa_handler = term_handler;
	sact.sa_flags   = SA_RESETHAND;
	sigaction(SIGINT, &sact, nullptr);
	sigaction(SIGTERM, &sact, nullptr);
	g_config_file = config_file_prg(opt_config_file, "delivery.cfg");
	if (opt_config_file != nullptr && g_config_file == nullptr)
		printf("[resource]: config_file_init %s: %s\n", opt_config_file, strerror(errno));
	if (g_config_file == nullptr)
		return EXIT_FAILURE;

	static constexpr cfg_directive cfg_default_values[] = {
		{"admin_mailbox", "root@localhost"},
		{"config_file_path", PKGSYSCONFDIR "/delivery:" PKGSYSCONFDIR},
		{"console_server_ip", "::1"},
		{"console_server_port", "6677"},
		{"context_average_mime", "8", CFG_SIZE, "1"},
		{"data_file_path", PKGDATADIR "/delivery:" PKGDATADIR},
		{"dequeue_maximum_mem", "1G", CFG_SIZE, "1"},
		{"dequeue_path", PKGSTATEQUEUEDIR},
		{"domain_list_valid", "true", CFG_BOOL},
		{"mpc_plugin_ignore_errors", "false", CFG_BOOL},
		{"mpc_plugin_path", PKGLIBDIR},
		{"running_identity", "gromox"},
		{"service_plugin_path", PKGLIBDIR},
		{"state_path", PKGSTATEDIR},
		{"work_threads_min", "16", CFG_SIZE, "1"},
		{},
	};
	config_file_apply(*g_config_file, cfg_default_values);

	auto str_val = g_config_file->get_value("host_id");
	if (str_val == NULL) {
		memset(temp_buff, 0, arsizeof(temp_buff));
		gethostname(temp_buff, arsizeof(temp_buff));
		resource_set_string("HOST_ID", temp_buff);
		str_val = temp_buff;
		printf("[system]: warning! cannot find host ID, OS host name will be "
			"used as host ID\n");
	}
	printf("[system]: host ID is %s\n", str_val);

	str_val = resource_get_string("DEFAULT_DOMAIN");
	if (str_val == NULL) {
		memset(temp_buff, 0, arsizeof(temp_buff));
		getdomainname(temp_buff, arsizeof(temp_buff));
		resource_set_string("DEFAULT_DOMAIN", temp_buff);
		str_val = temp_buff;
		printf("[system]: warning! cannot find default domain, OS domain name "
			"will be used as default domain\n");
	}
	printf("[system]: default domain is %s\n", str_val);

	unsigned int threads_min = g_config_file->get_ll("work_threads_min");
	unsigned int threads_max = 2 * threads_min;
    printf("[system]: minimum working threads number is %d\n", threads_min);

	if (resource_get_uint("WORK_THREADS_MAX", &threads_max)) {
		if (threads_max <= threads_min) {
			threads_max = threads_min + 1;
			resource_set_integer("WORK_THREADS_MAX", threads_max);
		}
    }
    printf("[system]: maximum working threads number is %d\n", threads_max);

	unsigned int free_contexts = threads_max;
	if (resource_get_uint("FREE_CONTEXT_NUM", &free_contexts)) {
		if (free_contexts < threads_max) {
			free_contexts = threads_max; 
			resource_set_integer("FREE_CONTEXT_NUM", free_contexts);
		}
	}
    printf("[system]: free contexts number is %d\n", free_contexts);
    
	unsigned int mime_ratio = g_config_file->get_ll("context_average_mime");
	printf("[system]: average mimes number for one context is %d\n",
		mime_ratio);

	size_t max_mem = g_config_file->get_ll("dequeue_maximum_mem");
	bytetoa(max_mem, temp_buff);
    printf("[message_dequeue]: maximum allocated memory is %s\n", temp_buff);
    
	const char *str_value = resource_get_string("MPC_PLUGIN_LIST");
	const char *const *mpc_plugin_list = NULL;
	if (str_value != NULL) {
		mpc_plugin_list = const_cast<const char * const *>(read_file_by_line(str_value));
		if (mpc_plugin_list == NULL) {
			printf("read_file_by_line %s: %s\n", str_value, strerror(errno));
			return EXIT_FAILURE;
		}
	}
 
	str_value = resource_get_string("SERVICE_PLUGIN_LIST");
	const char *const *service_plugin_list = NULL;
	if (str_value != NULL) {
		service_plugin_list = const_cast<const char * const *>(read_file_by_line(str_value));
		if (service_plugin_list == NULL) {
			printf("read_file_by_line %s: %s\n", str_value, strerror(errno));
			return EXIT_FAILURE;
		}
	}

	auto console_server_ip = g_config_file->get_value("console_server_ip");
	uint16_t console_server_port = g_config_file->get_ll("console_server_port");
	printf("[console_server]: console server address is [%s]:%hu\n",
	       *console_server_ip == '\0' ? "*" : console_server_ip, console_server_port);
	
	auto user_name = g_config_file->get_value("running_identity");
	if (*user_name != '\0') {
        puser_pass = getpwnam(user_name);
        if (NULL == puser_pass) {
			printf("[system]: no such user \"%s\"\n", user_name);
			return EXIT_FAILURE;
        }
        
        if (0 != setgid(puser_pass->pw_gid)) {
			printf("[system]: can not run group of \"%s\"\n", user_name);
			return EXIT_FAILURE;
        }
        if (0 != setuid(puser_pass->pw_uid)) {
			printf("[system]: can not run as \"%s\"\n", user_name);
			return EXIT_FAILURE;
        }
    }

	service_init({g_config_file->get_value("service_plugin_path"),
		g_config_file->get_value("config_file_path"),
		g_config_file->get_value("data_file_path"),
		g_config_file->get_value("state_path"),
		service_plugin_list != NULL ? service_plugin_list : g_dfl_svc_plugins,
		parse_bool(g_config_file->get_value("service_plugin_ignore_errors")),
		threads_max + free_contexts});
	printf("--------------------------- service plugins begin"
		   "---------------------------\n");
	if (service_run_early() != 0) {
		printf("[system]: failed to run PLUGIN_EARLY_INIT\n");
		return EXIT_FAILURE;
	}
    if (0 != service_run()) { 
		printf("---------------------------- service plugins end"
		   "----------------------------\n");
		printf("[system]: failed to run service\n");
		return EXIT_FAILURE;
    } else {
		printf("---------------------------- service plugins end"
		   "----------------------------\n");
    }
	auto cleanup_4 = make_scope_exit(service_stop);

    if (0 != system_services_run()) { 
		printf("[system]: failed to run system service\n");
		return EXIT_FAILURE;
    }
	auto cleanup_6 = make_scope_exit(system_services_stop);

	message_dequeue_init(g_config_file->get_value("dequeue_path"), max_mem);
    if (0 != message_dequeue_run()) { 
		printf("[system]: failed to run message dequeue\n");
		return EXIT_FAILURE;
    }
	auto cleanup_7 = make_scope_exit(message_dequeue_free);
	auto cleanup_8 = make_scope_exit(message_dequeue_stop);

    console_server_init(console_server_ip, console_server_port);
	console_server_register_command("system", cmd_handler_system_control);
	console_server_register_command("help", cmd_handler_help);
	console_server_register_command(nullptr, cmd_handler_mpc_plugins);
	console_server_register_command(nullptr, cmd_handler_service_plugins);

    if (0 != console_server_run()) {
		printf("[system]: failed to run console server\n");
		return EXIT_FAILURE;
    }
	auto cleanup_9 = make_scope_exit(console_server_free);
	auto cleanup_10 = make_scope_exit(console_server_stop);

	transporter_init(g_config_file->get_value("mpc_plugin_path"),
		mpc_plugin_list != nullptr ? mpc_plugin_list : g_dfl_mpc_plugins,
		threads_min, threads_max,
		free_contexts, mime_ratio,
		parse_bool(g_config_file->get_value("domain_list_valid")) ? TRUE : false,
		parse_bool(g_config_file->get_value("mpc_plugin_ignore_errors")));

	printf("--------------------------- mpc plugins begin"
		"---------------------------\n");
    if (0 != transporter_run()) { 
		printf(" ---------------------------- mpc plugins end"
			"-----------------------------\n");
		printf("[system]: failed to run transporter\n");
		return EXIT_FAILURE;
    } else {
		printf("----------------------------- mpc plugins end"
			"-----------------------------\n");
    }
	auto cleanup_11 = make_scope_exit(transporter_free);
	auto cleanup_12 = make_scope_exit(transporter_stop);

	retcode = EXIT_SUCCESS;
    printf("[system]: DELIVERY APP is now running\n");
	while (!g_notify_stop) {
        sleep(3);
		if (g_hup_signalled.exchange(false))
			service_reload_all();
    }
	return retcode;
} catch (const cfg_error &) {
	return EXIT_FAILURE;
}

static void term_handler(int signo)
{
	console_server_notify_main_stop();
}


 
