#pragma once

#include <cstdint>
#include <functional>

namespace RE
{
	class Actor;
}

namespace Undress
{
	void Install();
	void PrepareAnimations();
	bool AnimationsInstalled();

	bool BeginBeforeBed(RE::Actor* actor, std::function<void()> onDone);
	void AbortPending();

	void RememberDonned(std::uint32_t actorID, std::uint32_t itemID);
	bool WasDonned(std::uint32_t actorID, std::uint32_t itemID);

	void OnLieDown(RE::Actor* actor);
	void OnGetUp(RE::Actor* actor);
	void Update(float delta);
	void Reset();
}
