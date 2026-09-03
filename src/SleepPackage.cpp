#include "SleepPackage.h"

#include "Detours.h"
#include "ScopedFlag.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <cstring>
#include <intrin.h>

namespace
{
	constexpr std::uint32_t kBedLocationRadius = 128;
	constexpr auto          kMustReachLocation = static_cast<RE::PACKAGE_DATA::GeneralFlag>(1 << 1);

	constexpr REL::VariantID kProcedureSucceeded{ 28726, 29479, 0x4440E0 };
	constexpr REL::VariantID kProcedureFailed{ 28727, 29480, 0x444100 };
	constexpr REL::VariantID kEvaluatePackage{ 36407, 37401, 0x5E3990 };
	constexpr REL::VariantID kProcedureAdvance{ 38198, 39158, 0x64D950 };
	constexpr REL::VariantID kPackageSelection{ 38253, 39213, 0x650AD0 };

	using ProcedureFinishFn = void(void**);
	using EvaluatePackageFn = void(RE::Actor*, bool, bool);
	using ProcedureAdvanceFn = void(RE::AIProcess*, RE::Actor*, std::int32_t);
	using PackageSelectionFn = bool(RE::AIProcess*, RE::Actor*, bool);

	ProcedureFinishFn*  g_procedureSucceeded = nullptr;
	ProcedureFinishFn*  g_procedureFailed = nullptr;
	EvaluatePackageFn*  g_evaluatePackage = nullptr;
	ProcedureAdvanceFn* g_procedureAdvance = nullptr;
	PackageSelectionFn* g_packageSelection = nullptr;

	thread_local bool tl_releasing = false;

	bool WithinGameModule(std::uintptr_t address)
	{
		const auto        base = REL::Module::get().base();
		static const auto size = [](std::uintptr_t moduleBase) -> std::uintptr_t {
			const auto headerOffset = *reinterpret_cast<const std::int32_t*>(moduleBase + 0x3C);
			return *reinterpret_cast<const std::uint32_t*>(moduleBase + headerOffset + 0x50);
		}(base);
		return address >= base && address < base + size;
	}

	RE::PlayerCharacter* Player()
	{
		return RE::PlayerCharacter::GetSingleton();
	}

	bool IsCreatedSleepPackage(RE::TESPackage* package)
	{
		return package && package->packData.packType == RE::PACKAGE_TYPE::kSleep && package->packData.packFlags.any(RE::PACKAGE_DATA::GeneralFlag::kCreated);
	}

	RE::TESPackage* CurrentPackage(RE::Actor* actor)
	{
		auto* process = actor ? actor->GetActorRuntimeData().currentProcess : nullptr;
		return process ? process->currentPackage.package : nullptr;
	}

	RE::PackageLocation* PackageLocationFor(RE::TESPackage* package)
	{
		if (!package->packLoc) {
			auto* location = RE::malloc<RE::PackageLocation>(sizeof(RE::PackageLocation));
			if (!location) {
				return nullptr;
			}
			std::memset(static_cast<void*>(location), 0, sizeof(RE::PackageLocation));
			REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_PackageLocation[0] };
			*reinterpret_cast<std::uintptr_t*>(location) = vtable.address();
			package->packLoc = location;
		}
		return package->packLoc;
	}

	void LogProcedureFinish(void** context, const char* outcome)
	{
		auto* actor = context && context[0] ? *static_cast<RE::Actor**>(context[0]) : nullptr;
		if (!actor || actor == Player()) {
			return;
		}
		auto* package = CurrentPackage(actor);
		if (!IsCreatedSleepPackage(package)) {
			return;
		}
		SKSE::log::info("[followers] {:08X} sleep procedure {} in sit/sleep state {} package={:X}", actor->GetFormID(), outcome, static_cast<std::int32_t>(actor->AsActorState()->GetSitSleepState()), reinterpret_cast<std::uintptr_t>(package));
	}

	void ProcedureSucceeded(void** context)
	{
		LogProcedureFinish(context, "succeeded");
		g_procedureSucceeded(context);
	}

	void ProcedureFailed(void** context)
	{
		LogProcedureFinish(context, "failed");
		g_procedureFailed(context);
	}

	void EvaluatePackageLogged(RE::Actor* actor, bool immediate, bool resetAI)
	{
		if (actor && actor != Player() && IsCreatedSleepPackage(CurrentPackage(actor))) {
			const auto returnAddress = reinterpret_cast<std::uintptr_t>(_ReturnAddress());
			const auto caller = returnAddress - REL::Module::get().base();
			const auto state = actor->AsActorState()->GetSitSleepState();
			if (!tl_releasing && state >= RE::SIT_SLEEP_STATE::kWantToSleep && !WithinGameModule(returnAddress)) {
				SKSE::log::info("[followers] {:08X} EvaluatePackage(immediate={}, resetAI={}) from +{:X} swallowed while still in bed in sit/sleep state {}", actor->GetFormID(), immediate, resetAI, caller, static_cast<std::int32_t>(state));
				return;
			}
			SKSE::log::info("[followers] {:08X} EvaluatePackage(immediate={}, resetAI={}) from +{:X} while holding the created sleep package in sit/sleep state {}", actor->GetFormID(), immediate, resetAI, caller, static_cast<std::int32_t>(state));
		}
		g_evaluatePackage(actor, immediate, resetAI);
	}

	void ProcedureAdvanceLogged(RE::AIProcess* process, RE::Actor* actor, std::int32_t delta)
	{
		if (process && actor && actor != Player() && IsCreatedSleepPackage(process->currentPackage.package)) {
			const auto caller = reinterpret_cast<std::uintptr_t>(_ReturnAddress()) - REL::Module::get().base();
			SKSE::log::info("[followers] {:08X} procedure index {} {:+} from +{:X} in sit/sleep state {} lowFlags={:02X}", actor->GetFormID(), process->currentPackage.currentProcedureIndex, delta, caller, static_cast<std::int32_t>(actor->AsActorState()->GetSitSleepState()), static_cast<std::uint32_t>(process->lowProcessFlags.underlying()));
		}
		g_procedureAdvance(process, actor, delta);
	}

	bool KeepsCreatedSleepPackage(RE::AIProcess* process, RE::Actor* actor)
	{
		auto* package = process ? process->currentPackage.package : nullptr;
		if (!actor || actor == Player() || !IsCreatedSleepPackage(package)) {
			return false;
		}
		return package->packData.packFlags.any(RE::PACKAGE_DATA::GeneralFlag::kMustComplete) && actor->AsActorState()->GetSitSleepState() >= RE::SIT_SLEEP_STATE::kWantToSleep;
	}

	bool SelectPackage(RE::AIProcess* process, RE::Actor* actor, bool force)
	{
		if (KeepsCreatedSleepPackage(process, actor)) {
			SKSE::log::info("[followers] {:08X} package evaluation skipped while in bed (force={})", actor->GetFormID(), force);
			return false;
		}
		return g_packageSelection(process, actor, force);
	}
}

namespace SleepPackage
{
	void Install()
	{
		Detours::Attach(kProcedureSucceeded, &ProcedureSucceeded, g_procedureSucceeded, "procedure success log");
		Detours::Attach(kProcedureFailed, &ProcedureFailed, g_procedureFailed, "procedure failure log");
		Detours::Attach(kEvaluatePackage, &EvaluatePackageLogged, g_evaluatePackage, "package evaluation guard");
		Detours::Attach(kProcedureAdvance, &ProcedureAdvanceLogged, g_procedureAdvance, "procedure index log");
		Detours::Attach(kPackageSelection, &SelectPackage, g_packageSelection, "sleep package protection");
	}

	bool IsCreated(RE::TESPackage* package)
	{
		return IsCreatedSleepPackage(package);
	}

	bool Holds(RE::Actor* actor)
	{
		return IsCreatedSleepPackage(CurrentPackage(actor));
	}

	bool Send(RE::Actor* actor, RE::TESObjectREFR* furniture)
	{
		auto* process = actor && furniture ? actor->GetActorRuntimeData().currentProcess : nullptr;
		if (!process) {
			return false;
		}
		auto* package = RE::TESPackage::CreatePackage(RE::PACKAGE_PROCEDURE_TYPE::kSleep);
		if (!package) {
			return false;
		}
		auto* location = PackageLocationFor(package);
		if (!location) {
			return false;
		}
		package->packData.packFlags.reset(kMustReachLocation);
		package->packData.packFlags.set(RE::PACKAGE_DATA::GeneralFlag::kMustComplete);
		location->locType = RE::PackageLocation::Type::kNearReference;
		location->rad = kBedLocationRadius;
		location->data.refHandle = furniture->GetHandle();
		package->procedureType = RE::PACKAGE_PROCEDURE_TYPE::kSleep;
		actor->PutCreatedPackage(package, false, true, false);
		const auto& sched = package->packSched.psData;
		SKSE::log::info("[followers] {:08X} created package {:X}: current={} flags={:08X} type={} procedure={} schedule month={} day={} date={} hour={} minute={} duration={} index={} start={:.3f}", actor->GetFormID(), reinterpret_cast<std::uintptr_t>(package), process->currentPackage.package == package, package->packData.packFlags.underlying(), static_cast<std::int32_t>(package->packData.packType.get()), package->procedureType.underlying(), sched.month, static_cast<std::int32_t>(sched.dayOfWeek.get()), sched.date, sched.hour, sched.minute, sched.duration, process->currentPackage.currentProcedureIndex, process->currentPackage.packageStartTime);
		return process->currentPackage.package == package;
	}

	bool Release(RE::Actor* actor)
	{
		auto* package = CurrentPackage(actor);
		if (!IsCreatedSleepPackage(package)) {
			return false;
		}
		package->packData.packFlags.reset(RE::PACKAGE_DATA::GeneralFlag::kMustComplete);
		ScopedFlag releasing(tl_releasing);
		actor->EvaluatePackage(true, false);
		return true;
	}
}
