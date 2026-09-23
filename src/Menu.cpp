#include "Menu.h"

#include "Input.h"
#include "Settings.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>
#include <iterator>

#include <SKSEMenuFramework.h>

namespace
{
	namespace ui = ImGuiMCP;

	constexpr auto  kSection = "Sleep In Bed";
	constexpr auto  kPage = "Settings";
	constexpr auto  kIniPath = "Data/SKSE/Plugins/SleepInBedSKSE.ini";
	constexpr float kMinRadius = 128.0f;
	constexpr float kMaxRadius = 8192.0f;
	constexpr float kMaxHour = 23.99f;
	constexpr float kItemWidth = 260.0f;

	constexpr std::uint32_t kEscapeScancode = 1;
	constexpr std::int32_t  kNoCapturedKey = INT32_MIN;
	constexpr std::int32_t  kGamepadCodeBase = 266;

	constexpr ui::ImVec4 kAccentColor{ 0.55f, 0.85f, 1.0f, 1.0f };

	struct KeyName
	{
		std::int32_t code;
		const char*  name;
	};

	constexpr KeyName kKeyNames[] = {
		{ 1, "Escape" }, { 2, "1" }, { 3, "2" }, { 4, "3" }, { 5, "4" }, { 6, "5" }, { 7, "6" }, { 8, "7" }, { 9, "8" }, { 10, "9" }, { 11, "0" },
		{ 12, "-" }, { 13, "=" }, { 14, "Backspace" }, { 15, "Tab" },
		{ 16, "Q" }, { 17, "W" }, { 18, "E" }, { 19, "R" }, { 20, "T" }, { 21, "Y" }, { 22, "U" }, { 23, "I" }, { 24, "O" }, { 25, "P" },
		{ 26, "[" }, { 27, "]" }, { 28, "Enter" }, { 29, "Left Ctrl" },
		{ 30, "A" }, { 31, "S" }, { 32, "D" }, { 33, "F" }, { 34, "G" }, { 35, "H" }, { 36, "J" }, { 37, "K" }, { 38, "L" },
		{ 39, ";" }, { 40, "'" }, { 41, "`" }, { 42, "Left Shift" }, { 43, "\\" },
		{ 44, "Z" }, { 45, "X" }, { 46, "C" }, { 47, "V" }, { 48, "B" }, { 49, "N" }, { 50, "M" }, { 51, "," }, { 52, "." }, { 53, "/" },
		{ 54, "Right Shift" }, { 55, "Numpad *" }, { 56, "Left Alt" }, { 57, "Space" }, { 58, "Caps Lock" },
		{ 59, "F1" }, { 60, "F2" }, { 61, "F3" }, { 62, "F4" }, { 63, "F5" }, { 64, "F6" }, { 65, "F7" }, { 66, "F8" }, { 67, "F9" }, { 68, "F10" },
		{ 69, "Num Lock" }, { 70, "Scroll Lock" }, { 71, "Numpad 7" }, { 72, "Numpad 8" }, { 73, "Numpad 9" }, { 74, "Numpad -" },
		{ 75, "Numpad 4" }, { 76, "Numpad 5" }, { 77, "Numpad 6" }, { 78, "Numpad +" }, { 79, "Numpad 1" }, { 80, "Numpad 2" }, { 81, "Numpad 3" },
		{ 82, "Numpad 0" }, { 83, "Numpad ." }, { 87, "F11" }, { 88, "F12" },
		{ 156, "Numpad Enter" }, { 157, "Right Ctrl" }, { 181, "Numpad /" }, { 184, "Right Alt" },
		{ 199, "Home" }, { 200, "Up" }, { 201, "Page Up" }, { 203, "Left" }, { 205, "Right" }, { 207, "End" }, { 208, "Down" },
		{ 209, "Page Down" }, { 210, "Insert" }, { 211, "Delete" },
	};

	// Order matches the gamepad mapping in Input.cpp (266 .. 281).
	constexpr const char* kGamepadNames[] = {
		"D-pad Up", "D-pad Down", "D-pad Left", "D-pad Right", "Start", "Back",
		"Left Stick", "Right Stick", "LB", "RB", "A", "B", "X", "Y", "LT", "RT"
	};

	std::string KeyNameFor(std::int32_t a_code)
	{
		if (a_code < 0) {
			return _T("Wait key");
		}
		const auto gamepadIndex = a_code - kGamepadCodeBase;
		if (gamepadIndex >= 0 && gamepadIndex < static_cast<std::int32_t>(std::size(kGamepadNames))) {
			return std::string(_T("Gamepad ")) + _T(kGamepadNames[gamepadIndex]);
		}
		for (const auto& entry : kKeyNames) {
			if (entry.code == a_code) {
				return _T(entry.name);
			}
		}
		return std::string(_T("scancode ")) + std::to_string(a_code);
	}

	// The undress-mode combo needs "\0"-separated items, so the three entries
	// are translated individually and joined at render time.
	const char* UndressModeItems()
	{
		static char buffer[256];
		buffer[0] = '\0';
		std::size_t offset = 0;
		for (const char* key : { "Off", "Everything", "Everything except torso" }) {
			const char* text = _T(key);
			const auto  len = std::strlen(text) + 1;
			if (offset + len + 1 > sizeof(buffer)) {
				break;
			}
			std::memcpy(buffer + offset, text, len);
			offset += len;
		}
		if (offset < sizeof(buffer)) {
			buffer[offset] = '\0';
		}
		return buffer;
	}

	// The input callback runs outside the render pass, so the capture state and
	// the captured code are handed over atomically.
	std::atomic<bool>                     g_capturingKey{ false };
	std::atomic<std::int32_t>             g_capturedKey{ kNoCapturedKey };
	SKSEMenuFramework::Model::InputEvent* g_inputEvent{ nullptr };

	bool __stdcall OnInput(RE::InputEvent* a_event)
	{
		if (!g_capturingKey.load()) {
			return false;
		}
		if (!SKSEMenuFramework::IsAnyBlockingWindowOpened()) {
			g_capturingKey = false;
			return false;
		}
		if (!a_event || a_event->GetEventType() != RE::INPUT_EVENT_TYPE::kButton) {
			return false;
		}
		const auto button = a_event->AsButtonEvent();
		if (!button || !button->IsDown()) {
			return false;
		}

		switch (button->GetDevice()) {
		case RE::INPUT_DEVICE::kKeyboard:
			if (button->GetIDCode() == kEscapeScancode) {
				g_capturingKey = false;
				return true;
			}
			g_capturedKey = static_cast<std::int32_t>(button->GetIDCode());
			break;
		case RE::INPUT_DEVICE::kGamepad:
			if (const auto code = Input::GamepadCode(button->GetIDCode()); code >= 0) {
				g_capturedKey = code;
				break;
			}
			return false;
		default:
			return false;
		}

		g_capturingKey = false;
		return true;
	}

	bool Toggle(const char* label, bool* value, const char* tooltip)
	{
		const bool changed = ui::Checkbox(_T(label), value);
		ui::SetItemTooltip("%s", _T(tooltip));
		return changed;
	}

	bool HourSlider(const char* label, float* value, const char* tooltip)
	{
		ui::SliderFloat(_T(label), value, 0.0f, kMaxHour, "%.2f", ui::ImGuiSliderFlags_AlwaysClamp);
		const bool released = ui::IsItemDeactivatedAfterEdit();
		ui::SetItemTooltip("%s", _T(tooltip));
		return released;
	}

	void __stdcall RenderSettings()
	{
		auto& s = Settings::Mutable();
		bool  dirty = false;
		ui::PushItemWidth(kItemWidth);

		ui::SeparatorText(_T("Input"));
		if (const auto captured = g_capturedKey.exchange(kNoCapturedKey); captured != kNoCapturedKey) {
			s.sleepKey = captured;
			dirty = true;
		}
		ui::AlignTextToFramePadding();
		ui::TextUnformatted((std::string(_T("Sleep key: ")) + KeyNameFor(s.sleepKey)).c_str());
		ui::SetItemTooltip("%s", _T("Opens the sleep menu while lying in bed (or the serve-sentence prompt when a jail sentence is pending). The Wait key is the game's Wait binding."));
		ui::SameLine();
		if (g_capturingKey.load()) {
			ui::TextColored(kAccentColor, "%s", _T("press a key or button (Escape cancels)"));
			ui::SameLine();
			if (ui::SmallButton(_T("Cancel"))) {
				g_capturingKey = false;
			}
		} else {
			if (ui::Button(_T("Bind"))) {
				g_capturingKey = true;
			}
			ui::SetItemTooltip("%s", _T("Press any keyboard key or gamepad button to make it the sleep key."));
			ui::SameLine();
			ui::BeginDisabled(s.sleepKey < 0);
			if (ui::Button(_T("Use Wait key"))) {
				s.sleepKey = -1;
				dirty = true;
			}
			ui::EndDisabled();
			ui::SetItemTooltip("%s", _T("Back to the default: the game's Wait binding works as the sleep key."));
		}

		ui::SeparatorText(_T("Bed"));
		dirty |= Toggle("Allow owned beds", &s.allowOwnedBeds, "Lie down in beds owned by someone else and in beds an NPC has reserved for sleeping. Off: both are refused, owned beds with the vanilla message. Beds owned by the player or by a faction the player belongs to are always allowed.");
		dirty |= Toggle("Open the sleep menu after lying down", &s.openSleepMenuOnLieDown, "The sleep menu opens by itself once the player is in bed. Closing it leaves the player in bed.");

		ui::SeparatorText(_T("Followers"));
		dirty |= Toggle("Followers go to bed", &s.followersEnabled, "Followers lie down in free beds nearby when the player does and get up when the player does.");
		ui::BeginDisabled(!s.followersEnabled);
		dirty |= Toggle("Following spouse shares the player's bed", &s.spouseSharesBed, "A following spouse takes the free second sleep marker of the player's bed. With a single bed the spouse uses a nearby bed like any other follower.");
		dirty |= Toggle("Spouse at home joins the player's bed", &s.homeSpouseSharesBed, "A spouse who is nearby but not following also joins the player's bed when it has a free second marker. They are never sent to another bed.");
		dirty |= Toggle("Followers share the player's bed", &s.followersShareBed, "The nearest follower takes the free second marker of the player's bed when no spouse does. Off: only the spouse ever shares the player's bed.");
		dirty |= Toggle("Ignore bed ownership", &s.followersIgnoreOwnership, "Use any free bed. Off: only unowned beds and beds owned by the player or by the follower are used.");
		ui::SliderFloat(_T("Search radius"), &s.followerSearchRadius, kMinRadius, kMaxRadius, "%.0f", ui::ImGuiSliderFlags_AlwaysClamp | ui::ImGuiSliderFlags_Logarithmic);
		dirty |= ui::IsItemDeactivatedAfterEdit();
		ui::SetItemTooltip("%s", _T("Distance in game units, measured from the player and again from the follower, in which beds are considered. Followers in the same interior cell always count as nearby. 1024 covers a room or a house floor."));
		dirty |= Toggle("Only at night", &s.followersNightOnly, "Followers and the spouse only go to bed when the game hour is inside the night window.");
		ui::BeginDisabled(!s.followersNightOnly);
		dirty |= HourSlider("Night starts", &s.followerNightStart, "Start of the night window, game hour.");
		dirty |= HourSlider("Night ends", &s.followerNightEnd, "End of the night window, game hour. A window that ends before it starts (20 to 6) wraps past midnight.");
		ui::EndDisabled();
		ui::EndDisabled();

		ui::SeparatorText(_T("Undress"));
		dirty |= ui::Combo(_T("Player"), &s.undressPlayer, UndressModeItems());
		ui::SetItemTooltip("%s", _T("What the player takes off before lying down and puts back on after getting up. Everything: all worn armor, clothing, jewellery and the shield. Everything except torso: the piece on the body slot stays on. Worn weapons, shields, torches and quivers come off too."));
		dirty |= ui::Combo(_T("Followers and spouse"), &s.undressNpcs, UndressModeItems());
		ui::SetItemTooltip("%s", _T("The same for the followers and the spouse this mod sends to bed. NPCs sleeping on their own schedules are never touched."));
		dirty |= Toggle("Use Immersive Equipping Animations", &s.undressAnimations, "With Immersive Equipping Animations installed, pieces come off and go on one by one with its animations: the player undresses standing at the bed before lying down and dresses after getting up, followers undress where they stand before walking to their bed. Off, or without that mod, everything is swapped instantly.");

		ui::SeparatorText(_T("Logging"));
		if (Toggle("Write log file", &s.loggingEnabled, "Write SleepInBedSKSE.log with settings, hook installation and bed decisions. Takes effect immediately.")) {
			Settings::ApplyLogLevel();
			dirty = true;
		}

		ui::PopItemWidth();
		ui::Separator();
		if (ui::Button(_T("Reset to defaults"))) {
			Settings::ResetToDefaults();
		}
		ui::SetItemTooltip("%s", _T("Restore every setting to its default and save the INI."));
		ui::SameLine();
		if (ui::Button(_T("Reload INI"))) {
			Settings::Load();
			Settings::ApplyLogLevel();
		}
		ui::SetItemTooltip("%s", _T("Discard the current values and read the INI again."));
		ui::TextDisabled("%s", kIniPath);

		if (dirty) {
			Settings::Save();
		}
	}
}

namespace Menu
{
	void Install()
	{
		if (!GetMenuFrameworkModule()) {
			SKSE::log::info("[menu] SKSE Menu Framework not loaded, in-game settings page unavailable");
			return;
		}
		SKSEMenuFramework::SetSection(_T(kSection));
		SKSEMenuFramework::AddSectionItem(_T(kPage), RenderSettings);
		g_inputEvent = SKSEMenuFramework::AddInputEvent(OnInput);
		SKSE::log::info("[menu] registered {}/{} (framework {})", kSection, kPage, SKSEMenuFramework::GetMenuFrameworkVersion());
	}
}
