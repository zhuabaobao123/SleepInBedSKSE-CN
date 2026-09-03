#include "Detours.h"

#include <MinHook.h>
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

namespace Detours
{
	bool Begin()
	{
		const auto status = MH_Initialize();
		if (status != MH_OK) {
			SKSE::log::error("MinHook initialisation failed: {}", MH_StatusToString(status));
			return false;
		}
		return true;
	}

	bool AttachRaw(REL::VariantID id, void* detour, void** original, std::string_view name)
	{
		const auto target = id.address();
		const auto status = MH_CreateHook(reinterpret_cast<void*>(target), detour, original);
		if (status != MH_OK) {
			SKSE::log::error("[{}] detour at 0x{:X} (offset 0x{:X}) failed: {}", name, target, id.offset(), MH_StatusToString(status));
			return false;
		}
		SKSE::log::info("[{}] detour at 0x{:X} (offset 0x{:X})", name, target, id.offset());
		return true;
	}

	bool Commit()
	{
		const auto status = MH_EnableHook(MH_ALL_HOOKS);
		if (status != MH_OK) {
			SKSE::log::error("Enabling detours failed: {}", MH_StatusToString(status));
			return false;
		}
		return true;
	}
}
