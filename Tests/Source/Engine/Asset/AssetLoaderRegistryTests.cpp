#include "TestsPCH.h"

#include "Engine/Asset/AssetLoaderRegistry.h"

#include "Engine/Asset/BuiltinMeshes.h"
#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/MeshData.h"
#include "Support/SceneTestFixture.h"

namespace Engine {

	TEST_SUITE("Asset")
	{
		TEST_CASE("AssetLoaderRegistry: the M6 loaders are registered" * doctest::skip(true))
		{
			AssetLoaderRegistry loaders;
			RegisterBuiltinLoaders(loaders);
			const std::vector<AssetType> expected = { AssetType::Scene, AssetType::Prefab, AssetType::Mesh, AssetType::Material,
				AssetType::Texture, AssetType::Font };
			CHECK(loaders.GetTypes() == expected);
			for (const AssetType type : expected)
			{
				REQUIRE(loaders.Find(type) != nullptr);
				CHECK(loaders.Find(type)->GetType() == type);
			}
			CHECK(loaders.Find(AssetType::Environment) == nullptr);
		}

		TEST_CASE("AssetLoaderRegistry: dispatches by the cooked header's type" * doctest::skip(true))
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

		TEST_CASE("AssetLoaderRegistry: a type without a loader is Unsupported and corrupt bytes are errors" * doctest::skip(true))
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			AssetLoaderRegistry loaders;
			RegisterBuiltinLoaders(loaders);
			const std::byte payload[4] = {};
			const Buffer environment = WriteCookedArtifact(AssetType::Environment, 1, 1, payload);
			Result<AssetRef<Asset>> unsupported = loaders.Load(environment, { .Registry = registry.get(), .Handle = AssetHandle(0x201) });
			REQUIRE_FALSE(unsupported.has_value());
			CHECK(unsupported.error().GetCode() == ErrorCode::Unsupported);

			Buffer truncated = CookMesh(GenerateBuiltinMesh(BuiltinMesh::Quad), 1);
			truncated.resize(truncated.size() / 2);
			CHECK_FALSE(loaders.Load(truncated, { .Registry = registry.get(), .Handle = AssetHandle(0x104) }).has_value());
		}
	}

}
