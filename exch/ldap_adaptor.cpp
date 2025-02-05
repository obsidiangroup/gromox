// SPDX-License-Identifier: AGPL-3.0-or-later
// SPDX-FileCopyrightText: 2020–2021 grommunio GmbH
// This file is part of Gromox.
#define DECLARE_API_STATIC
#include <cerrno>
#include <typeinfo>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <utility>
#include <ldap.h>
#include <libHX/string.h>
#include <gromox/defs.h>
#include <gromox/resource_pool.hpp>
#include <gromox/svc_common.h>
#include <gromox/tie.hpp>
#include <gromox/config_file.hpp>
#include <gromox/util.hpp>
#include "ldap_adaptor.hpp"

using namespace gromox;
using namespace std::string_literals;

namespace {
struct ldapfree {
	void operator()(LDAP *ld) { ldap_unbind_ext_s(ld, nullptr, nullptr); }
	void operator()(LDAPMessage *m) { ldap_msgfree(m); }
};
}

using ldap_msg = std::unique_ptr<LDAPMessage, ldapfree>;
using ldap_ptr = std::unique_ptr<LDAP, ldapfree>;

namespace {
struct twoconn {
	ldap_ptr meta, bind;
};
}

enum {
	USER_PRIVILEGE_POP3_IMAP = 1 << 0,
	USER_PRIVILEGE_SMTP = 1 << 1,
};

static std::string g_ldap_host, g_search_base, g_mail_attr;
static std::string g_bind_user, g_bind_pass;
static bool g_use_tls;
static unsigned int g_dataconn_num = 4;
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
	while ((msg = ldap_next_message(ld, msg)) != nullptr)
		if (ldap_msgtype(msg) != LDAP_RES_SEARCH_RESULT &&
		    ldap_msgtype(msg) != LDAP_RES_SEARCH_REFERENCE)
			return false;
	return true;
}

static ldap_ptr make_conn(bool perform_bind)
{
	ldap_ptr ld;
	auto host = g_ldap_host.c_str();
	auto ret = ldap_initialize(&unique_tie(ld), *host == '\0' ? nullptr : host);
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
	if (!perform_bind)
		return ld;

	struct berval cred{};
	const char *binduser = nullptr;
	if (g_bind_user.size() != 0) {
		cred.bv_val = deconst(g_bind_pass.c_str());
		cred.bv_len = g_bind_pass.size();
		binduser = g_bind_user.c_str();
	}
	ret = ldap_sasl_bind_s(ld.get(), binduser, LDAP_SASL_SIMPLE, &cred,
	      nullptr, nullptr, nullptr);
	if (ret != LDAP_SUCCESS) {
		printf("[ldap_adaptor]: bind as \"%s\": %s\n",
		       g_bind_user.c_str(), ldap_err2string(ret));
		return {};
	}
	return ld;
}

static constexpr bool AVOID_BIND = false, DO_BIND = true;

template<typename F, typename... Args>
static auto gx_auto_retry_2(F &&func, ldap_ptr &ld, bool perform_bind, Args &&...args) ->
    decltype(func(nullptr, std::forward<Args>(args)...))
{
	if (ld == nullptr)
		ld = make_conn(perform_bind);
	if (ld == nullptr)
		return LDAP_SERVER_DOWN;
	auto ret = func(ld.get(), std::forward<Args>(args)...);
	if (ret != LDAP_SERVER_DOWN)
		return ret;
	ld = make_conn(perform_bind);
	if (ld == nullptr)
		return ret;
	return func(ld.get(), std::forward<Args>(args)...);
}

template<typename F, typename... Args>
static auto gx_auto_retry(F &&func, ldap_ptr &ld, Args &&...args) ->
    decltype(func(nullptr, std::forward<Args>(args)...))
{
	return gx_auto_retry_2(func, ld, DO_BIND, std::forward<Args>(args)...);
}

BOOL ldap_adaptor_login2(const char *username, const char *password)
{
	struct stdlib_free { void operator()(void *p) { free(p); } };
	auto tok = g_conn_pool.get_wait();
	ldap_msg msg;
	std::unique_ptr<char, stdlib_free> freeme;
	auto quoted = HX_strquote(username, HXQUOTE_LDAPRDN, &unique_tie(freeme));
	auto filter = g_mail_attr + "="s + quoted;
	auto ret = gx_auto_retry(ldap_search_ext_s, tok.res.meta,
	           g_search_base.size() != 0 ? g_search_base.c_str() : nullptr,
	           LDAP_SCOPE_SUBTREE, filter.c_str(),
	           const_cast<char **>(no_attrs), true, nullptr, nullptr,
	           nullptr, 2, &unique_tie(msg));
	if (ret != LDAP_SUCCESS) {
		printf("[ldap_adaptor]: search with filter %s: %s\n",
		       filter.c_str(), ldap_err2string(ret));
		return FALSE;
	}
	if (!validate_response(tok.res.meta.get(), msg.get())) {
		printf("[ldap_adaptor]: filter %s was ambiguous\n", filter.c_str());
		return FALSE;
	}

	auto firstmsg = ldap_first_message(tok.res.meta.get(), msg.get());
	if (firstmsg == nullptr)
		return FALSE;
	auto dn = ldap_get_dn(tok.res.meta.get(), firstmsg);
	if (dn == nullptr)
		return FALSE;

	struct berval bv;
	bv.bv_val = deconst(password != nullptr ? password : "");
	bv.bv_len = password != nullptr ? strlen(password) : 0;
	ret = gx_auto_retry_2(ldap_sasl_bind_s, tok.res.bind, AVOID_BIND, dn,
	      LDAP_SASL_SIMPLE, &bv, nullptr, nullptr, nullptr);
	if (ret == LDAP_SUCCESS)
		return TRUE;
	printf("[ldap_adaptor]: ldap_simple_bind %s: %s\n", dn, ldap_err2string(ret));
	return FALSE;
}

static bool ldap_adaptor_load() try
{
	auto pfile = config_file_initd("ldap_adaptor.cfg", get_config_path());
	if (pfile == nullptr) {
		printf("[ldap_adaptor]: config_file_initd ldap_adaptor.cfg: %s\n",
		       strerror(errno));
		return false;
	}

	auto val = pfile->get_value("data_connections");
	if (val != nullptr) {
		g_dataconn_num = strtoul(val, nullptr, 0);
		if (g_dataconn_num == 0)
			g_dataconn_num = 1;
	}

	val = pfile->get_value("ldap_host");
	g_ldap_host = val != nullptr ? val : "";
	g_use_tls = parse_bool(pfile->get_value("ldap_start_tls"));

	val = pfile->get_value("ldap_bind_user");
	if (val != nullptr)
		g_bind_user = val;
	val = pfile->get_value("ldap_bind_pass");
	if (val != nullptr)
		g_bind_pass = val;

	val = pfile->get_value("ldap_mail_attr");
	g_mail_attr = val != nullptr ? val : "mail";

	val = pfile->get_value("ldap_search_base");
	g_search_base = val != nullptr ? val : "";
	printf("[ldap_adaptor]: hosts <%s>%s, base <%s>, #conn=%d, mailattr=%s\n",
	       g_ldap_host.c_str(), g_use_tls ? " +TLS" : "",
	       g_search_base.c_str(), 2 * g_dataconn_num, g_mail_attr.c_str());
	g_conn_pool.resize(g_dataconn_num);
	g_conn_pool.bump();
	return true;
} catch (const std::bad_alloc &) {
	printf("[ldap_adaptor]: E-1455: ENOMEM\n");
	return false;
}

static BOOL svc_ldap_adaptor(int reason, void **ppdata) try
{
	if (reason == PLUGIN_FREE) {
		g_conn_pool.clear();
		return TRUE;
	} else if (reason == PLUGIN_RELOAD) {
		ldap_adaptor_load();
		return TRUE;
	}
	if (reason != PLUGIN_INIT)
		return TRUE;

	LINK_API(ppdata);
	if (!ldap_adaptor_load())
		return false;
	if (!register_service("ldap_auth_login2", ldap_adaptor_login2)) {
		printf("[ldap_adaptor]: failed to register services\n");
		return false;
	}
	return TRUE;
} catch (...) {
	return false;
}
SVC_ENTRY(svc_ldap_adaptor);
