#include "TestsPCH.h"
#include "EditorCore/Navigation/NavMeshBuilder.h"

#include "Engine/App/ProcessContext.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/NavModifierComponent.h"
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
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <optional>
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

		// The area of the polygon of `agent` whose XZ footprint holds the XZ point `point`, or nullopt where no polygon does.
		std::optional<uint8_t> FindAreaAt(const NavMeshAgent& agent, const glm::vec2& point)
		{
			const size_t slots = agent.VerticesPerPolygon;
			for (size_t polygon = 0; polygon < agent.GetPolygonCount(); ++polygon)
			{
				const uint32_t count = GetNavMeshPolygonVertexCount(agent, polygon);
				bool left = false;
				bool right = false;
				for (uint32_t corner = 0; corner < count; ++corner)
				{
					const glm::vec3 a = GetNavMeshVertexPosition(agent, agent.Polygons[polygon * 2 * slots + corner]);
					const glm::vec3 b = GetNavMeshVertexPosition(agent, agent.Polygons[polygon * 2 * slots + (corner + 1) % count]);
					const float side = (b.x - a.x) * (point.y - a.z) - (b.z - a.z) * (point.x - a.x);
					left = left || side > 0.0f;
					right = right || side < 0.0f;
				}
				if (!(left && right))
					return agent.Areas[polygon];
			}
			return std::nullopt;
		}

		// A 20 x 20 m floor whose top is at y = 0 and a Navigation component of one agent type, `agent`.
		void AddFloor(Scene& scene, const NavAgentType& agent = {})
		{
			static_cast<void>(Test::AddBoxBody(scene, "Floor", glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(10.0f, 0.5f, 10.0f), std::nullopt));
			Entity navigation = scene.CreateEntity("Navigation");
			NavigationComponent settings;
			settings.AgentTypes = { agent };
			navigation.AddComponent<NavigationComponent>(settings);
		}

		// A NavModifier `name` 1 m tall at the XZ point `center`.
		void AddModifier(Scene& scene, std::string_view name, const glm::vec2& center, const glm::vec2& halfExtents, NavModifierKind kind, float cost)
		{
			Entity entity = scene.CreateEntity(name);
			entity.Patch<TransformComponent>([&center](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(center.x, 0.0f, center.y);
			});
			entity.AddComponent<NavModifierComponent>(
				NavModifierComponent{ .Kind = kind, .Cost = cost, .HalfExtents = glm::vec3(halfExtents.x, 0.5f, halfExtents.y) });
		}

		// Recast's allocations while a FailingRecastAllocator is in place: the live count, and which allocation of at least
		// LargeRecastAllocation bytes fails.
		constexpr size_t LargeRecastAllocation = 4096;
		struct FailingRecastAllocations
		{
			std::atomic<size_t> Live{ 0 };
			std::atomic<size_t> LargeSeen{ 0 };
			size_t FailingLarge = 0; // 1 for the first large allocation
		};

		FailingRecastAllocations& GetFailingRecastAllocations()
		{
			static FailingRecastAllocations s_Allocations;
			return s_Allocations;
		}

		void* AllocateFailing(size_t size, rcAllocHint /*hint*/)
		{
			FailingRecastAllocations& allocations = GetFailingRecastAllocations();
			if (size >= LargeRecastAllocation && allocations.LargeSeen.fetch_add(1) + 1 == allocations.FailingLarge)
				return nullptr;
			void* memory = std::malloc(size);
			if (memory != nullptr)
				allocations.Live.fetch_add(1);
			return memory;
		}

		void FreeFailing(void* memory)
		{
			if (memory == nullptr)
				return;
			GetFailingRecastAllocations().Live.fetch_sub(1);
			std::free(memory);
		}

		// Recast's allocator for its lifetime: the C runtime's heap, counted, failing the `failingLarge`-th allocation of at
		// least LargeRecastAllocation bytes; the process's hooks come back at its end.
		class FailingRecastAllocator
		{
		public:
			explicit FailingRecastAllocator(size_t failingLarge)
			{
				RemoveNavMeshBuildHooks();
				FailingRecastAllocations& allocations = GetFailingRecastAllocations();
				allocations.Live.store(0);
				allocations.LargeSeen.store(0);
				allocations.FailingLarge = failingLarge;
				rcAllocSetCustom(&AllocateFailing, &FreeFailing);
			}
			~FailingRecastAllocator()
			{
				rcAllocSetCustom(nullptr, nullptr);
				InstallNavMeshBuildHooks();
			}
			FailingRecastAllocator(const FailingRecastAllocator&) = delete;
			FailingRecastAllocator& operator=(const FailingRecastAllocator&) = delete;
		};

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

		// A grid size or a cell index past int's range is undefined behaviour in Recast, which once let a far collider wrap the
		// grid's size and bake an empty mesh without an error: the builder measures the input in double and refuses it first.
		TEST_CASE("NavMeshBuilder: geometry or a modifier beyond the grid's reach, and a coordinate that is not finite, are refused before Recast reads them")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			{
				// A static collider a million kilometres away along X and Z.
				Test::SceneTestFixture fixture;
				AddFloor(fixture.GetScene());
				static_cast<void>(Test::AddBoxBody(fixture.GetScene(), "Far", glm::vec3(1.0e9f, 0.0f, 1.0e9f), glm::vec3(0.5f), std::nullopt));
				const Result<NavMeshData> far = BuildNavMesh(CollectInput(fixture.GetScene(), assets.GetManager()));
				REQUIRE_FALSE(far.has_value());
				CHECK(far.error().GetCode() == ErrorCode::Validation);
			}
			{
				// A tower taller than MaxNavBakeHeightCells cells of 0.1 m above the floor.
				Test::SceneTestFixture fixture;
				AddFloor(fixture.GetScene());
				static_cast<void>(Test::AddBoxBody(fixture.GetScene(), "Tower", glm::vec3(0.0f, 500.0f, 0.0f), glm::vec3(0.5f, 500.0f, 0.5f), std::nullopt));
				const Result<NavMeshData> tall = BuildNavMesh(CollectInput(fixture.GetScene(), assets.GetManager()));
				REQUIRE_FALSE(tall.has_value());
				CHECK(tall.error().GetCode() == ErrorCode::Validation);
			}
			{
				// A NavModifier a million kilometres away.
				Test::SceneTestFixture fixture;
				AddFloor(fixture.GetScene());
				AddModifier(fixture.GetScene(), "Far", glm::vec2(1.0e9f, 0.0f), glm::vec2(1.0f), NavModifierKind::Cost, 2.0f);
				const Result<NavMeshData> far = BuildNavMesh(CollectInput(fixture.GetScene(), assets.GetManager()));
				REQUIRE_FALSE(far.has_value());
				CHECK(far.error().GetCode() == ErrorCode::Validation);
			}
			// An input built by hand: a vertex or a modifier's extent that is not finite.
			NavigationBakeInput input;
			input.Vertices = { glm::vec3(0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(1.0f, 0.0f, 0.0f) };
			input.Indices = { 0, 1, 2 };
			REQUIRE(BuildNavMesh(input).has_value());
			for (const float bad : { std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN() })
			{
				NavigationBakeInput vertex = input;
				vertex.Vertices[1].x = bad;
				const Result<NavMeshData> refused = BuildNavMesh(vertex);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::Validation);
				NavigationBakeInput modifier = input;
				modifier.Modifiers.push_back(NavBakeModifier{ .Kind = NavModifierKind::Cost, .Cost = 2.0f, .MinY = -1.0f, .MaxY = bad });
				const Result<NavMeshData> unmarked = BuildNavMesh(modifier);
				REQUIRE_FALSE(unmarked.has_value());
				CHECK(unmarked.error().GetCode() == ErrorCode::Validation);
			}
			CHECK(GetLiveNavMeshBuildAllocationCount() == 0);
		}

		// The walkable test is "the slope is at most MaxSlope", so an agent type meant for flat ground still walks it.
		TEST_CASE("NavMeshBuilder: an agent type whose MaxSlope is 0 walks a flat floor")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			AddFloor(fixture.GetScene(), NavAgentType{ .Name = "Flat", .MaxSlope = 0.0f });
			const Result<NavMeshData> built = BuildNavMesh(CollectInput(fixture.GetScene(), assets.GetManager()));
			REQUIRE_MESSAGE(built.has_value(), built.error().ToString());
			CHECK(FindAgent(*built, "Flat").GetPolygonCount() > 0);
		}

		// Where boxes overlap, a NotWalkable box's hole stays a hole under any cost, and the highest cost wins whatever the
		// boxes' order in the scene.
		TEST_CASE("NavMeshBuilder: where modifiers overlap, NotWalkable wins, then the highest cost")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddFloor(scene);
			// The costlier box first in the scene's order, so marking in that order would let the cheaper one win the overlap.
			AddModifier(scene, "High", glm::vec2(0.0f), glm::vec2(2.0f), NavModifierKind::Cost, 8.0f);
			AddModifier(scene, "Low", glm::vec2(2.0f, 0.0f), glm::vec2(2.0f), NavModifierKind::Cost, 2.0f);
			AddModifier(scene, "Around", glm::vec2(-6.0f, 0.0f), glm::vec2(3.0f), NavModifierKind::Cost, 4.0f);
			AddModifier(scene, "Hole", glm::vec2(-6.0f, 0.0f), glm::vec2(1.0f), NavModifierKind::NotWalkable, 1.0f);
			const Result<NavMeshData> built = BuildNavMesh(CollectInput(scene, assets.GetManager()));
			REQUIRE_MESSAGE(built.has_value(), built.error().ToString());
			// The distinct costs sorted: area 1 costs 2, area 2 costs 4, area 3 costs 8.
			REQUIRE(built->AreaCosts == std::vector<float>{ 2.0f, 4.0f, 8.0f });
			const NavMeshAgent& agent = built->Agents[0];
			CHECK(FindAreaAt(agent, glm::vec2(1.0f, 0.0f)) == std::optional<uint8_t>(3));
			CHECK(FindAreaAt(agent, glm::vec2(-1.0f, 0.0f)) == std::optional<uint8_t>(3));
			CHECK(FindAreaAt(agent, glm::vec2(3.0f, 0.0f)) == std::optional<uint8_t>(1));
			CHECK(FindAreaAt(agent, glm::vec2(-6.0f, 2.3f)) == std::optional<uint8_t>(2));
			CHECK(FindAreaAt(agent, glm::vec2(-6.0f, 0.0f)) == std::nullopt);
			CHECK(FindAreaAt(agent, glm::vec2(7.0f, 7.0f)) == std::optional<uint8_t>(NavMeshWalkableArea));
		}

		TEST_CASE("NavMeshBuilder: modifiers of more distinct costs than a mesh holds are refused")
		{
			NavigationBakeInput input;
			for (uint32_t cost = 1; cost <= MaxNavMeshAreaCosts; ++cost)
				input.Modifiers.push_back(NavBakeModifier{ .Kind = NavModifierKind::Cost, .Cost = static_cast<float>(cost) });
			const Result<NavMeshData> most = BuildNavMesh(input);
			REQUIRE_MESSAGE(most.has_value(), most.error().ToString());
			CHECK(most->AreaCosts.size() == MaxNavMeshAreaCosts);
			// A repeated cost adds no area; one more distinct cost is refused.
			input.Modifiers.push_back(NavBakeModifier{ .Kind = NavModifierKind::Cost, .Cost = 1.0f });
			CHECK(BuildNavMesh(input).has_value());
			input.Modifiers.push_back(NavBakeModifier{ .Kind = NavModifierKind::Cost, .Cost = 100.0f });
			const Result<NavMeshData> refused = BuildNavMesh(input);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Validation);
		}

		// Recast's allocation failures are OutOfMemory, as BuildNavMesh documents, never a Validation error that sends an
		// agent to change the settings: the heightfield's column array (whose failure Recast reports without a reason) and
		// the rasterizer's first span pool (which Recast logs as out of memory). Recast's working data is freed either way.
		TEST_CASE("NavMeshBuilder: Recast's allocation failures are OutOfMemory and free its working data")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			AddFloor(fixture.GetScene());
			const NavigationBakeInput input = CollectInput(fixture.GetScene(), assets.GetManager());
			for (const size_t failing : { size_t{ 1 }, size_t{ 2 } })
			{
				CAPTURE(failing);
				std::optional<Result<NavMeshData>> built;
				{
					const FailingRecastAllocator allocator(failing);
					built.emplace(BuildNavMesh(input));
					CHECK(GetFailingRecastAllocations().Live.load() == 0);
					CHECK(GetFailingRecastAllocations().LargeSeen.load() >= failing);
				}
				REQUIRE(built.has_value());
				REQUIRE_FALSE(built->has_value());
				CHECK(built->error().GetCode() == ErrorCode::OutOfMemory);
			}
			CHECK(AreNavMeshBuildHooksInstalled());
			CHECK(BuildNavMesh(input).has_value());
		}
	}

}
