#include "Settings.h"

#include <SimpleIni.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <filesystem>
#include <format>
#include <string_view>

namespace
{
	constexpr std::int32_t kMaxKeyCode = 281;
	constexpr float        kMaxHour = 23.99f;

	constexpr std::int32_t kLegacyModeEverything = 1;
	constexpr std::int32_t kLegacyModeExceptTorso = 2;
	constexpr std::int32_t kLegacyModeSleepwear = 3;
	constexpr auto         kLegacyOutfitName = "Sleepwear";

	Settings              g_settings;
	std::filesystem::path g_loadedFrom;

	std::filesystem::path SettingsPath()
	{
		const auto name = SKSE::PluginDeclaration::GetSingleton()->GetName();
		return std::filesystem::path("Data") / "SKSE" / "Plugins" / std::format("{}.ini", name);
	}

	std::int32_t ReadKey(const CSimpleIniA& ini, const char* section, const char* key, std::int32_t fallback)
	{
		return static_cast<std::int32_t>(ini.GetLongValue(section, key, fallback));
	}

	float ReadHour(const CSimpleIniA& ini, const char* section, const char* key, float fallback)
	{
		const auto hour = static_cast<float>(ini.GetDoubleValue(section, key, fallback));
		return hour >= 0.0f && hour < 24.0f ? hour : fallback;
	}

	void WriteBool(CSimpleIniA& ini, const char* section, const char* key, bool value)
	{
		ini.SetLongValue(section, key, value ? 1 : 0);
	}

	void WriteFloat(CSimpleIniA& ini, const char* section, const char* key, float value)
	{
		ini.SetValue(section, key, std::format("{}", value).c_str());
	}

	std::string LegacyChoice(std::int32_t mode)
	{
		switch (mode) {
		case kLegacyModeEverything:
		case kLegacyModeExceptTorso:
			return std::string(Bedtime::kUndress);
		case kLegacyModeSleepwear:
			return kLegacyOutfitName;
		default:
			return std::string(Bedtime::kStay);
		}
	}

	std::string NormalizeChoice(std::string_view value, const std::vector<Settings::Outfit>& outfits)
	{
		const auto first = value.find_first_not_of(' ');
		value = first == std::string_view::npos ? std::string_view{} : value.substr(first, value.find_last_not_of(' ') - first + 1);
		if (Bedtime::SameName(value, Bedtime::kUndress)) {
			return std::string(Bedtime::kUndress);
		}
		for (const auto& outfit : outfits) {
			if (Bedtime::SameName(outfit.name, value)) {
				return outfit.name;
			}
		}
		return std::string(Bedtime::kStay);
	}

	void Sanitize(Settings& s)
	{
		const Settings defaults;
		s.sleepKey = std::clamp(s.sleepKey, -1, kMaxKeyCode);
		if (s.followerSearchRadius <= 0.0f) {
			s.followerSearchRadius = defaults.followerSearchRadius;
		}
		s.followerNightStart = std::clamp(s.followerNightStart, 0.0f, kMaxHour);
		s.followerNightEnd = std::clamp(s.followerNightEnd, 0.0f, kMaxHour);

		std::vector<Settings::Outfit> outfits;
		for (auto& outfit : s.outfits) {
			auto name = Bedtime::CleanOutfitName(outfit.name);
			const bool taken = std::any_of(outfits.begin(), outfits.end(), [&](const Settings::Outfit& kept) { return Bedtime::SameName(kept.name, name); });
			if (!name.empty() && !taken) {
				outfits.push_back({ std::move(name), std::move(outfit.items) });
			}
		}
		s.outfits = std::move(outfits);
		for (auto* column : { &s.bedtimePlayer, &s.bedtimeFollowers }) {
			Bedtime::ForEachCell(*column, [&](std::string& choice) { choice = NormalizeChoice(choice, s.outfits); });
		}
	}
}

const Settings& Settings::Get()
{
	return g_settings;
}

Settings& Settings::Mutable()
{
	return g_settings;
}

void Settings::Load()
{
	CSimpleIniA ini;
	ini.SetUnicode();

	const auto path = SettingsPath();
	if (ini.LoadFile(path.string().c_str()) < 0) {
		return;
	}
	g_loadedFrom = path;

	Settings loaded;
	loaded.sleepKey = ReadKey(ini, "Input", "iSleepKey", loaded.sleepKey);

	loaded.allowOwnedBeds = ini.GetBoolValue("Bed", "bAllowOwnedBeds", loaded.allowOwnedBeds);
	loaded.openSleepMenuOnLieDown = ini.GetBoolValue("Bed", "bOpenSleepMenuOnLieDown", loaded.openSleepMenuOnLieDown);

	loaded.followersEnabled = ini.GetBoolValue("Followers", "bEnabled", loaded.followersEnabled);
	loaded.spouseSharesBed = ini.GetBoolValue("Followers", "bSpouseSharesBed", loaded.spouseSharesBed);
	loaded.homeSpouseSharesBed = ini.GetBoolValue("Followers", "bHomeSpouseSharesBed", loaded.homeSpouseSharesBed);
	loaded.followersShareBed = ini.GetBoolValue("Followers", "bFollowersShareBed", loaded.followersShareBed);
	loaded.followersIgnoreOwnership = ini.GetBoolValue("Followers", "bIgnoreOwnership", loaded.followersIgnoreOwnership);
	const auto radius = static_cast<float>(ini.GetDoubleValue("Followers", "fSearchRadius", loaded.followerSearchRadius));
	if (radius > 0.0f) {
		loaded.followerSearchRadius = radius;
	}
	loaded.followersNightOnly = ini.GetBoolValue("Followers", "bNightOnly", loaded.followersNightOnly);
	loaded.followerNightStart = ReadHour(ini, "Followers", "fNightStart", loaded.followerNightStart);
	loaded.followerNightEnd = ReadHour(ini, "Followers", "fNightEnd", loaded.followerNightEnd);

	loaded.keepBodyPiece = ini.GetBoolValue("Undress", "bKeepBodyPiece", loaded.keepBodyPiece);
	loaded.undressAnimations = ini.GetBoolValue("Undress", "bAnimations", loaded.undressAnimations);

	if (ini.GetSectionSize("Bedtime") >= 0) {
		for (std::size_t place = 0; place < Bedtime::kPlaceCount; ++place) {
			const auto& info = Bedtime::kPlaces[place];
			loaded.bedtimePlayer.places[place] = ini.GetValue("Bedtime", info.playerKey, Bedtime::kStay.data());
			loaded.bedtimeFollowers.places[place] = ini.GetValue("Bedtime", info.followersKey, Bedtime::kStay.data());
		}
		const auto& everywhere = Bedtime::kEverywhere;
		loaded.bedtimePlayer.everywhere = ini.GetValue("Bedtime", everywhere.playerKey, loaded.bedtimePlayer.places[Bedtime::kElsewhere].c_str());
		loaded.bedtimeFollowers.everywhere = ini.GetValue("Bedtime", everywhere.followersKey, loaded.bedtimeFollowers.places[Bedtime::kElsewhere].c_str());
		const bool differ = !Bedtime::Uniform(loaded.bedtimePlayer.places) || !Bedtime::Uniform(loaded.bedtimeFollowers.places);
		loaded.bedtimePerPlace = ini.GetBoolValue("Bedtime", "bPerPlace", differ);
	} else {
		const auto player = ReadKey(ini, "Undress", "iPlayer", 0);
		const auto followers = ReadKey(ini, "Undress", "iNpcs", 0);
		loaded.bedtimePlayer.everywhere = LegacyChoice(player);
		loaded.bedtimeFollowers.everywhere = LegacyChoice(followers);
		const auto undressMode = player == kLegacyModeEverything || player == kLegacyModeExceptTorso ? player : followers;
		if (undressMode == kLegacyModeExceptTorso) {
			loaded.keepBodyPiece = true;
		}
		const std::string items = ini.GetValue("Sleepwear", "sItems", "");
		if (ini.GetSectionSize("Outfits") < 0 && (player == kLegacyModeSleepwear || followers == kLegacyModeSleepwear || !items.empty())) {
			loaded.outfits.push_back({ kLegacyOutfitName, items });
		}
	}

	CSimpleIniA::TNamesDepend keys;
	if (ini.GetAllKeys("Outfits", keys)) {
		keys.sort(CSimpleIniA::Entry::LoadOrder());
		for (const auto& key : keys) {
			loaded.outfits.push_back({ key.pItem, ini.GetValue("Outfits", key.pItem, "") });
		}
	}

	loaded.loggingEnabled = ini.GetBoolValue("Logging", "bEnabled", loaded.loggingEnabled);

	Sanitize(loaded);
	g_settings = loaded;
}

bool Settings::Save()
{
	CSimpleIniA ini;
	ini.SetUnicode();
	ini.SetSpaces(false);

	const auto path = SettingsPath();
	ini.LoadFile(path.string().c_str());

	auto& s = g_settings;
	Sanitize(s);
	ini.SetLongValue("Input", "iSleepKey", s.sleepKey);
	WriteBool(ini, "Bed", "bAllowOwnedBeds", s.allowOwnedBeds);
	WriteBool(ini, "Bed", "bOpenSleepMenuOnLieDown", s.openSleepMenuOnLieDown);
	WriteBool(ini, "Followers", "bEnabled", s.followersEnabled);
	WriteBool(ini, "Followers", "bSpouseSharesBed", s.spouseSharesBed);
	WriteBool(ini, "Followers", "bHomeSpouseSharesBed", s.homeSpouseSharesBed);
	WriteBool(ini, "Followers", "bFollowersShareBed", s.followersShareBed);
	WriteBool(ini, "Followers", "bIgnoreOwnership", s.followersIgnoreOwnership);
	WriteFloat(ini, "Followers", "fSearchRadius", s.followerSearchRadius);
	WriteBool(ini, "Followers", "bNightOnly", s.followersNightOnly);
	WriteFloat(ini, "Followers", "fNightStart", s.followerNightStart);
	WriteFloat(ini, "Followers", "fNightEnd", s.followerNightEnd);

	ini.Delete("Undress", "iPlayer");
	ini.Delete("Undress", "iNpcs");
	ini.Delete("Sleepwear", nullptr);
	WriteBool(ini, "Undress", "bKeepBodyPiece", s.keepBodyPiece);
	WriteBool(ini, "Undress", "bAnimations", s.undressAnimations);
	WriteBool(ini, "Bedtime", "bPerPlace", s.bedtimePerPlace);
	ini.SetValue("Bedtime", Bedtime::kEverywhere.playerKey, s.bedtimePlayer.everywhere.c_str());
	ini.SetValue("Bedtime", Bedtime::kEverywhere.followersKey, s.bedtimeFollowers.everywhere.c_str());
	for (std::size_t place = 0; place < Bedtime::kPlaceCount; ++place) {
		const auto& info = Bedtime::kPlaces[place];
		ini.SetValue("Bedtime", info.playerKey, s.bedtimePlayer.places[place].c_str());
		ini.SetValue("Bedtime", info.followersKey, s.bedtimeFollowers.places[place].c_str());
	}

	CSimpleIniA::TNamesDepend keys;
	ini.GetAllKeys("Outfits", keys);
	std::vector<std::string> stale;
	for (const auto& key : keys) {
		stale.emplace_back(key.pItem);
	}
	for (const auto& key : stale) {
		ini.Delete("Outfits", key.c_str());
	}
	for (const auto& outfit : s.outfits) {
		ini.SetValue("Outfits", outfit.name.c_str(), outfit.items.c_str());
	}

	WriteBool(ini, "Logging", "bEnabled", s.loggingEnabled);

	const bool saved = ini.SaveFile(path.string().c_str(), false) >= 0;
	if (saved) {
		g_loadedFrom = path;
		SKSE::log::info("[settings] saved to {}", path.string());
	} else {
		SKSE::log::info("[settings] failed to save {}", path.string());
	}
	return saved;
}

void Settings::ResetToDefaults()
{
	auto outfits = std::move(g_settings.outfits);
	g_settings = Settings{};
	g_settings.outfits = std::move(outfits);
	ApplyLogLevel();
	Save();
}

void Settings::ApplyLogLevel()
{
	spdlog::set_level(g_settings.loggingEnabled ? spdlog::level::info : spdlog::level::off);
}

void Settings::LogSummary()
{
	if (g_loadedFrom.empty()) {
		SKSE::log::info("No settings file at {}, using defaults", SettingsPath().string());
	} else {
		SKSE::log::info("Settings loaded from {}", g_loadedFrom.string());
	}
	const auto& s = g_settings;
	SKSE::log::info("  sleepKey={}", s.sleepKey);
	SKSE::log::info("  allowOwnedBeds={} openSleepMenuOnLieDown={}", s.allowOwnedBeds, s.openSleepMenuOnLieDown);
	SKSE::log::info("  followersEnabled={} spouseSharesBed={} homeSpouseSharesBed={} followersShareBed={} followersIgnoreOwnership={} followerSearchRadius={}", s.followersEnabled, s.spouseSharesBed, s.homeSpouseSharesBed, s.followersShareBed, s.followersIgnoreOwnership, s.followerSearchRadius);
	SKSE::log::info("  followersNightOnly={} followerNightStart={} followerNightEnd={}", s.followersNightOnly, s.followerNightStart, s.followerNightEnd);
	SKSE::log::info("  keepBodyPiece={} undressAnimations={}", s.keepBodyPiece, s.undressAnimations);
	SKSE::log::info("  bedtime perPlace={} everywhere: you={} followers={}", s.bedtimePerPlace, s.bedtimePlayer.everywhere, s.bedtimeFollowers.everywhere);
	for (std::size_t place = 0; place < Bedtime::kPlaceCount; ++place) {
		SKSE::log::info("  bedtime {}: you={} followers={}", Bedtime::kPlaces[place].label, s.bedtimePlayer.places[place], s.bedtimeFollowers.places[place]);
	}
	for (const auto& outfit : s.outfits) {
		SKSE::log::info("  outfit '{}': {}", outfit.name, outfit.items);
	}
}
