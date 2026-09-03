#include "Hearthfire.h"

#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>

#include <cctype>
#include <string_view>

namespace
{
	constexpr RE::FormID       kCoffinLocalId = 0x000801;
	constexpr std::string_view kCoffinPlugin = "HearthFires.esm";
	constexpr std::string_view kVerticalCoffinKeyword = "DLC1isVampireCoffinVertical";
	constexpr std::string_view kTemplateModel = "VampireCoffin01.nif";
	constexpr std::string_view kLidOpenEvent = "Open";

	RE::TESFurniture* g_coffin = nullptr;

	bool EndsWithNoCase(std::string_view text, std::string_view suffix)
	{
		if (text.size() < suffix.size()) {
			return false;
		}
		const auto tail = text.substr(text.size() - suffix.size());
		for (std::size_t i = 0; i < suffix.size(); ++i) {
			if (std::tolower(static_cast<unsigned char>(tail[i])) != std::tolower(static_cast<unsigned char>(suffix[i]))) {
				return false;
			}
		}
		return true;
	}

	RE::TESFurniture* FindVerticalCoffinTemplate(RE::BGSKeyword* keyword)
	{
		auto* handler = RE::TESDataHandler::GetSingleton();
		if (!handler) {
			return nullptr;
		}
		for (auto* furniture : handler->GetFormArray<RE::TESFurniture>()) {
			if (!furniture || !furniture->HasKeyword(keyword)) {
				continue;
			}
			const char* model = furniture->model.c_str();
			if (model && EndsWithNoCase(model, kTemplateModel)) {
				return furniture;
			}
		}
		return nullptr;
	}

	bool OccupiedByPlayer(RE::TESObjectREFR* furniture)
	{
		auto* player = RE::PlayerCharacter::GetSingleton();
		auto* process = player ? player->GetActorRuntimeData().currentProcess : nullptr;
		if (!process) {
			return false;
		}
		return process->GetOccupiedFurniture().get().get() == furniture;
	}
}

namespace Hearthfire
{
	void PrepareCoffin()
	{
		auto* handler = RE::TESDataHandler::GetSingleton();
		auto* coffin = handler ? handler->LookupForm<RE::TESFurniture>(kCoffinLocalId, kCoffinPlugin) : nullptr;
		if (!coffin) {
			SKSE::log::info("[hearthfire coffin] record not present");
			return;
		}
		auto* keyword = RE::TESForm::LookupByEditorID<RE::BGSKeyword>(kVerticalCoffinKeyword);
		if (!keyword) {
			SKSE::log::info("[hearthfire coffin] vertical coffin keyword not present");
			return;
		}
		if (coffin->HasKeyword(keyword)) {
			g_coffin = coffin;
			SKSE::log::info("[hearthfire coffin] already a vertical coffin, record left untouched");
			return;
		}
		auto* source = FindVerticalCoffinTemplate(keyword);
		if (!source) {
			SKSE::log::info("[hearthfire coffin] no vertical coffin template found");
			return;
		}
		coffin->model = source->model;
		coffin->furnFlags = source->furnFlags;
		coffin->workBenchData = source->workBenchData;
		coffin->entryPointDataArray = source->entryPointDataArray;
		coffin->formFlags |= source->formFlags & RE::TESFurniture::RecordFlags::kMustExitToTalk;
		coffin->AddKeyword(keyword);
		g_coffin = coffin;
		SKSE::log::info("[hearthfire coffin] {:08X} now uses the model and markers of {:08X}", coffin->GetFormID(), source->GetFormID());
	}

	void OpenLidOnExit(RE::TESObjectREFR* furniture, RE::TESObjectREFR* activator)
	{
		if (!g_coffin || !furniture || furniture->GetBaseObject() != g_coffin) {
			return;
		}
		if (activator != RE::PlayerCharacter::GetSingleton() || !OccupiedByPlayer(furniture)) {
			return;
		}
		const RE::BSFixedString lidOpen{ kLidOpenEvent.data() };
		furniture->NotifyAnimationGraph(lidOpen);
	}
}
