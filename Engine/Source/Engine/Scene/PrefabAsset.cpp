#include "EnginePCH.h"
#include "Engine/Scene/PrefabAsset.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Private/EntityDocument.h"
#include "Engine/Scene/Private/PrefabNesting.h"
#include "Engine/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <utility>

namespace Engine {

	Result<Prefab> LoadPrefabAsset(AssetManager& assets, AssetHandle handle, const TypeRegistry& registry, const IFieldSchemaSource* schemas,
		LoadReport& report)
	{
		if (!handle.IsValid())
			return MakeError(ErrorCode::InvalidArgument, "cannot load a prefab from the null asset handle");

		// The type is known from the registry without loading: an asset of another type is refused before it is imported.
		const std::string path = assets.GetReferencePath(handle);
		const std::string name = path.empty() ? handle.ToString() : path;
		const AssetType type = assets.GetAssetType(handle);
		if (type != AssetType::None && type != AssetType::Prefab)
		{
			return std::unexpected(Error(ErrorCode::Validation, std::format("'{}' is a {}, not a Prefab", name, AssetTypeToString(type)))
					.WithHint("pass the handle or path of a .prefab file or a glTF model"));
		}

		Result<AssetRef<Asset>> loaded = assets.Load(handle);
		if (!loaded)
			return std::unexpected(std::move(loaded).error().WithContext(std::format("loading the prefab {} '{}'", handle.ToString(), name)));
		const AssetRef<PrefabData> prefab = AssetCast<PrefabData>(*loaded);
		if (prefab == nullptr)
		{
			return std::unexpected(Error(ErrorCode::Validation, std::format("'{}' is a {}, not a Prefab", name, AssetTypeToString((*loaded)->GetAssetType())))
					.WithHint("pass the handle or path of a .prefab file or a glTF model"));
		}

		// The cooked document passed strict loading when it was imported (PrefabImporter, GltfImporter).
		LoadOptions options;
		options.Mode = LoadMode::Strict;
		options.Schemas = schemas;
		options.SourcePath = path;
		Result<Prefab> parsed = Prefab::FromJson(*prefab->Document, registry, options, report);
		if (!parsed)
			return std::unexpected(std::move(parsed).error().WithContext(std::format("reading the prefab {} '{}'", handle.ToString(), name)));
		return parsed;
	}

	Result<Entity> InstantiatePrefabAsset(Scene& scene, AssetManager& assets, const PrefabInstantiateOptions& instance,
		const PrefabOptions& options, LoadReport& report)
	{
		ENGINE_TRY_ASSIGN(const Prefab prefab, LoadPrefabAsset(assets, instance.PrefabHandle, scene.GetTypeRegistry(), options.Schemas, report));
		return PrefabInstantiator::Instantiate(scene, prefab, instance, options, report);
	}

	namespace Utils {

		// The body of UpdateNestedPrefabInstances (`rebuild`) and RefreshNestedPrefabOverrides: `prefab`'s document loaded
		// into a scratch scene, where the instances outermost in it are outermost instances, each of a prefab `versions` holds
		// rebuilt from that version when `rebuild` and its record refreshed against it, then written back.
		static Result<Prefab> RewriteNestedInstances(const Prefab& prefab, const TypeRegistry& registry, const std::map<AssetHandle, Prefab>& versions,
			const PrefabOptions& options, bool rebuild, LoadReport& report)
		{
			// The scratch generator never draws: every entity of the document has its ID.
			ENGINE_TRY_ASSIGN(const Json document, prefab.ToJson());
			UUIDGenerator scratchIds = UUIDGenerator::CreateDeterministic(0);
			SceneSpecification specification;
			specification.Name = "Prefab";
			specification.Registry = &registry;
			specification.IdGenerator = &scratchIds;
			const Scope<Scene> scratch = Scene::Create(specification);
			LoadOptions loadOptions;
			loadOptions.Schemas = options.Schemas;
			LoadReport loadReport;
			ENGINE_TRY(LoadEntityDocument(*scratch, document, DocumentKind::Prefab, loadOptions, loadReport));

			const Scene& loaded = *scratch;
			std::vector<std::pair<UUID, AssetHandle>> instances;
			for (const UUID id : loaded.GetCanonicalOrder())
			{
				const ConstEntity entity = loaded.FindEntityByID(id);
				const PrefabInstanceComponent* instance = entity.TryGetComponent<PrefabInstanceComponent>();
				if (instance != nullptr && !IsNestedInstanceRoot(entity) && versions.contains(instance->Prefab.GetHandle()))
					instances.emplace_back(id, instance->Prefab.GetHandle());
			}
			for (const auto& [id, handle] : instances)
			{
				const Entity root = scratch->FindEntityByID(id);
				const auto version = versions.find(handle);
				if (!root.IsValid() || version == versions.end())
					continue;
				if (rebuild)
					ENGINE_TRY(PrefabInstantiator::UpdateInstance(*scratch, root, version->second, options, report));
				// The record becomes exactly what differs from the version; a reference it cannot store keeps it as it was.
				if (Status refreshed = PrefabInstantiator::RefreshOverrides(root, version->second, options); !refreshed)
					ENGINE_CORE_WARN("The overrides of the instance {} nested in prefab '{}' were kept as recorded: {}", id, prefab.GetName(), refreshed.error().ToString());
			}

			ENGINE_CORE_ASSERT(loaded.GetRootEntities().size() == 1, "A validated prefab has exactly one root");
			return Prefab::CreateFromEntity(loaded.FindEntityByID(loaded.GetRootEntities().front()), prefab.GetName());
		}

	}

	Result<Prefab> UpdateNestedPrefabInstances(const Prefab& prefab, const TypeRegistry& registry, const std::map<AssetHandle, Prefab>& versions,
		const PrefabOptions& options, LoadReport& report)
	{
		return Utils::RewriteNestedInstances(prefab, registry, versions, options, true, report);
	}

	Result<Prefab> RefreshNestedPrefabOverrides(const Prefab& prefab, const TypeRegistry& registry, const std::map<AssetHandle, Prefab>& versions,
		const PrefabOptions& options)
	{
		LoadReport report;
		return Utils::RewriteNestedInstances(prefab, registry, versions, options, false, report);
	}

	std::vector<AssetHandle> GetNestedPrefabAssets(const Prefab& prefab, const TypeRegistry& registry)
	{
		std::vector<AssetHandle> handles;
		const ComponentInfo* instanceInfo = registry.FindComponent<PrefabInstanceComponent>();
		if (instanceInfo == nullptr)
			return handles;
		for (const Json& entity : prefab.GetEntities())
		{
			const auto components = entity.find("Components");
			if (components == entity.end() || !components->is_object())
				continue;
			const auto instance = components->find(instanceInfo->GetName());
			if (instance == components->end() || !instance->is_object())
				continue;
			const auto handle = instance->find("Prefab");
			if (handle == instance->end() || !handle->is_string())
				continue;
			if (const Result<UUID> id = JsonReader(*handle).ReadUUID(); id && id->IsValid())
				handles.push_back(AssetHandle(*id));
		}
		std::ranges::sort(handles);
		const auto [first, last] = std::ranges::unique(handles);
		handles.erase(first, last);
		return handles;
	}

}
