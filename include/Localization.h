#pragma once

#include <format>
#include <string>

// Localization: loads an optional JSON translation file of
// {"english source string": "中文翻译", ...} and serves lookups at render
// time. Missing file / missing key = English passthrough, so deleting the
// JSON restores the stock English UI. The DLL itself keeps English only.

namespace Localization
{
	// Load translations from a UTF-8 JSON object. Call once at startup,
	// before registering the settings page. Safe no-op when absent.
	void Load();

	// Returns the translation for a_key, or a_key itself when untranslated.
	// The pointer stays valid for the process lifetime (static cache).
	[[nodiscard]] const char* Translate(const char* a_key);
}

// Shorthand for wrapping UI literals: ImGui::Checkbox(_T("Followers go to bed"), ...)
#define _T(s) Localization::Translate(s)

// Format a string whose FORMAT STRING contains translatable text.
// std::format requires a compile-time format string and
// std::make_format_args refuses rvalue arguments, so both are
// bypassed here: the format string is looked up at runtime via _T,
// then each "{}" placeholder is replaced in order.
// Only "{}" placeholders are supported -- sufficient for this plugin.
// Use: _FMT("Right now: {} ({}).", LocationName(player), _T(label))
inline std::string FormatTr(const char* a_fmt, std::initializer_list<std::string> a_args)
{
	std::string out = Localization::Translate(a_fmt);
	for (const auto& arg : a_args) {
		const auto pos = out.find("{}");
		if (pos == std::string::npos) {
			break;
		}
		out.replace(pos, 2, arg);
	}
	return out;
}

#define _FMT(fmt, ...) FormatTr(fmt, {__VA_ARGS__})
