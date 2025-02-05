// SPDX-License-Identifier: GPL-2.0-only WITH linking exception
// SPDX-FileCopyrightText: 2020–2021 grommunio GmbH
// This file is part of Gromox.
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>
#include <libHX/string.h>
#include <gromox/database_mysql.hpp>
#include <gromox/defs.h>
#include "mysql_adaptor.h"
#include <gromox/util.hpp>
#include <cstdio>
#include <unistd.h>
#include <mysql.h>
#include "sql2.hpp"
#define MLIST_PRIVILEGE_ALL				0
#define MLIST_PRIVILEGE_INTERNAL		1
#define MLIST_PRIVILEGE_DOMAIN			2
#define MLIST_PRIVILEGE_SPECIFIED		3
#define MLIST_PRIVILEGE_OUTGOING		4

#define MLIST_RESULT_OK					0
#define MLIST_RESULT_NONE				1
#define MLIST_RESULT_PRIVIL_DOMAIN		2
#define MLIST_RESULT_PRIVIL_INTERNAL	3
#define MLIST_RESULT_PRIVIL_SPECIFIED	4
#define JOIN_WITH_DISPLAYTYPE "LEFT JOIN user_properties AS dt ON u.id=dt.user_id AND dt.proptag=956628995" /* PR_DISPLAY_TYPE_EX */

using namespace std::string_literals;
using namespace gromox;

enum {
	/* Reason codes (users.address_status) for forbidden login */
	AF_USER_NORMAL      = 0x00,
	AF_USER_SUSPENDED   = 0x01,
	AF_USER_OUTOFDATE   = 0x02,
	AF_USER_DELETED     = 0x03,
	AF_USER_SHAREDMBOX  = 0x04,
	AF_USER__MASK       = 0x07,

	AF_GROUP_NORMAL     = 0x00,
	AF_GROUP_SUSPENDED  = 0x10,
	AF_GROUP_OUTOFDATE  = 0x20,
	AF_GROUP_DELETED    = 0x30,
	AF_GROUP__MASK      = 0x30,

	AF_DOMAIN_NORMAL    = 0x00,
	AF_DOMAIN_SUSPENDED = 0x40,
	AF_DOMAIN_OUTOFDATE = 0x80,
	AF_DOMAIN_DELETED   = 0xC0,
	AF_DOMAIN__MASK     = 0xC0,
};

static std::mutex g_crypt_lock;

static void mysql_adaptor_encode_squote(const char *in, char *out);

int mysql_adaptor_run()
{
	if (!db_upgrade_check())
		return -1;
	return 0;
}

void mysql_adaptor_stop()
{
	g_sqlconn_pool.clear();
}

BOOL mysql_adaptor_meta(const char *username, const char *password,
    char *maildir, char *lang, char *reason, int length, unsigned int mode,
    char *encrypt_passwd, size_t encrypt_size, uint8_t *externid_present) try
{
	int temp_status;
	char temp_name[UADDR_SIZE*2];

	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr =
		"SELECT u.password, dt.propval_str AS dtypx, u.address_status, "
		"u.privilege_bits, u.maildir, u.lang, u.externid "
		"FROM users AS u " JOIN_WITH_DISPLAYTYPE
		" WHERE u.username='"s + temp_name + "' LIMIT 2";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1) {
		snprintf(reason, length, "user \"%s\" does not exist", username);
		return FALSE;
	}
	
	auto myrow = pmyres.fetch_row();
	auto dtypx = DT_MAILUSER;
	if (myrow[1] != nullptr)
		dtypx = static_cast<enum display_type>(strtoul(myrow[1], nullptr, 0));
	if (dtypx != DT_MAILUSER) {
		snprintf(reason, length, "\"%s\" is not a real user", username);
		return FALSE;
	}
	temp_status = atoi(myrow[2]);
	if (0 != temp_status) {
		auto uval = temp_status & AF_USER__MASK;
		if (temp_status & AF_DOMAIN__MASK) {
			snprintf(reason, length, "domain of user \"%s\" is disabled!",
				username);
		} else if (temp_status & AF_GROUP__MASK) {
			snprintf(reason, length, "group of user \"%s\" is disabled!",
				username);
		} else if (uval == AF_USER_SHAREDMBOX) {
			snprintf(reason, length, "\"%s\" is a shared mailbox with no login", username);
		} else if (uval != 0) {
			snprintf(reason, length, "user \"%s\" is disabled!", username);
		}
		return FALSE;
	}

	if (mode == USER_PRIVILEGE_POP3_IMAP && !(strtoul(myrow[3], nullptr, 0) & USER_PRIVILEGE_POP3_IMAP)) {
		snprintf(reason, length, "\"%s\" is not authorized to use the POP3/IMAP services", username);
		return false;
	}
	if (mode == USER_PRIVILEGE_SMTP && !(strtoul(myrow[3], nullptr, 0) & USER_PRIVILEGE_SMTP)) {
		snprintf(reason, length, "\"%s\" is not authorized to use the SMTP service", username);
		return false;
	}

	gx_strlcpy(encrypt_passwd, myrow[0], encrypt_size);
	strcpy(maildir, myrow[4]);
	if (NULL != lang) {
		strcpy(lang, myrow[5]);
	}
	encrypt_passwd[encrypt_size-1] = '\0';
	*externid_present = myrow[6] != nullptr;
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1701, e.what());
	return false;
}

static BOOL firsttime_password(const char *username, const char *password,
    char *encrypt_passwd, size_t encrypt_size, char *reason, int length) try
{
	const char *pdomain;
	pdomain = strchr(username, '@');
	if (NULL == pdomain) {
		strncpy(reason, "domain name should be included!", length);
		return FALSE;
	}
	pdomain++;

	std::unique_lock cr_hold(g_crypt_lock);
	gx_strlcpy(encrypt_passwd, crypt_wrapper(password), encrypt_size);
	cr_hold.unlock();

	char temp_name[UADDR_SIZE*2];
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr = "UPDATE users SET password='"s + encrypt_passwd +
	            "' WHERE username='" + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;

	qstr = "SELECT aliasname FROM aliases WHERE mainname='"s + temp_name + "'";
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;

	mysql_adaptor_encode_squote(pdomain, temp_name);
	qstr = "SELECT aliasname FROM aliases WHERE mainname='"s + temp_name + "'";
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres1 = mysql_store_result(conn.res.get());
	if (pmyres1 == nullptr)
		return false;

	size_t k, rows = pmyres.num_rows(), rows1 = pmyres1.num_rows();
	for (k = 0; k < rows1; k++) {
		char virtual_address[UADDR_SIZE];
		char *pat;

		auto myrow1 = pmyres1.fetch_row();
		gx_strlcpy(virtual_address, username, GX_ARRAY_SIZE(virtual_address));
		pat = strchr(virtual_address, '@') + 1;
		strcpy(pat, myrow1[0]);
		mysql_adaptor_encode_squote(virtual_address, temp_name);
		qstr = "UPDATE users SET password='"s + encrypt_passwd +
		       "' WHERE username='" + temp_name + "'";
		if (conn.res.query(qstr.c_str()) != 0)
		        /* ignore - logmsg already emitted */;
	}

	size_t j;
	for (j = 0; j < rows; j++) {
		auto myrow = pmyres.fetch_row();
		mysql_adaptor_encode_squote(myrow[0], temp_name);
		qstr = "UPDATE users SET password='"s + encrypt_passwd +
		       "' WHERE username='" + temp_name + "'";
		if (!conn.res.query(qstr.c_str()))
			continue;
		mysql_data_seek(pmyres1.get(), 0);
		for (k = 0; k < rows1; k++) {
			char virtual_address[UADDR_SIZE], *pat;

			auto myrow1 = pmyres1.fetch_row();
			gx_strlcpy(virtual_address, myrow[0], GX_ARRAY_SIZE(virtual_address));
			pat = strchr(virtual_address, '@') + 1;
			strcpy(pat, myrow1[0]);
			mysql_adaptor_encode_squote(virtual_address, temp_name);
			qstr = "UPDATE users SET password='"s + encrypt_passwd +
			       "' WHERE username='" + temp_name + "'";
			if (conn.res.query(qstr.c_str()) != 0)
				/* ignore - logmsg already emitted */;
		}
	}
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1702, e.what());
	return false;
}

static BOOL verify_password(const char *username, const char *password,
    const char *encrypt_passwd, char *reason, int length)
{
	std::unique_lock cr_hold(g_crypt_lock);
	if (0 == strcmp(crypt(password, encrypt_passwd), encrypt_passwd)) {
		return TRUE;
	}
	cr_hold.unlock();
	snprintf(reason, length, "password error, please check it "
	         "and retry");
	return FALSE;
}

BOOL mysql_adaptor_login2(const char *username, const char *password,
    char *encrypt_passwd, size_t encrypt_size, char *reason,
    int length)
{
	BOOL ret;
	if (g_parm.enable_firsttimepw && *encrypt_passwd == '\0')
		ret = firsttime_password(username, password, encrypt_passwd,
		      encrypt_size, reason, length);
	else
		ret = verify_password(username, password, encrypt_passwd, reason,
		      length);
	return ret;
}

BOOL mysql_adaptor_setpasswd(const char *username,
	const char *password, const char *new_password) try
{
	int temp_status;
	const char *pdomain;
	char *pat;
	char temp_name[UADDR_SIZE*2];
	char encrypt_passwd[40];
	char virtual_address[UADDR_SIZE];
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr =
		"SELECT u.password, dt.propval_str AS dtypx, u.address_status, "
		"u.privilege_bits FROM users AS u " JOIN_WITH_DISPLAYTYPE
		" WHERE u.username='"s + temp_name + "' LIMIT 2";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	auto dtypx = DT_MAILUSER;
	if (myrow[1] != nullptr)
		dtypx = static_cast<enum display_type>(strtoul(myrow[1], nullptr, 0));
	if (dtypx != DT_MAILUSER)
		return FALSE;
	temp_status = atoi(myrow[2]);
	if (0 != temp_status) {
		return FALSE;
	}
	
	if (0 == (atoi(myrow[3])&USER_PRIVILEGE_CHGPASSWD)) {
		return FALSE;
	}

	strncpy(encrypt_passwd, myrow[0], sizeof(encrypt_passwd));
	encrypt_passwd[sizeof(encrypt_passwd) - 1] = '\0';
	
	std::unique_lock cr_hold(g_crypt_lock);
	if ('\0' != encrypt_passwd[0] && 0 != strcmp(crypt(
		password, encrypt_passwd), encrypt_passwd)) {
		return FALSE;
	}
	cr_hold.unlock();
	pdomain = strchr(username, '@');
	if (NULL == pdomain) {
		return FALSE;
	}
	pdomain ++;

	cr_hold.lock();
	gx_strlcpy(encrypt_passwd, crypt_wrapper(new_password), arsizeof(encrypt_passwd));
	cr_hold.unlock();
	qstr = "UPDATE users SET password='"s + encrypt_passwd +
	       "' WHERE username='" + temp_name + "'";
	if (!conn.res.query(qstr.c_str()))
		return false;

	qstr = "SELECT aliasname FROM aliases WHERE mainname='"s + temp_name + "'";
	if (!conn.res.query(qstr.c_str()))
		return false;
	pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;

	mysql_adaptor_encode_squote(pdomain, temp_name);
	qstr = "SELECT aliasname FROM aliases WHERE mainname='"s + temp_name + "'";
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres1 = mysql_store_result(conn.res.get());
	if (pmyres1 == nullptr)
		return false;
	size_t rows = pmyres.num_rows(), rows1 = pmyres1.num_rows();
	for (size_t k = 0; k < rows1; ++k) {
		auto myrow1 = pmyres1.fetch_row();
		gx_strlcpy(virtual_address, username, GX_ARRAY_SIZE(virtual_address));
		pat = strchr(virtual_address, '@') + 1;
		strcpy(pat, myrow1[0]);
		mysql_adaptor_encode_squote(virtual_address, temp_name);
		qstr = "UPDATE users SET password='"s + encrypt_passwd +
		       "' WHERE username='" + temp_name + "'";
		if (conn.res.query(qstr.c_str()) != 0)
			/* ignore - logmsg already emitted */;
	}
	for (size_t j = 0; j < rows; ++j) {
		myrow = pmyres.fetch_row();
		mysql_adaptor_encode_squote(myrow[0], temp_name);
		qstr = "UPDATE users SET password='"s + encrypt_passwd +
		       "' WHERE username='" + temp_name + "'";
		if (!conn.res.query(qstr.c_str()))
			continue;
		mysql_data_seek(pmyres1.get(), 0);
		for (size_t k = 0; k < rows1; ++k) {
			auto myrow1 = pmyres1.fetch_row();
			gx_strlcpy(virtual_address, myrow[0], GX_ARRAY_SIZE(virtual_address));
			pat = strchr(virtual_address, '@') + 1;
			strcpy(pat, myrow1[0]);
			mysql_adaptor_encode_squote(virtual_address, temp_name);
			qstr = "UPDATE users SET password='"s + encrypt_passwd +
			       "' WHERE username='" + temp_name + "'";
			if (conn.res.query(qstr.c_str()) != 0)
				/* ignore - logmsg already emitted */;
		}
	}
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1703, e.what());
	return false;
}

BOOL mysql_adaptor_get_username_from_id(int user_id,
    char *username, size_t ulen) try
{
	auto qstr = "SELECT username FROM users WHERE id=" + std::to_string(user_id);
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	gx_strlcpy(username, myrow[0], ulen);
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1704, e.what());
	return false;
}

BOOL mysql_adaptor_get_id_from_username(const char *username, int *puser_id) try
{
	char temp_name[UADDR_SIZE*2];
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr = "SELECT id FROM users WHERE username='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	*puser_id = atoi(myrow[0]);
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1705, e.what());
	return false;
}

BOOL mysql_adaptor_get_id_from_maildir(const char *maildir, int *puser_id) try
{
	char temp_dir[512];
	
	mysql_adaptor_encode_squote(maildir, temp_dir);
	auto qstr =
		"SELECT u.id FROM users AS u " JOIN_WITH_DISPLAYTYPE
		" WHERE u.maildir='"s + temp_dir + "' AND dt.propval_str=0 LIMIT 2";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	*puser_id = atoi(myrow[0]);
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1706, e.what());
	return false;
}

BOOL mysql_adaptor_get_user_displayname(const char *username,
    char *pdisplayname) try
{
	char temp_name[UADDR_SIZE*2];
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr =
		"SELECT u2.propval_str AS real_name, "
		"u3.propval_str AS nickname, dt.propval_str AS dtypx FROM users AS u "
		JOIN_WITH_DISPLAYTYPE " "
		"LEFT JOIN user_properties AS u2 ON u.id=u2.user_id AND u2.proptag=805371935 " /* PR_DISPLAY_NAME */
		"LEFT JOIN user_properties AS u3 ON u.id=u3.user_id AND u3.proptag=978255903 " /* PR_NICKNAME */
		"WHERE u.username='"s + temp_name + "' LIMIT 2";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	auto dtypx = DT_MAILUSER;
	if (myrow[2] != nullptr)
		dtypx = static_cast<enum display_type>(strtoul(myrow[2], nullptr, 0));
	strcpy(pdisplayname,
	       dtypx == DT_DISTLIST ? username :
	       myrow[0] != nullptr && *myrow[0] != '\0' ? myrow[0] :
	       myrow[1] != nullptr && *myrow[1] != '\0' ? myrow[1] :
	       username);
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1707, e.what());
	return false;
}

BOOL mysql_adaptor_get_user_privilege_bits(const char *username,
    uint32_t *pprivilege_bits) try
{
	char temp_name[UADDR_SIZE*2];
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr = "SELECT privilege_bits FROM users WHERE username='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	*pprivilege_bits = atoi(myrow[0]);
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1708, e.what());
	return false;
}

BOOL mysql_adaptor_get_user_lang(const char *username, char *lang) try
{
	char temp_name[UADDR_SIZE*2];
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr = "SELECT lang FROM users WHERE username='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1) {
		lang[0] = '\0';	
	} else {
		auto myrow = pmyres.fetch_row();
		strcpy(lang, myrow[0]);
	}
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1709, e.what());
	return false;
}

BOOL mysql_adaptor_set_user_lang(const char *username, const char *lang) try
{
	char temp_name[UADDR_SIZE*2];
	std::string fq_string;
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr = "UPDATE users set lang='"s + lang +
		    "' WHERE username='" + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1710, e.what());
	return false;
}

static BOOL mysql_adaptor_expand_hierarchy(MYSQL *pmysql,
    std::vector<int> &seen, int class_id) try
{
	int child_id;
	auto qstr = "SELECT child_id FROM hierarchy WHERE class_id=" + std::to_string(class_id);
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();

	size_t i, rows = pmyres.num_rows();
	for (i = 0; i < rows; i++) {
		auto myrow = pmyres.fetch_row();
		child_id = atoi(myrow[0]);
		if (std::find(seen.cbegin(), seen.cend(), child_id) != seen.cend())
			continue;
		seen.push_back(child_id);
		if (!mysql_adaptor_expand_hierarchy(pmysql, seen, child_id))
			return FALSE;
	}
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1711, e.what());
	return false;
}

BOOL mysql_adaptor_get_timezone(const char *username, char *zone) try
{
	char temp_name[UADDR_SIZE*2];
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr = "SELECT timezone FROM users WHERE username='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1) {
		zone[0] = '\0';
	} else {
		auto myrow = pmyres.fetch_row();
		strcpy(zone, myrow[0]);
	}
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1712, e.what());
	return false;
}

BOOL mysql_adaptor_set_timezone(const char *username, const char *zone) try
{
	char temp_name[UADDR_SIZE*2];
	char temp_zone[128];
	
	mysql_adaptor_encode_squote(username, temp_name);
	mysql_adaptor_encode_squote(zone, temp_zone);
	auto qstr = "UPDATE users set timezone='"s + temp_zone +
	            "' WHERE username='" + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1713, e.what());
	return false;
}

BOOL mysql_adaptor_get_maildir(const char *username, char *maildir) try
{
	char temp_name[UADDR_SIZE*2];
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr = "SELECT maildir FROM users WHERE username='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	strncpy(maildir, myrow[0], 256);
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1714, e.what());
	return false;
}

BOOL mysql_adaptor_get_domainname_from_id(int domain_id, char *domainname) try
{
	auto qstr = "SELECT domainname FROM domains WHERE id=" + std::to_string(domain_id);
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	strncpy(domainname, myrow[0], 256);
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1715, e.what());
	return false;
}

BOOL mysql_adaptor_get_homedir(const char *domainname, char *homedir) try
{
	char temp_name[UDOM_SIZE*2];
	
	mysql_adaptor_encode_squote(domainname, temp_name);
	auto qstr = "SELECT homedir, domain_status FROM domains WHERE domainname='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	strncpy(homedir, myrow[0], 256);
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1716, e.what());
	return false;
}

BOOL mysql_adaptor_get_homedir_by_id(int domain_id, char *homedir) try
{
	auto qstr = "SELECT homedir FROM domains WHERE id=" + std::to_string(domain_id);
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	strncpy(homedir, myrow[0], 256);
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1717, e.what());
	return false;
}

BOOL mysql_adaptor_get_id_from_homedir(const char *homedir, int *pdomain_id) try
{
	char temp_dir[512];
	
	mysql_adaptor_encode_squote(homedir, temp_dir);
	auto qstr = "SELECT id FROM domains WHERE homedir='"s + temp_dir + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	*pdomain_id = atoi(myrow[0]);
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1718, e.what());
	return false;
}

BOOL mysql_adaptor_get_user_ids(const char *username, int *puser_id,
    int *pdomain_id, enum display_type *dtypx) try
{
	char temp_name[UADDR_SIZE*2];
	
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr =
		"SELECT u.id, u.domain_id, dt.propval_str AS dtypx"
		" FROM users AS u " JOIN_WITH_DISPLAYTYPE
		" WHERE u.username='"s + temp_name + "' LIMIT 2";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;	
	auto myrow = pmyres.fetch_row();
	*puser_id = atoi(myrow[0]);
	*pdomain_id = atoi(myrow[1]);
	if (dtypx != nullptr) {
		*dtypx = DT_MAILUSER;
		if (myrow[2] != nullptr)
			*dtypx = static_cast<enum display_type>(strtoul(myrow[2], nullptr, 0));
	}
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1719, e.what());
	return false;
}

BOOL mysql_adaptor_get_domain_ids(const char *domainname, int *pdomain_id,
    int *porg_id) try
{
	char temp_name[UDOM_SIZE*2];
	
	mysql_adaptor_encode_squote(domainname, temp_name);
	auto qstr = "SELECT id, org_id FROM domains WHERE domainname='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	*pdomain_id = atoi(myrow[0]);
	*porg_id = atoi(myrow[1]);
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1720, e.what());
	return false;
}

BOOL mysql_adaptor_get_mlist_ids(int user_id, int *pgroup_id,
    int *pdomain_id) try
{
	auto qstr = "SELECT dt.propval_str AS dtypx, u.domain_id, u.group_id "
	            "FROM users AS u " JOIN_WITH_DISPLAYTYPE
	            " WHERE id=" + std::to_string(user_id);
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	if (myrow == nullptr || myrow[0] == nullptr ||
	    static_cast<enum display_type>(strtoul(myrow[0], nullptr, 0)) != DT_DISTLIST)
		return FALSE;
	*pdomain_id = atoi(myrow[1]);
	*pgroup_id = atoi(myrow[2]);
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1721, e.what());
	return false;
}

BOOL mysql_adaptor_get_org_domains(int org_id, std::vector<int> &pfile) try
{
	auto qstr = "SELECT id FROM domains WHERE org_id=" + std::to_string(org_id);
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	size_t i, rows = pmyres.num_rows();
	pfile = std::vector<int>(rows);
	for (i=0; i<rows; i++) {
		auto myrow = pmyres.fetch_row();
		pfile[i] = strtoul(myrow[0], nullptr, 0);
	}
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1722, e.what());
	return false;
}

BOOL mysql_adaptor_get_domain_info(int domain_id, sql_domain &dinfo) try
{
	auto qstr = "SELECT domainname, title, address, homedir "
	            "FROM domains WHERE id=" + std::to_string(domain_id);
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	if (myrow == nullptr)
		return false;
	dinfo.name = myrow[0];
	dinfo.title = myrow[1];
	dinfo.address = myrow[2];
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1723, e.what());
	return false;
}

BOOL mysql_adaptor_check_same_org(int domain_id1, int domain_id2) try
{
	int org_id1;
	int org_id2;

	auto qstr = "SELECT org_id FROM domains WHERE id=" + std::to_string(domain_id1) +
	            " OR id=" + std::to_string(domain_id2);
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 2)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	org_id1 = atoi(myrow[0]);
	myrow = pmyres.fetch_row();
	org_id2 = atoi(myrow[0]);
	if (0 == org_id1 || 0 == org_id2 || org_id1 != org_id2) {
		return FALSE;
	}
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1724, e.what());
	return false;
}

BOOL mysql_adaptor_get_domain_groups(int domain_id,
    std::vector<sql_group> &pfile) try
{
	auto qstr = "SELECT id, groupname, title FROM groups "
	            "WHERE domain_id=" + std::to_string(domain_id);
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	size_t i, rows = pmyres.num_rows();
	std::vector<sql_group> gv(rows);
	for (i=0; i<rows; i++) {
		auto myrow = pmyres.fetch_row();
		gv[i].id = strtoul(myrow[0], nullptr, 0);
		gv[i].name = myrow[1];
		gv[i].title = myrow[2];
	}
	pfile = std::move(gv);
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1725, e.what());
	return false;
}

BOOL mysql_adaptor_get_group_classes(int group_id,
    std::vector<sql_class> &pfile) try
{
	auto qstr = "SELECT h.child_id, c.classname FROM hierarchy AS h "
	            "INNER JOIN classes AS c ON h.class_id=0 AND h.group_id=" +
	            std::to_string(group_id) + " AND h.child_id=c.id";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	size_t i, rows = pmyres.num_rows();
	std::vector<sql_class> cv(rows);
	for (i=0; i<rows; i++) {
		auto myrow = pmyres.fetch_row();
		cv[i].child_id = strtoul(myrow[0], nullptr, 0);
		cv[i].name = myrow[1];
	}
	pfile = std::move(cv);
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1726, e.what());
	return false;
}

BOOL mysql_adaptor_get_sub_classes(int class_id, std::vector<sql_class> &pfile) try
{
	auto qstr = "SELECT h.child_id, c.classname FROM hierarchy AS h"
	            " INNER JOIN classes AS c ON h.class_id=" + std::to_string(class_id) +
	            " AND h.child_id=c.id";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	size_t i, rows = pmyres.num_rows();
	std::vector<sql_class> cv(rows);
	for (i=0; i<rows; i++) {
		auto myrow = pmyres.fetch_row();
		cv[i].child_id = strtoul(myrow[0], nullptr, 0);
		cv[i].name = myrow[1];
	}
	pfile = std::move(cv);
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1727, e.what());
	return false;
}

static BOOL mysql_adaptor_hierarchy_include(sqlconn &conn,
    const char *account, int class_id) try
{
	int child_id;
	
	auto qstr = "SELECT username FROM members WHERE class_id="s +
	            std::to_string(class_id) + " AND username='" + account + "'";
	if (!conn.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.get());
	if (pmyres == nullptr)
		return FALSE;
	if (pmyres.num_rows() > 0)
		return TRUE;

	qstr = "SELECT child_id FROM hierarchy WHERE class_id=" + std::to_string(class_id);
	if (!conn.query(qstr.c_str()))
		return false;
	pmyres = mysql_store_result(conn.get());
	if (pmyres == nullptr)
		return FALSE;
	size_t i, rows = pmyres.num_rows();
	for (i=0; i<rows; i++) {
		auto myrow = pmyres.fetch_row();
		child_id = atoi(myrow[0]);
		if (mysql_adaptor_hierarchy_include(conn, account, child_id))
			return TRUE;
	}
	return FALSE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1728, e.what());
	return false;
}

BOOL mysql_adaptor_check_mlist_include(const char *mlist_name,
    const char *account) try
{
	int group_id;
	int class_id;
	int domain_id;
	BOOL b_result;
	int id, type;
	char temp_name[UADDR_SIZE*2];
	char *pencode_domain;
	char temp_account[512];
	
	if (NULL == strchr(mlist_name, '@')) {
		return FALSE;
	}
	
	mysql_adaptor_encode_squote(mlist_name, temp_name);
	pencode_domain = strchr(temp_name, '@') + 1;
	mysql_adaptor_encode_squote(account, temp_account);
	auto qstr = "SELECT id, list_type FROM mlists WHERE listname='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	if (pmyres.num_rows() != 1)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	
	id = atoi(myrow[0]);
	type = atoi(myrow[1]);
	
	b_result = FALSE;
	switch (type) {
	case MLIST_TYPE_NORMAL:
		qstr = "SELECT username FROM associations WHERE list_id=" +
		       std::to_string(id) + " AND username='"s + temp_account + "'";
		if (!conn.res.query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn.res.get());
		if (pmyres == nullptr)
			return false;
		if (pmyres.num_rows() > 0)
			b_result = TRUE;
		return b_result;
	case MLIST_TYPE_GROUP:
		qstr = "SELECT id FROM groups WHERE groupname='"s + temp_name + "'";
		if (!conn.res.query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn.res.get());
		if (pmyres == nullptr)
			return false;
		if (pmyres.num_rows() != 1)
			return FALSE;
		myrow = pmyres.fetch_row();
		group_id = atoi(myrow[0]);
		
		qstr = "SELECT username FROM users WHERE group_id=" + std::to_string(group_id) +
		       " AND username='" + temp_account + "'";
		if (!conn.res.query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn.res.get());
		if (pmyres == nullptr)
			return false;
		if (pmyres.num_rows() > 0)
			b_result = TRUE;
		return b_result;
	case MLIST_TYPE_DOMAIN:
		qstr = "SELECT id FROM domains WHERE domainname='"s + pencode_domain + "'";
		if (!conn.res.query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn.res.get());
		if (pmyres == nullptr)
			return false;
		if (pmyres.num_rows() != 1)
			return FALSE;
		myrow = pmyres.fetch_row();
		domain_id = atoi(myrow[0]);
		
		qstr = "SELECT username FROM users WHERE domain_id=" + std::to_string(domain_id) +
		       " AND username='" + temp_account + "'";
		if (!conn.res.query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn.res.get());
		if (pmyres == nullptr)
			return false;
		if (pmyres.num_rows() > 0)
			b_result = TRUE;
		return b_result;
	case MLIST_TYPE_CLASS:
		qstr = "SELECT id FROM classes WHERE listname='"s + temp_name + "'";
		if (!conn.res.query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn.res.get());
		if (pmyres == nullptr)
			return false;
		if (pmyres.num_rows() != 1)
			return FALSE;		
		myrow = pmyres.fetch_row();
		class_id = atoi(myrow[0]);
		b_result = mysql_adaptor_hierarchy_include(conn.res, temp_account, class_id);
		return b_result;
	default:
		return FALSE;
	}
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1729, e.what());
	return false;
}

static void mysql_adaptor_encode_squote(const char *in, char *out)
{
	int len, i, j;

	len = strlen(in);
	for (i=0, j=0; i<len; i++, j++) {
		if ('\'' == in[i] || '\\' == in[i]) {
			out[j] = '\\';
			j ++;
	}
		out[j] = in[i];
	}
	out[j] = '\0';
}

BOOL mysql_adaptor_check_same_org2(const char *domainname1,
    const char *domainname2) try
{
	int org_id1;
	int org_id2;
	char temp_name1[UDOM_SIZE*2], temp_name2[UDOM_SIZE*2];

	mysql_adaptor_encode_squote(domainname1, temp_name1);
	mysql_adaptor_encode_squote(domainname2, temp_name2);
	auto qstr = "SELECT org_id FROM domains WHERE domainname='"s + temp_name1 +
	            "' OR domainname='" + temp_name2 + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() != 2)
		return FALSE;
	auto myrow = pmyres.fetch_row();
	org_id1 = atoi(myrow[0]);
	myrow = pmyres.fetch_row();
	org_id2 = atoi(myrow[0]);
	if (0 == org_id1 || 0 == org_id2 || org_id1 != org_id2) {
		return FALSE;
	}
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1730, e.what());
	return false;
}

BOOL mysql_adaptor_check_user(const char *username, char *path) try
{
	char temp_name[UADDR_SIZE*2];

	if (path != nullptr)
		*path = '\0';
	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr =
		"SELECT DISTINCT u.address_status, u.maildir FROM users AS u "
		"LEFT JOIN aliases AS a ON u.username=a.mainname "
		"WHERE u.username='"s + temp_name + "' OR a.aliasname='" +
		temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();
	if (pmyres.num_rows() == 0) {
		return FALSE;
	} else if (pmyres.num_rows() > 1) {
		fprintf(stderr, "W-1510: userdb conflict: <%s> is in both \"users\" and \"aliases\"\n", username);
		return false;
	} else {
		auto myrow = pmyres.fetch_row();
		if (0 != atoi(myrow[0])) {
			if (NULL != path) {
				strcpy(path, myrow[1]);
			}
			return FALSE;
		} else {
			if (NULL != path) {
				strcpy(path, myrow[1]);
			}
			return TRUE;
		}
	}
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1731, e.what());
	return false;
}

BOOL mysql_adaptor_get_mlist(const char *username,  const char *from,
    int *presult, std::vector<std::string> &pfile) try
{
	int i, id, rows;
	int type, privilege;
	int group_id;
	int domain_id;
	BOOL b_chkintl;
	char *pencode_domain;
	char temp_name[UADDR_SIZE*2];

	*presult = MLIST_RESULT_NONE;
	const char *pdomain = strchr(username, '@');
	if (NULL == pdomain) {
		return TRUE;
	}

	pdomain++;
	const char *pfrom_domain = strchr(from, '@');
	if (NULL == pfrom_domain) {
		return TRUE;
	}

	pfrom_domain++;
	mysql_adaptor_encode_squote(username, temp_name);
	pencode_domain = strchr(temp_name, '@') + 1;

	auto qstr = "SELECT id, list_type, list_privilege FROM mlists "
	            "WHERE listname='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	if (pmyres.num_rows() != 1) {
		*presult = MLIST_RESULT_NONE;
		return TRUE;
	}
	auto myrow = pmyres.fetch_row();
	id = atoi(myrow[0]);
	type = atoi(myrow[1]);
	privilege = atoi(myrow[2]);

	switch (type) {
	case MLIST_TYPE_NORMAL:
		switch (privilege) {
		case MLIST_PRIVILEGE_ALL:
		case MLIST_PRIVILEGE_OUTGOING:
			b_chkintl = FALSE;
			break;
		case MLIST_PRIVILEGE_INTERNAL:
			b_chkintl = TRUE;
			break;
		case MLIST_PRIVILEGE_DOMAIN:
			if (0 != strcasecmp(pdomain, pfrom_domain)) {
				*presult = MLIST_RESULT_PRIVIL_DOMAIN;
				return TRUE;
			}
			b_chkintl = FALSE;
			break;
		case MLIST_PRIVILEGE_SPECIFIED:
			qstr = "SELECT username FROM specifieds WHERE list_id=" + std::to_string(id);
			if (!conn.res.query(qstr.c_str()))
				return false;
			pmyres = mysql_store_result(conn.res.get());
			if (pmyres == nullptr)
				return false;
			rows = pmyres.num_rows();
			for (i = 0; i < rows; i++) {
				myrow = pmyres.fetch_row();
				if (0 == strcasecmp(myrow[0], from) ||
					0 == strcasecmp(myrow[0], pfrom_domain)) {
					break;
				}
			}
			if (i == rows) {
				*presult = MLIST_RESULT_PRIVIL_SPECIFIED;
				return TRUE;
			}
			b_chkintl = FALSE;
			break;
		default:
			*presult = MLIST_RESULT_NONE;
			return TRUE;
		}
		qstr = "SELECT username FROM associations WHERE list_id=" + std::to_string(id);
		if (!conn.res.query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn.res.get());
		if (pmyres == nullptr)
			return false;
		rows = pmyres.num_rows();
		if (TRUE == b_chkintl) {
			for (i = 0; i < rows; i++) {
				myrow = pmyres.fetch_row();
				if (0 == strcasecmp(myrow[0], from)) {
					b_chkintl = FALSE;
					break;
				}
			}
		}

		if (TRUE == b_chkintl) {
			*presult = MLIST_RESULT_PRIVIL_INTERNAL;
			return TRUE;
		}
		mysql_data_seek(pmyres.get(), 0);
		for (i = 0; i < rows; i++) {
			myrow = pmyres.fetch_row();
			pfile.push_back(myrow[0]);
		}
		*presult = MLIST_RESULT_OK;
		return TRUE;
	case MLIST_TYPE_GROUP:
		switch (privilege) {
		case MLIST_PRIVILEGE_ALL:
		case MLIST_PRIVILEGE_OUTGOING:
			b_chkintl = FALSE;
			break;
		case MLIST_PRIVILEGE_INTERNAL:
			b_chkintl = TRUE;
			break;
		case MLIST_PRIVILEGE_DOMAIN:
			if (0 != strcasecmp(pdomain, pfrom_domain)) {
				*presult = MLIST_RESULT_PRIVIL_DOMAIN;
				return TRUE;
			}
			b_chkintl = FALSE;
			break;
		case MLIST_PRIVILEGE_SPECIFIED:
			qstr = "SELECT username FROM specifieds WHERE list_id=" + std::to_string(id);
			if (!conn.res.query(qstr.c_str()))
				return false;
			pmyres = mysql_store_result(conn.res.get());
			if (pmyres == nullptr)
				return false;
			rows = pmyres.num_rows();
			for (i = 0; i < rows; i++) {
				myrow = pmyres.fetch_row();
				if (0 == strcasecmp(myrow[0], from) ||
					0 == strcasecmp(myrow[0], pfrom_domain)) {
					break;
				}
			}
			if (i == rows) {
				*presult = MLIST_RESULT_PRIVIL_SPECIFIED;
				return TRUE;
			}
			b_chkintl = FALSE;
			break;
		default:
			*presult = MLIST_RESULT_NONE;
			return TRUE;
		}
		qstr = "SELECT id FROM groups WHERE groupname='"s + temp_name + "'";
		if (!conn.res.query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn.res.get());
		if (pmyres == nullptr)
			return false;
		if (pmyres.num_rows() != 1) {
			*presult = MLIST_RESULT_NONE;
			return TRUE;
		}
		myrow = pmyres.fetch_row();
		group_id = atoi(myrow[0]);
		qstr = "SELECT u.username, dt.propval_str AS dtypx FROM users AS u "
		       JOIN_WITH_DISPLAYTYPE " WHERE u.group_id=" + std::to_string(group_id);
		if (!conn.res.query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn.res.get());
		if (pmyres == nullptr)
			return false;
		rows = pmyres.num_rows();
		if (TRUE == b_chkintl) {
			for (i = 0; i < rows; i++) {
				myrow = pmyres.fetch_row();
				auto dtypx = DT_MAILUSER;
				if (myrow[1] != nullptr)
					dtypx = static_cast<enum display_type>(strtoul(myrow[1], nullptr, 0));
				if (dtypx == DT_MAILUSER && strcasecmp(myrow[0], from) == 0) {
					b_chkintl = FALSE;
					break;
				}
			}
		}

		if (TRUE == b_chkintl) {
			*presult = MLIST_RESULT_PRIVIL_INTERNAL;
			return TRUE;
		}
		mysql_data_seek(pmyres.get(), 0);
		for (i = 0; i < rows; i++) {
			myrow = pmyres.fetch_row();
			auto dtypx = DT_MAILUSER;
			if (myrow[1] != nullptr)
				dtypx = static_cast<enum display_type>(strtoul(myrow[1], nullptr, 0));
			if (dtypx == DT_MAILUSER)
				pfile.push_back(myrow[0]);
		}
		*presult = MLIST_RESULT_OK;
		return TRUE;

	case MLIST_TYPE_DOMAIN:
		switch (privilege) {
		case MLIST_PRIVILEGE_ALL:
		case MLIST_PRIVILEGE_OUTGOING:
			b_chkintl = FALSE;
			break;
		case MLIST_PRIVILEGE_INTERNAL:
			b_chkintl = TRUE;
			break;
		case MLIST_PRIVILEGE_DOMAIN:
			if (0 != strcasecmp(pdomain, pfrom_domain)) {
				*presult = MLIST_RESULT_PRIVIL_DOMAIN;
				return TRUE;
			}
			b_chkintl = FALSE;
			break;
		case MLIST_PRIVILEGE_SPECIFIED:
			qstr = "SELECT username FROM specifieds WHERE list_id=" + std::to_string(id);
			if (!conn.res.query(qstr.c_str()))
				return false;
			pmyres = mysql_store_result(conn.res.get());
			if (pmyres == nullptr)
				return false;
			rows = pmyres.num_rows();
			for (i = 0; i < rows; i++) {
				myrow = pmyres.fetch_row();
				if (0 == strcasecmp(myrow[0], from) ||
					0 == strcasecmp(myrow[0], pfrom_domain)) {
					break;
				}
			}
			if (i == rows) {
				*presult = MLIST_RESULT_PRIVIL_SPECIFIED;
				return TRUE;
			}
			b_chkintl = FALSE;
			break;
		default:
			*presult = MLIST_RESULT_NONE;
			return TRUE;
		}
		qstr = "SELECT id FROM domains WHERE domainname='"s + pencode_domain + "'";
		if (!conn.res.query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn.res.get());
		if (pmyres == nullptr)
			return false;
		if (pmyres.num_rows() != 1) {
			*presult = MLIST_RESULT_NONE;
			return TRUE;
		}
		myrow = pmyres.fetch_row();
		domain_id = atoi(myrow[0]);
		qstr = "SELECT u.username, dt.propval_str AS dtypx FROM users AS u "
		       JOIN_WITH_DISPLAYTYPE " WHERE u.domain_id=" + std::to_string(domain_id);
		if (!conn.res.query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn.res.get());
		if (pmyres == nullptr)
			return false;
		rows = pmyres.num_rows();
		if (TRUE == b_chkintl) {
			for (i = 0; i < rows; i++) {
				myrow = pmyres.fetch_row();
				auto dtypx = DT_MAILUSER;
				if (myrow[1] != nullptr)
					dtypx = static_cast<enum display_type>(strtoul(myrow[1], nullptr, 0));
				if (dtypx == DT_MAILUSER && strcasecmp(myrow[0], from) == 0) {
					b_chkintl = FALSE;
					break;
				}
			}
		}

		if (TRUE == b_chkintl) {
			*presult = MLIST_RESULT_PRIVIL_INTERNAL;
			return TRUE;
		}
		mysql_data_seek(pmyres.get(), 0);
		for (i = 0; i < rows; i++) {
			myrow = pmyres.fetch_row();
			auto dtypx = DT_MAILUSER;
			if (myrow[1] != nullptr)
				dtypx = static_cast<enum display_type>(strtoul(myrow[1], nullptr, 0));
			if (dtypx == DT_MAILUSER)
				pfile.push_back(myrow[0]);
		}
		*presult = MLIST_RESULT_OK;
		return TRUE;

	case MLIST_TYPE_CLASS: {
		switch (privilege) {
		case MLIST_PRIVILEGE_ALL:
		case MLIST_PRIVILEGE_OUTGOING:
			b_chkintl = FALSE;
			break;
		case MLIST_PRIVILEGE_INTERNAL:
			b_chkintl = TRUE;
			break;
		case MLIST_PRIVILEGE_DOMAIN:
			if (0 != strcasecmp(pdomain, pfrom_domain)) {
				*presult = MLIST_RESULT_PRIVIL_DOMAIN;
				return TRUE;
			}
			b_chkintl = FALSE;
			break;
		case MLIST_PRIVILEGE_SPECIFIED:
			qstr = "SELECT username FROM specifieds WHERE list_id=" + std::to_string(id);
			if (!conn.res.query(qstr.c_str()))
				return false;
			pmyres = mysql_store_result(conn.res.get());
			if (pmyres == nullptr)
				return false;
			rows = pmyres.num_rows();
			for (i = 0; i < rows; i++) {
				myrow = pmyres.fetch_row();
				if (0 == strcasecmp(myrow[0], from) ||
					0 == strcasecmp(myrow[0], pfrom_domain)) {
					break;
				}
			}
			if (i == rows) {
				*presult = MLIST_RESULT_PRIVIL_SPECIFIED;
				return TRUE;
			}
			b_chkintl = FALSE;
			break;
		default:
			*presult = MLIST_RESULT_NONE;
			return TRUE;
		}

		qstr = "SELECT id FROM classes WHERE listname='"s + temp_name + "'";
		if (!conn.res.query(qstr.c_str()))
			return false;
		pmyres = mysql_store_result(conn.res.get());
		if (pmyres == nullptr)
			return false;
		if (pmyres.num_rows() != 1) {
			*presult = MLIST_RESULT_NONE;
			return TRUE;
		}

		myrow = pmyres.fetch_row();
		int clsid = strtol(myrow[0], nullptr, 0);
		std::vector<int> file_temp{clsid};
		if (!mysql_adaptor_expand_hierarchy(conn.res.get(),
		    file_temp, clsid)) {
			*presult = MLIST_RESULT_NONE;
			return FALSE;
		}

		std::set<std::string, icasecmp> file_temp1;
		for (auto class_id : file_temp) {
			qstr = "SELECT username FROM members WHERE class_id=" + std::to_string(class_id);
			if (!conn.res.query(qstr.c_str()))
				return false;
			pmyres = mysql_store_result(conn.res.get());
			if (pmyres == nullptr)
				return false;
			rows = pmyres.num_rows();
			for (i = 0; i < rows; i++) {
				myrow = pmyres.fetch_row();
				file_temp1.emplace(myrow[0]);
			}
		}

		if (TRUE == b_chkintl)
			b_chkintl = file_temp1.find(from) == file_temp1.cend();
		if (TRUE == b_chkintl) {
			*presult = MLIST_RESULT_PRIVIL_INTERNAL;
			return TRUE;
		}
		for (auto &&t : file_temp1)
			pfile.push_back(std::move(t));
		*presult = MLIST_RESULT_OK;
		return TRUE;
	}
	default:
		*presult = MLIST_RESULT_NONE;
		return TRUE;
	}
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1732, e.what());
	return false;
}

BOOL mysql_adaptor_get_user_info(const char *username,
    char *maildir, char *lang, char *zone) try
{
	char temp_name[UADDR_SIZE*2];

	mysql_adaptor_encode_squote(username, temp_name);
	auto qstr = "SELECT maildir, address_status, lang, timezone "
	            "FROM users WHERE username='"s + temp_name + "'";
	auto conn = g_sqlconn_pool.get_wait();
	if (conn.res == nullptr)
		return false;
	if (!conn.res.query(qstr.c_str()))
		return false;
	DB_RESULT pmyres = mysql_store_result(conn.res.get());
	if (pmyres == nullptr)
		return false;
	conn.finish();

	if (pmyres.num_rows() != 1) {
		maildir[0] = '\0';
	} else {
		auto myrow = pmyres.fetch_row();
		if (0 == atoi(myrow[1])) {
			strcpy(maildir, myrow[0]);
			strcpy(lang, myrow[2]);
			strcpy(zone, myrow[3]);
		} else {
			maildir[0] = '\0';
		}
	}
	return TRUE;
} catch (const std::exception &e) {
	printf("E-%u: %s\n", 1733, e.what());
	return false;
}
