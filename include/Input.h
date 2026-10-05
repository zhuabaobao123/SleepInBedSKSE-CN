#pragma once

#include <cstdint>

namespace Input
{
	std::int32_t GamepadCode(std::uint32_t buttonMask);

	void Install();
}
