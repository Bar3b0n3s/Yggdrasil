#include "TestsPCH.h"

#include "Engine/Asset/BuiltinAssets.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Core/FileSystem.h"
#include "Support/TestData.h"

#include <algorithm>
#include <set>

namespace Engine {

	TEST_SUITE("Asset")
	{
		TEST_CASE("BuiltinAssets: the handles lie in the reserved range and placeholders map to built-ins")
		{
			CHECK(IsBuiltinAssetHandle(BuiltinAssetHandles::CubeMesh));
			CHECK(IsBuiltinAssetHandle(BuiltinAssetHandles::SkyEnvironment));
			CHECK_FALSE(IsBuiltinAssetHandle(AssetHandle()));
			CHECK_FALSE(IsBuiltinAssetHandle(AssetHandle(0x400)));
			CHECK(GetPlaceholderHandle(AssetType::Mesh) == BuiltinAssetHandles::CubeMesh);
			CHECK(GetPlaceholderHandle(AssetType::Texture) == BuiltinAssetHandles::MissingTexture);
			CHECK(GetPlaceholderHandle(AssetType::Material) == BuiltinAssetHandles::ErrorMaterial);
			CHECK(GetPlaceholderHandle(AssetType::Font) == BuiltinAssetHandles::DefaultFont);
			CHECK_FALSE(GetPlaceholderHandle(AssetType::Scene).IsValid());
			CHECK_FALSE(GetPlaceholderHandle(AssetType::Script).IsValid());
		}

		TEST_CASE("BuiltinAssets: the procedural entries are the compiled-in meshes, materials and textures")
		{
			const std::span<const BuiltinAssetEntry> entries = GetProceduralBuiltinEntries();
			REQUIRE(entries.size() == 14);
			std::set<std::string> paths;
			for (size_t index = 0; index < entries.size(); ++index)
			{
				CAPTURE(entries[index].Path);
				CHECK(IsBuiltinAssetHandle(entries[index].Handle));
				CHECK(entries[index].Source == BuiltinAssetSource::Procedural);
				CHECK(entries[index].Path.starts_with("engine://"));
				CHECK(entries[index].File.empty());
				CHECK(entries[index].Importer.empty());
				CHECK(entries[index].Settings.IsNull());
				CHECK(entries[index].Generator.empty());
				CHECK(paths.insert(entries[index].Path).second);
				if (index > 0)
					CHECK(entries[index - 1].Handle < entries[index].Handle);
			}
			CHECK(entries.front().Handle == BuiltinAssetHandles::CubeMesh);
			CHECK(entries.front().Path == "engine://Meshes/Cube");
			CHECK(entries.back().Handle == BuiltinAssetHandles::MissingTexture);
		}

		TEST_CASE("BuiltinAssets: EngineAssets.json names exactly the compiled-in handles" * doctest::skip(true))
		{
			Result<std::string> text = FileSystem::ReadText(Test::GetRepositoryRoot() / "Resources" / BuiltinAssetCatalog::FileName);
			REQUIRE_MESSAGE(text.has_value(), text.error().ToString());
			Result<BuiltinAssetCatalog> catalog = BuiltinAssetCatalog::Parse(*text);
			REQUIRE_MESSAGE(catalog.has_value(), catalog.error().ToString());
			// The file is canonical (load then save is byte-identical, §6).
			CHECK(catalog->ToText() == *text);
			for (const BuiltinAssetEntry& procedural : GetProceduralBuiltinEntries())
			{
				CAPTURE(procedural.Path);
				const BuiltinAssetEntry* entry = catalog->Find(procedural.Handle);
				REQUIRE(entry != nullptr);
				CHECK(*entry == procedural);
			}
			const BuiltinAssetEntry* font = catalog->Find(BuiltinAssetHandles::DefaultFont);
			REQUIRE(font != nullptr);
			CHECK(font->Path == "engine://Fonts/Default");
			CHECK(font->Source == BuiltinAssetSource::File);
			CHECK(font->File == "Fonts/Inter-Regular.ttf");
			CHECK(font->Importer == "Font");
			CHECK(catalog->FindByPath("engine://Environments/Studio") != nullptr);
			CHECK(catalog->FindByPath("engine://Environments/Sky") != nullptr);
			// Every File entry's file is committed.
			for (const BuiltinAssetEntry& entry : catalog->GetEntries())
			{
				if (entry.Source == BuiltinAssetSource::File)
					CHECK(FileSystem::Exists(Test::GetRepositoryRoot() / "Resources" / entry.File));
			}
			CHECK(catalog->GetEntries().size() == GetProceduralBuiltinEntries().size() + 3);
		}

		TEST_CASE("BuiltinAssets: every procedural built-in is generated and valid" * doctest::skip(true))
		{
			for (const BuiltinAssetEntry& entry : GetProceduralBuiltinEntries())
			{
				CAPTURE(entry.Path);
				Result<AssetRef<Asset>> asset = CreateProceduralBuiltinAsset(entry.Handle);
				REQUIRE_MESSAGE(asset.has_value(), asset.error().ToString());
				CHECK((*asset)->GetAssetType() == entry.Type);
				if (const AssetRef<MeshData> mesh = AssetCast<MeshData>(*asset))
					CHECK(ValidateMeshData(*mesh).has_value());
				if (const AssetRef<TextureData> texture = AssetCast<TextureData>(*asset))
					CHECK(ValidateTextureData(*texture).has_value());
			}
			Result<AssetRef<Asset>> notProcedural = CreateProceduralBuiltinAsset(BuiltinAssetHandles::DefaultFont);
			REQUIRE_FALSE(notProcedural.has_value());
			CHECK(notProcedural.error().GetCode() == ErrorCode::NotFound);
			const MaterialData error = CreateBuiltinMaterial(BuiltinMaterial::Error);
			CHECK(error.Emissive != glm::vec3(0.0f));
			const MaterialData standard = CreateBuiltinMaterial(BuiltinMaterial::Default);
			CHECK(standard.BaseColor == glm::vec4(1.0f));
			CHECK(standard.Roughness == 0.5f);
		}
	}

}
