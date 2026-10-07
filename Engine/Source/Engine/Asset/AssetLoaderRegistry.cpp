#include "EnginePCH.h"
#include "Engine/Asset/AssetLoaderRegistry.h"

// M6 contract stub (Roadmap rule 3): stream A (asset core and registry) implements the loader registry and the M6 loaders
// over the payload readers of the data types (MeshData, TextureData, MaterialData, FontData, DocumentData).

namespace Engine {

	AssetLoaderRegistry::AssetLoaderRegistry() = default;

	AssetLoaderRegistry::~AssetLoaderRegistry() = default;

	void AssetLoaderRegistry::Register(Scope<IAssetLoader> /*loader*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	const IAssetLoader* AssetLoaderRegistry::Find(AssetType /*type*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	std::vector<AssetType> AssetLoaderRegistry::GetTypes() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	Result<AssetRef<Asset>> AssetLoaderRegistry::Load(std::span<const std::byte> /*cooked*/, const AssetLoadContext& /*context*/) const
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "AssetLoaderRegistry::Load is an M6 contract stub");
	}

	void RegisterBuiltinLoaders(AssetLoaderRegistry& /*registry*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
