#pragma once

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
