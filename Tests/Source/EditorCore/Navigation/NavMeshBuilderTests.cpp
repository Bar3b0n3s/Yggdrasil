#include "TestsPCH.h"
#include "EditorCore/Navigation/NavMeshBuilder.h"

#include "Engine/App/ProcessContext.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/NavigationBakeInput.h"
#include "Engine/Scene/Scene.h"
#include "Support/AssetTestFixture.h"
#include "Support/DeathTest.h"
#include "Support/NavigationTestScene.h"
#include "Support/PhysicsTestScene.h"
#include "Support/SceneTestFixture.h"

#include <RecastAlloc.h>
#include <RecastAssert.h>

#include <doctest/doctest.h>

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <string_view>
#include <vector>

// The navigation bake's builder and Recast's process-level hooks (EditorCore/Navigation/NavMeshBuilder.h;
// Docs/Decisions/0045-m30-navigation.md).

namespace Engine {

	namespace {

		// The bake input of `scene` with the default project's one layer.
		NavigationBakeInput CollectInput(const Scene& scene, AssetManager& assets)
		{
			Result<NavigationBakeInput> input = CollectNavigationBakeInput(scene, PhysicsLayerTable(), assets);
			REQUIRE_MESSAGE(input.has_value(), input.error().ToString());
			return std::move(*input);
		}

		// The agent named `name` of `mesh`.
		const NavMeshAgent& FindAgent(const NavMeshData& mesh, std::string_view name)
		{
			const auto found = std::find_if(mesh.Agents.begin(), mesh.Agents.end(), [name](const NavMeshAgent& agent)
			{
				return agent.Name == name;
			});
			REQUIRE(found != mesh.Agents.end());
			return *found;
		}

		// Every polygon vertex of `agent` in world space.
		std::vector<glm::vec3> GetPolygonVertices(const NavMeshAgent& agent)
		{
			std::vector<glm::vec3> vertices;
			for (size_t index = 0; index < agent.GetVertexCount(); ++index)
				vertices.push_back(GetNavMeshVertexPosition(agent, index));
			return vertices;
		}

	}

	ENGINE_DEATH_TEST("EditorCore/RecastAssertReachesTheEngineHandler")
	{
		// A condition the compiler cannot fold: no Recast allocation is live in a fresh child process.
		const size_t live = GetLiveNavMeshBuildAllocationCount();
		rcAssert(live == 1);
	}

	ENGINE_DEATH_TEST("EditorCore/RecastHooksRemovedUnderLiveAllocation")
	{
		void* block = rcAlloc(16, RC_ALLOC_PERM);
		if (block != nullptr)
			RemoveNavMeshBuildHooks();
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("NavMeshBuilder: the Tests main hands Recast's hooks to the process context, as the editor does")
		{
			const ProcessContext* context = ProcessContext::GetCurrent();
			REQUIRE(context != nullptr);
			CHECK(context->GetSpecification().NavMeshBuildHooks.Install == &InstallNavMeshBuildHooks);
			CHECK(context->GetSpecification().NavMeshBuildHooks.Remove == &RemoveNavMeshBuildHooks);
			CHECK(AreNavMeshBuildHooksInstalled());
			// No bake leaves Recast's working data behind.
			CHECK(GetLiveNavMeshBuildAllocationCount() == 0);
		}

		TEST_CASE("NavMeshBuilder: the allocator hook counts every live Recast allocation")
		{
			void* first = rcAlloc(64, RC_ALLOC_TEMP);
			REQUIRE(first != nullptr);
			void* second = rcAlloc(32, RC_ALLOC_PERM);
			REQUIRE(second != nullptr);
			CHECK(GetLiveNavMeshBuildAllocationCount() == 2);
			rcFree(first);
			rcFree(second);
			CHECK(GetLiveNavMeshBuildAllocationCount() == 0);
			// Freeing null frees nothing and counts nothing.
			rcFree(nullptr);
			CHECK(GetLiveNavMeshBuildAllocationCount() == 0);
		}

		TEST_CASE("NavMeshBuilder: a failed Recast assert reaches the engine's assert handler")
		{
			ENGINE_CHECK_DEATH("EditorCore/RecastAssertReachesTheEngineHandler", "Recast: assertion failed");
		}

		TEST_CASE("NavMeshBuilder: removing the hooks refuses a live Recast allocation")
		{
			ENGINE_CHECK_DEATH("EditorCore/RecastHooksRemovedUnderLiveAllocation", "live Recast allocation");
		}

		// World Tree's brief 1 ("How the game will check it") on the bake's side: the arena's walkable floor and platform are
		// meshed, the pillar and the lava channel are not, each agent type keeps its radius from them, the mud patch carries its
		// cost, and no Recast allocation outlives the bake. The paths themselves are the queries' cases.
		TEST_CASE("NavMeshBuilder: the arena's mesh keeps each agent type's clearance from the pillar and the lava and marks the mud's cost")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddNavigationArena(fixture.GetScene()));
			const NavigationBakeInput input = CollectInput(fixture.GetScene(), assets.GetManager());
			const Result<NavMeshData> built = BuildNavMesh(input);
			REQUIRE_MESSAGE(built.has_value(), built.error().ToString());
			CHECK(GetLiveNavMeshBuildAllocationCount() == 0);
			CHECK(ValidateNavMeshData(*built).has_value());
			CHECK(built->InputHash == HashNavigationBakeInput(input));
			REQUIRE(built->AreaCosts.size() == 1);
			CHECK(built->AreaCosts[0] == Test::NavArenaCost);
			REQUIRE(built->Agents.size() == 2);
			CHECK(built->Agents[0].Name == "Small");
			CHECK(built->Agents[1].Name == "Large");

			for (const NavMeshAgent* agent : { &FindAgent(*built, "Small"), &FindAgent(*built, "Large") })
			{
				CAPTURE(agent->Name);
				REQUIRE(agent->GetPolygonCount() > 0);
				// The clearance, less a cell of the voxels' rounding.
				const float clearance = agent->Radius - agent->CellSize;
				bool onPlatform = false;
				bool onFloor = false;
				for (const glm::vec3& vertex : GetPolygonVertices(*agent))
				{
					CHECK_FALSE(Test::IsInsideFootprint(vertex, Test::NavArenaPillarCenter, Test::NavArenaPillarHalfExtents, clearance));
					// The channel's footprint, at the floor's height (the platform is far from it).
					if (vertex.y < 0.5f)
						CHECK_FALSE(Test::IsInsideFootprint(vertex, Test::NavArenaLavaCenter, Test::NavArenaLavaHalfExtents, clearance));
					onPlatform = onPlatform
						|| (vertex.y > 1.8f && Test::IsInsideFootprint(vertex, Test::NavArenaPlatformCenter, Test::NavArenaPlatformHalfExtents, 0.0f));
					onFloor = onFloor || std::abs(vertex.y) < 0.3f;
				}
				CHECK(onPlatform);
				CHECK(onFloor);
				// The mud's polygons carry area 1, the cost's; every other polygon the walkable area.
				CHECK(std::any_of(agent->Areas.begin(), agent->Areas.end(), [](uint8_t area)
				{
					return area == 1;
				}));
				for (const uint8_t area : agent->Areas)
					CHECK((area == 1 || area == NavMeshWalkableArea));
			}
			CHECK(FindAgent(*built, "Small").Radius == 0.4f);
			CHECK(FindAgent(*built, "Large").Radius == 1.0f);
		}

		// The acceptance "two bakes give identical bytes", on the builder's side.
		TEST_CASE("NavMeshBuilder: equal inputs build byte-identical meshes")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddNavigationArena(fixture.GetScene()));
			const Result<NavMeshData> first = BuildNavMesh(CollectInput(fixture.GetScene(), assets.GetManager()));
			const Result<NavMeshData> second = BuildNavMesh(CollectInput(fixture.GetScene(), assets.GetManager()));
			REQUIRE(first.has_value());
			REQUIRE(second.has_value());
			CHECK(WriteNavMeshSource(*first) == WriteNavMeshSource(*second));
		}

		TEST_CASE("NavMeshBuilder: a slope steeper than an agent type's MaxSlope is not walkable for it")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// A slab tilted 60 degrees about Z, alone: walkable only for an agent type that climbs 60 degrees. Both agent types
			// step 1 m, so the ledge filter, which drops a cell whose neighbours differ by more than a step, keeps the slope's
			// cells (0.35 m apart at 0.2 m cells) and only the slope test tells them apart.
			Entity slab = Test::AddBoxBody(scene, "Slab", glm::vec3(0.0f), glm::vec3(4.0f, 0.1f, 4.0f), std::nullopt);
			slab.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = glm::angleAxis(glm::radians(60.0f), glm::vec3(0.0f, 0.0f, 1.0f));
			});
			Entity navigation = scene.CreateEntity("Navigation");
			NavigationComponent settings;
			settings.AgentTypes = { NavAgentType{ .Name = "Walker", .MaxSlope = 45.0f, .MaxStep = 1.0f },
				NavAgentType{ .Name = "Climber", .MaxSlope = 70.0f, .MaxStep = 1.0f } };
			navigation.AddComponent<NavigationComponent>(settings);
			const Result<NavMeshData> built = BuildNavMesh(CollectInput(scene, assets.GetManager()));
			REQUIRE_MESSAGE(built.has_value(), built.error().ToString());
			CHECK(FindAgent(*built, "Walker").GetPolygonCount() == 0);
			CHECK(FindAgent(*built, "Climber").GetPolygonCount() > 0);
		}

		TEST_CASE("NavMeshBuilder: a scene without static geometry bakes agent types without polygons, and a grid too large is refused")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity navigation = scene.CreateEntity("Navigation");
			navigation.AddComponent<NavigationComponent>();
			const Result<NavMeshData> empty = BuildNavMesh(CollectInput(scene, assets.GetManager()));
			REQUIRE_MESSAGE(empty.has_value(), empty.error().ToString());
			REQUIRE(empty->Agents.size() == 1);
			CHECK(empty->Agents[0].GetPolygonCount() == 0);
			CHECK(ValidateNavMeshData(*empty).has_value());

			// A floor wider than MaxNavBakeGridCells cells of 0.2 m.
			static_cast<void>(Test::AddBoxBody(scene, "Field", glm::vec3(0.0f), glm::vec3(500.0f, 0.5f, 10.0f), std::nullopt));
			const Result<NavMeshData> large = BuildNavMesh(CollectInput(scene, assets.GetManager()));
			REQUIRE_FALSE(large.has_value());
			CHECK(large.error().GetCode() == ErrorCode::Validation);
			CHECK(GetLiveNavMeshBuildAllocationCount() == 0);
		}
	}

}
