#include "Sleepwear.h"

#include "Bedtime.h"
#include "Settings.h"
#include "Undress.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cfloat>
#include <charconv>
#include <chrono>
#include <cstring>
#include <format>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include <SKSEMenuFramework.h>

namespace
{
	namespace ui = ImGuiMCP;

	using Bedtime::kPlaceCount;

	constexpr const char* kKeywordEditorID = "SleepInBed_Sleepwear";
	constexpr RE::FormID  kPlayerFactionID = 0x000DB1;
	constexpr int         kMaxLocationHops = 16;
	constexpr int         kMaxNameAttempts = 1000;
	constexpr float       kBrowserHeight = 220.0f;
	constexpr float       kPlaceColumnWidth = 110.0f;
	constexpr auto        kMenuRefreshInterval = std::chrono::seconds(1);

	constexpr ui::ImVec4 kMissingColor{ 1.0f, 0.45f, 0.45f, 1.0f };
	constexpr ui::ImVec4 kWarningColor{ 1.0f, 0.78f, 0.35f, 1.0f };

	constexpr std::uint32_t kSeenHome = 1u << 0;
	constexpr std::uint32_t kSeenTavern = 1u << 1;
	constexpr std::uint32_t kSeenDungeon = 1u << 2;
	constexpr std::uint32_t kSeenShop = 1u << 3;
	constexpr std::uint32_t kSeenCity = 1u << 4;
	constexpr std::uint32_t kSeenTown = 1u << 5;
	constexpr std::uint32_t kSeenSettlement = 1u << 6;
	constexpr std::uint32_t kSeenWilds = 1u << 7;
	constexpr std::uint32_t kSettled = kSeenHome | kSeenTavern | kSeenDungeon | kSeenShop | kSeenCity | kSeenTown | kSeenSettlement;

	struct LocationType
	{
		const char*   keyword;
		std::uint32_t bit;
	};

	constexpr std::array<LocationType, 7> kLocationTypes{ {
		{ "LocTypePlayerHouse", kSeenHome },
		{ "LocTypeInn", kSeenTavern },
		{ "LocTypeDungeon", kSeenDungeon },
		{ "LocTypeStore", kSeenShop },
		{ "LocTypeCity", kSeenCity },
		{ "LocTypeTown", kSeenTown },
		{ "LocTypeSettlement", kSeenSettlement },
	} };

	struct Entry
	{
		std::string plugin;
		RE::FormID  localID = 0;
		std::string token;
		RE::FormID  runtimeID = 0;
		std::string name;
	};

	struct Outfit
	{
		std::string                    name;
		std::vector<Entry>             entries;
		std::unordered_set<RE::FormID> ids;
	};

	struct ItemRow
	{
		std::string token;
		std::string label;
		std::string removeId;
		bool        missing = false;
	};

	struct BrowserRow
	{
		std::string token;
		std::string label;
		std::string labelLower;
	};

	struct OutfitRows
	{
		std::string          name;
		std::vector<ItemRow> items;
		std::string          serialized;
	};

	struct Snapshot
	{
		bool                    perPlace = false;
		Bedtime::Column         player;
		Bedtime::Column         followers;
		std::vector<OutfitRows> outfits;
		std::vector<BrowserRow> browser;
		std::string             whereLine;
		std::string             playerLine;
		std::string             followersLine;
		bool                    resolved = false;
		bool                    saveRequested = false;
	};

	struct Configuration
	{
		bool                          perPlace;
		Bedtime::Column               player;
		Bedtime::Column               followers;
		std::vector<Settings::Outfit> outfits;
	};

	std::vector<Outfit>                                g_outfits;
	bool                                               g_perPlace = false;
	Bedtime::Column                                    g_player;
	Bedtime::Column                                    g_followers;
	std::array<RE::BGSKeyword*, kLocationTypes.size()> g_locationKeywords{};
	RE::BGSKeyword*                                    g_sleepwearKeyword = nullptr;
	RE::BGSKeyword*                                    g_armorHelmet = nullptr;
	RE::BGSKeyword*                                    g_clothingHead = nullptr;
	std::atomic<bool>                                  g_resolved{ false };

	std::mutex                 g_snapshotMutex;
	Snapshot                   g_snapshot;
	std::atomic<std::uint32_t> g_generation{ 0 };

	struct View
	{
		Snapshot                              cache;
		std::uint32_t                         generation = 0xFFFFFFFF;
		std::string                           selected;
		char                                  nameBuffer[Bedtime::kMaxNameLength + 1] = "";
		char                                  filter[64] = "";
		bool                                  renaming = false;
		bool                                  confirmDelete = false;
		bool                                  browserOpen = false;
		std::chrono::steady_clock::time_point lastHere{};
		std::chrono::steady_clock::time_point lastBrowser{};
	};

	View g_view;

	void Schedule(std::function<void()> task)
	{
		if (auto* tasks = SKSE::GetTaskInterface()) {
			tasks->AddTask(std::move(task));
		}
	}

	char Lower(char c)
	{
		return static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
	}

	std::string Lowercase(std::string_view text)
	{
		std::string lowered;
		lowered.reserve(text.size());
		for (const char c : text) {
			lowered.push_back(Lower(c));
		}
		return lowered;
	}

	bool SameToken(std::string_view a, std::string_view b)
	{
		return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y) { return Lower(x) == Lower(y); });
	}

	std::string_view Trim(std::string_view text)
	{
		while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front()))) {
			text.remove_prefix(1);
		}
		while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back()))) {
			text.remove_suffix(1);
		}
		return text;
	}

	std::optional<Entry> ParseToken(std::string_view token)
	{
		const auto separator = token.find('|');
		if (separator == std::string_view::npos) {
			return std::nullopt;
		}
		const auto plugin = Trim(token.substr(0, separator));
		auto       id = Trim(token.substr(separator + 1));
		if (plugin.empty() || id.empty()) {
			return std::nullopt;
		}
		if (id.starts_with("0x") || id.starts_with("0X")) {
			id.remove_prefix(2);
		}
		RE::FormID localID = 0;
		const auto result = std::from_chars(id.data(), id.data() + id.size(), localID, 16);
		if (result.ec != std::errc{} || result.ptr != id.data() + id.size()) {
			return std::nullopt;
		}
		Entry entry;
		entry.plugin = std::string(plugin);
		entry.localID = localID;
		entry.token = std::format("{}|0x{:X}", entry.plugin, localID);
		return entry;
	}

	std::string TokenFor(RE::TESForm* form)
	{
		const auto* file = form->GetFile(0);
		if (!file) {
			return {};
		}
		const auto localID = form->GetFormID() & (file->IsLight() ? 0xFFFu : 0xFFFFFFu);
		return std::format("{}|0x{:X}", file->GetFilename(), localID);
	}

	Outfit* FindOutfit(std::string_view name)
	{
		for (auto& outfit : g_outfits) {
			if (Bedtime::SameName(outfit.name, name)) {
				return &outfit;
			}
		}
		return nullptr;
	}

	bool Listed(const Outfit& outfit, std::string_view token)
	{
		return std::any_of(outfit.entries.begin(), outfit.entries.end(), [&](const Entry& e) { return SameToken(e.token, token); });
	}

	void ResolveOutfit(Outfit& outfit)
	{
		outfit.ids.clear();
		auto* handler = RE::TESDataHandler::GetSingleton();
		for (auto& entry : outfit.entries) {
			entry.runtimeID = 0;
			entry.name.clear();
			auto* form = handler ? handler->LookupForm(entry.localID, entry.plugin) : nullptr;
			if (!form) {
				continue;
			}
			auto* armor = form->As<RE::TESObjectARMO>();
			if (!armor) {
				entry.name = _FMT("{} (not armor)", entry.token);
				continue;
			}
			entry.runtimeID = armor->GetFormID();
			const auto* name = armor->GetName();
			entry.name = name && *name ? name : entry.token;
			outfit.ids.insert(entry.runtimeID);
		}
	}

	std::string Serialize(const Outfit& outfit)
	{
		std::string serialized;
		for (const auto& entry : outfit.entries) {
			if (!serialized.empty()) {
				serialized += ',';
			}
			serialized += entry.token;
		}
		return serialized;
	}

	void ParseItems(Outfit& outfit, std::string_view configured)
	{
		outfit.entries.clear();
		std::size_t start = 0;
		while (start <= configured.size()) {
			const auto end = configured.find(',', start);
			const auto piece = Trim(configured.substr(start, end == std::string_view::npos ? std::string_view::npos : end - start));
			if (!piece.empty()) {
				if (auto entry = ParseToken(piece); entry && !Listed(outfit, entry->token)) {
					outfit.entries.push_back(std::move(*entry));
				} else if (!entry) {
					SKSE::log::info("[sleepwear] outfit '{}': entry '{}' is not PluginName|0xFormID, skipped", outfit.name, piece);
				}
			}
			if (end == std::string_view::npos) {
				break;
			}
			start = end + 1;
		}
	}

	Configuration Configured()
	{
		const auto& s = Settings::Get();
		return { s.bedtimePerPlace, s.bedtimePlayer, s.bedtimeFollowers, s.outfits };
	}

	void Apply(const Configuration& configuration)
	{
		g_outfits.clear();
		for (const auto& configured : configuration.outfits) {
			Outfit outfit;
			outfit.name = configured.name;
			ParseItems(outfit, configured.items);
			ResolveOutfit(outfit);
			g_outfits.push_back(std::move(outfit));
		}
		g_perPlace = configuration.perPlace;
		g_player = configuration.player;
		g_followers = configuration.followers;
	}

	void PublishSnapshot(bool requestSave)
	{
		std::vector<OutfitRows> outfits;
		outfits.reserve(g_outfits.size());
		for (const auto& outfit : g_outfits) {
			OutfitRows rows;
			rows.name = outfit.name;
			rows.serialized = Serialize(outfit);
			rows.items.reserve(outfit.entries.size());
			for (const auto& entry : outfit.entries) {
				ItemRow row;
				row.token = entry.token;
				row.missing = entry.runtimeID == 0;
				row.label = !entry.name.empty() ? entry.name : _FMT("{} (missing)", entry.token);
				row.removeId = std::format("x##rm{}", entry.token);
				rows.items.push_back(std::move(row));
			}
			outfits.push_back(std::move(rows));
		}
		{
			std::scoped_lock lock(g_snapshotMutex);
			g_snapshot.perPlace = g_perPlace;
			g_snapshot.player = g_player;
			g_snapshot.followers = g_followers;
			g_snapshot.outfits = std::move(outfits);
			g_snapshot.resolved = g_resolved.load();
			g_snapshot.saveRequested |= requestSave;
		}
		++g_generation;
	}

	bool Indoors(RE::TESObjectCELL* cell)
	{
		if (!cell) {
			return false;
		}
		if (cell->IsInteriorCell()) {
			return true;
		}
		const auto* worldSpace = cell->GetRuntimeData().worldSpace;
		return worldSpace && worldSpace->flags.any(RE::TESWorldSpace::Flag::kNoSky);
	}

	bool OwnedByPlayer(RE::TESObjectCELL* cell, RE::PlayerCharacter* player)
	{
		const auto* owner = cell ? cell->GetOwner() : nullptr;
		return owner && (owner == player->GetActorBase() || owner->GetFormID() == kPlayerFactionID);
	}

	std::uint32_t Detect(RE::PlayerCharacter* player)
	{
		std::uint32_t seen = 0;
		int           hops = 0;
		for (auto* location = player->GetCurrentLocation(); location && hops < kMaxLocationHops; location = location->parentLoc, ++hops) {
			for (std::size_t i = 0; i < kLocationTypes.size(); ++i) {
				if (g_locationKeywords[i] && location->HasKeyword(g_locationKeywords[i])) {
					seen |= kLocationTypes[i].bit;
				}
			}
		}
		auto* cell = player->GetParentCell();
		if ((seen & kSeenHome) == 0 && OwnedByPlayer(cell, player)) {
			seen |= kSeenHome;
		}
		if ((seen & kSettled) == 0 && !Indoors(cell)) {
			seen |= kSeenWilds;
		}
		return seen;
	}

	std::size_t PlaceOf(std::uint32_t seen)
	{
		if ((seen & kSeenHome) != 0) {
			return Bedtime::kHome;
		}
		if ((seen & kSeenTavern) != 0) {
			return Bedtime::kTavern;
		}
		if ((seen & kSeenDungeon) != 0) {
			return Bedtime::kDungeon;
		}
		if ((seen & kSeenWilds) != 0) {
			return Bedtime::kWilds;
		}
		return Bedtime::kElsewhere;
	}

	std::size_t CurrentPlace()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		return player ? PlaceOf(Detect(player)) : Bedtime::kElsewhere;
	}

	constexpr std::size_t kEverywhereRow = kPlaceCount;

	const Bedtime::Place& RowInfo(std::size_t row)
	{
		return row == kEverywhereRow ? Bedtime::kEverywhere : Bedtime::kPlaces[row];
	}

	std::string& Cell(Bedtime::Column& column, std::size_t row)
	{
		return row == kEverywhereRow ? column.everywhere : column.places[row];
	}

	void StartPlaces(Bedtime::Column& column)
	{
		if (Bedtime::Uniform(column.places)) {
			column.places.fill(column.everywhere);
		}
	}

	const std::string& ChoiceFor(RE::Actor* actor)
	{
		const auto& column = actor->IsPlayerRef() ? g_player : g_followers;
		return g_perPlace ? column.places[CurrentPlace()] : column.everywhere;
	}

	std::int32_t ModeOf(std::string_view choice)
	{
		if (Bedtime::SameName(choice, Bedtime::kUndress)) {
			return Settings::Get().keepBodyPiece ? Bedtime::kModeExceptTorso : Bedtime::kModeEverything;
		}
		if (Bedtime::SameName(choice, Bedtime::kStay)) {
			return Bedtime::kModeOff;
		}
		return FindOutfit(choice) ? Bedtime::kModeOutfit : Bedtime::kModeOff;
	}

	std::string Outcome(std::string_view choice)
	{
		if (Bedtime::SameName(choice, Bedtime::kUndress)) {
			return _T("undress");
		}
		if (const auto* outfit = FindOutfit(choice)) {
			return _FMT("change into {}", outfit->name);
		}
		return _T("stay dressed");
	}

	std::string LocationName(RE::PlayerCharacter* player)
	{
		const auto* location = player->GetCurrentLocation();
		const auto* name = location ? location->GetName() : nullptr;
		return name && *name ? std::string(name) : std::string("an unnamed place");
	}

	void RefreshHere()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !g_resolved) {
			return;
		}
		const auto place = PlaceOf(Detect(player));
		auto       where = _FMT("Right now: {} ({}).", LocationName(player), _T(Bedtime::kPlaces[place].label));
		auto       you = _FMT("You'd {}.", Outcome(g_player.places[place]));
		auto       followers = _FMT("Followers would {}.", Outcome(g_followers.places[place]));
		{
			std::scoped_lock lock(g_snapshotMutex);
			if (g_snapshot.whereLine == where && g_snapshot.playerLine == you && g_snapshot.followersLine == followers) {
				return;
			}
			g_snapshot.whereLine = std::move(where);
			g_snapshot.playerLine = std::move(you);
			g_snapshot.followersLine = std::move(followers);
		}
		++g_generation;
	}

	bool Tagged(RE::TESObjectARMO* armor, RE::BGSKeyword* keyword)
	{
		return keyword && armor->HasKeyword(keyword);
	}

	bool IsWig(RE::TESObjectARMO* armor)
	{
		using Slot = RE::BGSBipedObjectForm::BipedObjectSlot;
		if (!armor->HasPartOf(Slot::kHair) && !armor->HasPartOf(Slot::kLongHair)) {
			return false;
		}
		return !Tagged(armor, g_armorHelmet) && !Tagged(armor, g_clothingHead);
	}

	bool Protected(RE::TESObjectARMO* armor)
	{
		if ((armor->GetFormFlags() & RE::TESObjectARMO::RecordFlags::kNonPlayable) != 0) {
			return true;
		}
		return armor->GetSlotMask().underlying() == 0 || IsWig(armor);
	}

	bool IsPart(RE::TESObjectARMO* armor, const Outfit& outfit)
	{
		return outfit.ids.contains(armor->GetFormID()) || Tagged(armor, g_sleepwearKeyword);
	}

	RE::TESObjectREFR::InventoryItemMap ArmorInventory(RE::Actor* actor)
	{
		return actor->GetInventory([](RE::TESBoundObject& object) { return object.IsArmor(); });
	}

	void RefreshBrowser()
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		if (!player || !g_resolved) {
			return;
		}
		std::vector<BrowserRow> rows;
		for (const auto& [object, data] : ArmorInventory(player)) {
			if (data.first <= 0 || !object) {
				continue;
			}
			auto* armor = object->As<RE::TESObjectARMO>();
			if (!armor || Protected(armor)) {
				continue;
			}
			auto token = TokenFor(armor);
			if (token.empty()) {
				continue;
			}
			const auto* name = armor->GetName();
			const auto  shown = name && *name ? std::string(name) : token;
			BrowserRow  row;
			row.label = std::format("{}{}##inv{:08X}", shown, Tagged(armor, g_sleepwearKeyword) ? _T(" (keyword-tagged)") : "", armor->GetFormID());
			row.labelLower = Lowercase(shown);
			row.token = std::move(token);
			rows.push_back(std::move(row));
		}
		std::sort(rows.begin(), rows.end(), [](const BrowserRow& a, const BrowserRow& b) { return a.labelLower < b.labelLower; });
		{
			std::scoped_lock lock(g_snapshotMutex);
			g_snapshot.browser = std::move(rows);
		}
		++g_generation;
	}

	void Changed()
	{
		PublishSnapshot(true);
		RefreshHere();
	}

	void SetChoice(bool followers, std::size_t row, std::string choice)
	{
		Schedule([followers, row, choice = std::move(choice)] {
			auto& cell = Cell(followers ? g_followers : g_player, row);
			if (cell == choice) {
				return;
			}
			cell = choice;
			Changed();
			SKSE::log::info("[bedtime] {} {}: {}", followers ? "followers" : "you", RowInfo(row).phrase, choice);
		});
	}

	void SetPerPlace(bool on)
	{
		Schedule([on] {
			if (g_perPlace == on) {
				return;
			}
			g_perPlace = on;
			if (on) {
				StartPlaces(g_player);
				StartPlaces(g_followers);
			}
			Changed();
			SKSE::log::info("[bedtime] per place {}", on ? "on" : "off");
		});
	}

	void CreateOutfit(std::string name)
	{
		Schedule([name = std::move(name)] {
			if (name.empty() || FindOutfit(name)) {
				return;
			}
			Outfit outfit;
			outfit.name = name;
			g_outfits.push_back(std::move(outfit));
			Changed();
			SKSE::log::info("[sleepwear] outfit '{}' created", name);
		});
	}

	void RenameOutfit(std::string from, std::string to)
	{
		Schedule([from = std::move(from), to = std::move(to)] {
			auto* outfit = FindOutfit(from);
			const auto* taken = FindOutfit(to);
			if (!outfit || to.empty() || (taken && taken != outfit)) {
				return;
			}
			const auto previous = outfit->name;
			outfit->name = to;
			for (auto* column : { &g_player, &g_followers }) {
				Bedtime::ForEachCell(*column, [&](std::string& cell) {
					if (Bedtime::SameName(cell, previous)) {
						cell = to;
					}
				});
			}
			Changed();
			SKSE::log::info("[sleepwear] outfit '{}' renamed to '{}'", previous, to);
		});
	}

	void DeleteOutfit(std::string name)
	{
		Schedule([name = std::move(name)] {
			if (std::erase_if(g_outfits, [&](const Outfit& outfit) { return Bedtime::SameName(outfit.name, name); }) == 0) {
				return;
			}
			for (auto* column : { &g_player, &g_followers }) {
				Bedtime::ForEachCell(*column, [&](std::string& cell) {
					if (Bedtime::SameName(cell, name)) {
						cell = std::string(Bedtime::kStay);
					}
				});
			}
			Changed();
			SKSE::log::info("[sleepwear] outfit '{}' deleted, sleepers who had it now stay dressed", name);
		});
	}

	void CaptureWorn(std::string name)
	{
		Schedule([name = std::move(name)] {
			auto* player = RE::PlayerCharacter::GetSingleton();
			auto* outfit = FindOutfit(name);
			if (!player || !outfit) {
				return;
			}
			std::vector<Entry> captured;
			for (const auto& [object, data] : ArmorInventory(player)) {
				const auto& [count, entry] = data;
				if (count <= 0 || !object || !entry || !entry->IsWorn()) {
					continue;
				}
				auto* armor = object->As<RE::TESObjectARMO>();
				if (!armor || Protected(armor)) {
					continue;
				}
				const auto token = TokenFor(armor);
				if (token.empty()) {
					SKSE::log::info("[sleepwear] worn {:08X} has no source plugin, not captured", armor->GetFormID());
					continue;
				}
				if (auto parsed = ParseToken(token)) {
					captured.push_back(std::move(*parsed));
				}
			}
			if (captured.empty()) {
				SKSE::log::info("[sleepwear] capture found nothing worn that qualifies, outfit '{}' unchanged", outfit->name);
				return;
			}
			outfit->entries = std::move(captured);
			ResolveOutfit(*outfit);
			Changed();
			SKSE::log::info("[sleepwear] outfit '{}': captured {} worn piece(s)", outfit->name, outfit->entries.size());
		});
	}

	void AddItem(std::string name, std::string token)
	{
		Schedule([name = std::move(name), token = std::move(token)] {
			auto* outfit = FindOutfit(name);
			auto  entry = ParseToken(token);
			if (!outfit || !entry || Listed(*outfit, entry->token)) {
				return;
			}
			outfit->entries.push_back(std::move(*entry));
			ResolveOutfit(*outfit);
			Changed();
			SKSE::log::info("[sleepwear] outfit '{}': {} piece(s)", outfit->name, outfit->entries.size());
		});
	}

	void RemoveItem(std::string name, std::string token)
	{
		Schedule([name = std::move(name), token = std::move(token)] {
			auto* outfit = FindOutfit(name);
			if (!outfit || std::erase_if(outfit->entries, [&](const Entry& e) { return SameToken(e.token, token); }) == 0) {
				return;
			}
			ResolveOutfit(*outfit);
			Changed();
			SKSE::log::info("[sleepwear] outfit '{}': {} piece(s)", outfit->name, outfit->entries.size());
		});
	}

	void ClearOutfit(std::string name)
	{
		Schedule([name = std::move(name)] {
			auto* outfit = FindOutfit(name);
			if (!outfit || outfit->entries.empty()) {
				return;
			}
			outfit->entries.clear();
			ResolveOutfit(*outfit);
			Changed();
			SKSE::log::info("[sleepwear] outfit '{}' cleared", outfit->name);
		});
	}

	void DonNow(RE::Actor* actor)
	{
		auto* manager = RE::ActorEquipManager::GetSingleton();
		if (!manager || !actor->Is3DLoaded()) {
			return;
		}
		const auto  actorID = actor->GetFormID();
		const auto* outfit = FindOutfit(ChoiceFor(actor));
		if (!outfit) {
			return;
		}

		struct Candidate
		{
			RE::TESBoundObject* object;
			RE::TESObjectARMO*  armor;
		};
		std::vector<Candidate>         candidates;
		std::unordered_set<RE::FormID> carriedIDs;
		std::uint32_t                  blockedMask = 0;
		std::uint32_t                  alreadyOn = 0;
		for (const auto& [object, data] : ArmorInventory(actor)) {
			const auto& [count, entry] = data;
			if (count <= 0 || !object) {
				continue;
			}
			auto* armor = object->As<RE::TESObjectARMO>();
			if (!armor) {
				continue;
			}
			carriedIDs.insert(armor->GetFormID());
			const bool worn = entry && entry->IsWorn();
			if (worn && Protected(armor)) {
				blockedMask |= armor->GetSlotMask().underlying();
				continue;
			}
			if (!IsPart(armor, *outfit)) {
				continue;
			}
			if (worn || Undress::WasDonned(actorID, armor->GetFormID())) {
				blockedMask |= armor->GetSlotMask().underlying();
				++alreadyOn;
				continue;
			}
			candidates.push_back({ object, armor });
		}

		std::uint32_t donned = 0;
		std::uint32_t skipped = 0;
		for (const auto& candidate : candidates) {
			const auto mask = candidate.armor->GetSlotMask().underlying();
			if ((mask & blockedMask) != 0) {
				++skipped;
				continue;
			}
			manager->EquipObject(actor, candidate.object, nullptr, 1, nullptr, true, false, false, false);
			Undress::RememberDonned(actorID, candidate.armor->GetFormID());
			blockedMask |= mask;
			++donned;
		}
		std::uint32_t notCarried = 0;
		for (const auto& entry : outfit->entries) {
			if (entry.runtimeID != 0 && !carriedIDs.contains(entry.runtimeID)) {
				++notCarried;
			}
		}
		const auto where = g_perPlace ? std::format(" ({})", _T(Bedtime::kPlaces[CurrentPlace()].phrase)) : std::string();
		SKSE::log::info("[sleepwear] {:08X} outfit '{}'{}: donned {} piece(s), {} already on, {} not carried, {} skipped (slot kept)", actorID, outfit->name, where, donned, alreadyOn, notCarried, skipped);
	}

	int ChoiceIndex(const Snapshot& cache, std::string_view choice)
	{
		if (Bedtime::SameName(choice, Bedtime::kUndress)) {
			return 1;
		}
		for (std::size_t i = 0; i < cache.outfits.size(); ++i) {
			if (Bedtime::SameName(cache.outfits[i].name, choice)) {
				return static_cast<int>(i) + 2;
			}
		}
		return 0;
	}

	std::string ChoiceAt(const Snapshot& cache, int index)
	{
		if (index == 1) {
			return std::string(Bedtime::kUndress);
		}
		if (index >= 2 && static_cast<std::size_t>(index - 2) < cache.outfits.size()) {
			return cache.outfits[static_cast<std::size_t>(index - 2)].name;
		}
		return std::string(Bedtime::kStay);
	}

	std::string UniqueName(const Snapshot& cache, std::string_view wanted, std::string_view renaming)
	{
		auto base = Bedtime::CleanOutfitName(wanted);
		if (base.empty()) {
			return {};
		}
		const auto taken = [&](std::string_view name) {
			return std::any_of(cache.outfits.begin(), cache.outfits.end(), [&](const OutfitRows& outfit) {
				return Bedtime::SameName(outfit.name, name) && !Bedtime::SameName(outfit.name, renaming);
			});
		};
		if (!taken(base)) {
			return base;
		}
		base = Bedtime::CleanOutfitName(std::string_view(base).substr(0, Bedtime::kMaxNameLength - 4));
		for (int n = 2; n < kMaxNameAttempts; ++n) {
			auto candidate = std::format("{} {}", base, n);
			if (!taken(candidate)) {
				return candidate;
			}
		}
		return {};
	}

	std::string WornWhere(const Snapshot& cache, std::string_view name)
	{
		const auto where = [&](const Bedtime::Column& column) {
			if (!cache.perPlace) {
				return std::string(Bedtime::SameName(column.everywhere, name) ? Bedtime::kEverywhere.phrase : "");
			}
			std::string text;
			for (std::size_t place = 0; place < kPlaceCount; ++place) {
				if (Bedtime::SameName(column.places[place], name)) {
					if (!text.empty()) {
						text += ", ";
					}
					text += _T(Bedtime::kPlaces[place].phrase);
				}
			}
			return text;
		};
		const auto you = where(cache.player);
		const auto followers = where(cache.followers);
		if (you.empty() && followers.empty()) {
			return _T("Not worn anywhere yet. Pick it in the Bedtime table on the Settings tab.");
		}
		std::string text;
		if (!you.empty()) {
			text = _FMT("Worn by you {}.", you);
		}
		if (!followers.empty()) {
			text += _FMT("{}Worn by followers {}.", text.empty() ? "" : " ", followers);
		}
		return text;
	}

	void StartRename(View& view, std::string_view name)
	{
		const auto length = std::min(name.size(), sizeof(view.nameBuffer) - 1);
		std::memcpy(view.nameBuffer, name.data(), length);
		view.nameBuffer[length] = '\0';
		view.renaming = true;
		view.confirmDelete = false;
	}

	void NewOutfit(View& view)
	{
		auto name = UniqueName(view.cache, view.cache.outfits.empty() ? "Sleepwear" : "Outfit", {});
		if (name.empty()) {
			return;
		}
		CreateOutfit(name);
		view.selected = name;
		StartRename(view, name);
	}

	void ChoiceCell(View& view, bool followers, std::size_t row, const std::string& options)
	{
		ui::TableNextColumn();
		auto&      cell = Cell(followers ? view.cache.followers : view.cache.player, row);
		int        index = ChoiceIndex(view.cache, cell);
		const auto id = std::format("##{}{}", followers ? "followers" : "you", row);
		ui::SetNextItemWidth(-FLT_MIN);
		if (ui::Combo(id.c_str(), &index, options.c_str())) {
			auto choice = ChoiceAt(view.cache, index);
			cell = choice;
			SetChoice(followers, row, std::move(choice));
		}
	}

	void ChoiceRow(View& view, std::size_t row, const std::string& options, bool followersOn)
	{
		const auto& info = RowInfo(row);
		ui::TableNextRow();
		ui::TableNextColumn();
		ui::AlignTextToFramePadding();
		ui::TextUnformatted(_T(info.label));
		ui::SetItemTooltip("%s", _T(info.description));
		ChoiceCell(view, false, row, options);
		ui::BeginDisabled(!followersOn);
		ChoiceCell(view, true, row, options);
		ui::EndDisabled();
		if (!followersOn) {
			ui::SetItemTooltip("%s", _T("Followers don't go to bed while \"Followers go to bed\" is off."));
		}
	}

	bool DrawBedtime(View& view, Settings& s)
	{
		ui::SeparatorText(_T("Bedtime"));
		if (!view.cache.resolved) {
			ui::TextDisabled("%s", _T("Waiting for game data..."));
			return false;
		}

		bool       dirty = false;
		const bool followersOn = s.followersEnabled;
		bool       perPlace = view.cache.perPlace;
		if (ui::Checkbox(_T("Different per place"), &perPlace)) {
			view.cache.perPlace = perPlace;
			if (perPlace) {
				StartPlaces(view.cache.player);
				StartPlaces(view.cache.followers);
			}
			SetPerPlace(perPlace);
		}
		ui::SetItemTooltip("%s", _T("Choose separately for home, taverns, dungeons, the wilds and anywhere else. Off: one choice everywhere."));

		if (view.cache.perPlace) {
			if (const auto now = std::chrono::steady_clock::now(); now - view.lastHere >= kMenuRefreshInterval) {
				view.lastHere = now;
				Schedule([] { RefreshHere(); });
			}
			if (!view.cache.whereLine.empty()) {
				ui::TextDisabled("%s", view.cache.whereLine.c_str());
				const auto outcome = followersOn ? std::format("{} {}", view.cache.playerLine, view.cache.followersLine) : view.cache.playerLine;
				ui::TextDisabled("%s", outcome.c_str());
			}
		}

		std::string options = _T("Stay dressed");
		options.push_back('\0');
		options += _T("Undress");
		options.push_back('\0');
		for (const auto& outfit : view.cache.outfits) {
			options += outfit.name;
			options.push_back('\0');
		}

		if (ui::BeginTable("##bedtime", 3, ui::ImGuiTableFlags_SizingStretchSame)) {
			ui::TableSetupColumn(_T("Where"), ui::ImGuiTableColumnFlags_WidthFixed, kPlaceColumnWidth);
			ui::TableSetupColumn(_T("You"));
			ui::TableSetupColumn(followersOn ? _T("Followers") : _T("Followers (off)"));
			ui::TableHeadersRow();
			if (view.cache.perPlace) {
				for (std::size_t place = 0; place < kPlaceCount; ++place) {
					ChoiceRow(view, place, options, followersOn);
				}
			} else {
				ChoiceRow(view, kEverywhereRow, options, followersOn);
			}
			ui::EndTable();
		}
		if (view.cache.outfits.empty()) {
			ui::TextDisabled("%s", _T("To change into sleepwear at bedtime, make an outfit on the Outfits tab."));
		} else if (followersOn) {
			ui::TextDisabled("%s", _T("Followers put on an outfit's pieces only if they carry them, so give them copies."));
		}

		dirty |= ui::Checkbox(_T("Keep the body piece on when undressing"), &s.keepBodyPiece);
		ui::SetItemTooltip("%s", _T("With Undress, the piece on the body slot stays on, such as a shirt, a robe or a cuirass. An outfit replaces it unless it is part of that outfit. Wigs always stay on."));

		const bool installed = Undress::AnimationsInstalled();
		ui::BeginDisabled(!installed);
		dirty |= ui::Checkbox(_T("Use Immersive Equipping Animations"), &s.undressAnimations);
		ui::EndDisabled();
		ui::SetItemTooltip("%s", installed ?
			_T("Pieces come off and go on one by one with its animations: you change standing at the bed before lying down and after getting up, followers change where they stand before walking to their bed.") :
			_T("Immersive Equipping Animations isn't installed, so clothes are swapped instantly."));
		return dirty;
	}

	void DrawItems(View& view, const OutfitRows& outfit)
	{
		if (ui::Button(_T("Capture what I'm wearing"))) {
			CaptureWorn(outfit.name);
		}
		ui::SetItemTooltip("%s", _T("Replaces this outfit with what you're wearing right now. Put the nightwear on first, then click this."));

		if (outfit.items.empty()) {
			ui::TextDisabled("%s", _T("Empty. Sleepers with this outfit just undress."));
		} else {
			for (const auto& row : outfit.items) {
				if (ui::SmallButton(row.removeId.c_str())) {
					RemoveItem(outfit.name, row.token);
				}
				ui::SetItemTooltip("%s", _T("Remove this piece from the outfit."));
				ui::SameLine();
				if (row.missing) {
					ui::TextColored(kMissingColor, "%s", row.label.c_str());
					ui::SetItemTooltip("%s", _T("The plugin this piece comes from is not loaded. The entry is kept and works again once the plugin is back."));
				} else {
					ui::TextUnformatted(row.label.c_str());
				}
			}
			if (ui::SmallButton(_T("Clear"))) {
				ClearOutfit(outfit.name);
			}
			ui::SetItemTooltip("%s", _T("Remove every piece from this outfit."));
		}

		const bool browserOpen = ui::CollapsingHeader(_T("Add from inventory"));
		if (browserOpen) {
			if (const auto now = std::chrono::steady_clock::now(); !view.browserOpen || now - view.lastBrowser >= kMenuRefreshInterval) {
				view.lastBrowser = now;
				Schedule([] { RefreshBrowser(); });
			}
			ui::InputTextWithHint("##sleepwearFilter", _T("Filter by name"), view.filter, sizeof(view.filter));
			const auto filter = Lowercase(view.filter);
			if (ui::BeginChild("##sleepwearInventory", ui::ImVec2{ 0.0f, kBrowserHeight }, ui::ImGuiChildFlags_Border)) {
				for (const auto& row : view.cache.browser) {
					if (!filter.empty() && row.labelLower.find(filter) == std::string::npos) {
						continue;
					}
					bool listed = std::any_of(outfit.items.begin(), outfit.items.end(), [&](const ItemRow& item) { return SameToken(item.token, row.token); });
					if (ui::Checkbox(row.label.c_str(), &listed)) {
						if (listed) {
							AddItem(outfit.name, row.token);
						} else {
							RemoveItem(outfit.name, row.token);
						}
					}
				}
				if (view.cache.browser.empty()) {
					ui::TextDisabled("%s", _T("No wearable items found."));
				}
			}
			ui::EndChild();
		}
		view.browserOpen = browserOpen;
	}

	void DrawOutfits(View& view)
	{
		if (!view.cache.resolved) {
			ui::TextDisabled("%s", _T("Waiting for game data..."));
			return;
		}
		const auto& outfits = view.cache.outfits;
		if (outfits.empty()) {
			ui::TextDisabled("%s", _T("No outfits yet. An outfit is sleepwear to change into at bedtime."));
			if (ui::Button(_T("New outfit"))) {
				NewOutfit(view);
			}
			view.browserOpen = false;
			return;
		}

		auto current = std::find_if(outfits.begin(), outfits.end(), [&](const OutfitRows& outfit) { return Bedtime::SameName(outfit.name, view.selected); });
		if (current == outfits.end()) {
			current = outfits.begin();
			view.selected = current->name;
			view.renaming = false;
			view.confirmDelete = false;
		}
		const auto& outfit = *current;

		std::string names;
		for (const auto& entry : outfits) {
			names += std::format("{} ({})", entry.name, entry.items.size());
			names.push_back('\0');
		}
		int index = static_cast<int>(current - outfits.begin());
		if (ui::Combo("##outfit", &index, names.c_str())) {
			view.selected = outfits[static_cast<std::size_t>(index)].name;
			view.renaming = false;
			view.confirmDelete = false;
		}
		ui::SameLine();
		if (ui::Button(_T("New"))) {
			NewOutfit(view);
		}
		ui::SetItemTooltip("%s", _T("Start a new, empty outfit."));
		ui::SameLine();
		if (ui::Button(_T("Rename"))) {
			StartRename(view, outfit.name);
		}
		ui::SameLine();
		if (ui::Button(_T("Delete"))) {
			view.confirmDelete = true;
			view.renaming = false;
		}

		if (view.renaming) {
			const bool entered = ui::InputText("##outfitName", view.nameBuffer, sizeof(view.nameBuffer), ui::ImGuiInputTextFlags_EnterReturnsTrue);
			ui::SameLine();
			const bool confirmed = ui::Button(_T("OK"));
			ui::SameLine();
			if (ui::Button(_T("Cancel"))) {
				view.renaming = false;
			}
			if (entered || confirmed) {
				const auto name = UniqueName(view.cache, view.nameBuffer, outfit.name);
				if (!name.empty() && name != outfit.name) {
					RenameOutfit(outfit.name, name);
					view.selected = name;
				}
				view.renaming = false;
			}
		}
		if (view.confirmDelete) {
			ui::TextColored(kWarningColor, _T("Delete %s? Where it's picked, sleepers will stay dressed instead."), outfit.name.c_str());
			ui::SameLine();
			if (ui::SmallButton(_T("Delete##confirm"))) {
				DeleteOutfit(outfit.name);
				view.confirmDelete = false;
			}
			ui::SameLine();
			if (ui::SmallButton(_T("Keep"))) {
				view.confirmDelete = false;
			}
		}

		ui::TextDisabled("%s", WornWhere(view.cache, outfit.name).c_str());
		DrawItems(view, outfit);
	}
}

namespace Sleepwear
{
	void Prepare()
	{
		g_sleepwearKeyword = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(kKeywordEditorID);
		g_armorHelmet = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ArmorHelmet");
		g_clothingHead = RE::TESForm::LookupByEditorID<RE::BGSKeyword>("ClothingHead");
		std::size_t found = 0;
		for (std::size_t i = 0; i < kLocationTypes.size(); ++i) {
			g_locationKeywords[i] = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(kLocationTypes[i].keyword);
			found += g_locationKeywords[i] ? 1 : 0;
		}
		Apply(Configured());
		g_resolved = true;
		PublishSnapshot(false);
		std::string summary;
		for (const auto& outfit : g_outfits) {
			if (!summary.empty()) {
				summary += ", ";
			}
			summary += std::format("'{}' {}/{}", outfit.name, outfit.ids.size(), outfit.entries.size());
		}
		SKSE::log::info("[sleepwear] {} outfit(s){}{}; keyword {}; {}/{} location types found", g_outfits.size(), summary.empty() ? "" : ": ", summary, g_sleepwearKeyword ? "found" : "missing", found, kLocationTypes.size());
	}

	void Reset()
	{
		Schedule([] {
			RefreshHere();
			RefreshBrowser();
		});
	}

	std::int32_t ModeFor(RE::Actor* actor)
	{
		if (!actor || !g_resolved) {
			return Bedtime::kModeOff;
		}
		return ModeOf(ChoiceFor(actor));
	}

	bool PartOfOutfit(RE::Actor* actor, RE::TESObjectARMO* armor)
	{
		if (!actor || !armor || !g_resolved) {
			return false;
		}
		const auto* outfit = FindOutfit(ChoiceFor(actor));
		return outfit && IsPart(armor, *outfit);
	}

	void OnUndressed(RE::Actor* actor)
	{
		if (!actor || actor->IsChild() || actor->IsDead() || !g_resolved) {
			return;
		}
		if (ModeFor(actor) == Bedtime::kModeOutfit) {
			DonNow(actor);
		}
	}

	void ScheduleReload()
	{
		Schedule([configuration = Configured()] {
			Apply(configuration);
			PublishSnapshot(false);
			RefreshHere();
			RefreshBrowser();
		});
	}

	bool SyncMenu()
	{
		auto& view = g_view;
		bool  save = false;
		if (const auto generation = g_generation.load(); generation != view.generation) {
			std::scoped_lock lock(g_snapshotMutex);
			view.cache = g_snapshot;
			save = std::exchange(g_snapshot.saveRequested, false);
			view.generation = generation;
		}
		if (!save) {
			return false;
		}
		auto& s = Settings::Mutable();
		s.bedtimePerPlace = view.cache.perPlace;
		s.bedtimePlayer = view.cache.player;
		s.bedtimeFollowers = view.cache.followers;
		s.outfits.clear();
		for (const auto& outfit : view.cache.outfits) {
			s.outfits.push_back({ outfit.name, outfit.serialized });
		}
		return true;
	}

	std::size_t OutfitCount()
	{
		return g_view.cache.outfits.size();
	}

	bool RenderBedtime()
	{
		return DrawBedtime(g_view, Settings::Mutable());
	}

	void RenderOutfits()
	{
		DrawOutfits(g_view);
	}
}
