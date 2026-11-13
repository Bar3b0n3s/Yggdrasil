#include "TestsPCH.h"

#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Asset/TerrainGeometry.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Support/ScriptTestFixture.h"

#include <glm/gtc/matrix_inverse.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// The terrain script surface (M26, C-TERR; Docs/Decisions/0037-m26-contract.md decision 26, stream T-B): Terrain:SampleHeight
// and SampleNormal read the surface under a world point by Asset/TerrainGeometry.h's rule, through the terrain entity's world
// matrix: its XZ in the terrain's local space, the local surface there, and back to world space.

namespace Engine {

	namespace {

		constexpr AssetHandle HillHandle{ 0x7e44a1u };
		constexpr AssetHandle MissingHandle{ 0x7e44a2u };

		// 9 x 9 samples rising along +X to 32,000, with sample (4, 4) raised by 12,000.
		Ref<HeightfieldData> MakeHill()
		{
			Ref<HeightfieldData> heightfield = CreateRef<HeightfieldData>(MakeFlatHeightfield(9));
			for (uint32_t z = 0; z < 9; ++z)
			{
				for (uint32_t x = 0; x < 9; ++x)
					heightfield->Heights[x + 9 * z] = static_cast<uint16_t>(x * 4000u + (x == 4 && z == 4 ? 12000u : 0u));
			}
			return heightfield;
		}

		// An entity `name` with a Terrain of `heightfield` over 8 x 8 metres, 6 m high, moved, turned a quarter about +Y and
		// scaled non-uniformly, so every part of its world matrix enters the sampling.
		Entity AddTerrain(Scene& scene, std::string_view name, AssetHandle heightfield)
		{
			Entity entity = scene.CreateEntity(name);
			entity.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(3.0f, -2.0f, 1.0f);
				transform.Rotation = glm::quat(0.70710677f, 0.0f, 0.70710677f, 0.0f); // a quarter turn about +Y
				transform.Scale = glm::vec3(2.0f, 0.5f, 1.5f);
			});
			TerrainComponent terrain;
			terrain.Heightfield.SetHandle(heightfield);
			terrain.Size = glm::vec2(8.0f);
			terrain.HeightScale = 6.0f;
			entity.AddComponent<TerrainComponent>(terrain);
			return entity;
		}

		// The world point over the local point (x, z) of `entity`'s terrain, its surface's world height and its world unit
		// normal there, from the local surface and the entity's world matrix.
		struct ExpectedSurface
		{
			glm::vec3 Query = glm::vec3(0.0f);
			float Height = 0.0f;
			glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);
		};

		ExpectedSurface GetExpectedSurface(ConstEntity entity, const HeightfieldData& heightfield, glm::vec2 local)
		{
			const TerrainComponent& terrain = entity.GetComponent<TerrainComponent>();
			const TerrainExtent extent{ .Size = terrain.Size, .HeightScale = terrain.HeightScale };
			const std::optional<float> height = SampleTerrainHeight(heightfield, extent, local);
			const std::optional<glm::vec3> normal = SampleTerrainNormal(heightfield, extent, local);
			REQUIRE(height.has_value());
			REQUIRE(normal.has_value());
			const glm::mat4 world = TransformSystem::ComputeWorldMatrix(entity);
			// A query point 10 m above the local plane: only its XZ in the terrain's space matters.
			return ExpectedSurface{ .Query = glm::vec3(world * glm::vec4(local.x, 10.0f, local.y, 1.0f)),
				.Height = glm::vec3(world * glm::vec4(local.x, *height, local.y, 1.0f)).y,
				.Normal = glm::normalize(glm::inverseTranspose(glm::mat3(world)) * *normal) };
		}

		std::string FormatVector(const glm::vec3& value)
		{
			return std::format("vector.create({}, {}, {})", value.x, value.y, value.z);
		}

		void CheckTerrainScript(Test::ScriptTestFixture& fixture, std::string_view source)
		{
			const Result<ScriptEvaluation> result = fixture.Evaluate(source);
			REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
		}

		// The acceptance case's work in `mode`, whose API coverage it adds to `snapshots` ("ScriptApi: every registered
		// function, method, property, operator and constructor has a test").
		void TerrainCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			const Ref<HeightfieldData> hill = MakeHill();
			fixture.GetAssetManager().Publish(HillHandle, hill, "Assets/Terrain/Hill.heightfield");
			Scene& scene = fixture.GetScene();
			const Entity terrain = AddTerrain(scene, "Hill", HillHandle);
			static_cast<void>(AddTerrain(scene, "Bare", AssetHandle()));
			static_cast<void>(AddTerrain(scene, "Broken", MissingHandle));
			REQUIRE(fixture.Start());
			const uint64_t revision = scene.GetRevision();

			// On the raised sample's triangle (local (3.7, 3.4), above the cell's diagonal) and on the plain ramp (local (6.2,
			// 0.9)); outside the terrain (local (-0.5, 2) and (2, 8.5)) nothing.
			const ExpectedSurface bump = GetExpectedSurface(terrain, *hill, glm::vec2(3.7f, 3.4f));
			const ExpectedSurface ramp = GetExpectedSurface(terrain, *hill, glm::vec2(6.2f, 0.9f));
			const glm::mat4 world = TransformSystem::ComputeWorldMatrix(terrain);
			const glm::vec3 west(world * glm::vec4(-0.5f, 0.0f, 2.0f, 1.0f));
			const glm::vec3 north(world * glm::vec4(2.0f, 0.0f, 8.5f, 1.0f));
			CheckTerrainScript(fixture, std::format(R"(
local hill = Scene.FindByName("Hill").Terrain
local function near(a, b) return math.abs(a - b) < 1e-4 end
local function nearVector(a, b) return vector.magnitude(a - b) < 1e-4 end
for _, case in {{ {{{}, {}, {}}}, {{{}, {}, {}}} }} do
	local query, height, normal = case[1], case[2], case[3]
	assert(near(hill:SampleHeight(query), height))
	assert(nearVector(hill:SampleNormal(query), normal))
	assert(near(vector.magnitude(hill:SampleNormal(query)), 1))
end
assert(hill:SampleHeight({}) == nil and hill:SampleNormal({}) == nil)
assert(hill:SampleHeight({}) == nil and hill:SampleNormal({}) == nil)
local bare = Scene.FindByName("Bare").Terrain
assert(bare:SampleHeight(vector.zero) == nil and bare:SampleNormal(vector.zero) == nil)
local broken = Scene.FindByName("Broken").Terrain
assert(broken:SampleHeight(vector.create(3, 0, 1)) == nil and broken:SampleNormal(vector.create(3, 0, 1)) == nil)
assert(not pcall(function() return hill:SampleHeight(vector.create(0 / 0, 0, 0)) end))
return true
)",
											FormatVector(bump.Query), bump.Height, FormatVector(bump.Normal), FormatVector(ramp.Query), ramp.Height, FormatVector(ramp.Normal), FormatVector(west), FormatVector(west), FormatVector(north), FormatVector(north)));
			// Reads only.
			CHECK(scene.GetRevision() == revision);
			CHECK(fixture.ExternalMutations.empty());
			CHECK(fixture.Errors.empty());
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage);
			snapshots.push_back(*coverage);
		}

	}

	namespace Test {

		void RunTerrainBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			TerrainCoverage(mode, snapshots);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptTerrain: SampleHeight and SampleNormal read the surface under a world point, nil outside it or without a heightfield")
		{
			std::vector<ScriptApiCoverage> snapshots;
			TerrainCoverage(RunModes::Editor, snapshots);
		}

		// Reads only: a read-only evaluation (an edit scene's, script.eval outside play) samples the terrain too.
		TEST_CASE("ScriptTerrain: a read-only evaluation samples the terrain")
		{
			Test::ScriptTestFixture fixture({ .ReadOnly = true });
			const Ref<HeightfieldData> hill = MakeHill();
			fixture.GetAssetManager().Publish(HillHandle, hill, "Assets/Terrain/Hill.heightfield");
			const Entity terrain = AddTerrain(fixture.GetScene(), "Hill", HillHandle);
			REQUIRE(fixture.Start());
			const ExpectedSurface ramp = GetExpectedSurface(terrain, *hill, glm::vec2(6.2f, 0.9f));
			CheckTerrainScript(fixture, std::format(R"(
local hill = Scene.FindByName("Hill").Terrain
assert(math.abs(hill:SampleHeight({}) - {}) < 1e-4)
assert(hill:SampleNormal({}) ~= nil)
return true
)",
											FormatVector(ramp.Query), ramp.Height, FormatVector(ramp.Query)));
		}
	}

}
