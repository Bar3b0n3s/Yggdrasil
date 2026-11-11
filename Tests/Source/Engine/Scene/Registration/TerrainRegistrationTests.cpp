#include "TestsPCH.h"

#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Reflection/ValidationContext.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Support/SceneTestFixture.h"

#include <string_view>

// The Terrain category (M26, C26.1, C-TERR; Docs/Decisions/0037-m26-contract.md decision 4): the contract registers the
// Terrain and the TerrainCollider (collision split from the drawing, as TilemapCollider is from TilemapRenderer), so these
// cases are live; their behaviour is frozen by the skipped cases of the terrain's import, physics, rendering and editing.

namespace Engine {

	namespace {

		const ComponentInfo& RequireTerrainComponent(const TypeRegistry& registry, std::string_view name)
		{
			const ComponentInfo* info = registry.FindComponent(name);
			REQUIRE_MESSAGE(info != nullptr, name);
			return *info;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("TerrainRegistration: the Terrain and its collider are registered after the probes, the collider requiring the Terrain")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& probe = RequireTerrainComponent(*registry, "ReflectionProbe");
			const ComponentInfo& terrain = RequireTerrainComponent(*registry, "Terrain");
			const ComponentInfo& collider = RequireTerrainComponent(*registry, "TerrainCollider");
			CHECK(terrain.GetIndex() == probe.GetIndex() + 1);
			CHECK(collider.GetIndex() == terrain.GetIndex() + 1);
			for (const ComponentInfo* info : { &terrain, &collider })
			{
				CAPTURE(info->GetName());
				CHECK(info->GetCategory() == "Terrain");
				CHECK(info->GetVersion() == 1);
				REQUIRE(info->GetRequires().size() == 1);
				CHECK(info->GetExcludes().empty());
			}
			CHECK(terrain.GetRequires()[0] == registry->FindComponent("Transform"));
			CHECK(collider.GetRequires()[0] == &terrain);
			CHECK(terrain.FindField("Heightfield")->GetMeta().AssetFilter == "Heightfield");
			CHECK(terrain.FindField("SplatMap")->GetMeta().AssetFilter == "Texture");
			REQUIRE(terrain.FindField("Layers") != nullptr);
			CHECK(terrain.FindField("Layers")->GetKind() == FieldType::Array);
			const StructInfo* layer = registry->FindStruct("TerrainLayer");
			REQUIRE(layer != nullptr);
			CHECK(layer->FindField("BaseColorMap")->GetMeta().AssetFilter == "Texture");
			CHECK(layer->FindField("NormalMap")->GetMeta().AssetFilter == "Texture");
			CHECK(collider.FindField("Layer")->GetKind() == FieldType::String);
		}

		TEST_CASE("TerrainRegistration: a terrain holds at most four layers")
		{
			const Scope<TypeRegistry> registry = Test::CreateBuiltinRegistry();
			const ComponentInfo& info = RequireTerrainComponent(*registry, "Terrain");
			ResolveContext resolve;
			resolve.Registry = registry.get();
			TerrainComponent terrain;
			terrain.Layers.resize(MaxTerrainLayers);
			ValidationContext valid;
			info.Validate(&terrain, resolve, valid);
			CHECK(valid.GetErrorCount() == 0);
			terrain.Layers.resize(MaxTerrainLayers + 1);
			ValidationContext invalid;
			info.Validate(&terrain, resolve, invalid);
			CHECK(invalid.GetErrorCount() == 1);
		}
	}

}
