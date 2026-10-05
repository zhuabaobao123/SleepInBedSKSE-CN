#pragma once

#include <algorithm>
#include <array>
#include <cctype>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace Bedtime
{
	inline constexpr std::size_t kHome = 0;
	inline constexpr std::size_t kTavern = 1;
	inline constexpr std::size_t kDungeon = 2;
	inline constexpr std::size_t kWilds = 3;
	inline constexpr std::size_t kElsewhere = 4;
	inline constexpr std::size_t kPlaceCount = 5;

	struct Place
	{
		const char* label;
		const char* phrase;
		const char* playerKey;
		const char* followersKey;
		const char* description;
	};

	inline constexpr std::array<Place, kPlaceCount> kPlaces{ {
		{ "Home", "at home", "sHome", "sHomeFollowers", "A player house, or any cell the player owns." },
		{ "Tavern", "in taverns", "sTavern", "sTavernFollowers", "An inn." },
		{ "Dungeon", "in dungeons", "sDungeon", "sDungeonFollowers", "Caves, ruins, forts and other dungeons." },
		{ "Wilds", "in the wilds", "sWilds", "sWildsFollowers", "Outdoors, away from any settlement." },
		{ "Anywhere else", "anywhere else", "sElsewhere", "sElsewhereFollowers", "Everywhere else, including cities, towns and settlements." },
	} };

	inline constexpr Place kEverywhere{ "Everywhere", "everywhere", "sEverywhere", "sEverywhereFollowers", "Wherever you sleep." };

	inline constexpr std::string_view kStay = "stay";
	inline constexpr std::string_view kUndress = "undress";

	inline constexpr std::int32_t kModeOff = 0;
	inline constexpr std::int32_t kModeEverything = 1;
	inline constexpr std::int32_t kModeExceptTorso = 2;
	inline constexpr std::int32_t kModeOutfit = 3;

	inline constexpr std::size_t kMaxNameLength = 32;

	inline std::array<std::string, kPlaceCount> AllStay()
	{
		std::array<std::string, kPlaceCount> choices;
		choices.fill(std::string(kStay));
		return choices;
	}

	inline bool SameName(std::string_view a, std::string_view b)
	{
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) {
			return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y));
		});
	}

	struct Column
	{
		std::string                          everywhere{ kStay };
		std::array<std::string, kPlaceCount> places{ AllStay() };
	};

	inline bool Uniform(const std::array<std::string, kPlaceCount>& places)
	{
		return std::all_of(places.begin(), places.end(), [&](const std::string& choice) { return SameName(choice, places.front()); });
	}

	template <class F>
	void ForEachCell(Column& column, F&& visit)
	{
		visit(column.everywhere);
		for (auto& place : column.places) {
			visit(place);
		}
	}

	inline std::string CleanOutfitName(std::string_view raw)
	{
		std::string name;
		for (const char c : raw) {
			if (static_cast<unsigned char>(c) < 0x20 || std::string_view("=;#[],|\"").find(c) != std::string_view::npos) {
				continue;
			}
			name.push_back(c);
		}
		if (name.size() > kMaxNameLength) {
			name.resize(kMaxNameLength);
			while (!name.empty() && (static_cast<unsigned char>(name.back()) & 0xC0) == 0x80) {
				name.pop_back();
			}
			if (!name.empty() && static_cast<unsigned char>(name.back()) >= 0xC0) {
				name.pop_back();
			}
		}
		const auto first = name.find_first_not_of(' ');
		if (first == std::string::npos) {
			return {};
		}
		name = name.substr(first, name.find_last_not_of(' ') - first + 1);
		if (SameName(name, kStay) || SameName(name, kUndress)) {
			name += " outfit";
		}
		return name;
	}
}
