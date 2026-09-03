#include "Bed.h"

#include "BedAccess.h"
#include "Detours.h"
#include "Followers.h"
#include "Hearthfire.h"
#include "ScopedFlag.h"
#include "Settings.h"
#include "SleepPackage.h"
#include "Undress.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

namespace
{
	constexpr std::size_t   kFurnitureActivateSlot = 0x37;
	constexpr std::uint32_t kQueuedMenuFrameLimit = 120;
	constexpr std::uint32_t kSneakExitFrameLimit = 60;
	constexpr std::int32_t  kActionPriority = 2;

	constexpr REL::VariantID kAnimationPauseTimer{ 38895, 39941, 0x688EE0 };
	constexpr REL::VariantID kSitSleepStateChanged{ 38871, 39912, 0x6878D0 };
	constexpr REL::VariantID kPlayerAction{ 41271, 42350, 0x72C380 };

	thread_local bool tl_directActivation = false;
	thread_local bool tl_ignoreSneak = false;

	bool                g_sleepMenuQueued = false;
	RE::ObjectRefHandle g_queuedBed;
	std::uint32_t       g_queuedFrames = 0;
	bool                g_lieDownAfterSneak = false;
	RE::ObjectRefHandle g_bedAfterSneak;
	std::uint32_t       g_sneakExitFrames = 0;

	using ActivateButtonFn = void(RE::ActivateHandler*, RE::ButtonEvent*, RE::PlayerControlsData*);
	using FurnitureActivateFn = bool(RE::TESFurniture*, RE::TESObjectREFR*, RE::TESObjectREFR*, std::uint8_t, RE::TESBoundObject*, std::int32_t);
	using PlayerUpdateFn = void(RE::PlayerCharacter*, float);
	using AnimationPauseTimerFn = void(RE::AIProcess*, float);
	using SitSleepStateChangedFn = std::uint64_t(RE::AIProcess*, RE::Actor*, std::int32_t, void*, std::int32_t);
	using PlayerActionFn = bool(RE::PlayerControls*, std::int32_t, std::int32_t);

	REL::Relocation<ActivateButtonFn>    g_activateButton;
	REL::Relocation<PlayerUpdateFn>      g_playerUpdate;
	REL::Relocation<FurnitureActivateFn> g_furnitureActivate;
	REL::Relocation<PlayerActionFn>      g_playerAction;

	AnimationPauseTimerFn*  g_animationPauseTimer = nullptr;
	SitSleepStateChangedFn* g_sitSleepStateChanged = nullptr;

	RE::PlayerCharacter* Player()
	{
		return RE::PlayerCharacter::GetSingleton();
	}

	bool OwnedByPlayer(RE::AIProcess* process)
	{
		auto* player = Player();
		return player && process && process == player->GetActorRuntimeData().currentProcess;
	}

	void OpenEyes(RE::Actor* actor)
	{
		auto* face = actor->GetFaceGenAnimationData();
		if (!face) {
			return;
		}
		const float blink = face->modifier3.count > 0 ? face->modifier3.values[0] : -1.0f;
		SKSE::log::info("[eyes] {:08X} on wake: hold={} stage={} timer={:.2f} blink={:.2f}", actor->GetFormID(), face->unk21A, static_cast<std::uint32_t>(face->eyesBlinkingStage), face->eyesBlinkingTimer, blink);
		face->unk21A = 0;
		face->Reset(0.0f, false, true, false, false);
		face->eyesBlinkingStage = RE::BSFaceGenAnimationData::EyesBlinkingStage::BlinkDelay;
		face->eyesBlinkingTimer = 0.0f;
	}

	void QueueSleepMenu(RE::TESObjectREFR* bed)
	{
		g_sleepMenuQueued = true;
		g_queuedBed = bed->GetHandle();
		g_queuedFrames = 0;
	}

	void DropQueuedSleepMenu()
	{
		g_sleepMenuQueued = false;
		g_queuedBed = RE::ObjectRefHandle();
	}

	void OpenQueuedSleepMenu(RE::AIProcess* process)
	{
		const bool sameBed = process->GetOccupiedFurniture() == g_queuedBed;
		DropQueuedSleepMenu();
		if (sameBed && Bed::PlayerCanRespond() && !Bed::HasPendingSentence()) {
			SKSE::log::info("[sleep menu on lie down] opening");
			Bed::OpenSleepMenu();
		}
	}

	bool WantsToMove()
	{
		auto* controls = RE::PlayerControls::GetSingleton();
		if (!controls) {
			return false;
		}
		const auto& input = controls->data.moveInputVec;
		return input.x != 0.0f || input.y != 0.0f;
	}

	void ActivateButtonWithContext(RE::ActivateHandler* handler, RE::ButtonEvent* event, RE::PlayerControlsData* data)
	{
		ScopedFlag context(tl_directActivation);
		g_activateButton(handler, event, data);
	}

	void LeaveSneakBeforeLyingDown(RE::TESObjectREFR* bed)
	{
		g_lieDownAfterSneak = true;
		g_bedAfterSneak = bed->GetHandle();
		g_sneakExitFrames = 0;
		auto* controls = RE::PlayerControls::GetSingleton();
		if (controls && g_playerAction.address()) {
			g_playerAction(controls, static_cast<std::int32_t>(RE::DEFAULT_OBJECT::kActionSneak), kActionPriority);
		}
		SKSE::log::info("[sneak] leaving sneak before lying down in {:08X}", bed->GetFormID());
	}

	void LieDownAfterSneak(RE::PlayerCharacter* player)
	{
		if (player->IsSneaking() && ++g_sneakExitFrames <= kSneakExitFrameLimit) {
			return;
		}
		g_lieDownAfterSneak = false;
		auto bedPtr = g_bedAfterSneak.get();
		g_bedAfterSneak = RE::ObjectRefHandle();
		auto* bed = bedPtr.get();
		if (!bed || player->AsActorState()->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) {
			return;
		}
		SKSE::log::info("[sneak] {} after {} frames, lying down in {:08X}", player->IsSneaking() ? "still sneaking" : "standing", g_sneakExitFrames, bed->GetFormID());
		ScopedFlag ignore(tl_ignoreSneak);
		ScopedFlag context(tl_directActivation);
		bed->ActivateRef(player, 0, nullptr, 1, false);
	}

	void LieDownAfterUndress(RE::ObjectRefHandle handle)
	{
		auto  bedPtr = handle.get();
		auto* bed = bedPtr.get();
		auto* player = Player();
		if (!bed || !player || player->AsActorState()->GetSitSleepState() != RE::SIT_SLEEP_STATE::kNormal) {
			return;
		}
		SKSE::log::info("[undress] player undressed, lying down in {:08X}", bed->GetFormID());
		ScopedFlag ignore(tl_ignoreSneak);
		ScopedFlag context(tl_directActivation);
		bed->ActivateRef(player, 0, nullptr, 1, false);
	}

	void PlayerUpdateWithStandUp(RE::PlayerCharacter* player, float delta)
	{
		g_playerUpdate(player, delta);

		if (!player) {
			return;
		}
		Undress::Update(delta);
		const auto state = player->AsActorState()->GetSitSleepState();
		if (g_lieDownAfterSneak) {
			LieDownAfterSneak(player);
		}
		if (g_sleepMenuQueued && state == RE::SIT_SLEEP_STATE::kNormal && ++g_queuedFrames > kQueuedMenuFrameLimit) {
			DropQueuedSleepMenu();
		}
		if (state != RE::SIT_SLEEP_STATE::kIsSleeping) {
			return;
		}
		Followers::Tick();
		if (player->IsOnMount() || !WantsToMove()) {
			return;
		}
		auto* process = player->GetActorRuntimeData().currentProcess;
		if (!process) {
			return;
		}
		if (auto furniture = process->GetOccupiedFurniture().get(); furniture) {
			furniture->ActivateRef(player, 0, nullptr, 1, false);
		}
	}

	bool ActivateWithSleepChecks(RE::TESFurniture* furniture, RE::TESObjectREFR* target, RE::TESObjectREFR* activator, std::uint8_t flag, RE::TESBoundObject* object, std::int32_t count)
	{
		auto*      player = Player();
		const bool playerFromStanding = player && activator == player && player->AsActorState()->GetSitSleepState() == RE::SIT_SLEEP_STATE::kNormal;
		if (playerFromStanding && tl_directActivation) {
			const bool bed = BedAccess::IsBed(target);
			const bool allowed = !bed || BedAccess::AllowsSleep(player, target);
			SKSE::log::info("[bed availability] furniture {:08X} bed={} allowed={}", target ? target->GetFormID() : 0, bed, allowed);
			if (!allowed) {
				return true;
			}
			if (bed && !tl_ignoreSneak && player->IsSneaking()) {
				LeaveSneakBeforeLyingDown(target);
				return true;
			}
			if (bed && Undress::BeginBeforeBed(player, [handle = target->GetHandle()] { LieDownAfterUndress(handle); })) {
				SKSE::log::info("[undress] player undresses before lying down in {:08X}", target->GetFormID());
				return true;
			}
			if (bed && Settings::Get().openSleepMenuOnLieDown) {
				QueueSleepMenu(target);
			}
		}
		Hearthfire::OpenLidOnExit(target, activator);
		return g_furnitureActivate(furniture, target, activator, flag, object, count);
	}

	void AnimationPauseTimer(RE::AIProcess* process, float seconds)
	{
		if (OwnedByPlayer(process)) {
			return;
		}
		g_animationPauseTimer(process, seconds);
	}

	std::uint64_t SitSleepStateChanged(RE::AIProcess* process, RE::Actor* actor, std::int32_t state, void* extra, std::int32_t flags)
	{
		const bool wasSleeping = actor && actor->AsActorState()->GetSitSleepState() == RE::SIT_SLEEP_STATE::kIsSleeping;
		if (actor && process && !OwnedByPlayer(process)) {
			auto*      package = process->currentPackage.package;
			const bool created = SleepPackage::IsCreated(package);
			if (created || wasSleeping) {
				SKSE::log::info("[followers] {:08X} sit/sleep state {} -> {} package={:X} form={:08X} type={} created={} index={} lowFlags={:02X}", actor->GetFormID(), static_cast<std::int32_t>(actor->AsActorState()->GetSitSleepState()), state, reinterpret_cast<std::uintptr_t>(package), package ? package->GetFormID() : 0, package ? static_cast<std::int32_t>(package->packData.packType.get()) : -1, created, process->currentPackage.currentProcedureIndex, static_cast<std::uint32_t>(process->lowProcessFlags.underlying()));
			}
		}
		const auto result = g_sitSleepStateChanged(process, actor, state, extra, flags);
		if (wasSleeping && state != static_cast<std::int32_t>(RE::SIT_SLEEP_STATE::kIsSleeping)) {
			OpenEyes(actor);
		}
		if (OwnedByPlayer(process) && process->middleHigh) {
			process->middleHigh->unk2D0 = 0.0f;
		}
		if (g_sleepMenuQueued && OwnedByPlayer(process)) {
			if (state == static_cast<std::int32_t>(RE::SIT_SLEEP_STATE::kIsSleeping)) {
				OpenQueuedSleepMenu(process);
			} else if (state < static_cast<std::int32_t>(RE::SIT_SLEEP_STATE::kWantToSleep)) {
				DropQueuedSleepMenu();
			}
		}
		if (OwnedByPlayer(process)) {
			if (state == static_cast<std::int32_t>(RE::SIT_SLEEP_STATE::kIsSleeping)) {
				Followers::OnPlayerLayDown(process->GetOccupiedFurniture().get().get());
			} else if (state == static_cast<std::int32_t>(RE::SIT_SLEEP_STATE::kWantToWake) || state < static_cast<std::int32_t>(RE::SIT_SLEEP_STATE::kWantToSleep)) {
				Followers::OnPlayerStoodUp();
			}
		}
		if (actor) {
			if (state == static_cast<std::int32_t>(RE::SIT_SLEEP_STATE::kWantToSleep)) {
				Undress::OnLieDown(actor);
			} else if (state < static_cast<std::int32_t>(RE::SIT_SLEEP_STATE::kWantToSleep)) {
				Undress::OnGetUp(actor);
			}
		}
		return result;
	}

	std::size_t ActivateButtonSlot()
	{
		return REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_7_99) ? 6 : 4;
	}

	std::size_t PlayerUpdateSlot()
	{
		return REL::Module::IsVR() ? 0xAE : 0xAD;
	}

	void SentenceChoice(std::uint8_t choice)
	{
		if (choice == 1) {
			if (auto* player = Player()) {
				player->ServePrisonTime();
			}
		}
	}

	void InstallVirtualHooks()
	{
		REL::Relocation<std::uintptr_t> activateHandler{ RE::VTABLE_ActivateHandler[0] };
		const auto                      buttonSlot = ActivateButtonSlot();
		g_activateButton = REL::Relocation<ActivateButtonFn>{ activateHandler.write_vfunc(buttonSlot, ActivateButtonWithContext) };
		SKSE::log::info("[direct activation context] ActivateHandler slot {} redirected", buttonSlot);

		REL::Relocation<std::uintptr_t> playerCharacter{ RE::VTABLE_PlayerCharacter[0] };
		const auto                      updateSlot = PlayerUpdateSlot();
		g_playerUpdate = REL::Relocation<PlayerUpdateFn>{ playerCharacter.write_vfunc(updateSlot, PlayerUpdateWithStandUp) };
		SKSE::log::info("[stand up with movement] PlayerCharacter slot {:#x} redirected", updateSlot);

		REL::Relocation<std::uintptr_t> furniture{ RE::VTABLE_TESFurniture[0] };
		g_furnitureActivate = REL::Relocation<FurnitureActivateFn>{ furniture.write_vfunc(kFurnitureActivateSlot, ActivateWithSleepChecks) };
		SKSE::log::info("[bed availability checks] TESFurniture slot {:#x} redirected", kFurnitureActivateSlot);

		g_playerAction = REL::Relocation<PlayerActionFn>{ kPlayerAction };
		SKSE::log::info("[sneak] player action dispatch at {:#x}", g_playerAction.address());
	}

	void InstallDetours()
	{
		Detours::Attach(kAnimationPauseTimer, &AnimationPauseTimer, g_animationPauseTimer, "animation pause timer");
		Detours::Attach(kSitSleepStateChanged, &SitSleepStateChanged, g_sitSleepStateChanged, "sit sleep state change");
	}
}

namespace Bed
{
	void Install()
	{
		InstallVirtualHooks();
		InstallDetours();
	}

	bool PlayerCanRespond()
	{
		auto* ui = RE::UI::GetSingleton();
		auto* player = Player();
		if (!ui || !player) {
			return false;
		}
		if (ui->numPausesGame > 0 || ui->IsMenuOpen(RE::FaderMenu::MENU_NAME)) {
			return false;
		}
		const auto life = player->AsActorState()->GetLifeState();
		return life != RE::ACTOR_LIFE_STATE::kDying && life != RE::ACTOR_LIFE_STATE::kDead;
	}

	bool IsPlayerLyingInBed()
	{
		auto* player = Player();
		return player && player->AsActorState()->GetSitSleepState() == RE::SIT_SLEEP_STATE::kIsSleeping;
	}

	bool HasPendingSentence()
	{
		auto* player = Player();
		if (!player) {
			return false;
		}
		return player->GetPlayerRuntimeData().jailSentence > 0 && !player->GetPlayerFlags().escaping;
	}

	bool ActivationLiesDown()
	{
		return tl_directActivation;
	}

	bool PlayerWantsToMove()
	{
		return WantsToMove();
	}

	bool LieDown(RE::Actor* actor, RE::TESObjectREFR* furniture)
	{
		if (!actor || !furniture) {
			return false;
		}
		if (actor != Player()) {
			return SleepPackage::Send(actor, furniture);
		}
		ScopedFlag context(tl_directActivation);
		return furniture->ActivateRef(actor, 0, nullptr, 1, false);
	}

	bool StandUp(RE::Actor* actor)
	{
		auto* process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
		if (!process) {
			return false;
		}
		if (actor != Player()) {
			return SleepPackage::Release(actor);
		}
		const bool inFurniture = static_cast<bool>(process->GetOccupiedFurniture());
		if (auto furniture = process->GetOccupiedFurniture().get(); furniture) {
			furniture->ActivateRef(actor, 0, nullptr, 1, false);
		}
		return inFurniture;
	}

	void OpenSleepMenu()
	{
		RE::SleepWaitMenu::ToggleOpenMenu(true);
	}

	void OfferServeSentence()
	{
		auto* collection = RE::GameSettingCollection::GetSingleton();
		if (!collection) {
			return;
		}
		auto* question = collection->GetSetting("sServeSentenceQuestion");
		auto* yes = collection->GetSetting("sYes");
		auto* no = collection->GetSetting("sNo");
		if (!question || !yes || !no) {
			return;
		}
		RE::MessageBoxMenu::Create(question->GetString(), &SentenceChoice, 1, 0x19, 4, yes->GetString(), no->GetString());
	}
}
