/*
 * InspIRCd -- Internet Relay Chat Daemon
 *
 *   Copyright (C) 2007 Dennis Friis <peavey@inspircd.org>
 *   Copyright (C) 2007 Robin Burchell <robin+git@viroteck.net>
 *   Copyright (C) 2006 Craig Edwards <craigedwards@brainbox.cc>
 *
 * This file is part of InspIRCd.  InspIRCd is free software: you can
 * redistribute it and/or modify it under the terms of the GNU General Public
 * License as published by the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or FITNESS
 * FOR A PARTICULAR PURPOSE.  See the GNU General Public License for more
 * details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 */


#include "inspircd.h"

/** Handles user mode +H
 */
class HideOper : public SimpleUserModeHandler
{
 public:
	size_t opercount;

	HideOper(Module* Creator) : SimpleUserModeHandler(Creator, "hideoper", 'H')
		, opercount(0)
	{
		oper = true;
	}

	ModeAction OnModeChange(User* source, User* dest, Channel* channel, std::string& parameter, bool adding)
	{
		if (SimpleUserModeHandler::OnModeChange(source, dest, channel, parameter, adding) == MODEACTION_DENY)
			return MODEACTION_DENY;

		if (adding)
			opercount++;
		else
			opercount--;

		return MODEACTION_ALLOW;
	}
};

class ModuleHideOper : public Module, public Whois::LineEventListener
{
	HideOper hm;
	bool active;
 public:
	ModuleHideOper()
		: Whois::LineEventListener(this)
		, hm(this)
		, active(false)
	{
	}

	Version GetVersion() CXX11_OVERRIDE
	{
		return Version("Provides support for hiding oper status with user mode +H", VF_VENDOR);
	}

	void OnUserQuit(User* user, const std::string&, const std::string&) CXX11_OVERRIDE
	{
		if (user->IsModeSet(hm))
			hm.opercount--;
	}

	ModResult OnNumeric(User* user, const Numeric::Numeric& numeric) CXX11_OVERRIDE
	{
		if (numeric.GetNumeric() != 252 || active || user->HasPrivPermission("users/auspex"))
			return MOD_RES_PASSTHRU;

		// If there are no visible operators then we shouldn't send the numeric.
		size_t opercount = ServerInstance->Users->all_opers.size() - hm.opercount;
		if (opercount)
		{
			active = true;
			user->WriteNumeric(252, opercount, "operator(s) online");
			active = false;
		}
		return MOD_RES_DENY;
	}

	ModResult OnWhoisLine(Whois::Context& whois, Numeric::Numeric& numeric) CXX11_OVERRIDE
	{
		/* Dont display numeric 313 (RPL_WHOISOPER) if they have +H set and the
		 * person doing the WHOIS is not an oper
		 */
		if (numeric.GetNumeric() != 313)
			return MOD_RES_PASSTHRU;

		if (!whois.GetTarget()->IsModeSet(hm))
			return MOD_RES_PASSTHRU;

		if (!whois.GetSource()->HasPrivPermission("users/auspex"))
			return MOD_RES_DENY;

		return MOD_RES_PASSTHRU;
	}

	void OnSendWhoLine(User* source, const std::vector<std::string>& params, User* user, Membership* memb, std::string& line) CXX11_OVERRIDE
	{
		if (user->IsModeSet(hm) && !source->HasPrivPermission("users/auspex"))
		{
			// hide the "*" that marks the user as an oper from the /WHO line
			std::string::size_type spcolon = line.find(" :");
			if (spcolon == std::string::npos)
				return; // Another module hid the user completely
			std::string::size_type sp = line.rfind(' ', spcolon-1);
			std::string::size_type pos = line.find('*', sp);
			if (pos != std::string::npos)
				line.erase(pos, 1);
			// hide the line completely if doing a "/who * o" query
			if (params.size() > 1 && params[1].find('o') != std::string::npos)
				line.clear();
		}
	}

	ModResult OnStats(Stats::Context& stats) CXX11_OVERRIDE
	{
		if (stats.GetSymbol() != 'P')
			return MOD_RES_PASSTHRU;

		unsigned int count = 0;
		const UserManager::OperList& opers = ServerInstance->Users->all_opers;
		for (UserManager::OperList::const_iterator i = opers.begin(); i != opers.end(); ++i)
		{
			User* oper = *i;
			if (!oper->server->IsULine() && (stats.GetSource()->IsOper() || !oper->IsModeSet(hm)))
			{
				LocalUser* lu = IS_LOCAL(oper);
				stats.AddRow(249, oper->nick + " (" + oper->ident + "@" + oper->dhost + ") Idle: " +
						(lu ? ConvToStr(ServerInstance->Time() - lu->idle_lastmsg) + " secs" : "unavailable"));
				count++;
			}
		}
		stats.AddRow(249, ConvToStr(count)+" OPER(s)");

		return MOD_RES_DENY;
	}
};

MODULE_INIT(ModuleHideOper)
