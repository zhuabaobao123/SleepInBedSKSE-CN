#pragma once

#include <REL/ID.h>

#include <cstdint>
#include <string_view>

namespace Detours
{
	bool Begin();
	bool AttachRaw(REL::VariantID id, void* detour, void** original, std::string_view name);
	bool Commit();

	template <class F>
	bool Attach(REL::VariantID id, F* detour, F*& original, std::string_view name)
	{
		return AttachRaw(id, reinterpret_cast<void*>(detour), reinterpret_cast<void**>(&original), name);
	}
}
