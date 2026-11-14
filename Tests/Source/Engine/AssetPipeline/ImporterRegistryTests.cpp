#include "TestsPCH.h"

#include "Engine/AssetPipeline/ImporterRegistry.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Support/SceneTestFixture.h"

namespace Engine {

	TEST_SUITE("AssetPipeline")
	{
		TEST_CASE("ImporterRegistry: the built-in importers cover their extensions")
		{
			ImporterRegistry importers;
			RegisterBuiltinImporters(importers);
			struct Expected
			{
				std::string_view Extension{};
				std::string_view Id{};
				AssetType MainType = AssetType::None;
			};
			const Expected expected[] = {
				{ ".png", "Texture", AssetType::Texture },
				{ ".jpg", "Texture", AssetType::Texture },
				{ ".jpeg", "Texture", AssetType::Texture },
				{ ".tga", "Texture", AssetType::Texture },
				{ ".bmp", "Texture", AssetType::Texture },
				{ ".gltf", "Gltf", AssetType::Prefab },
				{ ".glb", "Gltf", AssetType::Prefab },
				{ ".material", "Material", AssetType::Material },
				{ ".scene", "Scene", AssetType::Scene },
				{ ".prefab", "Prefab", AssetType::Prefab },
				{ ".ttf", "Font", AssetType::Font },
				{ ".otf", "Font", AssetType::Font },
				{ ".hdr", "Environment", AssetType::Environment },
				{ ".exr", "Environment", AssetType::Environment },
				// M12.
				{ ".wav", "Audio", AssetType::AudioClip },
				{ ".flac", "Audio", AssetType::AudioClip },
				{ ".mp3", "Audio", AssetType::AudioClip },
				{ ".ogg", "Audio", AssetType::AudioClip },
				{ ".sfx", "SoundEffect", AssetType::AudioClip },
				{ ".luau", "Script", AssetType::Script },
				{ ".replay", "Replay", AssetType::Replay },
				// M18.
				{ ".spritesheet", "SpriteSheet", AssetType::SpriteSheet },
				// M21.
				{ ".tilemap", "Tilemap", AssetType::Tilemap },
				// M22.
				{ ".stringtable", "StringTable", AssetType::StringTable },
				// M24: a graph is a Script asset of its own importer (Docs/Decisions/0035-m24-contract.md decision 2).
				{ ".graph", "Graph", AssetType::Script },
				// M26 (Docs/Decisions/0037-m26-contract.md decisions 8, 15 and 20).
				{ ".irradiance", "IrradianceVolume", AssetType::IrradianceVolume },
				{ ".cube", "Lut", AssetType::Lut },
				{ ".heightfield", "Heightfield", AssetType::Heightfield },
			};
			for (const Expected& entry : expected)
			{
				CAPTURE(std::string(entry.Extension));
				const IAssetImporter* importer = importers.FindForExtension(entry.Extension);
				REQUIRE(importer != nullptr);
				CHECK(importer->GetId() == entry.Id);
				CHECK(importer->GetMainType() == entry.MainType);
				CHECK(importers.FindById(entry.Id) == importer);
				CHECK(importer->GetVersion() >= 1);
			}
			CHECK(importers.FindForExtension(".GLB") == importers.FindById("Gltf"));
			CHECK(importers.FindForExtension(".HDR") == importers.FindById("Environment"));
			CHECK(importers.FindForExtension(".unknown") == nullptr);
			CHECK(importers.FindForExtension(".SpriteSheet") == importers.FindById("SpriteSheet"));
			CHECK(importers.FindForExtension(".StringTable") == importers.FindById("StringTable"));
			CHECK(importers.FindForExtension(".Graph") == importers.FindById("Graph"));
			CHECK(importers.FindForExtension(".CUBE") == importers.FindById("Lut"));
			CHECK(importers.FindForExtension(".HeightField") == importers.FindById("Heightfield"));
			CHECK(importers.GetImporters().size() == 18);
		}

		TEST_CASE("ImporterRegistry: Describe lists ids, main types and extensions sorted by id")
		{
			ImporterRegistry importers;
			RegisterBuiltinImporters(importers);
			const std::vector<AssetImporterDescription> descriptions = importers.Describe();
			REQUIRE(descriptions.size() == 18);
			CHECK(descriptions.front().Id == "Audio");
			CHECK(descriptions.back().Id == "Tilemap");
			for (size_t index = 1; index < descriptions.size(); ++index)
				CHECK(descriptions[index - 1].Id < descriptions[index].Id);
		}

		TEST_CASE("ImporterRegistry: every importer's settings struct is registered")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			ImporterRegistry importers;
			RegisterBuiltinImporters(importers);
			for (const IAssetImporter* importer : importers.GetImporters())
			{
				CAPTURE(std::string(importer->GetId()));
				if (!importer->GetSettingsTypeName().empty())
					CHECK(registry->FindStruct(importer->GetSettingsTypeName()) != nullptr);
			}
			CHECK(registry->FindStruct("TextureImportSettings") != nullptr);
			CHECK(registry->FindStruct("GltfImportSettings") != nullptr);
			CHECK(registry->FindStruct("FontImportSettings") != nullptr);
			CHECK(registry->FindStruct("EnvironmentImportSettings") != nullptr);
			CHECK(registry->FindEnum("TextureUsage") != nullptr);
			CHECK(registry->FindStruct("AudioImportSettings") != nullptr);
			CHECK(registry->FindStruct("SoundEffect") != nullptr);
			// M18: the sprite sheet importer has no settings; its document struct is the Asset module's (RegisterAssetTypes).
			CHECK(importers.FindById("SpriteSheet")->GetSettingsTypeName().empty());
			CHECK(registry->FindStruct("SpriteSheet") != nullptr);
			// M21: neither has the tilemap importer; its document struct is the Asset module's too.
			CHECK(importers.FindById("Tilemap")->GetSettingsTypeName().empty());
			CHECK(registry->FindStruct("Tilemap") != nullptr);
			// M22: neither has the string table importer; its document structs are the Asset module's too.
			CHECK(importers.FindById("StringTable")->GetSettingsTypeName().empty());
			CHECK(registry->FindStruct("StringTable") != nullptr);
			CHECK(registry->FindStruct("StringTableEntry") != nullptr);
			// M24: neither has the graph importer; its document types are registered with the importers' (RegisterGraphTypes).
			CHECK(importers.FindById("Graph")->GetSettingsTypeName().empty());
			for (const std::string_view name : { "Graph", "GraphField", "GraphVariable", "GraphNode", "GraphLink", "GraphPinRef" })
				CHECK_MESSAGE(registry->FindStruct(name) != nullptr, name);
			CHECK(registry->FindEnum("GraphValueKind") != nullptr);
			// M26: nor has the irradiance volume importer (a binary source written by lighting.bake).
			CHECK(importers.FindById("IrradianceVolume")->GetSettingsTypeName().empty());
			// M26: nor has the heightfield importer; its source is binary samples (Asset/HeightfieldData.h).
			CHECK(importers.FindById("Heightfield")->GetSettingsTypeName().empty());
		}
	}

}
