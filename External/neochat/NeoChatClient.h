// NeoChatClient.h -- optional, header-only helper around NeoChatApi.h.
//
// Generic version of CoFrance's NeoChat client. No link dependency: NeoChat.dll is looked up at
// runtime on every call, because it can be loaded after your plugin, or unloaded/reloaded at any
// time. Call from EuroScope's UI thread only.
//
// Requires <windows.h> and EuroScopePlugIn.h to be included before this header.

#pragma once

#include <string>
#include <vector>

#include "NeoChatApi.h"

namespace NeoChat
{
namespace detail
{
	// Resolved on every use, never cached: NeoChat may be unloaded at any time, which would
	// leave a cached pointer aiming at freed memory.
	template <typename Fn>
	inline Fn Resolve(const char* name, int minVersion = 1)
	{
		HMODULE neochat = GetModuleHandleA("NeoChat.dll");
		if (!neochat) return nullptr;
		auto version = reinterpret_cast<NeoChat_GetApiVersion_t>(GetProcAddress(neochat, "NeoChat_GetApiVersion"));
		if (!version || version() < minVersion) return nullptr;
		return reinterpret_cast<Fn>(GetProcAddress(neochat, name));
	}

	// EuroScope strings are in the ANSI code page; NeoChat's API takes UTF-8.
	inline std::string AnsiToUtf8(const char* text)
	{
		if (!text || !*text) return {};
		bool ascii = true;
		for (const char* c = text; *c; ++c) {
			if (static_cast<unsigned char>(*c) >= 0x80) { ascii = false; break; }
		}
		if (ascii) return text;

		const int wideLen = MultiByteToWideChar(CP_ACP, 0, text, -1, nullptr, 0);
		if (wideLen <= 0) return text;
		std::wstring wide(static_cast<size_t>(wideLen), L'\0');
		MultiByteToWideChar(CP_ACP, 0, text, -1, wide.data(), wideLen);

		const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
		if (utf8Len <= 0) return text;
		std::string utf8(static_cast<size_t>(utf8Len), '\0');
		WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), utf8Len, nullptr, nullptr);
		utf8.pop_back(); // terminating null written by the conversion
		return utf8;
	}
}

// True when NeoChat.dll is loaded and exposes at least `minVersion` of the API.
inline bool IsAvailable(int minVersion = 1)
{
	return detail::Resolve<NeoChat_GetApiVersion_t>("NeoChat_GetApiVersion", minVersion) != nullptr;
}

// Same parameters as CPlugIn::DisplayUserMessage. Sends the line straight to NeoChat when it is
// loaded and active, otherwise (or if NeoChat refuses it) to EuroScope through the SDK.
// Never both, so the line is never shown twice.
inline void DisplayUserMessage(EuroScopePlugIn::CPlugIn* plugin, const char* handler, const char* sender,
	const char* message, bool showHandler, bool showUnread, bool showUnreadEvenIfBusy,
	bool startFlashing, bool needConfirmation)
{
	if (auto add = detail::Resolve<NeoChat_AddMessage_t>("NeoChat_AddMessage")) {
		const std::string tab = detail::AnsiToUtf8(handler);
		const std::string from = detail::AnsiToUtf8(sender);
		const std::string text = detail::AnsiToUtf8(message);
		if (add(tab.c_str(), from.c_str(), text.c_str(), showUnread ? 1 : 0) == NEOCHAT_OK) {
			return;
		}
	}
	if (plugin) {
		plugin->DisplayUserMessage(handler, sender, message, showHandler, showUnread,
			showUnreadEvenIfBusy, startFlashing, needConfirmation);
	}
}

// Pre-fills NeoChat's input box (API version 2) for the user to review and send; nothing is sent.
// `tab`: nullptr/"" keeps the current tab, "@" the primary frequency, a callsign that private tab.
// Returns false when NeoChat is absent, too old or refuses it: use your EuroScope fallback then.
inline bool PreInput(const std::string& text, const char* tab = nullptr, bool focus = true, bool append = false)
{
	auto setInput = detail::Resolve<NeoChat_SetInputText_t>("NeoChat_SetInputText", 2);
	if (!setInput) return false;
	const std::string utf8Text = detail::AnsiToUtf8(text.c_str());
	const std::string utf8Tab = detail::AnsiToUtf8(tab);
	return setInput(utf8Text.c_str(), utf8Tab.c_str(),
		(focus ? NEOCHAT_INPUT_FOCUS : 0u) | (append ? NEOCHAT_INPUT_APPEND : 0u)) == NEOCHAT_OK;
}

// Registers your dot-commands for NeoChat's autocomplete / valid-command highlighting.
// Idempotent and cheap: call periodically (OnTimer) so it survives NeoChat loading late or reloading.
inline void RegisterCommands(const std::vector<const char*>& commands)
{
	auto reg = detail::Resolve<NeoChat_RegisterCommand_t>("NeoChat_RegisterCommand");
	if (!reg) return;
	for (const char* command : commands) reg(command);
}

// Removes your dot-commands from NeoChat, if it is still loaded. Call on plugin exit.
inline void UnregisterCommands(const std::vector<const char*>& commands)
{
	auto unreg = detail::Resolve<NeoChat_UnregisterCommand_t>("NeoChat_UnregisterCommand");
	if (!unreg) return;
	for (const char* command : commands) unreg(command);
}
}
