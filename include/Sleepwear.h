#pragma once

#include <cstddef>
#include <cstdint>

namespace RE
{
	class Actor;
	class TESObjectARMO;
}

namespace Sleepwear
{
	void Prepare();
	void Reset();

	std::int32_t ModeFor(RE::Actor* actor);
	bool         PartOfOutfit(RE::Actor* actor, RE::TESObjectARMO* armor);
	void         OnUndressed(RE::Actor* actor);

	void ScheduleReload();

	bool        SyncMenu();
	std::size_t OutfitCount();
	bool        RenderBedtime();
	void        RenderOutfits();
}
