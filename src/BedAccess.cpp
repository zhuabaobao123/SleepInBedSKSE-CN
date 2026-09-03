#include "BedAccess.h"

#include "Bed.h"
#include "Detours.h"
#include "ScopedFlag.h"
#include "Settings.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

namespace
{
	constexpr std::uint32_t kNoReservation = 0xFFFFFFFF;

	constexpr REL::VariantID kSleepMarkerQuery{ 17025, 17410, 0x22B710 };
	constexpr REL::VariantID kSleepTargetCheck{ 39371, 40443, 0x6BD770 };
	constexpr REL::VariantID kFreeMarkerQuery{ 19766, 20161, 0x2B7050 };
	constexpr REL::VariantID kOwnerCheck{ 19805, 20210, 0x2B8650 };
	constexpr REL::VariantID kSitSleepActivateTarget{ 28400, 29150, 0x435370 };
	constexpr REL::VariantID kMarkerReservation{ 19765, 20160, 0x2B6E70 };

	thread_local bool tl_checkingSleepTarget = false;
	thread_local bool tl_reservingForSitSleep = false;

	using SleepMarkerQueryFn = bool(std::uint32_t, RE::NiAVObject*);
	using SleepTargetCheckFn = bool(RE::PlayerCharacter*, RE::TESObjectREFR*);
	using FreeMarkerQueryFn = bool(RE::TESObjectREFR*, bool);
	using OwnerCheckFn = bool(RE::TESObjectREFR*, const RE::Actor*, bool, bool);
	using SitSleepActivateTargetFn = std::uint64_t(void*, void*);
	using MarkerReservationFn = bool(RE::TESObjectREFR*, std::uint32_t, RE::Actor*, bool, bool);

	SleepMarkerQueryFn*       g_sleepMarkerQuery = nullptr;
	SleepTargetCheckFn*       g_sleepTargetCheck = nullptr;
	FreeMarkerQueryFn*        g_freeMarkerQuery = nullptr;
	OwnerCheckFn*             g_ownerCheck = nullptr;
	SitSleepActivateTargetFn* g_sitSleepActivateTarget = nullptr;
	MarkerReservationFn*      g_markerReservation = nullptr;

	std::uint32_t CountSleepMarkers(RE::TESObjectREFR* furniture)
	{
		auto* node = furniture ? furniture->Get3D() : nullptr;
		if (!node || !g_sleepMarkerQuery) {
			return 0;
		}
		std::uint32_t count = 0;
		const auto    total = RE::BSFurnitureMarkerNode::GetNumFurnitureMarkers(node);
		for (std::uint32_t index = 0; index < total; ++index) {
			if (g_sleepMarkerQuery(index, node)) {
				++count;
			}
		}
		return count;
	}

	bool SleepMarkerQuery(std::uint32_t markerIndex, RE::NiAVObject* furnitureNode)
	{
		if (!g_sleepMarkerQuery(markerIndex, furnitureNode)) {
			return false;
		}
		return !Bed::ActivationLiesDown();
	}

	bool SleepTargetCheck(RE::PlayerCharacter* player, RE::TESObjectREFR* bed)
	{
		ScopedFlag context(tl_checkingSleepTarget);
		return g_sleepTargetCheck(player, bed);
	}

	bool FreeMarkerQuery(RE::TESObjectREFR* furniture, bool ignoreReserved)
	{
		if (tl_checkingSleepTarget && !Settings::Get().allowOwnedBeds) {
			ignoreReserved = false;
		}
		return g_freeMarkerQuery(furniture, ignoreReserved);
	}

	bool OwnerCheck(RE::TESObjectREFR* refr, const RE::Actor* actor, bool useFaction, bool requiresOwner)
	{
		if (tl_checkingSleepTarget && Settings::Get().allowOwnedBeds) {
			return true;
		}
		return g_ownerCheck(refr, actor, useFaction, requiresOwner);
	}

	std::uint64_t SitSleepActivateTarget(void* execState, void* context)
	{
		ScopedFlag reserving(tl_reservingForSitSleep);
		return g_sitSleepActivateTarget(execState, context);
	}

	void ReleaseHeldReservation(RE::TESObjectREFR* furniture, RE::Actor* actor, std::uint32_t markerIndex)
	{
		auto* process = actor->GetActorRuntimeData().currentProcess;
		auto* data = process ? process->middleHigh : nullptr;
		if (!data || data->reservationSlot == markerIndex) {
			return;
		}
		const auto held = data->reservationSlot;
		data->reservationSlot = markerIndex;
		if (held != kNoReservation) {
			g_markerReservation(furniture, held, nullptr, false, false);
		}
	}

	bool MarkerReservation(RE::TESObjectREFR* furniture, std::uint32_t markerIndex, RE::Actor* actor, bool reserve, bool ignoreUsed)
	{
		if (tl_reservingForSitSleep && actor) {
			ReleaseHeldReservation(furniture, actor, markerIndex);
		}
		return g_markerReservation(furniture, markerIndex, actor, reserve, ignoreUsed);
	}
}

namespace BedAccess
{
	void Install()
	{
		Detours::Attach(kSleepMarkerQuery, &SleepMarkerQuery, g_sleepMarkerQuery, "bed activation decision");
		Detours::Attach(kSleepTargetCheck, &SleepTargetCheck, g_sleepTargetCheck, "sleep target check context");
		Detours::Attach(kFreeMarkerQuery, &FreeMarkerQuery, g_freeMarkerQuery, "reserved marker rejection");
		Detours::Attach(kOwnerCheck, &OwnerCheck, g_ownerCheck, "owned bed acceptance");
		Detours::Attach(kSitSleepActivateTarget, &SitSleepActivateTarget, g_sitSleepActivateTarget, "sit sleep activation context");
		Detours::Attach(kMarkerReservation, &MarkerReservation, g_markerReservation, "marker reservation");
	}

	bool IsBed(RE::TESObjectREFR* furniture)
	{
		return CountSleepMarkers(furniture) > 0;
	}

	std::uint32_t SleepMarkerCount(RE::TESObjectREFR* furniture)
	{
		return CountSleepMarkers(furniture);
	}

	bool HasFreeMarker(RE::TESObjectREFR* furniture)
	{
		return furniture && g_freeMarkerQuery && g_freeMarkerQuery(furniture, false);
	}

	bool AllowsSleep(RE::PlayerCharacter* player, RE::TESObjectREFR* bed)
	{
		return g_sleepTargetCheck && SleepTargetCheck(player, bed);
	}
}
