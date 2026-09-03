#pragma once

#include <functional>

namespace RE
{
	class Actor;
}

namespace Undress
{
	void Install();
	void PrepareAnimations();

	bool BeginBeforeBed(RE::Actor* actor, std::function<void()> onDone);
	void AbortPending();
	void OnLieDown(RE::Actor* actor);
	void OnGetUp(RE::Actor* actor);
	void Update(float delta);
	void Reset();
}
