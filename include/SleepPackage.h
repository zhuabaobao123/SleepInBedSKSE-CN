#pragma once

namespace RE
{
	class Actor;
	class TESObjectREFR;
	class TESPackage;
}

namespace SleepPackage
{
	void Install();

	bool IsCreated(RE::TESPackage* package);
	bool Holds(RE::Actor* actor);
	bool Send(RE::Actor* actor, RE::TESObjectREFR* furniture);
	bool Release(RE::Actor* actor);
}
