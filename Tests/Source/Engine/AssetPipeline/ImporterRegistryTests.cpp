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
			CHECK(importers.FindForExtension(".luau") == nullptr);
			CHECK(importers.GetImporters().size() == 9);
		}

		TEST_CASE("ImporterRegistry: Describe lists ids, main types and extensions sorted by id")
		{
			ImporterRegistry importers;
			RegisterBuiltinImporters(importers);
			const std::vector<AssetImporterDescription> descriptions = importers.Describe();
			REQUIRE(descriptions.size() == 9);
			CHECK(descriptions.front().Id == "Audio");
			CHECK(descriptions.back().Id == "Texture");
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
		}
	}

}
