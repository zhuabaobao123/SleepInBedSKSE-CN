// Localization: runtime string table for the Chinese UI translation.
// The JSON lives next to the DLL as SleepInBedSKSE.json (DLL同名,
// the plugin itself only reads SleepInBedSKSE.ini so the name is free).
// Delete the JSON to restore English; edit entries to re-skin the UI.

#include "Localization.h"

#include <SKSE/SKSE.h>

#include <fstream>
#include <mutex>
#include <string>
#include <unordered_map>

namespace
{
	constexpr auto kTranslationFile = "Data/SKSE/Plugins/SleepInBedSKSE.json";

	std::unordered_map<std::string, std::string> s_table;
	std::unordered_map<std::string, std::string> s_cache;
	std::mutex                                   s_mutex;
	bool                                         s_loaded = false;

	void StripUtf8Bom(std::string& a_text)
	{
		constexpr char kBom[] = { '\xEF', '\xBB', '\xBF' };
		if (a_text.size() >= 3 && a_text[0] == kBom[0] && a_text[1] == kBom[1] && a_text[2] == kBom[2]) {
			a_text.erase(0, 3);
		}
	}
}

namespace Localization
{
	void Load()
	{
		std::lock_guard<std::mutex> lock(s_mutex);
		if (s_loaded) {
			return;
		}
		s_loaded = true;
		s_table.clear();
		s_cache.clear();

		std::ifstream file(kTranslationFile, std::ios::binary);
		if (!file) {
			SKSE::log::info("[i18n] {} not found, using English UI", kTranslationFile);
			return;
		}
		std::string text((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
		StripUtf8Bom(text);

		rapidjson::Document doc;
		doc.Parse(text.c_str(), text.size());
		if (doc.HasParseError()) {
			SKSE::log::error("[i18n] {} parse error at offset {}: {}, using English UI", kTranslationFile,
				doc.GetErrorOffset(), rapidjson::GetParseError_En(doc.GetParseError()));
			return;
		}
		if (!doc.IsObject()) {
			SKSE::log::error("[i18n] {} root is not an object, using English UI", kTranslationFile);
			return;
		}
		for (auto it = doc.MemberBegin(); it != doc.MemberEnd(); ++it) {
			if (it->name.IsString() && it->value.IsString()) {
				s_table.emplace(it->name.GetString(), it->value.GetString());
			}
		}
		SKSE::log::info("[i18n] loaded {} translation entries from {}", s_table.size(), kTranslationFile);
	}

	const char* Translate(const char* a_key)
	{
		if (!a_key || *a_key == '\0') {
			return a_key;
		}
		std::lock_guard<std::mutex> lock(s_mutex);
		if (auto cached = s_cache.find(a_key); cached != s_cache.end()) {
			return cached->second.c_str();
		}
		std::string out;
		if (auto found = s_table.find(a_key); found != s_table.end()) {
			out = found->second;
		} else {
			out = a_key;
		}
		return s_cache.emplace(a_key, std::move(out)).first->second.c_str();
	}
}
