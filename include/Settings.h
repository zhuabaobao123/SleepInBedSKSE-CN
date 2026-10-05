#pragma once

#include "Bedtime.h"

#include <cstdint>
#include <string>
#include <vector>

struct Settings
{
	struct Outfit
	{
		std::string name;
		std::string items;
	};

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

	bool keepBodyPiece{ false };
	bool undressAnimations{ true };

	bool                bedtimePerPlace{ false };
	Bedtime::Column     bedtimePlayer{};
	Bedtime::Column     bedtimeFollowers{};
	std::vector<Outfit> outfits{};

	bool loggingEnabled{ false };

	static const Settings& Get();
	static Settings&       Mutable();
	static void            Load();
	static bool            Save();
	static void            ResetToDefaults();
	static void            ApplyLogLevel();
	static void            LogSummary();
};
