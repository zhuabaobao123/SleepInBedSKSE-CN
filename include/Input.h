#pragma once

#include <cstdint>

namespace Input
{
	// Maps a BSWin32GamepadDevice button mask to the mod's gamepad key codes
	// (266 = D-pad up .. 281 = right trigger), or -1 for unknown buttons.
	std::int32_t GamepadCode(std::uint32_t buttonMask);

	void Install();
}
