#include "EnginePCH.h"
#include "Engine/Asset/BuiltinAssets.h"

#include <array>

// M6 contract stub (Roadmap rule 3): stream F (built-ins, GPU cache, shipped assets) implements the catalogue and the
// procedural built-ins. The procedural entry table is the frozen data of BuiltinAssetHandles.

namespace Engine {

	namespace {

		// One compiled-in entry of GetProceduralBuiltinEntries.
		struct ProceduralEntry
		{
			AssetHandle Handle{};
			std::string_view Path{};
			AssetType Type = AssetType::None;
		};

		constexpr std::array<ProceduralEntry, 14> ProceduralEntries = { {
			{ BuiltinAssetHandles::CubeMesh, "engine://Meshes/Cube", AssetType::Mesh },
			{ BuiltinAssetHandles::SphereMesh, "engine://Meshes/Sphere", AssetType::Mesh },
			{ BuiltinAssetHandles::PlaneMesh, "engine://Meshes/Plane", AssetType::Mesh },
			{ BuiltinAssetHandles::QuadMesh, "engine://Meshes/Quad", AssetType::Mesh },
			{ BuiltinAssetHandles::CylinderMesh, "engine://Meshes/Cylinder", AssetType::Mesh },
			{ BuiltinAssetHandles::CapsuleMesh, "engine://Meshes/Capsule", AssetType::Mesh },
			{ BuiltinAssetHandles::ConeMesh, "engine://Meshes/Cone", AssetType::Mesh },
			{ BuiltinAssetHandles::DefaultMaterial, "engine://Materials/Default", AssetType::Material },
			{ BuiltinAssetHandles::ErrorMaterial, "engine://Materials/Error", AssetType::Material },
			{ BuiltinAssetHandles::WhiteTexture, "engine://Textures/White", AssetType::Texture },
			{ BuiltinAssetHandles::BlackTexture, "engine://Textures/Black", AssetType::Texture },
			{ BuiltinAssetHandles::FlatNormalTexture, "engine://Textures/FlatNormal", AssetType::Texture },
			{ BuiltinAssetHandles::CheckerTexture, "engine://Textures/Checker", AssetType::Texture },
			{ BuiltinAssetHandles::MissingTexture, "engine://Textures/Missing", AssetType::Texture },
		} };

	}

	std::span<const BuiltinAssetEntry> GetProceduralBuiltinEntries()
	{
		static const std::vector<BuiltinAssetEntry> Entries = []()
		{
			std::vector<BuiltinAssetEntry> entries;
			entries.reserve(ProceduralEntries.size());
			for (const ProceduralEntry& entry : ProceduralEntries)
			{
				entries.push_back({
					.Handle = entry.Handle,
					.Path = std::string(entry.Path),
					.Type = entry.Type,
					.Source = BuiltinAssetSource::Procedural,
					.File = {},
					.Importer = {},
					.Settings = VariantValue(),
					.Generator = {},
				});
			}
			return entries;
		}();
		return Entries;
	}

	Result<BuiltinAssetCatalog> BuiltinAssetCatalog::Parse(std::string_view /*text*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "BuiltinAssetCatalog::Parse is an M6 contract stub");
	}

	Result<BuiltinAssetCatalog> BuiltinAssetCatalog::Load(const VirtualFileSystem& /*vfs*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "BuiltinAssetCatalog::Load is an M6 contract stub");
	}

	std::string BuiltinAssetCatalog::ToText() const
	{
		ENGINE_CONTRACT_STUB();
		return {};
	}

	const BuiltinAssetEntry* BuiltinAssetCatalog::Find(AssetHandle /*handle*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	const BuiltinAssetEntry* BuiltinAssetCatalog::FindByPath(std::string_view /*path*/) const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	Result<AssetRef<Asset>> CreateProceduralBuiltinAsset(AssetHandle /*handle*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "CreateProceduralBuiltinAsset is an M6 contract stub");
	}

	MaterialData CreateBuiltinMaterial(BuiltinMaterial /*material*/)
	{
		ENGINE_CONTRACT_STUB();
		return MaterialData();
	}

}
