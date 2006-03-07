/*
 * Copyright (C) 2001  Dizzy
 * Copyright (C) 2004  Donny Redmond (dredmond@linuxmail.org)
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111-1307, USA.
 */
#include "common/setup_before.h"
#include "mail.h"

#include <sstream>
#include <fstream>
#include <iomanip>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <cerrno>

#include "compat/strcasecmp.h"
#include "compat/mkdir.h"
#include "compat/statmacros.h"
#include "common/eventlog.h"
#include "common/xalloc.h"
#include "account.h"
#include "message.h"
#include "prefs.h"
#include "connection.h"
#include "common/setup_after.h"


namespace pvpgn
{

namespace bnetd
{

static int identify_mail_function(const std::string&);
static void mail_usage(t_connection*);
static void mail_func_send(t_connection*, std::istream&);
static void mail_func_read(t_connection*, std::istream&);
static void mail_func_delete(t_connection*, std::istream&);
static unsigned get_mail_quota(t_account *);

/* Mail API */
/* for now this functions are only for internal use */

Mail::Mail(const std::string& sender, const std::string& message, const std::time_t timestamp)
:sender_(sender), message_(message), timestamp_(timestamp)
{
}

Mail::~Mail() throw()
{
}

const std::string&
Mail::sender() const
{
	return sender_;
}

const std::string&
Mail::message() const
{
	return message_;
}

const std::time_t&
Mail::timestamp() const
{
	return timestamp_;
}

Mailbox::Mailbox(unsigned uid_)
:uid(uid_), path(buildPath(prefs_get_maildir())), mdir(openDir())
{
}

std::string
Mailbox::buildPath(const std::string& root) const
{
	std::ostringstream ostr;
	ostr << root << "/" << std::setfill('0') << std::setw(6) << uid;
	return ostr.str();
}

std::auto_ptr<Directory>
Mailbox::openDir() const
{
	std::auto_ptr<Directory> dir;
	try {
		dir.reset(new Directory(path));
	} catch (const Directory::OpenError& ex) {
	}

	return dir;
}

std::auto_ptr<Directory>
Mailbox::createOpenDir()
{
	p_mkdir(prefs_get_maildir(), S_IRWXU | S_IRGRP | S_IXGRP | S_IROTH | S_IXOTH);
	p_mkdir(path.c_str(), S_IRWXU | S_IXGRP | S_IRGRP | S_IROTH | S_IXOTH);

	return openDir();
}

Mailbox::~Mailbox() throw()
{
}

unsigned
Mailbox::size() const
{
	/* consider NULL mdir as empty mailbox */
	if (!mdir.get()) return 0;

	mdir->rewind();

	char const * dentry;
	unsigned count = 0;
	while ((dentry=mdir->read())) if (dentry[0]!='.') count++;
	return count;
}

bool
Mailbox::empty() const
{
	/* consider NULL mdir as empty mailbox */
	if (!mdir.get()) return true;

	mdir->rewind();

	char const * dentry;
	while ((dentry=mdir->read())) if (dentry[0]!='.') return false;
	return true;
}

void
Mailbox::deliver(const std::string& sender, const std::string& mess)
{
	if (!mdir.get()) {
		mdir = createOpenDir();
		if (!mdir.get()) {
			ERROR0("could not write into directory");
			throw DeliverError("could not write into directory");
		}
	}

	std::ostringstream ostr;
	ostr << path << '/' << std::setfill('0') << std::setw(15) << static_cast<unsigned long>(std::time(0));

	std::ofstream fd(ostr.str().c_str());
	if (!fd) {
		ERROR1("error opening mail file. check permissions: '%s'", path.c_str());
		throw DeliverError("error opening mail file. check permissions: " + path);
	}

	fd << sender << std::endl << mess << std::endl;
}

Mail
Mailbox::read(const std::string& fname, const std::time_t& timestamp) const
{
	std::ifstream fd(fname.c_str());
	if (!fd) {
		ERROR1("error opening mail file: %s", fname.c_str());
		throw ReadError("error opening mail file: " + fname);
	}

	std::string sender;
	std::getline(fd, sender);

	std::string message;
	std::getline(fd, message);

	return Mail(sender, message, timestamp);
}

Mail
Mailbox::read(unsigned int idx) const
{
	if (!mdir.get()) {
		INFO0("mail not found");
		throw ReadError("mail not found");
	}

	mdir->rewind();
	const char * dentry = mdir->read();
	for(unsigned i = 0; i < idx && (dentry = mdir->read());)
		if (dentry[0] != '.') ++i;
	if (!dentry) {
		INFO0("mail not found");
		throw ReadError("mail not found");
	}

	std::string fname(path);
	fname += '/';
	fname += dentry;

	return read(fname, std::atoi(dentry));
}

void
Mailbox::readAll(MailList& dest) const
{
	if (!mdir.get()) {
		/* no maildir, so emulate like empty maildir */
		return;
	}

	mdir->rewind();

	std::string fname(path);
	fname += '/';

	const char* dentry;
	while((dentry = mdir->read())) {
		if (dentry[0] == '.') continue;

		try {
			dest.push_back(read(fname + dentry, std::atoi(dentry)));
		} catch (const ReadError&) {
			/* ignore ReadError in reading a specific message and try to read as much as we can */
		}
	}
}

void
Mailbox::erase(unsigned int idx)
{
	if (!mdir.get()) {
		WARN0("index out of range");
		return;
	}

	mdir->rewind();
	const char* dentry = mdir->read();
	for(unsigned i = 0; i < idx && (dentry = mdir->read());)
		if (dentry[0] != '.') ++i;

	if (!dentry) {
		WARN0("index out of range");
		return;
	}

	std::string fname(path);
	fname += '/';
	fname += dentry;

	if (std::remove(fname.c_str()) < 0)
		INFO2("could not std::remove file \"%s\" (std::remove: %s)", fname.c_str(), std::strerror(errno));
}

void
Mailbox::clear()
{
	if (!mdir.get()) {
		/* nothing to clear */
		return;
	}

	std::string fname(path);
	fname += '/';

	mdir->rewind();

	const char* dentry;
	while((dentry = mdir->read())) {
		std::remove((fname + dentry).c_str());
	}
}

extern int handle_mail_command(t_connection * c, char const * text)
{
	if (!prefs_get_mail_support()) {
		message_send_text(c,message_type_error,c,"This server has NO mail support.");
		return -1;
	}

	std::istringstream istr(text);
	std::string token;

	/* stkip "/mail" */
	istr >> token;

	/* get the mail function */
	token.clear();
	istr >> token;

	switch (identify_mail_function(token.c_str())) {
	case MAIL_FUNC_SEND:
		mail_func_send(c, istr);
		break;
	case MAIL_FUNC_READ:
		mail_func_read(c, istr);
		break;
	case MAIL_FUNC_DELETE:
		mail_func_delete(c, istr);
		break;
	case MAIL_FUNC_HELP:
		message_send_text(c, message_type_info, c, "The mail command supports the following patterns.");
		mail_usage(c);
		break;
	default:
		message_send_text(c, message_type_error, c, "The command its incorrect. Use one of the following patterns.");
		mail_usage(c);
	}

	return 0;
}

static int identify_mail_function(const std::string& funcstr)
{
	if (funcstr.empty() || !strcasecmp(funcstr.c_str(), "read") || !strcasecmp(funcstr.c_str(), "r"))
		return MAIL_FUNC_READ;
	if (!strcasecmp(funcstr.c_str(), "send") || !strcasecmp(funcstr.c_str(), "s"))
		return MAIL_FUNC_SEND;
	if (!strcasecmp(funcstr.c_str(), "delete") || !strcasecmp(funcstr.c_str(), "del"))
		return MAIL_FUNC_DELETE;
	if (!strcasecmp(funcstr.c_str(), "help") || !strcasecmp(funcstr.c_str(), "h"))
		return MAIL_FUNC_HELP;

	return MAIL_FUNC_UNKNOWN;
}

static unsigned get_mail_quota(t_account * user)
{
	int quota;
	char const * user_quota = account_get_strattr(user,"BNET\\auth\\mailquota");

	if (!user_quota) quota = prefs_get_mail_quota();
	else {
		quota = std::atoi(user_quota);
		if (quota < 1) quota=1;
		if (quota > MAX_MAIL_QUOTA) quota = MAX_MAIL_QUOTA;
	}

	return quota;
}

static void mail_func_send(t_connection * c, std::istream& istr)
{
	if (!c) {
		ERROR0("got NULL connection");
		return;
	}

	std::string dest;
	istr >> dest;
	if (dest.empty()) {
		message_send_text(c,message_type_error,c,"You must specify the receiver");
		message_send_text(c,message_type_error,c,"Syntax: /mail send <receiver> <message>");
		return;
	}

	std::string message;
	std::getline(istr, message);
	std::string::size_type pos(message.find_first_not_of(" \t"));
	if (pos == std::string::npos) {
		message_send_text(c,message_type_error,c,"Your message is empty!");
		message_send_text(c,message_type_error,c,"Syntax: /mail send <receiver> <message>");
		return;
	}

	t_account * recv = accountlist_find_account(dest.c_str());
	if (!recv) {
		message_send_text(c,message_type_error,c,"Receiver UNKNOWN!");
		return;
	}

	Mailbox mbox(account_get_uid(recv));
	if (get_mail_quota(recv) <= mbox.size()) {
		message_send_text(c,message_type_error,c,"Receiver has reached his mail quota. Your message will NOT be sent.");
		return;
	}

	try {
		mbox.deliver(conn_get_username(c), message.substr(pos));
		message_send_text(c, message_type_info, c, "Your mail has been sent successfully.");
	} catch (const Mailbox::DeliverError&) {
		message_send_text(c,message_type_error,c,"There was an error completing your request!");
	}
}

bool NonNumericChar(const char ch)
{
	if (ch < '0' || ch > '9') return true;
	return false;
}

static void mail_func_read(t_connection * c, std::istream& istr)
{
	if (!c) {
		ERROR0("got NULL connection");
		return;
	}

	std::string token;
	istr >> token;

	t_account * user = conn_get_account(c);
	Mailbox mbox(account_get_uid(user));

	if (token.empty()) { /* user wants to see the mail summary */
		if (mbox.empty()) {
			message_send_text(c,message_type_info,c,"You have no mail.");
			return;
		}

		MailList mlist;
		mbox.readAll(mlist);

		std::ostringstream ostr;
		ostr << "You have " << mbox.size() << " messages. Your mail quote is set to " << get_mail_quota(user) << '.';
		message_send_text(c, message_type_info, c, ostr.str().c_str());
		message_send_text(c, message_type_info, c, "ID    Sender          Date");
		message_send_text(c, message_type_info, c, "-------------------------------------");

		for(MailList::const_iterator it(mlist.begin()); it != mlist.end(); ++it) {
			ostr.str("");
			ostr << std::setfill('0') << std::setw(2) << std::right << (it - mlist.begin()) << "    "
			     << std::setfill(' ') << std::setw(14) << std::left << it->sender() << ' ';
			char buff[128];
			std::strftime(buff, sizeof(buff), "%a %b %d %H:%M:%S %Y", std::localtime(&it->timestamp()));
			ostr << buff;
			message_send_text(c, message_type_info, c, ostr.str().c_str());
		}

		message_send_text(c,message_type_info,c,"Use /mail read <ID> to read the content of any message");
	} else { /* user wants to read a message */
		if (std::find_if(token.begin(), token.end(), NonNumericChar) != token.end()) {
			message_send_text(c,message_type_error,c,"Invalid index. Please use /mail read <index> where <index> is a number.");
			return;
		}

		try {
			unsigned idx = std::atoi(token.c_str());
			Mail mail(mbox.read(idx));

			std::ostringstream ostr;
			ostr << "Message #" << idx << " from " << mail.sender() << " on ";
			char buff[128];
			std::strftime(buff, sizeof(buff), "%a %b %d %H:%M:%S %Y", std::localtime(&mail.timestamp()));
			ostr << buff << ':';
			message_send_text(c, message_type_info, c, ostr.str().c_str());
			message_send_text(c, message_type_info, c, mail.message().c_str());
		} catch (const Mailbox::ReadError&) {
			message_send_text(c,message_type_error,c,"There was an error completing your request.");
		}
	}
}

static void mail_func_delete(t_connection * c, std::istream& istr)
{
	if (!c) {
		ERROR0("got NULL connection");
		return;
	}

	std::string token;
	istr >> token;

	if (token.empty()) {
		message_send_text(c,message_type_error,c,"Please specify which message to delete. Use the following syntax: /mail delete {<index>|all} .");
		return;
	}

	t_account * user = conn_get_account(c);
	Mailbox mbox(account_get_uid(user));

	if (token == "all") {
		mbox.clear();
		message_send_text(c, message_type_info, c, "Successfully deleted messages.");
	} else {
		if (std::find_if(token.begin(), token.end(), NonNumericChar) != token.end()) {
			message_send_text(c,message_type_error,c,"Invalid index. Please use /mail delete {<index>|all} where <index> is a number.");
			return;
		}

		mbox.erase(std::atoi(token.c_str()));
		message_send_text(c,message_type_info,c, "Succesfully deleted message.");
	}
}

static void mail_usage(t_connection * c)
{
	message_send_text(c,message_type_info,c,"to print this information:");
	message_send_text(c,message_type_info,c,"    /mail help");
	message_send_text(c,message_type_info,c,"to print an index of you messages:");
	message_send_text(c,message_type_info,c,"    /mail [read]");
	message_send_text(c,message_type_info,c,"to send a message:");
	message_send_text(c,message_type_info,c,"    /mail send <receiver> <message>");
	message_send_text(c,message_type_info,c,"to read a message:");
	message_send_text(c,message_type_info,c,"    /mail read <index num>");
	message_send_text(c,message_type_info,c,"to delete a message:");
	message_send_text(c,message_type_info,c,"    /mail delete {<index>|all}");
	message_send_text(c,message_type_info,c,"Commands may be abbreviated as follows:");
	message_send_text(c,message_type_info,c,"    help: h");
	message_send_text(c,message_type_info,c,"    read: r");
	message_send_text(c,message_type_info,c,"    send: s");
	message_send_text(c,message_type_info,c,"    delete: del");
}

extern unsigned check_mail(t_connection const * c)
{
	if (!c) {
		ERROR0("got NULL connection");
		return 0;
	}

	return Mailbox(account_get_uid(conn_get_account(c))).size();
}

}

}
