#include "EnginePCH.h"
#include "Engine/Scene/PrefabAsset.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Scene/Scene.h"

namespace Engine {

	Result<Prefab> LoadPrefabAsset(AssetManager& assets, AssetHandle handle, const TypeRegistry& registry, LoadReport& report)
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
		options.SourcePath = path;
		Result<Prefab> parsed = Prefab::FromJson(*prefab->Document, registry, options, report);
		if (!parsed)
			return std::unexpected(std::move(parsed).error().WithContext(std::format("reading the prefab {} '{}'", handle.ToString(), name)));
		return parsed;
	}

	Result<Entity> InstantiatePrefabAsset(Scene& scene, AssetManager& assets, const PrefabInstantiateOptions& instance,
		const PrefabOptions& options, LoadReport& report)
	{
		ENGINE_TRY_ASSIGN(const Prefab prefab, LoadPrefabAsset(assets, instance.PrefabHandle, scene.GetTypeRegistry(), report));
		return PrefabInstantiator::Instantiate(scene, prefab, instance, options, report);
	}

}
