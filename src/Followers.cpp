#include "Followers.h"

#include "Bed.h"
#include "BedAccess.h"
#include "Settings.h"
#include "SleepPackage.h"
#include "Undress.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <vector>

namespace
{
	constexpr RE::FormID    kActorTypeNpc = 0x13794;
	constexpr float         kChildBedPenalty = 1.0e12f;
	constexpr RE::FormID    kNffSandboxFaction = 0x019665;
	constexpr const char*   kNffPlugin = "nwsFollowerFramework.esp";
	constexpr std::int8_t   kNffSandboxActive = 1;
	constexpr std::int8_t   kNffSandboxHalted = 2;
	constexpr std::uint32_t kTickFrames = 90;
	constexpr std::uint32_t kMaxResends = 5;

	struct Assignment
	{
		RE::ActorHandle     actor;
		RE::ObjectRefHandle bed;
		std::uint32_t       resends;
		bool                sandboxHalted;
		std::int32_t        loggedState{ -1 };
		RE::TESPackage*     loggedPackage{ nullptr };
		std::int32_t        loggedIndex{ -1 };
	};

	struct BedSlot
	{
		RE::TESObjectREFR* bed;
		bool               childBed;
		std::uint32_t      capacity;
	};

	struct Sleepers
	{
		RE::Actor*              spouse = nullptr;
		bool                    spouseIsTeammate = false;
		std::vector<RE::Actor*> others;
	};

	std::atomic<bool>          g_active{ false };
	std::atomic<std::uint32_t> g_generation{ 0 };
	std::vector<Assignment>    g_assignments;
	std::uint32_t              g_tickFrames = 0;

	RE::TESFaction* NffSandboxFaction()
	{
		auto* handler = RE::TESDataHandler::GetSingleton();
		return handler ? handler->LookupForm<RE::TESFaction>(kNffSandboxFaction, kNffPlugin) : nullptr;
	}

	bool HaltNffSandbox(RE::Actor* actor)
	{
		auto* faction = NffSandboxFaction();
		if (!faction || !actor->IsInFaction(faction) || actor->GetFactionRank(faction, false) != kNffSandboxActive) {
			return false;
		}
		actor->AddToFaction(faction, kNffSandboxHalted);
		SKSE::log::info("[followers] {:08X} NFF sandbox halted while in bed", actor->GetFormID());
		return true;
	}

	void ResumeNffSandbox(RE::Actor* actor)
	{
		auto* faction = NffSandboxFaction();
		if (!faction || !actor->IsInFaction(faction) || actor->GetFactionRank(faction, false) != kNffSandboxHalted) {
			return;
		}
		actor->AddToFaction(faction, kNffSandboxActive);
		SKSE::log::info("[followers] {:08X} NFF sandbox resumed", actor->GetFormID());
	}

	RE::PlayerCharacter* Player()
	{
		return RE::PlayerCharacter::GetSingleton();
	}

	float SearchRadius()
	{
		return Settings::Get().followerSearchRadius;
	}

	float GameHour()
	{
		auto* calendar = RE::Calendar::GetSingleton();
		return calendar ? calendar->GetHour() : -1.0f;
	}

	bool IsSleepingTime(float hour)
	{
		const auto& settings = Settings::Get();
		if (!settings.followersNightOnly || hour < 0.0f) {
			return true;
		}
		const float start = settings.followerNightStart;
		const float end = settings.followerNightEnd;
		if (start <= end) {
			return hour >= start && hour < end;
		}
		return hour >= start || hour < end;
	}

	float SquaredDistance(RE::TESObjectREFR* a, RE::TESObjectREFR* b)
	{
		return a->GetPosition().GetSquaredDistance(b->GetPosition());
	}

	bool NearPlayer(RE::Actor* actor, RE::PlayerCharacter* player)
	{
		auto* cell = actor->GetParentCell();
		if (cell && cell == player->GetParentCell()) {
			return true;
		}
		const float radius = SearchRadius();
		return SquaredDistance(actor, player) <= radius * radius;
	}

	bool IsSpouse(RE::Actor* actor, RE::PlayerCharacter* player)
	{
		auto* base = actor->GetActorBase();
		auto* playerBase = player->GetActorBase();
		if (!base || !playerBase) {
			return false;
		}
		auto* relationship = RE::BGSRelationship::GetRelationship(base, playerBase);
		if (!relationship) {
			relationship = RE::BGSRelationship::GetRelationship(playerBase, base);
		}
		return relationship && relationship->level == RE::BGSRelationship::RELATIONSHIP_LEVEL::kLover;
	}

	bool IsChildBed(RE::TESObjectREFR* bed)
	{
		auto* base = bed ? bed->GetBaseObject() : nullptr;
		return base && (base->GetFormFlags() & RE::TESFurniture::RecordFlags::kChildCanUse) != 0;
	}

	const char* BedRefusal(RE::Actor* actor, RE::PlayerCharacter* player, RE::BGSKeyword* npcKeyword)
	{
		if (actor == player) {
			return "player";
		}
		if (!actor->Is3DLoaded()) {
			return "not loaded";
		}
		if (actor->IsDead()) {
			return "dead";
		}
		if (actor->IsInCombat()) {
			return "in combat";
		}
		if (actor->IsOnMount()) {
			return "mounted";
		}
		if (actor->IsCommandedActor() || actor->IsSummoned()) {
			return "summoned";
		}
		if (!actor->IsAIEnabled()) {
			return "AI disabled";
		}
		if (!actor->HasKeyword(npcKeyword)) {
			return "not an NPC";
		}
		if (actor->AsActorState()->GetSitSleepState() >= RE::SIT_SLEEP_STATE::kWantToSleep) {
			return "already sleeping";
		}
		return nullptr;
	}

	Sleepers CollectSleepers(RE::PlayerCharacter* player, RE::BGSKeyword* npcKeyword)
	{
		const auto& settings = Settings::Get();
		Sleepers    sleepers;
		auto*       lists = RE::ProcessLists::GetSingleton();
		if (!lists) {
			return sleepers;
		}
		lists->ForEachHighActor([&](RE::Actor* actor) {
			if (!actor || actor == player || !NearPlayer(actor, player)) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			const bool teammate = actor->IsPlayerTeammate();
			const bool spouseAllowed = teammate ? settings.spouseSharesBed : settings.homeSpouseSharesBed;
			const bool spouse = !sleepers.spouse && spouseAllowed && IsSpouse(actor, player);
			if (!teammate && !spouse) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			if (const char* refusal = BedRefusal(actor, player, npcKeyword)) {
				SKSE::log::info("[followers] {:08X} skipped: {}", actor->GetFormID(), refusal);
				return RE::BSContainer::ForEachResult::kContinue;
			}
			if (spouse) {
				sleepers.spouse = actor;
				sleepers.spouseIsTeammate = teammate;
			} else {
				sleepers.others.push_back(actor);
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
		std::sort(sleepers.others.begin(), sleepers.others.end(), [player](RE::Actor* a, RE::Actor* b) {
			return SquaredDistance(a, player) < SquaredDistance(b, player);
		});
		return sleepers;
	}

	bool AllowedToUse(RE::Actor* actor, RE::TESObjectREFR* bed, bool childBed, RE::PlayerCharacter* player)
	{
		if (actor->IsChild() && !childBed) {
			return false;
		}
		if (Settings::Get().followersIgnoreOwnership) {
			return true;
		}
		return bed->IsAnOwner(player, true, false) || bed->IsAnOwner(actor, true, false);
	}

	std::uint32_t FreeSleepSlots(RE::TESObjectREFR* bed)
	{
		if (!BedAccess::HasFreeMarker(bed)) {
			return 0;
		}
		const auto sleep = BedAccess::SleepMarkerCount(bed);
		if (sleep == 0) {
			return 0;
		}
		std::uint32_t used = 0;
		if (auto* markers = bed->extraList.GetByType<RE::ExtraUsedMarkers>()) {
			used = markers->usedMarkers.size();
		}
		return std::max<std::uint32_t>(1, sleep - std::min(sleep, used));
	}

	std::vector<BedSlot> CollectBeds(RE::PlayerCharacter* player, RE::TESObjectREFR* playerBed)
	{
		std::vector<BedSlot> beds;
		auto*                tes = RE::TES::GetSingleton();
		if (!tes) {
			return beds;
		}
		tes->ForEachReferenceInRange(player, SearchRadius(), [&](RE::TESObjectREFR* ref) {
			if (!ref || ref == playerBed || ref->IsDisabled() || ref->IsMarkedForDeletion() || !ref->Is3DLoaded()) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			auto* base = ref->GetBaseObject();
			if (!base || !base->As<RE::TESFurniture>() || !BedAccess::IsBed(ref)) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			const auto capacity = FreeSleepSlots(ref);
			if (capacity == 0) {
				return RE::BSContainer::ForEachResult::kContinue;
			}
			beds.push_back({ ref, IsChildBed(ref), capacity });
			return RE::BSContainer::ForEachResult::kContinue;
		});
		return beds;
	}

	BedSlot* PickBed(RE::Actor* actor, std::vector<BedSlot>& beds, RE::PlayerCharacter* player)
	{
		BedSlot*    best = nullptr;
		float       bestScore = 0.0f;
		const float radius = SearchRadius();
		for (auto& slot : beds) {
			if (slot.capacity == 0 || !AllowedToUse(actor, slot.bed, slot.childBed, player)) {
				continue;
			}
			float score = SquaredDistance(actor, slot.bed);
			if (score > radius * radius) {
				continue;
			}
			if (slot.childBed && !actor->IsChild()) {
				score += kChildBedPenalty;
			}
			if (!best || score < bestScore) {
				best = &slot;
				bestScore = score;
			}
		}
		return best;
	}

	void SendAfterUndress(RE::ActorHandle actorHandle, RE::ObjectRefHandle bedHandle, const char* role)
	{
		auto  actorPtr = actorHandle.get();
		auto  bedPtr = bedHandle.get();
		auto* actor = actorPtr.get();
		auto* bed = bedPtr.get();
		if (!actor || !bed) {
			return;
		}
		if (!g_active || !Bed::IsPlayerLyingInBed()) {
			SKSE::log::info("[followers] {} {:08X} undressed but the player is up again, not sent to bed", role, actor->GetFormID());
			return;
		}
		const bool sent = Bed::LieDown(actor, bed);
		SKSE::log::info("[followers] {} {:08X} -> bed {:08X} {} after undressing", role, actor->GetFormID(), bed->GetFormID(), sent ? "sent to bed" : "refused");
		if (sent) {
			g_assignments.push_back({ actor->GetHandle(), bed->GetHandle(), 0, HaltNffSandbox(actor) });
		}
	}

	void Assign(RE::Actor* actor, RE::TESObjectREFR* bed, BedSlot* slot, const char* role)
	{
		const bool seated = actor->AsActorState()->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal;
		if (Undress::BeginBeforeBed(actor, [actorHandle = actor->GetHandle(), bedHandle = bed->GetHandle(), role] { SendAfterUndress(actorHandle, bedHandle, role); })) {
			SKSE::log::info("[followers] {} {:08X} undresses before going to bed {:08X}", role, actor->GetFormID(), bed->GetFormID());
			if (slot && slot->capacity > 0) {
				--slot->capacity;
			}
			return;
		}
		const bool sent = Bed::LieDown(actor, bed);
		SKSE::log::info("[followers] {} {:08X} -> bed {:08X} {}{}", role, actor->GetFormID(), bed->GetFormID(), sent ? "sent to bed" : "refused", seated ? " (getting up first)" : "");
		if (!sent) {
			return;
		}
		const bool halted = HaltNffSandbox(actor);
		g_assignments.push_back({ actor->GetHandle(), bed->GetHandle(), 0, halted });
		if (slot && slot->capacity > 0) {
			--slot->capacity;
		}
	}

	void LogAssignmentChanges(Assignment& assignment, RE::Actor* actor)
	{
		auto*      process = actor->GetActorRuntimeData().currentProcess;
		auto*      package = process ? process->currentPackage.package : nullptr;
		const auto state = static_cast<std::int32_t>(actor->AsActorState()->GetSitSleepState());
		const auto index = process ? process->currentPackage.currentProcedureIndex : -1;
		if (state == assignment.loggedState && package == assignment.loggedPackage && index == assignment.loggedIndex) {
			return;
		}
		assignment.loggedState = state;
		assignment.loggedPackage = package;
		assignment.loggedIndex = index;
		SKSE::log::info("[followers] {:08X} status: sit/sleep state {} package={:X} form={:08X} type={} created={} pathing={} index={} start={:.3f} lowFlags={:02X}", actor->GetFormID(), state, reinterpret_cast<std::uintptr_t>(package), package ? package->GetFormID() : 0, package ? static_cast<std::int32_t>(package->packData.packType.get()) : -1, SleepPackage::Holds(actor), actor->IsPathing(), index, process ? process->currentPackage.packageStartTime : 0.0f, process ? static_cast<std::uint32_t>(process->lowProcessFlags.underlying()) : 0u);
	}

	void ResendLostPackages()
	{
		for (auto& assignment : g_assignments) {
			auto  actorPtr = assignment.actor.get();
			auto  bedPtr = assignment.bed.get();
			auto* actor = actorPtr.get();
			auto* bed = bedPtr.get();
			if (!actor || !bed) {
				continue;
			}
			LogAssignmentChanges(assignment, actor);
			if (assignment.resends >= kMaxResends) {
				continue;
			}
			const auto state = actor->AsActorState()->GetSitSleepState();
			if (state >= RE::SIT_SLEEP_STATE::kWantToSleep || SleepPackage::Holds(actor)) {
				continue;
			}
			++assignment.resends;
			const bool sent = Bed::LieDown(actor, bed);
			SKSE::log::info("[followers] {:08X} lost its sleep package in sit/sleep state {}, {} (attempt {}/{})", actor->GetFormID(), static_cast<std::int32_t>(state), sent ? "sent again" : "refused", assignment.resends, kMaxResends);
			if (sent && !assignment.sandboxHalted) {
				assignment.sandboxHalted = HaltNffSandbox(actor);
			}
		}
	}

	void LayFollowersDown(RE::ObjectRefHandle bedHandle)
	{
		auto* player = Player();
		if (!player || !Bed::IsPlayerLyingInBed()) {
			return;
		}
		const float hour = GameHour();
		if (!IsSleepingTime(hour)) {
			SKSE::log::info("[followers] {:.1f}h is outside the night window, nobody is sent to bed", hour);
			return;
		}
		auto* npcKeyword = RE::TESForm::LookupByID<RE::BGSKeyword>(kActorTypeNpc);
		if (!npcKeyword) {
			SKSE::log::info("[followers] ActorTypeNPC keyword missing");
			return;
		}
		auto  playerBedPtr = bedHandle.get();
		auto* playerBed = playerBedPtr.get();
		auto  sleepers = CollectSleepers(player, npcKeyword);
		if (!sleepers.spouse && sleepers.others.empty()) {
			SKSE::log::info("[followers] nobody nearby to send to bed");
			return;
		}
		auto beds = CollectBeds(player, playerBed);
		SKSE::log::info("[followers] spouse={} followers={} beds={}", sleepers.spouse ? sleepers.spouse->GetFormID() : 0, sleepers.others.size(), beds.size());
		BedSlot playerSlot{ playerBed, IsChildBed(playerBed), playerBed ? FreeSleepSlots(playerBed) : 0 };
		if (auto* spouse = sleepers.spouse) {
			const bool shareable = playerSlot.capacity > 0 && AllowedToUse(spouse, playerBed, playerSlot.childBed, player);
			if (shareable) {
				Assign(spouse, playerBed, &playerSlot, "spouse");
			} else if (sleepers.spouseIsTeammate) {
				sleepers.others.insert(sleepers.others.begin(), spouse);
			} else {
				SKSE::log::info("[followers] spouse {:08X} stays: player's bed has no free marker", spouse->GetFormID());
			}
		}
		const bool shareWithFollowers = Settings::Get().followersShareBed;
		for (auto* actor : sleepers.others) {
			if (shareWithFollowers && playerSlot.capacity > 0 && AllowedToUse(actor, playerBed, playerSlot.childBed, player)) {
				Assign(actor, playerBed, &playerSlot, "follower");
			} else if (auto* slot = PickBed(actor, beds, player)) {
				Assign(actor, slot->bed, slot, "follower");
			} else {
				SKSE::log::info("[followers] follower {:08X}: no free bed", actor->GetFormID());
			}
		}
	}

	void ReleaseStrayPackages()
	{
		auto* lists = RE::ProcessLists::GetSingleton();
		auto* player = Player();
		if (!lists || !player) {
			return;
		}
		lists->ForEachHighActor([player](RE::Actor* actor) {
			if (actor && actor != player && SleepPackage::Holds(actor)) {
				Bed::StandUp(actor);
				ResumeNffSandbox(actor);
				SKSE::log::info("[followers] {:08X} released from a leftover sleep package", actor->GetFormID());
			}
			return RE::BSContainer::ForEachResult::kContinue;
		});
	}

	void StandFollowersUp()
	{
		Undress::AbortPending();
		for (auto& assignment : g_assignments) {
			auto  actorPtr = assignment.actor.get();
			auto* actor = actorPtr.get();
			if (!actor) {
				continue;
			}
			const bool standing = Bed::StandUp(actor);
			SKSE::log::info("[followers] {:08X} {}", actor->GetFormID(), standing ? "released from sleep package" : "no sleep package");
			if (assignment.sandboxHalted) {
				ResumeNffSandbox(actor);
			}
		}
		g_assignments.clear();
		g_tickFrames = 0;
		ReleaseStrayPackages();
	}

	void Schedule(std::function<void()> task)
	{
		if (auto* tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask(std::move(task));
		}
	}
}

namespace Followers
{
	void Install()
	{
		const auto& s = Settings::Get();
		SKSE::log::info("[followers] enabled={} spouseSharesBed={} homeSpouseSharesBed={} followersShareBed={} ignoreOwnership={} searchRadius={} nightOnly={} night={}-{}", s.followersEnabled, s.spouseSharesBed, s.homeSpouseSharesBed, s.followersShareBed, s.followersIgnoreOwnership, s.followerSearchRadius, s.followersNightOnly, s.followerNightStart, s.followerNightEnd);
	}

	void OnPlayerLayDown(RE::TESObjectREFR* bed)
	{
		if (!Settings::Get().followersEnabled || g_active.exchange(true)) {
			return;
		}
		const auto handle = bed ? bed->GetHandle() : RE::ObjectRefHandle();
		const auto generation = ++g_generation;
		Schedule([handle, generation] {
			if (generation != g_generation) {
				return;
			}
			LayFollowersDown(handle);
		});
	}

	void Tick()
	{
		if (!g_active || g_assignments.empty()) {
			return;
		}
		if (++g_tickFrames < kTickFrames) {
			return;
		}
		g_tickFrames = 0;
		ResendLostPackages();
	}

	void OnPlayerStoodUp()
	{
		if (!g_active.exchange(false)) {
			return;
		}
		const auto generation = ++g_generation;
		Schedule([generation] {
			if (generation != g_generation) {
				return;
			}
			StandFollowersUp();
		});
	}

	void Reset()
	{
		g_assignments.clear();
		++g_generation;
		const bool lying = Settings::Get().followersEnabled && Bed::IsPlayerLyingInBed();
		g_active = lying;
		if (!lying) {
			Schedule([] {
				ReleaseStrayPackages();
			});
		}
	}
}
