#include "TestsPCH.h"

#include "Engine/Asset/AssetLoaderRegistry.h"

#include "Engine/Asset/BuiltinMeshes.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Asset/MeshData.h"
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
				AssetType::Texture, AssetType::Environment, AssetType::AudioClip, AssetType::Script, AssetType::Font, AssetType::Replay };
			CHECK(loaders.GetTypes() == expected);
			for (const AssetType type : expected)
			{
				REQUIRE(loaders.Find(type) != nullptr);
				CHECK(loaders.Find(type)->GetType() == type);
			}
			CHECK(loaders.Find(AssetType::None) == nullptr);
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
