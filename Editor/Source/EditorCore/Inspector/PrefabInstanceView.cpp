#include "EditorPCH.h"
#include "EditorCore/Inspector/PrefabInstanceView.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Components/PrefabLinkComponent.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/PrefabAsset.h"
#include "Engine/Scene/PrefabInstantiator.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <format>
#include <optional>
#include <span>
#include <utility>

namespace Engine {

	namespace Utils {

		// The file name of an asset's reference path, or its id when it has none.
		static std::string PrefabViewAssetName(const EditorContext& editor, AssetHandle handle)
		{
			const std::string path = editor.GetAssets().GetReferencePath(handle);
			if (path.empty())
				return handle.ToString();
			const size_t slash = path.find_last_of('/');
			return slash == std::string::npos ? path : path.substr(slash + 1);
		}

		// The name a prefab document gives the entity an override path names: in the outermost prefab's document an
		// instance nested in it sits at its root's id, so the entity is fold(DeriveInstanceID, path[0], the rest).
		static std::optional<std::string> FindPrefabEntityName(const Prefab& prefab, std::span<const UUID> path)
		{
			if (path.empty())
				return std::nullopt;
			UUID id = path.front();
			for (const UUID element : path.subspan(1))
				id = PrefabInstantiator::DeriveInstanceID(id, element);
			const Json* entity = prefab.FindEntity(id);
			if (entity == nullptr)
				return std::nullopt;
			const std::optional<JsonReader> name = JsonReader(*entity).FindMember("Name");
			if (!name.has_value())
				return std::nullopt;
			Result<std::string> text = name->ReadString();
			return text ? std::optional<std::string>(std::move(*text)) : std::nullopt;
		}

		// The scene entity an override path names in the instance rooted at `outermost`: the root for the root's own path,
		// else fold(DeriveInstanceID, outermost, path); none for an empty path, which a malformed file can hold.
		static ConstEntity FindPrefabPathEntity(const Scene& scene, ConstEntity outermost, std::span<const UUID> rootPath, std::span<const UUID> path)
		{
			if (path.empty())
				return {};
			if (std::ranges::equal(path, rootPath))
				return outermost;
			UUID id = outermost.GetUUID();
			for (const UUID element : path)
				id = PrefabInstantiator::DeriveInstanceID(id, element);
			return scene.FindEntityByID(id);
		}

		// The target named by its id when no entity or prefab name is found: the path's last id ("(empty path)" for none).
		static std::string PrefabPathFallbackLabel(std::span<const UUID> path)
		{
			return path.empty() ? std::string("(empty path)") : path.back().ToString();
		}

		static std::string DescribePrefabOverride(const PrefabOverride& prefabOverride)
		{
			switch (prefabOverride.Kind)
			{
				case PrefabOverrideKind::Field:           return prefabOverride.Component + "." + prefabOverride.Field;
				case PrefabOverrideKind::AddComponent:    return "Added " + prefabOverride.Component;
				case PrefabOverrideKind::RemoveComponent: return "Removed " + prefabOverride.Component;
				case PrefabOverrideKind::EntityKey:       return prefabOverride.Field;
				case PrefabOverrideKind::RemoveEntity:    return "Removed";
			}
			return {};
		}

		// The description of a Field override of one script slot (Docs/Decisions/0031-m20-contract.md decision 19), naming
		// the slot and the script `target` holds there: "Script slot 1 (Fire.luau): Fields", without the script's name when
		// the target or its slot has none. nullopt for every other override.
		static std::optional<std::string> DescribeScriptSlotOverride(const EditorContext& editor, const PrefabOverride& prefabOverride, ConstEntity target)
		{
			if (prefabOverride.Kind != PrefabOverrideKind::Field || prefabOverride.Component != "Script")
				return std::nullopt;
			const std::optional<ScriptSlotField> slot = PrefabInstantiator::ParseScriptSlotField(prefabOverride.Field);
			if (!slot.has_value())
				return std::nullopt;
			std::string script;
			const Result<Json> component = target.IsValid() ? ComponentAccess::GetComponentJson(target, "Script") : Result<Json>(Json());
			const auto slots = component.has_value() && component->is_object() ? component->find("Scripts") : Json::const_iterator();
			if (component.has_value() && component->is_object() && slots != component->end() && slots->is_array() && slot->Slot < slots->size())
			{
				const Json& entry = (*slots)[slot->Slot];
				const auto handle = entry.is_object() ? entry.find("Script") : entry.end();
				const Result<UUID> id = entry.is_object() && handle != entry.end() && handle->is_string() ? JsonReader(*handle).ReadUUID() : Result<UUID>(UUID());
				if (id.has_value() && id->IsValid())
					script = std::format(" ({})", PrefabViewAssetName(editor, *id));
			}
			return std::format("Script slot {}{}: {}", slot->Slot, script, slot->Member);
		}

	}

	Json MakePrefabOverrideKey(const EditorContext& editor, const PrefabOverride& prefabOverride)
	{
		Json path = Json::array();
		for (const UUID element : prefabOverride.Path)
			path.push_back(element.ToString());
		Json key{ { "path", std::move(path) } };
		const EnumInfo* kinds = editor.GetTypeRegistry().FindEnum<PrefabOverrideKind>();
		ENGINE_ASSERT(kinds != nullptr, "PrefabOverrideKind is registered by every engine context");
		const EnumEntry* kind = kinds->FindByValue(static_cast<int64_t>(prefabOverride.Kind));
		ENGINE_ASSERT(kind != nullptr, "every PrefabOverrideKind value is registered");
		key["kind"] = kind->Name;
		if (!prefabOverride.Component.empty())
			key["component"] = prefabOverride.Component;
		if (!prefabOverride.Field.empty())
			key["field"] = prefabOverride.Field;
		return key;
	}

	Result<PrefabInstanceView> DescribePrefabInstance(EditorContext& editor, Entity entity)
	{
		if (!entity.IsValid() || !entity.HasComponent<PrefabLinkComponent>())
			return MakeError(ErrorCode::InvalidArgument, "the entity belongs to no prefab instance");
		ENGINE_TRY_ASSIGN(const std::vector<Entity> levels, PrefabInstantiator::GetInstanceLevels(entity));
		if (levels.empty())
			return MakeError(ErrorCode::InvalidArgument, "the entity belongs to no prefab instance");
		const Scene& scene = *entity.GetScene();
		PrefabInstanceView view;
		view.Outermost = levels.front().GetUUID();
		view.Instance = levels.back().GetUUID();
		ENGINE_TRY_ASSIGN(view.EntityPath, PrefabInstantiator::GetOverridePath(entity));
		for (const Entity level : levels)
		{
			const AssetHandle prefab = level.GetComponent<PrefabInstanceComponent>().Prefab.GetHandle();
			view.Levels.push_back({ .Root = level.GetUUID(), .Prefab = prefab, .Label = std::format("{} ({})", Utils::PrefabViewAssetName(editor, prefab), scene.GetEntityPath(level)) });
		}
		const ConstEntity outermost = levels.front();
		view.OutermostPrefab = view.Levels.front().Prefab;
		ENGINE_TRY_ASSIGN(const std::vector<UUID> rootPath, PrefabInstantiator::GetOverridePath(outermost));
		std::vector<UUID> instancePath;
		if (levels.size() > 1)
		{
			ENGINE_TRY_ASSIGN(instancePath, PrefabInstantiator::GetOverridePath(levels.back()));
		}
		// The outermost prefab's document names removed entities; read once, only when a row needs it.
		std::optional<Result<Prefab>> prefab;
		for (const PrefabOverride& prefabOverride : outermost.GetComponent<PrefabInstanceComponent>().Overrides)
		{
			if (!std::ranges::starts_with(prefabOverride.Path, instancePath))
				continue;
			PrefabOverrideRow row{ .Override = prefabOverride, .Target = {}, .Description = Utils::DescribePrefabOverride(prefabOverride), .RevertKey = MakePrefabOverrideKey(editor, prefabOverride) };
			if (prefabOverride.Kind == PrefabOverrideKind::RemoveEntity)
			{
				if (!prefab.has_value())
				{
					LoadReport report;
					Result<Ref<const ScriptFieldSchemaSource>> schemas = editor.GetScriptSchemaSnapshot();
					prefab = schemas ? LoadPrefabAsset(editor.GetAssets(), view.OutermostPrefab, editor.GetTypeRegistry(), schemas->get(), report)
									 : Result<Prefab>(std::unexpected(std::move(schemas).error()));
				}
				const std::optional<std::string> name = *prefab ? Utils::FindPrefabEntityName(**prefab, prefabOverride.Path) : std::nullopt;
				row.Target = name.value_or(Utils::PrefabPathFallbackLabel(prefabOverride.Path));
			}
			else
			{
				const ConstEntity target = Utils::FindPrefabPathEntity(scene, outermost, rootPath, prefabOverride.Path);
				row.Target = target.IsValid() ? scene.GetEntityPath(target) : Utils::PrefabPathFallbackLabel(prefabOverride.Path);
				if (std::optional<std::string> slot = Utils::DescribeScriptSlotOverride(editor, prefabOverride, target))
					row.Description = std::move(*slot);
			}
			view.Overrides.push_back(std::move(row));
		}
		return view;
	}

}
