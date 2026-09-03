#pragma once

namespace RE
{
	class Actor;
	class TESObjectREFR;
}

namespace Bed
{
	void Install();

	bool PlayerCanRespond();
	bool IsPlayerLyingInBed();
	bool HasPendingSentence();
	bool ActivationLiesDown();
	bool PlayerWantsToMove();

	bool LieDown(RE::Actor* actor, RE::TESObjectREFR* furniture);
	bool StandUp(RE::Actor* actor);

	void OpenSleepMenu();
	void OfferServeSentence();
}
