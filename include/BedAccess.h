#pragma once

#include <cstdint>

namespace RE
{
	class PlayerCharacter;
	class TESObjectREFR;
}

namespace BedAccess
{
	void Install();

	bool          IsBed(RE::TESObjectREFR* furniture);
	std::uint32_t SleepMarkerCount(RE::TESObjectREFR* furniture);
	bool          HasFreeMarker(RE::TESObjectREFR* furniture);
	bool          AllowsSleep(RE::PlayerCharacter* player, RE::TESObjectREFR* bed);
}
