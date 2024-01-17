// SPDX-License-Identifier: GPL-2.0-only

#include "muika/modules/jqftu/entry.hpp"
#include "muika/modules/jqftu/Session.hpp"
#include "muika/modules/jqftu/Command.hpp"

namespace muika {
namespace modules {
namespace jqftu {

static inline bool is_space_or_null(char c)
{
	return c == ' ' || c == '\t' || c == '\n' || c == '\0';
}

module_ret_t entry(muika::Muika &m, TgBot::Message::Ptr &msg)
{
	Session *sess = Session::getSession(msg->chat->id);
	const char *txt;

	if (sess) {
		sess->answer(msg);
		Session::putSession(sess);
		return MOD_ENTRY_CONTINUE;
	}

	if (msg->text.length() < 6)
		return MOD_ENTRY_CONTINUE;

	/*
	 * The command must start with '/' or '!' or '.'.
	 */
	txt = msg->text.c_str();
	if (txt[0] != '/' && txt[0] != '!' && txt[0] != '.')
		return MOD_ENTRY_CONTINUE;

	if (memcmp(&txt[1], "jqftu", 5) != 0 || !is_space_or_null(txt[6]))
		return MOD_ENTRY_CONTINUE;

	Command c(m, msg);
	c.execute();
	return MOD_ENTRY_STOP;
}

} /* namespace muika::modules::jqftu */
} /* namespace muika::modules */
} /* namespace muika */
