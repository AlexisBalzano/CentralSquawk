#pragma once

#include <algorithm>
#include <cctype>
#include <string>

/**
* @brief Return a copy of the input string converted to uppercase.
* @param str The input string to convert.
* @return A new string that is the uppercase version of the input string.
*/
inline std::string ToUpper(std::string str)
{
	std::string result = str;
	std::transform(result.begin(), result.end(), result.begin(), ::toupper);
	return result;
}

/**
* @brief Copy a string the SDK returned, treating a null pointer as empty.
* @param str The pointer returned by an EuroScope getter, possibly null.
* @return The string, or an empty one.
*
* Most CFlightPlanData getters can return null for a field the pilot never
* filed, so every one of them needs this on the way into a std::string.
*/
inline std::string SafeString(const char* str)
{
	return str == nullptr ? std::string{} : std::string(str);
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
