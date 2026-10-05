#include "Campfire.h"

#include "Bed.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <string_view>

namespace
{
	constexpr RE::FormID       kBedrollLocalId = 0x007EFA;
	constexpr std::string_view kCampfirePlugin = "Campfire.esm";

	RE::TESFurniture* g_bedroll = nullptr;
}

namespace Campfire
{
	void PrepareBedroll()
	{
		auto* handler = RE::TESDataHandler::GetSingleton();
		g_bedroll = handler ? handler->LookupForm<RE::TESFurniture>(kBedrollLocalId, kCampfirePlugin) : nullptr;
		if (g_bedroll) {
			SKSE::log::info("[campfire] tent bedroll {:08X} found, its activations open the sleep menu directly", g_bedroll->GetFormID());
		} else {
			SKSE::log::info("[campfire] not present");
		}
	}

	bool OpenSleepMenuInstead(RE::TESObjectREFR* furniture, RE::TESObjectREFR* activator)
	{
		if (!g_bedroll || !furniture || furniture->GetBaseObject() != g_bedroll) {
			return false;
		}
		if (activator != RE::PlayerCharacter::GetSingleton()) {
			return false;
		}
		SKSE::log::info("[campfire] sleep menu opened instead of activating the tent bedroll {:08X}", furniture->GetFormID());
		if (Bed::HasPendingSentence()) {
			Bed::OfferServeSentence();
		} else {
			Bed::OpenSleepMenu();
		}
		return true;
	}
}
