#include "Menu.h"

#include "Settings.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <format>

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

	void __stdcall RenderSettings()
	{
		auto& s = Settings::Mutable();
		bool  dirty = false;
		ui::PushItemWidth(kItemWidth);

		ui::SeparatorText(_T("Input"));
		ui::InputInt(_T("Sleep key"), &s.sleepKey, 1, 10);
		dirty |= ui::IsItemDeactivatedAfterEdit();
		ui::SetItemTooltip("%s", _T("-1 uses the game's Wait binding. Keyboard keys are DirectX scan codes; gamepad buttons are 266 (D-pad up) to 281 (right trigger)."));
		if (s.sleepKey < 0) {
			ui::SameLine();
			ui::TextDisabled(_T("Wait key"));
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
		SKSE::log::info("[menu] registered {}/{} (framework {})", kSection, kPage, SKSEMenuFramework::GetMenuFrameworkVersion());
	}
}
