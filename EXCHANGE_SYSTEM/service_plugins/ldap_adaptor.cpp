/* SPDX-License-Identifier: AGPL-3.0-or-later */
#include <cerrno>
#include <typeinfo>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <ldap.h>
#include <pthread.h>
#include <libHX/string.h>
#include <gromox/resource_pool.hpp>
#include <gromox/svc_common.h>
#include <gromox/tie.hpp>
#include "config_file.h"
#include "ldap_adaptor.h"
#include "util.h"

using namespace gromox;
using namespace std::string_literals;

struct ldapfree {
	void operator()(LDAP *ld) { ldap_unbind_ext_s(ld, nullptr, nullptr); }
	void operator()(LDAPMessage *m) { ldap_msgfree(m); }
};

using ldap_msg = std::unique_ptr<LDAPMessage, ldapfree>;
using ldap_ptr = std::unique_ptr<LDAP, ldapfree>;

struct twoconn {
	ldap_ptr meta, bind;
};

enum {
	USER_PRIVILEGE_POP3_IMAP = 1 << 0,
	USER_PRIVILEGE_SMTP = 1 << 1,
};

DECLARE_API;

static std::string g_config_path, g_ldap_host, g_search_base, g_mail_attr;
static std::string g_bind_user, g_bind_pass;
static std::mutex g_conn_mtx;
static bool g_use_tls;
static resource_pool<twoconn> g_conn_pool;

static constexpr const char *no_attrs[] = {nullptr};

/**
 * Make sure the response has exactly one entry and at most
 * one optional informational search trailer.
 */
static bool validate_response(LDAP *ld, LDAPMessage *result)
{
	auto msg = ldap_first_message(ld, result);
	if (msg == nullptr || ldap_msgtype(msg) != LDAP_RES_SEARCH_ENTRY)
		return false;
	msg = ldap_next_message(ld, msg);
	if (msg == nullptr)
		return true;
	if (ldap_msgtype(msg) != LDAP_RES_SEARCH_RESULT)
		return false;
	return ldap_next_message(ld, msg) == nullptr;
}

static constexpr const char *zero_attrs[] = {nullptr};

static ldap_ptr make_conn()
{
	ldap_ptr ld;
	auto ret = ldap_initialize(&unique_tie(ld), nullptr); //g_ldap_host.c_str());
	if (ret != LDAP_SUCCESS)
		return {};
	static constexpr int version = LDAP_VERSION3;
	ret = ldap_set_option(ld.get(), LDAP_OPT_PROTOCOL_VERSION, &version);
	if (ret != LDAP_SUCCESS)
		return {};
	ret = ldap_set_option(ld.get(), LDAP_OPT_REFERRALS, LDAP_OPT_OFF);
	if (ret != LDAP_SUCCESS)
		return {};
	if (g_use_tls) {
		ret = ldap_start_tls_s(ld.get(), nullptr, nullptr);
		if (ret != LDAP_SUCCESS) {
			printf("ldap_start_tls_s: %s\n", ldap_err2string(ret));
			return {};
		}
	}

	struct berval cred;
	cred.bv_val = const_cast<char *>(g_bind_pass.c_str());
	cred.bv_len = g_bind_pass.size();
	ret = ldap_sasl_bind_s(ld.get(), g_bind_user.size() == 0 ? nullptr : g_bind_user.c_str(),
	      LDAP_SASL_SIMPLE, &cred, nullptr, nullptr, nullptr);
	if (ret != LDAP_SUCCESS) {
		printf("[ldap_adaptor]: bind as \"%s\": %s\n",
		       g_bind_user.c_str(), ldap_err2string(ret));
		return {};
	}
	return ld;
}

static BOOL ldap_adaptor_login2(const char *username, const char *password,
    char *maildir, char *lang, char *reason, int length, unsigned int mode)
{
	struct stdlib_free { void operator()(void *p) { free(p); } };
	twoconn ld = g_conn_pool.get_wait();
	if (ld.meta == nullptr || ld.bind == nullptr)
		return FALSE;

	ldap_msg msg;
	std::unique_ptr<char, stdlib_free> freeme;
	auto quoted = HX_strquote(username, HXQUOTE_LDAPRDN, &unique_tie(freeme));
	auto filter = g_mail_attr + "="s + quoted;
	auto ret = ldap_search_ext_s(ld.meta.get(), g_search_base.c_str(),
	           LDAP_SCOPE_SUBTREE, filter.c_str(), const_cast<char **>(no_attrs),
	           true, nullptr, nullptr, nullptr, 2, &unique_tie(msg));
	if (ret != LDAP_SUCCESS) {
		printf("[ldap_adaptor]: search with filter %s: %s\n",
		       filter.c_str(), ldap_err2string(ret));
		return FALSE;
	}
	if (!validate_response(ld.meta.get(), msg.get())) {
		printf("[ldap_adaptor]: filter %s was ambiguous\n", filter.c_str());
		return FALSE;
	}

	auto firstmsg = ldap_first_message(ld.meta.get(), msg.get());
	if (firstmsg == nullptr)
		return FALSE;
	auto dn = ldap_get_dn(ld.meta.get(), firstmsg);
	if (dn == nullptr)
		return FALSE;

	struct berval bv;
	bv.bv_val = const_cast<char *>(password != nullptr ? password : "");
	bv.bv_len = password != nullptr ? strlen(password) : 0;
	ret = ldap_sasl_bind_s(ld.bind.get(), dn, LDAP_SASL_SIMPLE, &bv,
	      nullptr, nullptr, nullptr);
	if (ret == LDAP_SUCCESS) {
		g_conn_pool.put(std::move(ld));
		return TRUE;
	}
	if (ret == LDAP_INVALID_CREDENTIALS) {
		g_conn_pool.put(std::move(ld));
		return FALSE;
	}
	printf("[ldap_adaptor]: ldap_simple_bind 1 %s: %s\n", dn, ldap_err2string(ret));
	ldap_unbind_ext(ld.bind.release(), nullptr, nullptr);
	ld.bind = make_conn();
	if (ld.bind == nullptr)
		return FALSE;
	ret = ldap_sasl_bind_s(ld.bind.get(), dn, LDAP_SASL_SIMPLE, &bv,
	      nullptr, nullptr, nullptr);
	g_conn_pool.put(std::move(ld));
	if (ret == LDAP_SUCCESS)
		return TRUE;
	printf("[ldap_adaptor]: ldap_simple_bind 2 %s: %s\n", dn, ldap_err2string(ret));
	return FALSE;
}

static bool ldap_adaptor_load_base()
{
	twoconn ld = g_conn_pool.get_wait();
	if (ld.meta == nullptr || ld.bind == nullptr)
		return false;

	ldap_msg msg;
	auto ret = ldap_search_ext_s(ld.meta.get(), nullptr,
	           LDAP_SCOPE_BASE, nullptr, const_cast<char **>(zero_attrs),
	           true, nullptr, nullptr, nullptr, 1, &unique_tie(msg));
	if (ret != LDAP_SUCCESS) {
		printf("[ldap_adaptor]: base lookup: %s\n", ldap_err2string(ret));
		return false;
	}
	if (!validate_response(ld.meta.get(), msg.get())) {
		printf("[ldap_adaptor]: base lookup: no good result\n");
		return false;
	}
	auto firstmsg = ldap_first_message(ld.meta.get(), msg.get());
	if (firstmsg == nullptr)
		return false;
	auto dn = ldap_get_dn(ld.meta.get(), firstmsg);
	if (dn == nullptr)
		return false;
	g_search_base = dn;
	printf("[ldap_adaptor]: discovered base \"%s\"\n", g_search_base.c_str());
	return true;
}

static bool ldap_adaptor_load()
{
	/* get the plugin name from system api */
	g_config_path = get_config_path() + "/ldap_adaptor.cfg"s;
	auto pfile = config_file_init2(nullptr, g_config_path.c_str());
	if (pfile == nullptr) {
		printf("[ldap_adaptor]: config_file_init %s: %s\n",
		       g_config_path.c_str(), strerror(errno));
		return false;
	}

	unsigned int conn_num = 4;
	auto val = config_file_get_value(pfile, "data_connections");
	if (val != nullptr) {
		conn_num = strtoul(val, nullptr, 0);
		if (conn_num < 0)
			conn_num = 1;
	}
	printf("[ldap_adaptor]: using up to %d connections\n", 2 * conn_num);

	val = config_file_get_value(pfile, "ldap_host");
	g_ldap_host = val != nullptr ? val : "";
	printf("[ldap_adaptor]: hostlist is \"%s\"\n", g_ldap_host.c_str());

	val = config_file_get_value(pfile, "ldap_bind_user");
	if (val != nullptr)
		g_bind_user = val;
	val = config_file_get_value(pfile, "ldap_bind_pass");
	if (val != nullptr)
		g_bind_pass = val;

	val = config_file_get_value(pfile, "ldap_mail_attr");
	g_mail_attr = val != nullptr ? val : "mail";
	printf("[ldap_adaptor]: ldap mail attribute is \"%s\"\n", g_mail_attr.c_str());
	config_file_free(pfile);

	for (unsigned int i = 0; i < conn_num; ++i) {
		twoconn ld;
		ld.meta = make_conn();
		if (ld.meta == nullptr)
			/*
			 * If we cannot create a connection, trying again
			 * likely won't make it better.
			 */
			break;
		ld.bind = make_conn();
		g_conn_pool.put(std::move(ld));
	}
	if (g_conn_pool.size() == 0)
		return FALSE;

	val = config_file_get_value(pfile, "ldap_search_base");
	g_search_base = val != nullptr ? val : "";
	if (g_search_base.size() == 0 && !ldap_adaptor_load_base())
		return false;
	return true;
}

BOOL SVC_LibMain(int reason, void **ppdata) try
{
	if (reason == PLUGIN_FREE) {
		g_conn_pool.clear();
		return TRUE;
	}
	if (reason != PLUGIN_INIT)
		return false;

	LINK_API(ppdata);
	if (!register_service("ldap_adaptor_load", reinterpret_cast<void *>(ldap_adaptor_load))) {
		printf("[ldap_adaptor]: failed to register \"ldap_adaptor_load\" service\n");
		return false;
	}
	if (!register_service("ldap_auth_login2", reinterpret_cast<void *>(ldap_adaptor_login2))) {
		printf("[ldap_adaptor]: failed to register \"auth_login_exch\" service\n");
		return false;
	}
	return TRUE;
} catch (...) {
	return false;
}