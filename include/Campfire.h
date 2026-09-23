#pragma once

namespace RE
{
	class TESObjectREFR;
}

namespace Campfire
{
	void PrepareBedroll();
	bool OpenSleepMenuInstead(RE::TESObjectREFR* furniture, RE::TESObjectREFR* activator);
}
