#include "Settings.h"

#include <SimpleIni.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <filesystem>
#include <format>

namespace
{
	constexpr std::int32_t kMaxKeyCode = 281;
	constexpr float        kMaxHour = 23.99f;
	constexpr std::int32_t kMaxUndressMode = 2;

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

	void Sanitize(Settings& s)
	{
		const Settings defaults;
		s.sleepKey = std::clamp(s.sleepKey, -1, kMaxKeyCode);
		if (s.followerSearchRadius <= 0.0f) {
			s.followerSearchRadius = defaults.followerSearchRadius;
		}
		s.followerNightStart = std::clamp(s.followerNightStart, 0.0f, kMaxHour);
		s.followerNightEnd = std::clamp(s.followerNightEnd, 0.0f, kMaxHour);
		s.undressPlayer = std::clamp(s.undressPlayer, 0, kMaxUndressMode);
		s.undressNpcs = std::clamp(s.undressNpcs, 0, kMaxUndressMode);
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

	loaded.undressPlayer = std::clamp(ReadKey(ini, "Undress", "iPlayer", loaded.undressPlayer), 0, kMaxUndressMode);
	loaded.undressNpcs = std::clamp(ReadKey(ini, "Undress", "iNpcs", loaded.undressNpcs), 0, kMaxUndressMode);
	loaded.undressAnimations = ini.GetBoolValue("Undress", "bAnimations", loaded.undressAnimations);

	loaded.loggingEnabled = ini.GetBoolValue("Logging", "bEnabled", loaded.loggingEnabled);

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
	ini.SetLongValue("Undress", "iPlayer", s.undressPlayer);
	ini.SetLongValue("Undress", "iNpcs", s.undressNpcs);
	WriteBool(ini, "Undress", "bAnimations", s.undressAnimations);
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
	g_settings = Settings{};
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
	SKSE::log::info("  undressPlayer={} undressNpcs={} undressAnimations={}", s.undressPlayer, s.undressNpcs, s.undressAnimations);
}
