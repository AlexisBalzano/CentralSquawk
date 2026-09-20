#pragma once
#include <algorithm>
#include <cctype>
#include <sstream>
#include <string>
#include "CentralSquawk.h"

namespace centralSquawk {

inline bool CentralSquawk::OnCompileCommand(const char* sCommandLine)
{
	if (sCommandLine == nullptr) return false;

	std::string line(sCommandLine);

	auto trim = [](std::string& s) {
		const auto notSpace = [](int ch) { return !std::isspace(ch); };
		s.erase(s.begin(), std::find_if(s.begin(), s.end(), notSpace));
		s.erase(std::find_if(s.rbegin(), s.rend(), notSpace).base(), s.end());
	};
	trim(line);
	if (line.empty()) return false;

	// Remove optional leading '.' (Euroscope command convention)
	if (!line.empty() && line[0] == '.') line.erase(0, 1);

	std::istringstream iss(line);
	std::string cmd;
	iss >> cmd;
	if (cmd.empty()) return false;

	auto toLower = [](std::string s) {
		std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		return s;
	};
	const std::string lcmd = toLower(cmd);

	if (lcmd != "centralsquawk")
		return false;

	std::string sub;
	iss >> sub;
	sub = toLower(sub);

	if (sub == "version")
	{
		DisplayMessage(std::string("Central Squawk version: ") + PLUGIN_VERSION);
		return true;
	}

	if (sub == "status")
	{
		// Which world the plugin thinks it is in, which server that points at,
		// and WHY it thinks so. Worth spelling out: a simulator session that
		// silently talks to the live server looks exactly like one that works,
		// and the reason is the part a controller can act on.
		const NetworkMode mode = ResolveMode();
		const auto name = [](NetworkMode m) {
			return m == NetworkMode::Sweatbox ? "simulator"
				 : m == NetworkMode::Live     ? "live"
											  : "offline";
		};

		DisplayMessage(std::string("Mode: ") + name(mode) +
					   (mode == NetworkMode::Offline ? "" : "   server: " + EndpointFor(mode)));

		const ModeOverride override = modeOverride_.load(std::memory_order_acquire);
		if (override != ModeOverride::Auto) {
			DisplayMessage(std::string("Reason: forced with .centralsquawk mode ") +
						   (override == ModeOverride::ForceSim ? "sim" : "live"));
		}
		else if (connectionMode_.load(std::memory_order_acquire) == NetworkMode::Sweatbox) {
			DisplayMessage("Reason: EuroScope reports a simulator connection.");
		}
		else if (inferredSim_.load(std::memory_order_acquire)) {
			DisplayMessage("Reason: the live server does not show this callsign logged on, "
						   "so this is a training connection.");
		}
		else if (mode == NetworkMode::Live) {
			DisplayMessage("Reason: the live server shows this callsign logged on to VATSIM.");
		}

		if (mode == NetworkMode::Sweatbox)
		{
			DisplayMessage(std::string("Simulator feed: ") +
						   (feeding_.load(std::memory_order_acquire) ? "this client is feeding the session picture"
									 : "another client is feeding, or no push has succeeded yet"));
		}
		return true;
	}

	if (sub == "mode")
	{
		std::string value;
		iss >> value;
		value = toLower(value);

		// An escape hatch, not a safety mechanism. The live server refuses a
		// callsign it cannot see whichever way this is set, so the worst a
		// wrong setting can do is cost codes.
		if (value == "auto")      modeOverride_.store(ModeOverride::Auto, std::memory_order_release);
		else if (value == "live") modeOverride_.store(ModeOverride::ForceLive, std::memory_order_release);
		else if (value == "sim" || value == "sweatbox")
								  modeOverride_.store(ModeOverride::ForceSim, std::memory_order_release);
		else {
			DisplayMessage("Usage: .centralsquawk mode auto|live|sim");
			return true;
		}

		DisplayMessage("Mode set to " + value + ". Run .centralsquawk status to see what took effect.");
		return true;
	}

	DisplayMessage("Commands: .centralsquawk version | status | mode auto|live|sim");
	return true;
}

} // namespace rampAgent