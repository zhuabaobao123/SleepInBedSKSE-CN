#pragma once

namespace RE
{
	class TESObjectREFR;
}

namespace Followers
{
	void Install();

	void OnPlayerLayDown(RE::TESObjectREFR* bed);
	void OnPlayerStoodUp();
	void Tick();
	void Reset();
}
