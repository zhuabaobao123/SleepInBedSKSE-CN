#pragma once

#include <cstdint>

struct Settings
{
	std::int32_t sleepKey{ -1 };

	bool allowOwnedBeds{ false };
	bool openSleepMenuOnLieDown{ false };

	bool  followersEnabled{ true };
	bool  spouseSharesBed{ true };
	bool  homeSpouseSharesBed{ true };
	bool  followersShareBed{ false };
	bool  followersIgnoreOwnership{ false };
	float followerSearchRadius{ 1024.0f };
	bool  followersNightOnly{ false };
	float followerNightStart{ 20.0f };
	float followerNightEnd{ 6.0f };

	std::int32_t undressPlayer{ 2 };
	std::int32_t undressNpcs{ 2 };
	bool         undressAnimations{ true };

	bool loggingEnabled{ false };

	static const Settings& Get();
	static Settings&       Mutable();
	static void            Load();
	static bool            Save();
	static void            ResetToDefaults();
	static void            ApplyLogLevel();
	static void            LogSummary();
};
