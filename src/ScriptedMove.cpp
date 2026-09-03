#include "ScriptedMove.h"

#include "Detours.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <new>
#include <unordered_map>

namespace
{
	constexpr REL::VariantID    kMoveFunctorExecute{ 55572, 56101, 0x9CAF50 };
	constexpr std::size_t       kRequeueSlot = 3;
	constexpr int           kMaxDeferrals = 3;

	using ExecuteFn = RE::BSScript::Variable*(RE::SkyrimScript::MoveToFunctor*, RE::BSScript::Variable*);
	using RequeueFn = bool(RE::SkyrimScript::DelayFunctor*);

	ExecuteFn*                 g_execute = nullptr;
	REL::Relocation<RequeueFn> g_requeue;

	std::unordered_map<const void*, int> g_deferrals;
	thread_local const void*             tl_deferredFunctor = nullptr;

	bool MovesSleepingPlayer(RE::SkyrimScript::MoveToFunctor* functor)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || functor->source.get().get() != player) {
			return false;
		}
		const auto state = player->AsActorState()->GetSitSleepState();
		return state >= RE::SIT_SLEEP_STATE::kWantToSleep && state <= RE::SIT_SLEEP_STATE::kWantToWake;
	}

	RE::BSScript::Variable* ExecuteAfterLeavingBed(RE::SkyrimScript::MoveToFunctor* functor, RE::BSScript::Variable* result)
	{
		if (MovesSleepingPlayer(functor)) {
			auto& attempts = g_deferrals[functor];
			if (attempts < kMaxDeferrals) {
				++attempts;
				RE::PlayerCharacter::GetSingleton()->StopInteractingQuick(true);
				SKSE::log::info("[scripted move] player left the bed ahead of a script move, attempt {}", attempts);
				tl_deferredFunctor = functor;
				return new (result) RE::BSScript::Variable();
			}
		}
		g_deferrals.erase(functor);
		return g_execute(functor, result);
	}

	bool RequeueWhenDeferred(RE::SkyrimScript::DelayFunctor* functor)
	{
		if (tl_deferredFunctor == functor) {
			tl_deferredFunctor = nullptr;
			return true;
		}
		return g_requeue(functor);
	}
}

namespace ScriptedMove
{
	void Install()
	{
		Detours::Attach(kMoveFunctorExecute, &ExecuteAfterLeavingBed, g_execute, "scripted move execution");

		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_SkyrimScript____MoveToFunctor[0] };
		g_requeue = REL::Relocation<RequeueFn>{ vtable.write_vfunc(kRequeueSlot, RequeueWhenDeferred) };
		SKSE::log::info("[scripted move] MoveToFunctor slot {} redirected", kRequeueSlot);
	}
}
