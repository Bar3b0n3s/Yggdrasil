#include "TestsPCH.h"

#include "Engine/Asset/AssetLoaderRegistry.h"

#include "Engine/Asset/AudioClipData.h"
#include "Engine/Asset/BuiltinMeshes.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

namespace Engine {

	TEST_SUITE("Asset")
	{
		TEST_CASE("AssetLoaderRegistry: the built-in loaders are registered")
		{
			AssetLoaderRegistry loaders;
			RegisterBuiltinLoaders(loaders);
			// Every supported asset kind, in persisted AssetType order.
			const std::vector<AssetType> expected = { AssetType::Scene, AssetType::Prefab, AssetType::Mesh, AssetType::Material,
				AssetType::Texture, AssetType::Environment, AssetType::AudioClip, AssetType::Script, AssetType::Font, AssetType::Replay,
				AssetType::SpriteSheet, AssetType::Tilemap, AssetType::Skeleton, AssetType::AnimationClip };
			CHECK(loaders.GetTypes() == expected);
			for (const AssetType type : expected)
			{
				REQUIRE(loaders.Find(type) != nullptr);
				CHECK(loaders.Find(type)->GetType() == type);
			}
			CHECK(loaders.Find(AssetType::None) == nullptr);
		}

		// M23 (Docs/Decisions/0034-m23-contract.md decision 20): the version the caches compare a cached artifact's header
		// with is the one each loader requires.
		TEST_CASE("AssetLoaderRegistry: GetCookedFormatVersion names the payload format version each built-in loader reads")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			AssetLoaderRegistry loaders;
			RegisterBuiltinLoaders(loaders);
			CHECK_FALSE(GetCookedFormatVersion(AssetType::None).has_value());
			// Every AssetType has a version and a loader, a type appended later included.
			uint16_t value = 1;
			for (; AssetTypeFromString(AssetTypeToString(static_cast<AssetType>(value))) == static_cast<AssetType>(value); ++value)
			{
				const AssetType type = static_cast<AssetType>(value);
				CAPTURE(AssetTypeToString(type));
				const std::optional<uint16_t> version = GetCookedFormatVersion(type);
				REQUIRE(version.has_value());
				REQUIRE(loaders.Find(type) != nullptr);
				// The loader refuses the versions around it as UnsupportedVersion and never refuses its own as such (an empty
				// payload fails as its reader's Parse or Validation instead).
				const std::byte payload[1] = {};
				for (const uint16_t other : { static_cast<uint16_t>(*version - 1), static_cast<uint16_t>(*version + 1) })
				{
					const Result<AssetRef<Asset>> refused = loaders.Load(WriteCookedArtifact(type, other, 1, payload), { .Registry = registry.get(), .Handle = AssetHandle(0x301) });
					REQUIRE_FALSE(refused.has_value());
					CHECK(refused.error().GetCode() == ErrorCode::UnsupportedVersion);
				}
				const Result<AssetRef<Asset>> own = loaders.Load(WriteCookedArtifact(type, *version, 1, payload), { .Registry = registry.get(), .Handle = AssetHandle(0x301) });
				CHECK((own.has_value() || own.error().GetCode() != ErrorCode::UnsupportedVersion));
			}
			CHECK_FALSE(GetCookedFormatVersion(static_cast<AssetType>(value)).has_value());
			CHECK(GetCookedFormatVersion(AssetType::AudioClip) == AudioClipData::FormatVersion);
			CHECK(GetCookedFormatVersion(AssetType::Texture) == TextureData::FormatVersion);
			CHECK(GetCookedFormatVersion(AssetType::Mesh) == MeshData::FormatVersion);
		}

		TEST_CASE("AssetLoaderRegistry: dispatches by the cooked header's type")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			AssetLoaderRegistry loaders;
			RegisterBuiltinLoaders(loaders);
			const MeshData cube = GenerateBuiltinMesh(BuiltinMesh::Cube);
			const Buffer cooked = CookMesh(cube, 1);
			Result<AssetRef<Asset>> loaded = loaders.Load(cooked, { .Registry = registry.get(), .Handle = AssetHandle(0x101) });
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			const AssetRef<MeshData> mesh = AssetCast<MeshData>(*loaded);
			REQUIRE(mesh != nullptr);
			CHECK(SerializeMeshPayload(*mesh) == SerializeMeshPayload(cube));
		}

		TEST_CASE("AssetLoaderRegistry: a type without a loader is Unsupported and corrupt bytes are errors")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			AssetLoaderRegistry loaders;
			const std::byte payload[4] = {};
			const Buffer replay = WriteCookedArtifact(AssetType::Replay, 1, 1, payload);
			Result<AssetRef<Asset>> unsupported = loaders.Load(replay, { .Registry = registry.get(), .Handle = AssetHandle(0x201) });
			REQUIRE_FALSE(unsupported.has_value());
			CHECK(unsupported.error().GetCode() == ErrorCode::Unsupported);
			RegisterBuiltinLoaders(loaders);

			Buffer truncated = CookMesh(GenerateBuiltinMesh(BuiltinMesh::Quad), 1);
			truncated.resize(truncated.size() / 2);
			CHECK_FALSE(loaders.Load(truncated, { .Registry = registry.get(), .Handle = AssetHandle(0x104) }).has_value());
		}

		TEST_CASE("AssetLoaderRegistry: scene and prefab documents reach their loaders, errors name the handle")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			AssetLoaderRegistry loaders;
			RegisterBuiltinLoaders(loaders);
			Result<Json> document = JsonReader::Parse(R"({"Format": "Prefab", "Version": 1, "Entities": []})");
			REQUIRE(document.has_value());
			Result<Buffer> cooked = CookDocument(AssetType::Prefab, *document, 1);
			REQUIRE(cooked.has_value());
			Result<AssetRef<Asset>> loaded = loaders.Load(*cooked, { .Registry = registry.get(), .Handle = AssetHandle(0x1234) });
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			const AssetRef<PrefabData> prefab = AssetCast<PrefabData>(*loaded);
			REQUIRE(prefab != nullptr);
			CHECK(*prefab->Document == *document);

			// A prefab document cooked as a scene is rejected by the scene loader, naming the asset.
			Result<Buffer> mislabelled = CookDocument(AssetType::Scene, *document, 1);
			REQUIRE(mislabelled.has_value());
			Result<AssetRef<Asset>> rejected = loaders.Load(*mislabelled, { .Registry = registry.get(), .Handle = AssetHandle(0x1234) });
			REQUIRE_FALSE(rejected.has_value());
			CHECK(rejected.error().ToString().find("0000000000001234") != std::string::npos);
		}
	}

}
