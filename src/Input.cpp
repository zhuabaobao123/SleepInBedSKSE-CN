#include "Input.h"

#include "Bed.h"
#include "Settings.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <array>
#include <utility>

namespace
{
	using CanProcessFn = bool(RE::MenuOpenHandler*, RE::InputEvent*);
	using ProcessButtonFn = bool(RE::MenuOpenHandler*, RE::ButtonEvent*);

	REL::Relocation<CanProcessFn>    g_canProcess;
	REL::Relocation<ProcessButtonFn> g_processButton;

	constexpr std::int32_t kGamepadCodeBase = 266;

	constexpr std::array<std::pair<std::uint32_t, std::int32_t>, 16> kGamepadCodes{ {
		{ RE::BSWin32GamepadDevice::Key::kUp, kGamepadCodeBase + 0 },
		{ RE::BSWin32GamepadDevice::Key::kDown, kGamepadCodeBase + 1 },
		{ RE::BSWin32GamepadDevice::Key::kLeft, kGamepadCodeBase + 2 },
		{ RE::BSWin32GamepadDevice::Key::kRight, kGamepadCodeBase + 3 },
		{ RE::BSWin32GamepadDevice::Key::kStart, kGamepadCodeBase + 4 },
		{ RE::BSWin32GamepadDevice::Key::kBack, kGamepadCodeBase + 5 },
		{ RE::BSWin32GamepadDevice::Key::kLeftThumb, kGamepadCodeBase + 6 },
		{ RE::BSWin32GamepadDevice::Key::kRightThumb, kGamepadCodeBase + 7 },
		{ RE::BSWin32GamepadDevice::Key::kLeftShoulder, kGamepadCodeBase + 8 },
		{ RE::BSWin32GamepadDevice::Key::kRightShoulder, kGamepadCodeBase + 9 },
		{ RE::BSWin32GamepadDevice::Key::kA, kGamepadCodeBase + 10 },
		{ RE::BSWin32GamepadDevice::Key::kB, kGamepadCodeBase + 11 },
		{ RE::BSWin32GamepadDevice::Key::kX, kGamepadCodeBase + 12 },
		{ RE::BSWin32GamepadDevice::Key::kY, kGamepadCodeBase + 13 },
		{ RE::BSWin32GamepadDevice::Key::kLeftTrigger, kGamepadCodeBase + 14 },
		{ RE::BSWin32GamepadDevice::Key::kRightTrigger, kGamepadCodeBase + 15 },
	} };

	bool IsSleepKey(const RE::ButtonEvent& event)
	{
		const auto key = Settings::Get().sleepKey;
		if (key < 0) {
			auto* events = RE::UserEvents::GetSingleton();
			return events && event.QUserEvent() == events->wait;
		}
		switch (event.GetDevice()) {
		case RE::INPUT_DEVICE::kKeyboard:
			return static_cast<std::int32_t>(event.GetIDCode()) == key;
		case RE::INPUT_DEVICE::kGamepad:
			return Input::GamepadCode(event.GetIDCode()) == key;
		default:
			return false;
		}
	}

	bool CanProcessWithSleepKey(RE::MenuOpenHandler* handler, RE::InputEvent* event)
	{
		if (event && event->GetEventType() == RE::INPUT_EVENT_TYPE::kButton) {
			const auto* button = event->AsButtonEvent();
			if (button && IsSleepKey(*button)) {
				return true;
			}
		}
		return g_canProcess(handler, event);
	}

	bool ProcessButtonWithSleepKey(RE::MenuOpenHandler* handler, RE::ButtonEvent* event)
	{
		if (event && event->IsDown() && IsSleepKey(*event) && Bed::PlayerCanRespond()) {
			if (Bed::HasPendingSentence()) {
				Bed::OfferServeSentence();
				return true;
			}
			if (Bed::IsPlayerLyingInBed()) {
				Bed::OpenSleepMenu();
				return true;
			}
		}
		return g_processButton(handler, event);
	}

	std::size_t ProcessButtonSlot()
	{
		if (REL::Module::IsVR()) {
			return 8;
		}
		return REL::Module::IsAtLeast(SKSE::RUNTIME_SSE_1_7_99) ? 7 : 5;
	}
}

namespace Input
{
	std::int32_t GamepadCode(std::uint32_t buttonMask)
	{
		for (const auto& [mask, code] : kGamepadCodes) {
			if (mask == buttonMask) {
				return code;
			}
		}
		return -1;
	}

	void Install()
	{
		REL::Relocation<std::uintptr_t> vtable{ RE::VTABLE_MenuOpenHandler[0] };
		const auto                      buttonSlot = ProcessButtonSlot();
		g_canProcess = REL::Relocation<CanProcessFn>{ vtable.write_vfunc(1, CanProcessWithSleepKey) };
		g_processButton = REL::Relocation<ProcessButtonFn>{ vtable.write_vfunc(buttonSlot, ProcessButtonWithSleepKey) };
		SKSE::log::info("[bed input] MenuOpenHandler slots 1 and {} redirected", buttonSlot);
	}
}
