#include "TestsPCH.h"
#include "Support/InMemoryAssetManager.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MaterialData.h"

namespace Engine {

	TEST_SUITE("Support")
	{
		TEST_CASE("InMemoryAssetManager: readable paths and handles resolve across replacement and removal")
		{
			Test::InMemoryAssetManager assets;
			const AssetHandle handle(0x123456);
			assets.Publish(handle, CreateRef<MaterialData>(), "Assets/Named.material");
			CHECK(assets.Resolve("Assets/Named.material") == handle);
			CHECK(assets.Resolve(handle.ToString()) == handle);
			CHECK_FALSE(assets.Resolve("assets/named.material").has_value());
			assets.Publish(handle, CreateRef<MaterialData>());
			CHECK(assets.GetReferencePath(handle) == "Assets/Named.material");
			assets.Publish(handle, CreateRef<MaterialData>(), "Assets/Moved.material");
			CHECK_FALSE(assets.Resolve("Assets/Named.material").has_value());
			CHECK(assets.Resolve("Assets/Moved.material") == handle);
			assets.Remove(handle);
			CHECK_FALSE(assets.Resolve("Assets/Moved.material").has_value());
			CHECK_FALSE(assets.Resolve(handle.ToString()).has_value());
			CHECK(assets.Resolve("engine://Meshes/Cube") == BuiltinAssetHandles::CubeMesh);
		}

		TEST_CASE("InMemoryAssetManager: published assets load with bumped versions and removed ones are NotFound")
		{
			Test::InMemoryAssetManager assets;
			const AssetHandle handle(0x9a01);
			CHECK(assets.GetVersion(handle) == 0);
			assets.Publish(handle, CreateRef<MaterialData>());
			CHECK(assets.GetVersion(handle) == 1);
			CHECK(assets.GetState(handle) == AssetState::Loaded);
			CHECK(assets.GetAssetType(handle) == AssetType::Material);
			Result<AssetRef<Asset>> loaded = assets.Load(handle);
			REQUIRE(loaded.has_value());
			CHECK(AssetCast<MaterialData>(*loaded) != nullptr);
			assets.Publish(handle, CreateRef<MaterialData>());
			CHECK(assets.GetVersion(handle) == 2);
			assets.Remove(handle);
			const Result<AssetRef<Asset>> removed = assets.Load(handle);
			REQUIRE_FALSE(removed.has_value());
			CHECK(removed.error().GetCode() == ErrorCode::NotFound);
			CHECK(assets.GetReferencePath(handle) == "Assets/Test/0000000000009a01");
		}

		TEST_CASE("InMemoryAssetManager: procedural built-ins come from the base, and a published built-in replaces it")
		{
			Test::InMemoryAssetManager assets;
			Result<AssetRef<Asset>> cube = assets.Load(BuiltinAssetHandles::CubeMesh);
			REQUIRE(cube.has_value());
			CHECK((*cube)->GetAssetType() == AssetType::Mesh);
			CHECK(assets.GetReferencePath(BuiltinAssetHandles::CubeMesh) == "engine://Meshes/Cube");
			assets.Publish(BuiltinAssetHandles::StudioEnvironment, CreateRef<MaterialData>());
			Result<AssetRef<Asset>> replaced = assets.Load(BuiltinAssetHandles::StudioEnvironment);
			REQUIRE(replaced.has_value());
			CHECK((*replaced)->GetAssetType() == AssetType::Material);
		}
	}

}
