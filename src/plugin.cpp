// Plugin entry point (not part of the author's source package, recreated
// from the module interfaces for this CN build).

#include "Bed.h"
#include "BedAccess.h"
#include "Detours.h"
#include "Followers.h"
#include "Hearthfire.h"
#include "Input.h"
#include "Menu.h"
#include "ScriptedMove.h"
#include "Settings.h"
#include "SleepPackage.h"
#include "Undress.h"

namespace
{
	void OnMessage(SKSE::MessagingInterface::Message* a_msg)
	{
		if (!a_msg) {
			return;
		}
		switch (a_msg->type) {
		case SKSE::MessagingInterface::kDataLoaded:
			// Form lookups need the data handler ready.
			Hearthfire::PrepareCoffin();
			Undress::PrepareAnimations();
			break;
		case SKSE::MessagingInterface::kNewGame:
		case SKSE::MessagingInterface::kPreLoadGame:
			// Drop per-game state before a fresh or loaded game starts.
			Followers::Reset();
			Undress::Reset();
			break;
		default:
			break;
		}
	}
}

SKSEPluginLoad(const SKSE::LoadInterface* a_skse)
{
	SKSE::Init(a_skse);

	Settings::Load();
	Settings::ApplyLogLevel();
	Settings::LogSummary();

	if (!Detours::Begin()) {
		SKSE::log::error("SleepInBedSKSE failed to initialize MinHook, aborting load");
		return false;
	}

	Bed::Install();
	BedAccess::Install();
	Followers::Install();
	Input::Install();
	Menu::Install();
	ScriptedMove::Install();
	SleepPackage::Install();
	Undress::Install();

	Localization::Load();

	if (!Detours::Commit()) {
		SKSE::log::error("SleepInBedSKSE failed to commit hooks, aborting load");
		return false;
	}

	if (const auto messaging = SKSE::GetMessagingInterface()) {
		messaging->RegisterListener(OnMessage);
	}

	SKSE::log::info("SleepInBedSKSE loaded");
	return true;
}
