#pragma once

#include <algorithm>
#include <cctype>
#include <string>
#include <Windows.h>

/**
* @brief Return a copy of the input string with its ASCII letters uppercased.
* @param str The input string to convert.
* @return A new string that is the uppercase version of the input string.
*
* ASCII only, on purpose: the strings are UTF-8, and ::toupper on a byte of a
* multi-byte sequence would corrupt it (and a negative char is undefined).
*/
inline std::string ToUpper(std::string str)
{
	std::transform(str.begin(), str.end(), str.begin(), [](unsigned char c) {
		return static_cast<char>(c >= 'a' && c <= 'z' ? c - ('a' - 'A') : c);
	});
	return str;
}

/**
* @brief Copy a string the SDK returned as UTF-8, treating a null pointer as empty.
* @param str The pointer returned by an EuroScope getter, possibly null.
* @return The string in UTF-8, or an empty one.
*
* Most CFlightPlanData getters can return null for a field the pilot never
* filed, so every one of them needs this on the way into a std::string.
*
* EuroScope strings are in the ANSI code page, and a pilot's route or remarks
* can carry any byte. nlohmann::json throws on invalid UTF-8 when it is dumped,
* so everything is converted here, on the way in.
*/
inline std::string SafeString(const char* str)
{
	if (str == nullptr || *str == '\0') return {};

	bool ascii = true;
	for (const char* c = str; *c; ++c) {
		if (static_cast<unsigned char>(*c) >= 0x80) { ascii = false; break; }
	}
	if (ascii) return str;

	const int wideLen = MultiByteToWideChar(CP_ACP, 0, str, -1, nullptr, 0);
	if (wideLen <= 0) return {};
	std::wstring wide(static_cast<size_t>(wideLen), L'\0');
	MultiByteToWideChar(CP_ACP, 0, str, -1, wide.data(), wideLen);

	const int utf8Len = WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, nullptr, 0, nullptr, nullptr);
	if (utf8Len <= 0) return {};
	std::string utf8(static_cast<size_t>(utf8Len), '\0');
	WideCharToMultiByte(CP_UTF8, 0, wide.c_str(), -1, utf8.data(), utf8Len, nullptr, nullptr);
	utf8.pop_back(); // the terminating null the conversion wrote
	return utf8;
}

/**
* @brief Whether a string is a well formed SSR code: exactly four octal digits.
* @param code The candidate code.
* @return true when the string could be a transponder code.
*
* Used to tell a code the controller typed from a popup menu label, so menu
* sentinels can never be mistaken for input.
*/
inline bool IsWellFormedSquawk(const std::string& code)
{
	if (code.size() != 4) return false;
	return std::all_of(code.begin(), code.end(),
					   [](unsigned char c) { return c >= '0' && c <= '7'; });
}
