#include "Undress.h"

#include "Bed.h"
#include "Settings.h"
#include "SleepPackage.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

#undef GetObject

namespace
{
	constexpr std::uint32_t kUniqueID = 0x53494255;
	constexpr std::uint32_t kRecordType = 0x554E4452;
	constexpr std::uint32_t kRecordVersion = 2;
	constexpr std::uint32_t kReconcileFrames = 90;
	constexpr std::uint32_t kStrandedTicks = 4;
	constexpr std::uint32_t kAbandonedTicks = 20;
	constexpr float         kFurnitureRange = 512.0f;
	constexpr std::int32_t  kModeOff = 0;
	constexpr std::int32_t  kModeExceptTorso = 2;
	constexpr std::uint8_t  kSlotDefault = 0;
	constexpr std::uint8_t  kSlotLeft = 1;

	constexpr std::string_view kAnimationPlugin = "Immersive Equipping Animations.esp";
	constexpr RE::FormID       kPlayerGuardId = 0x00080C;
	constexpr RE::FormID       kFollowerGuardId = 0x000D98;
	constexpr const char*      kStopEvent = "OffsetStop";

	enum class Piece : std::uint8_t
	{
		kHelmet,
		kHood,
		kHands,
		kBoots,
		kCuirass,
		kNeck,
		kRing,
		kOther
	};

	enum class Direction : std::uint8_t
	{
		kUndress,
		kDress
	};

	struct Animation
	{
		const char*  event;
		float        seconds;
		std::uint8_t order;
	};

	struct Step
	{
		RE::FormID   item;
		Animation    animation;
		std::uint8_t slot = kSlotDefault;
	};

	struct Sequence
	{
		RE::FormID            actor;
		Direction             direction;
		std::vector<Step>     steps;
		std::size_t           index = 0;
		float                 remaining = 0.0f;
		bool                  playing = false;
		bool                  headTracking = false;
		bool                  movementArmed = false;
		std::function<void()> onDone;
	};

	struct Keywords
	{
		RE::BGSKeyword* armorHelmet = nullptr;
		RE::BGSKeyword* clothingHead = nullptr;
		RE::BGSKeyword* armorCuirass = nullptr;
		RE::BGSKeyword* clothingBody = nullptr;
	};

	constexpr Animation kNoAnimation{ nullptr, 0.0f, 255 };

	struct Remembered
	{
		RE::FormID   form;
		std::uint8_t slot;
	};

	std::unordered_map<RE::FormID, std::vector<Remembered>> g_records;
	std::unordered_map<RE::FormID, std::uint32_t>            g_strandedTicks;
	std::atomic<std::size_t>                                 g_recordCount{ 0 };
	std::vector<Sequence>                                    g_sequences;
	std::uint32_t                                            g_reconcileFrames = 0;
	Keywords                                                 g_keywords;
	RE::TESGlobal*                                           g_playerGuard = nullptr;
	RE::TESGlobal*                                           g_followerGuard = nullptr;
	bool                                                     g_animations = false;
	bool                                                     g_restoreFirstPerson = false;

	void Schedule(std::function<void()> task)
	{
		if (auto* tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask(std::move(task));
		}
	}

	void SyncCount()
	{
		g_recordCount = g_records.size();
	}

	RE::Actor* FindActor(RE::FormID actorID)
	{
		return RE::TESForm::LookupByID<RE::Actor>(actorID);
	}

	bool IsEquippable(RE::TESBoundObject& object)
	{
		return object.IsArmor() || object.IsWeapon() || object.IsAmmo() || object.Is(RE::FormType::Light);
	}

	RE::TESObjectREFR::InventoryItemMap EquippableInventory(RE::Actor* actor)
	{
		return actor->GetInventory([](RE::TESBoundObject& object) { return IsEquippable(object); });
	}

	struct Held
	{
		RE::TESBoundObject* object = nullptr;
		std::int32_t        count = 0;
	};

	Held Carried(RE::Actor* actor, RE::FormID itemID)
	{
		for (const auto& [object, data] : EquippableInventory(actor)) {
			if (object && object->GetFormID() == itemID && data.first > 0) {
				return { object, data.first };
			}
		}
		return {};
	}

	bool InBed(RE::Actor* actor)
	{
		return actor->AsActorState()->GetSitSleepState() >= RE::SIT_SLEEP_STATE::kWantToSleep;
	}

	std::int32_t ModeFor(RE::Actor* actor)
	{
		const auto& settings = Settings::Get();
		if (actor->IsPlayerRef()) {
			return settings.undressPlayer;
		}
		return SleepPackage::Holds(actor) ? settings.undressNpcs : kModeOff;
	}

	std::int32_t AssignedMode(RE::Actor* actor)
	{
		const auto& settings = Settings::Get();
		return actor->IsPlayerRef() ? settings.undressPlayer : settings.undressNpcs;
	}

	bool Tagged(RE::TESObjectARMO* armor, RE::BGSKeyword* keyword)
	{
		return keyword && armor->HasKeyword(keyword);
	}

	bool IsSlotless(RE::TESObjectARMO* armor)
	{
		return armor->GetSlotMask() == RE::BGSBipedObjectForm::BipedObjectSlot::kNone;
	}

	bool IsWig(RE::TESObjectARMO* armor)
	{
		using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
		if (!armor->HasPartOf(Slot::kHair) && !armor->HasPartOf(Slot::kLongHair)) {
			return false;
		}
		return !Tagged(armor, g_keywords.armorHelmet) && !Tagged(armor, g_keywords.clothingHead);
	}

	bool Keeps(RE::TESObjectARMO* armor, std::int32_t mode)
	{
		if ((armor->GetFormFlags() & RE::TESObjectARMO::RecordFlags::kNonPlayable) != 0) {
			return true;
		}
		if (IsSlotless(armor) || IsWig(armor)) {
			return true;
		}
		return mode == kModeExceptTorso && armor->HasPartOf(RE::BGSBipedObjectForm::BipedObjectSlot::kBody);
	}

	Piece Classify(RE::TESObjectARMO* armor)
	{
		using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
		if (Tagged(armor, g_keywords.armorHelmet)) {
			return Piece::kHelmet;
		}
		if (Tagged(armor, g_keywords.clothingHead)) {
			return Piece::kHood;
		}
		if (armor->HasPartOf(Slot::kHands)) {
			return Piece::kHands;
		}
		if (armor->HasPartOf(Slot::kFeet)) {
			return Piece::kBoots;
		}
		if (Tagged(armor, g_keywords.armorCuirass) || Tagged(armor, g_keywords.clothingBody) || armor->HasPartOf(Slot::kBody)) {
			return Piece::kCuirass;
		}
		if (armor->HasPartOf(Slot::kAmulet)) {
			return Piece::kNeck;
		}
		if (armor->HasPartOf(Slot::kRing)) {
			return Piece::kRing;
		}
		return Piece::kOther;
	}

	struct WornItem
	{
		RE::TESBoundObject* object;
		Piece               piece;
		std::uint8_t        slot;
	};

	std::vector<WornItem> WornEquipment(RE::Actor* actor, std::int32_t mode)
	{
		std::vector<WornItem> worn;
		for (const auto& [object, data] : EquippableInventory(actor)) {
			const auto& [count, entry] = data;
			if (count <= 0 || !object || !entry || !entry->IsWorn()) {
				continue;
			}
			if (auto* armor = object->As<RE::TESObjectARMO>()) {
				if (!Keeps(armor, mode)) {
					worn.push_back({ object, Classify(armor), kSlotDefault });
				}
				continue;
			}
			if (object->IsWeapon()) {
				if (entry->IsWorn(false)) {
					worn.push_back({ object, Piece::kOther, kSlotDefault });
				}
				if (entry->IsWorn(true)) {
					worn.push_back({ object, Piece::kOther, kSlotLeft });
				}
				continue;
			}
			if (object->Is(RE::FormType::Light)) {
				worn.push_back({ object, Piece::kOther, kSlotLeft });
				continue;
			}
			worn.push_back({ object, Piece::kOther, kSlotDefault });
		}
		return worn;
	}

	Animation UndressAnimation(Piece piece)
	{
		switch (piece) {
		case Piece::kHelmet:
			return { "unequiphelmet", 2.5f, 0 };
		case Piece::kHood:
			return { "unequiphelmet", 2.2f, 0 };
		case Piece::kHands:
			return { "unequiphands", 2.5f, 1 };
		case Piece::kCuirass:
			return { "unequipcuirass", 2.5f, 2 };
		case Piece::kBoots:
			return { "unequipboots", 3.5f, 3 };
		case Piece::kNeck:
			return { "unequipneck", 3.5f, 4 };
		case Piece::kRing:
			return { "equipring", 3.6f, 5 };
		default:
			return kNoAnimation;
		}
	}

	Animation DressAnimation(Piece piece)
	{
		switch (piece) {
		case Piece::kCuirass:
			return { "equipcuirass", 2.5f, 0 };
		case Piece::kBoots:
			return { "equipboots", 3.5f, 1 };
		case Piece::kHands:
			return { "equiphands", 2.5f, 2 };
		case Piece::kHelmet:
			return { "equiphelmet", 2.2f, 3 };
		case Piece::kHood:
			return { "equiphood", 2.2f, 3 };
		case Piece::kNeck:
			return { "equipneck", 3.6f, 4 };
		case Piece::kRing:
			return { "equipring", 3.6f, 5 };
		default:
			return kNoAnimation;
		}
	}

	void Remember(RE::FormID actorID, RE::FormID itemID, std::uint8_t slot)
	{
		auto&      items = g_records[actorID];
		const auto found = std::find_if(items.begin(), items.end(), [&](const Remembered& r) { return r.form == itemID && r.slot == slot; });
		if (found == items.end()) {
			items.push_back({ itemID, slot });
		}
		SyncCount();
	}

	void Forget(RE::FormID actorID)
	{
		g_records.erase(actorID);
		g_strandedTicks.erase(actorID);
		SyncCount();
	}

	std::size_t Remembered(RE::FormID actorID)
	{
		const auto record = g_records.find(actorID);
		return record == g_records.end() ? 0 : record->second.size();
	}

	const RE::BGSEquipSlot* LeftHandSlot()
	{
		auto* manager = RE::BGSDefaultObjectManager::GetSingleton();
		return manager ? manager->GetObject<RE::BGSEquipSlot>(RE::DEFAULT_OBJECTS::kLeftHandEquip) : nullptr;
	}

	bool TakeOff(RE::Actor* actor, RE::FormID itemID, std::uint8_t slot)
	{
		auto*      manager = RE::ActorEquipManager::GetSingleton();
		const auto held = Carried(actor, itemID);
		if (!manager || !held.object) {
			return false;
		}
		Remember(actor->GetFormID(), itemID, slot);
		const auto* equipSlot = slot == kSlotLeft ? LeftHandSlot() : nullptr;
		return manager->UnequipObject(actor, held.object, nullptr, 1, equipSlot, true, false, false, false);
	}

	bool PutOn(RE::Actor* actor, RE::FormID itemID, std::uint8_t slot)
	{
		auto*      manager = RE::ActorEquipManager::GetSingleton();
		const auto held = Carried(actor, itemID);
		if (!manager || !held.object) {
			return false;
		}
		const auto* equipSlot = slot == kSlotLeft ? LeftHandSlot() : nullptr;
		const auto  count = held.object->IsAmmo() ? static_cast<std::uint32_t>(held.count) : 1u;
		manager->EquipObject(actor, held.object, nullptr, count, equipSlot, false, false, false, false);
		return true;
	}

	bool SendEvent(RE::Actor* actor, const char* event)
	{
		return actor->NotifyAnimationGraph(RE::BSFixedString{ event });
	}

	Sequence* FindSequence(RE::FormID actorID)
	{
		for (auto& sequence : g_sequences) {
			if (sequence.actor == actorID) {
				return &sequence;
			}
		}
		return nullptr;
	}

	bool AnySequence(bool player)
	{
		for (const auto& sequence : g_sequences) {
			if ((sequence.actor == 0x14) == player) {
				return true;
			}
		}
		return false;
	}

	void HoldGuard(RE::FormID actorID)
	{
		auto* guard = actorID == 0x14 ? g_playerGuard : g_followerGuard;
		if (guard) {
			guard->value = 1.0f;
		}
	}

	void ReleaseGuard(RE::FormID actorID)
	{
		const bool player = actorID == 0x14;
		auto*      guard = player ? g_playerGuard : g_followerGuard;
		if (guard && !AnySequence(player)) {
			guard->value = 0.0f;
		}
	}

	void EnterThirdPerson()
	{
		auto* camera = RE::PlayerCamera::GetSingleton();
		if (camera && camera->IsInFirstPerson()) {
			camera->ForceThirdPerson();
			g_restoreFirstPerson = true;
		}
	}

	void RestoreFirstPerson()
	{
		if (!g_restoreFirstPerson) {
			return;
		}
		g_restoreFirstPerson = false;
		if (auto* camera = RE::PlayerCamera::GetSingleton()) {
			camera->ForceFirstPerson();
		}
	}

	void SuspendHeadTracking(RE::Actor* actor, Sequence& sequence)
	{
		auto& state = actor->AsActorState()->actorState2;
		sequence.headTracking = state.headTracking != 0;
		state.headTracking = 0;
	}

	void ResumeHeadTracking(RE::Actor* actor, const Sequence& sequence)
	{
		if (sequence.headTracking) {
			actor->AsActorState()->actorState2.headTracking = 1;
		}
	}

	bool Eligible(RE::Actor* actor)
	{
		if (!g_animations || !Settings::Get().undressAnimations) {
			return false;
		}
		auto* state = actor->AsActorState();
		return actor->Is3DLoaded() && !actor->IsDead() && !actor->IsInCombat() && !actor->IsOnMount() && !state->IsWeaponDrawn() && !state->IsSwimming() && state->GetSitSleepState() == RE::SIT_SLEEP_STATE::kNormal;
	}

	const char* Name(Direction direction)
	{
		return direction == Direction::kUndress ? "undress" : "dress";
	}

	bool StartSequence(RE::Actor* actor, Direction direction, std::vector<Step> steps, std::function<void()> onDone)
	{
		const auto actorID = actor->GetFormID();
		bool       animated = false;
		for (const auto& step : steps) {
			animated = animated || step.animation.event != nullptr;
		}
		if (!animated || FindSequence(actorID)) {
			return false;
		}
		std::stable_sort(steps.begin(), steps.end(), [](const Step& a, const Step& b) { return a.animation.order < b.animation.order; });
		Sequence sequence{ actorID, direction, std::move(steps) };
		sequence.onDone = std::move(onDone);
		if (actor->IsPlayerRef()) {
			EnterThirdPerson();
		} else {
			SuspendHeadTracking(actor, sequence);
		}
		HoldGuard(actorID);
		SKSE::log::info("[undress] {:08X} animated {} of {} piece(s) started", actorID, Name(direction), sequence.steps.size());
		g_sequences.push_back(std::move(sequence));
		return true;
	}

	void Swap(const Sequence& sequence, RE::Actor* actor, const Step& step)
	{
		if (sequence.direction == Direction::kUndress) {
			TakeOff(actor, step.item, step.slot);
		} else {
			PutOn(actor, step.item, step.slot);
		}
	}

	void FlushRemaining(Sequence& sequence, RE::Actor* actor)
	{
		if (sequence.playing) {
			SendEvent(actor, kStopEvent);
			sequence.playing = false;
		}
		for (; sequence.index < sequence.steps.size(); ++sequence.index) {
			Swap(sequence, actor, sequence.steps[sequence.index]);
		}
	}

	void Undo(Sequence& sequence, RE::Actor* actor)
	{
		if (sequence.playing) {
			SendEvent(actor, kStopEvent);
			sequence.playing = false;
		}
		if (sequence.direction == Direction::kUndress) {
			for (std::size_t i = 0; i < sequence.index && i < sequence.steps.size(); ++i) {
				PutOn(actor, sequence.steps[i].item, sequence.steps[i].slot);
			}
			Forget(sequence.actor);
		} else {
			for (std::size_t i = sequence.index; i < sequence.steps.size(); ++i) {
				PutOn(actor, sequence.steps[i].item, sequence.steps[i].slot);
			}
			Forget(sequence.actor);
		}
	}

	void Wrap(Sequence& sequence, RE::Actor* actor, bool finished)
	{
		if (sequence.direction == Direction::kDress && actor) {
			Forget(sequence.actor);
		}
		if (sequence.actor == 0x14) {
			if (sequence.direction == Direction::kDress || !finished) {
				RestoreFirstPerson();
			}
		} else if (actor) {
			ResumeHeadTracking(actor, sequence);
		}
	}

	enum class Outcome
	{
		kRunning,
		kFinished,
		kInstant,
		kAborted
	};

	Outcome Advance(Sequence& sequence, float delta)
	{
		auto* actor = FindActor(sequence.actor);
		if (!actor || actor->IsDead() || !actor->Is3DLoaded()) {
			Wrap(sequence, nullptr, false);
			return Outcome::kAborted;
		}
		if (InBed(actor)) {
			if (sequence.direction == Direction::kUndress) {
				FlushRemaining(sequence, actor);
			} else {
				Undo(sequence, actor);
			}
			Wrap(sequence, actor, false);
			return Outcome::kInstant;
		}
		if (sequence.direction == Direction::kUndress && actor->IsPlayerRef()) {
			const bool moving = Bed::PlayerWantsToMove();
			sequence.movementArmed = sequence.movementArmed || !moving;
			if (moving && sequence.movementArmed) {
				Undo(sequence, actor);
				Wrap(sequence, actor, false);
				return Outcome::kAborted;
			}
		}
		if (sequence.direction == Direction::kUndress && actor->IsInCombat()) {
			Undo(sequence, actor);
			Wrap(sequence, actor, false);
			return Outcome::kAborted;
		}
		if (sequence.index >= sequence.steps.size()) {
			Wrap(sequence, actor, true);
			return Outcome::kFinished;
		}
		const auto& step = sequence.steps[sequence.index];
		if (!sequence.playing) {
			if (!step.animation.event) {
				Swap(sequence, actor, step);
				++sequence.index;
				return Outcome::kRunning;
			}
			if (sequence.direction == Direction::kDress) {
				PutOn(actor, step.item, step.slot);
			}
			if (!SendEvent(actor, step.animation.event)) {
				SKSE::log::info("[undress] {:08X} animation event {} not handled, piece swapped instantly", sequence.actor, step.animation.event);
				if (sequence.direction == Direction::kUndress) {
					TakeOff(actor, step.item, step.slot);
				}
				++sequence.index;
				return Outcome::kRunning;
			}
			sequence.playing = true;
			sequence.remaining = step.animation.seconds;
			return Outcome::kRunning;
		}
		sequence.remaining -= delta;
		if (sequence.remaining > 0.0f) {
			return Outcome::kRunning;
		}
		SendEvent(actor, kStopEvent);
		if (sequence.direction == Direction::kUndress) {
			TakeOff(actor, step.item, step.slot);
		}
		sequence.playing = false;
		++sequence.index;
		return Outcome::kRunning;
	}

	void RunSequences(float delta, bool abortUndress)
	{
		std::vector<std::function<void()>> callbacks;
		for (std::size_t i = 0; i < g_sequences.size();) {
			auto&   sequence = g_sequences[i];
			Outcome outcome;
			if (abortUndress && sequence.direction == Direction::kUndress) {
				auto* actor = FindActor(sequence.actor);
				if (actor) {
					Undo(sequence, actor);
				}
				Wrap(sequence, actor, false);
				outcome = Outcome::kAborted;
			} else {
				outcome = Advance(sequence, delta);
			}
			if (outcome == Outcome::kRunning) {
				++i;
				continue;
			}
			const auto actorID = sequence.actor;
			const auto direction = sequence.direction;
			if (outcome == Outcome::kFinished && sequence.onDone) {
				callbacks.push_back(std::move(sequence.onDone));
			}
			g_sequences.erase(g_sequences.begin() + static_cast<std::ptrdiff_t>(i));
			ReleaseGuard(actorID);
			SKSE::log::info("[undress] {:08X} animated {} {}", actorID, Name(direction), outcome == Outcome::kFinished ? "finished" : outcome == Outcome::kInstant ? "cut short, remaining pieces swapped instantly" : "aborted");
		}
		for (auto& callback : callbacks) {
			callback();
		}
	}

	void UndressNow(RE::FormID actorID, std::int32_t mode)
	{
		auto* actor = FindActor(actorID);
		if (!actor || !actor->Is3DLoaded() || !InBed(actor)) {
			return;
		}
		const auto worn = WornEquipment(actor, mode);
		if (worn.empty()) {
			return;
		}
		std::uint32_t refused = 0;
		for (const auto& item : worn) {
			if (!TakeOff(actor, item.object->GetFormID(), item.slot)) {
				++refused;
			}
		}
		SKSE::log::info("[undress] {:08X} mode {}: {} piece(s) taken off, {} queued, {} remembered", actorID, mode, worn.size(), refused, Remembered(actorID));
	}

	void DressNow(RE::FormID actorID, bool animate)
	{
		const auto record = g_records.find(actorID);
		if (record == g_records.end()) {
			return;
		}
		auto* actor = FindActor(actorID);
		if (!actor || !actor->Is3DLoaded()) {
			if (animate) {
				SKSE::log::info("[undress] {:08X} cannot dress yet: actor {}", actorID, actor ? "not loaded" : "not found");
			}
			return;
		}
		if (animate && Eligible(actor) && !FindSequence(actorID)) {
			std::vector<Step> steps;
			for (const auto& remembered : record->second) {
				auto* object = Carried(actor, remembered.form).object;
				if (!object) {
					continue;
				}
				auto*      armor = object->As<RE::TESObjectARMO>();
				const auto piece = armor ? Classify(armor) : Piece::kOther;
				steps.push_back({ remembered.form, DressAnimation(piece), remembered.slot });
			}
			if (StartSequence(actor, Direction::kDress, std::move(steps), nullptr)) {
				return;
			}
		}
		std::uint32_t equipped = 0;
		std::uint32_t missing = 0;
		for (const auto& remembered : record->second) {
			if (PutOn(actor, remembered.form, remembered.slot)) {
				++equipped;
			} else {
				++missing;
			}
		}
		Forget(actorID);
		SKSE::log::info("[undress] {:08X} dressed: {} piece(s) put back, {} no longer carried", actorID, equipped, missing);
	}

	bool LyingNearOccupiedFurniture(RE::Actor* actor)
	{
		auto* process = actor->GetActorRuntimeData().currentProcess;
		if (!process) {
			return false;
		}
		const auto furniture = process->GetOccupiedFurniture().get();
		if (!furniture) {
			return false;
		}
		return actor->GetPosition().GetDistance(furniture->GetPosition()) <= kFurnitureRange;
	}

	void DressIfUp(RE::FormID actorID)
	{
		auto* actor = FindActor(actorID);
		if (!actor || FindSequence(actorID)) {
			g_strandedTicks.erase(actorID);
			return;
		}
		if (!InBed(actor) && !SleepPackage::Holds(actor)) {
			DressNow(actorID, false);
			return;
		}
		if (LyingNearOccupiedFurniture(actor)) {
			g_strandedTicks.erase(actorID);
			return;
		}
		const bool held = SleepPackage::Holds(actor);
		const auto ticks = ++g_strandedTicks[actorID];
		if (ticks < (held ? kAbandonedTicks : kStrandedTicks)) {
			return;
		}
		SKSE::log::info("[undress] {:08X} away from any bed for {} check(s) in sit/sleep state {} (created package={}), dressing", actorID, ticks, static_cast<std::int32_t>(actor->AsActorState()->GetSitSleepState()), held);
		DressNow(actorID, false);
	}

	void DressWhoeverIsUp()
	{
		std::vector<RE::FormID> actors;
		actors.reserve(g_records.size());
		for (const auto& [actorID, items] : g_records) {
			actors.push_back(actorID);
		}
		for (const auto actorID : actors) {
			DressIfUp(actorID);
		}
	}

	void DropSequences()
	{
		g_sequences.clear();
		g_restoreFirstPerson = false;
		if (g_playerGuard) {
			g_playerGuard->value = 0.0f;
		}
		if (g_followerGuard) {
			g_followerGuard->value = 0.0f;
		}
	}

	template <class T>
	bool Read(SKSE::SerializationInterface* intfc, T& value)
	{
		return intfc->ReadRecordData(value) == sizeof(T);
	}

	bool ReadRecord(SKSE::SerializationInterface* intfc, std::uint32_t version)
	{
		std::uint32_t actorCount = 0;
		if (!Read(intfc, actorCount)) {
			return false;
		}
		for (std::uint32_t i = 0; i < actorCount; ++i) {
			RE::FormID    savedActor = 0;
			std::uint32_t itemCount = 0;
			if (!Read(intfc, savedActor) || !Read(intfc, itemCount)) {
				return false;
			}
			RE::FormID actorID = 0;
			const bool actorKnown = intfc->ResolveFormID(savedActor, actorID);
			for (std::uint32_t j = 0; j < itemCount; ++j) {
				RE::FormID savedItem = 0;
				if (!Read(intfc, savedItem)) {
					return false;
				}
				std::uint8_t slot = kSlotDefault;
				if (version >= 2 && !Read(intfc, slot)) {
					return false;
				}
				RE::FormID itemID = 0;
				if (actorKnown && intfc->ResolveFormID(savedItem, itemID)) {
					Remember(actorID, itemID, slot);
				}
			}
		}
		return true;
	}

	void OnSave(SKSE::SerializationInterface* intfc)
	{
		if (!intfc->OpenRecord(kRecordType, kRecordVersion)) {
			SKSE::log::info("[undress] co-save record could not be opened");
			return;
		}
		intfc->WriteRecordData(static_cast<std::uint32_t>(g_records.size()));
		for (const auto& [actorID, items] : g_records) {
			intfc->WriteRecordData(actorID);
			intfc->WriteRecordData(static_cast<std::uint32_t>(items.size()));
			for (const auto& remembered : items) {
				intfc->WriteRecordData(remembered.form);
				intfc->WriteRecordData(remembered.slot);
			}
		}
		SKSE::log::info("[undress] co-save written for {} actor(s)", g_records.size());
	}

	void OnLoad(SKSE::SerializationInterface* intfc)
	{
		g_records.clear();
		g_strandedTicks.clear();
		std::uint32_t type = 0;
		std::uint32_t version = 0;
		std::uint32_t length = 0;
		while (intfc->GetNextRecordInfo(type, version, length)) {
			if (type != kRecordType || (version != 1 && version != 2)) {
				continue;
			}
			if (!ReadRecord(intfc, version)) {
				SKSE::log::info("[undress] co-save record truncated");
				break;
			}
		}
		SyncCount();
		SKSE::log::info("[undress] co-save loaded: {} actor(s) still undressed", g_records.size());
	}

	void OnRevert(SKSE::SerializationInterface*)
	{
		g_records.clear();
		g_strandedTicks.clear();
		SyncCount();
		DropSequences();
		g_reconcileFrames = 0;
	}

	void OnFormDelete(RE::VMHandle handle)
	{
		const auto actorID = static_cast<RE::FormID>(handle & 0xFFFFFFFF);
		if (!g_records.contains(actorID)) {
			return;
		}
		if (RE::TESForm::LookupByID(actorID)) {
			SKSE::log::info("[undress] form delete {:016X} ignored, {:08X} still exists", handle, actorID);
			return;
		}
		SKSE::log::info("[undress] form delete {:016X}, dropping the record for {:08X}", handle, actorID);
		Forget(actorID);
	}
}

namespace Undress
{
	void Install()
	{
		const auto* serialization = SKSE::GetSerializationInterface();
		if (serialization) {
			serialization->SetUniqueID(kUniqueID);
			serialization->SetSaveCallback(OnSave);
			serialization->SetLoadCallback(OnLoad);
			serialization->SetRevertCallback(OnRevert);
			serialization->SetFormDeleteCallback(OnFormDelete);
		}
		const auto& s = Settings::Get();
		SKSE::log::info("[undress] player={} npcs={} animations={} co-save {}", s.undressPlayer, s.undressNpcs, s.undressAnimations, serialization ? "registered" : "unavailable");
	}

	void PrepareAnimations()
	{
		auto* handler = RE::TESDataHandler::GetSingleton();
		g_playerGuard = handler ? handler->LookupForm<RE::TESGlobal>(kPlayerGuardId, kAnimationPlugin) : nullptr;
		g_followerGuard = handler ? handler->LookupForm<RE::TESGlobal>(kFollowerGuardId, kAnimationPlugin) : nullptr;
		g_keywords.armorHelmet = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ArmorHelmet");
		g_keywords.clothingHead = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ClothingHead");
		g_keywords.armorCuirass = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ArmorCuirass");
		g_keywords.clothingBody = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ClothingBody");
		g_animations = g_playerGuard && g_followerGuard;
		SKSE::log::info("[undress] Immersive Equipping Animations {}", g_animations ? "found, equip animations available" : "not loaded, pieces are swapped instantly");
	}

	bool BeginBeforeBed(RE::Actor* actor, std::function<void()> onDone)
	{
		if (!actor || actor->IsChild() || !Eligible(actor)) {
			return false;
		}
		const auto actorID = actor->GetFormID();
		if (g_records.contains(actorID) || FindSequence(actorID)) {
			return false;
		}
		const auto mode = AssignedMode(actor);
		if (mode == kModeOff) {
			return false;
		}
		std::vector<Step> steps;
		for (const auto& item : WornEquipment(actor, mode)) {
			steps.push_back({ item.object->GetFormID(), UndressAnimation(item.piece), item.slot });
		}
		return StartSequence(actor, Direction::kUndress, std::move(steps), std::move(onDone));
	}

	void AbortPending()
	{
		RunSequences(0.0f, true);
	}

	void OnLieDown(RE::Actor* actor)
	{
		if (!actor || actor->IsChild() || actor->IsDead()) {
			return;
		}
		const auto mode = ModeFor(actor);
		if (mode == kModeOff) {
			return;
		}
		const auto actorID = actor->GetFormID();
		Schedule([actorID, mode] {
			UndressNow(actorID, mode);
		});
	}

	void OnGetUp(RE::Actor* actor)
	{
		if (!actor || g_recordCount == 0) {
			return;
		}
		const auto actorID = actor->GetFormID();
		Schedule([actorID] {
			DressNow(actorID, true);
		});
	}

	void Update(float delta)
	{
		if (!g_sequences.empty()) {
			RunSequences(delta, false);
		}
		if (g_records.empty() || ++g_reconcileFrames < kReconcileFrames) {
			return;
		}
		g_reconcileFrames = 0;
		DressWhoeverIsUp();
	}

	void Reset()
	{
		DropSequences();
		g_reconcileFrames = 0;
		Schedule([] {
			DressWhoeverIsUp();
		});
	}
}
