#include "TestsPCH.h"
#include "Engine/Navigation/NavigationWorld.h"

#include "EditorCore/Navigation/NavMeshBuilder.h"
#include "Engine/Asset/NavMeshData.h"
#include "Engine/Core/Random.h"
#include "Engine/Navigation/NavigationRuntime.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/NavigationComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Scene.h"
#include "Support/AssetTestFixture.h"
#include "Support/NavigationTestScene.h"
#include "Support/SceneTestFixture.h"

#include <doctest/doctest.h>

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

// The run-time navigation mesh and its queries (Navigation/NavigationWorld.h; Docs/Decisions/0045-m30-navigation.md
// decision 10; World Tree's brief 1, "How the game will check it"), on the shared arena (Support/NavigationTestScene.h).

namespace Engine {

	namespace {

		constexpr size_t Small = 0;
		constexpr size_t Large = 1;
		constexpr size_t AnyCorners = 256;

		// The world of `mesh`.
		Scope<NavigationWorld> CreateWorld(const NavMeshData& mesh)
		{
			Result<Scope<NavigationWorld>> world = NavigationWorld::Create(mesh);
			REQUIRE_MESSAGE(world.has_value(), world.error().ToString());
			return std::move(*world);
		}

		// The arena's bake (AddNavigationArena), with an island added when asked: a 3 x 3 m block whose top, 1.5 m above the
		// floor, no agent can climb onto (each steps at most 0.4 m).
		NavMeshData BakeArena(bool island = false)
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddNavigationArena(fixture.GetScene()));
			if (island)
			{
				Entity block = fixture.GetScene().CreateEntity("Island");
				block.Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(-8.0f, 0.5f, 8.0f);
				});
				block.AddComponent<BoxColliderComponent>(BoxColliderComponent{ .HalfExtents = glm::vec3(1.5f, 1.0f, 1.5f) });
			}
			return Test::BakeNavigationScene(fixture.GetScene(), assets.GetManager());
		}

		// The smallest XZ distance from a point along `corners` (sampled every 5 cm) to the footprint of the box at `center`
		// with `halfExtents`: 0 when the path enters it.
		float GetFootprintClearance(const std::vector<glm::vec3>& corners, const glm::vec3& center, const glm::vec3& halfExtents)
		{
			float clearance = std::numeric_limits<float>::max();
			for (size_t index = 0; index + 1 < corners.size(); ++index)
			{
				const glm::vec3 from = corners[index];
				const glm::vec3 to = corners[index + 1];
				const int samples = static_cast<int>(glm::length(to - from) / 0.05f) + 1;
				for (int sample = 0; sample <= samples; ++sample)
				{
					const glm::vec3 point = from + (to - from) * (static_cast<float>(sample) / static_cast<float>(samples));
					const glm::vec2 outside = glm::max(glm::abs(glm::vec2(point.x - center.x, point.z - center.z)) - glm::vec2(halfExtents.x, halfExtents.z), glm::vec2(0.0f));
					clearance = std::min(clearance, glm::length(outside));
				}
			}
			return clearance;
		}

		// A hand-made mesh of one agent type over a 4 x 2 m floor at y = 0 (1 m cells): two square polygons sharing the edge
		// x = 2, and, when `degenerate`, a polygon of zero area along that edge (a repeated vertex, which validation
		// accepts). With `onlyDegenerate`, the zero-area polygon is the mesh's only one.
		NavMeshData MakeHandMadeMesh(bool degenerate, bool onlyDegenerate = false)
		{
			NavMeshData mesh;
			NavMeshAgent agent;
			agent.Name = "Flat";
			agent.CellSize = 1.0f;
			agent.CellHeight = 1.0f;
			agent.BoundsMin = glm::vec3(0.0f);
			agent.BoundsMax = glm::vec3(4.0f, 1.0f, 2.0f);
			agent.Vertices = { 0, 0, 0, 2, 0, 0, 2, 0, 2, 0, 0, 2, 4, 0, 0, 4, 0, 2 };
			constexpr uint16_t N = NavMeshNullIndex;
			// Detour's winding: dtTriArea2D is positive over each polygon's fan.
			std::vector<std::vector<uint16_t>> polygons;
			std::vector<std::vector<uint16_t>> neighbours;
			if (!onlyDegenerate)
			{
				polygons.push_back({ 0, 3, 2, 1 });
				neighbours.push_back({ N, N, 1, N });
				polygons.push_back({ 1, 2, 5, 4 });
				neighbours.push_back({ 0, N, N, N });
			}
			if (degenerate || onlyDegenerate)
			{
				polygons.push_back({ 1, 2, 2 });
				neighbours.push_back({ N, N, N });
			}
			uint32_t detailBase = 0;
			uint32_t triangleBase = 0;
			for (size_t polygon = 0; polygon < polygons.size(); ++polygon)
			{
				const std::vector<uint16_t>& vertices = polygons[polygon];
				for (size_t slot = 0; slot < NavMeshMaxVerticesPerPolygon; ++slot)
					agent.Polygons.push_back(slot < vertices.size() ? vertices[slot] : N);
				for (size_t slot = 0; slot < NavMeshMaxVerticesPerPolygon; ++slot)
					agent.Polygons.push_back(slot < vertices.size() ? neighbours[polygon][slot] : N);
				agent.Areas.push_back(NavMeshWalkableArea);
				const uint32_t count = static_cast<uint32_t>(vertices.size());
				agent.DetailMeshes.insert(agent.DetailMeshes.end(), { detailBase, count, triangleBase, count - 2 });
				for (const uint16_t vertex : vertices)
					for (size_t axis = 0; axis < 3; ++axis)
						agent.DetailVertices.push_back(static_cast<float>(agent.Vertices[vertex * 3 + axis]));
				// A fan: each triangle's edge from vertex 1 to 2 is on the polygon's boundary, and so are the fan's first and last
				// edges (Recast's flags, NavMeshDetailBoundaryEdges).
				for (uint32_t corner = 2; corner < count; ++corner)
				{
					const uint8_t flags = static_cast<uint8_t>((corner == 2 ? 0x01 : 0) | 0x04 | (corner + 1 == count ? 0x10 : 0));
					agent.DetailTriangles.insert(agent.DetailTriangles.end(), { 0, static_cast<uint8_t>(corner - 1), static_cast<uint8_t>(corner), flags });
				}
				detailBase += count;
				triangleBase += count - 2;
			}
			mesh.Agents.push_back(std::move(agent));
			REQUIRE(ValidateNavMeshData(mesh).has_value());
			return mesh;
		}

	}

	TEST_SUITE("Navigation")
	{
		TEST_CASE("NavigationWorld: a path goes around the pillar, up the ramp and never across the lava, and the large agent keeps its clearance")
		{
			// Roadmap M30 and World Tree's brief 1: "a path from one corner to the other goes around the pillars, up the ramp
			// and never across the lava; the large agent's path keeps its larger clearance".
			const NavMeshData mesh = BakeArena();
			Scope<NavigationWorld> world = CreateWorld(mesh);
			REQUIRE(world->GetAgentCount() == 2);
			CHECK(world->GetAgentName(Small) == "Small");
			CHECK(world->FindAgent("Large") == Large);
			CHECK_FALSE(world->FindAgent("large").has_value());

			float pillarClearance[2] = {};
			float lavaClearance[2] = {};
			for (const size_t agent : { Small, Large })
			{
				const NavMeshAgent& baked = mesh.Agents[agent];
				CAPTURE(baked.Name);
				// The clearance a path keeps from a wall: the agent's radius, less a cell of the voxels' rounding and the
				// contours' simplification, which cuts corners by up to NavBakeMaxEdgeError cells.
				const float clearance = baked.Radius - (1.0f + NavBakeMaxEdgeError) * baked.CellSize;

				// Across the pillar: the path bends around it.
				const std::optional<NavigationPath> pillar = world->FindPath(agent, glm::vec3(-6.0f, 0.0f, 0.0f), glm::vec3(-2.0f, 0.0f, 0.0f), AnyCorners);
				REQUIRE(pillar.has_value());
				CHECK(pillar->Complete);
				CHECK(pillar->Corners.size() >= 3);
				pillarClearance[agent] = GetFootprintClearance(pillar->Corners, Test::NavArenaPillarCenter, Test::NavArenaPillarHalfExtents);
				CAPTURE(pillarClearance[agent]);
				CHECK(pillarClearance[agent] >= clearance);

				// Up the ramp to the platform: it climbs only on the ramp.
				const std::optional<NavigationPath> ramp = world->FindPath(agent, glm::vec3(-6.0f, 0.0f, 6.0f), glm::vec3(6.0f, 2.0f, 6.0f), AnyCorners);
				REQUIRE(ramp.has_value());
				CHECK(ramp->Complete);
				CHECK(ramp->Corners.back().y == doctest::Approx(2.0f).epsilon(0.1));
				for (const glm::vec3& corner : ramp->Corners)
					if (corner.y > 0.3f && corner.y < 1.7f)
						CHECK((corner.z > 4.5f && corner.z < 7.5f));

				// Across the lava channel: the path goes around its +X end and never over it.
				const std::optional<NavigationPath> lava = world->FindPath(agent, glm::vec3(-6.0f, 0.0f, -8.0f), glm::vec3(-6.0f, 0.0f, -2.0f), AnyCorners);
				REQUIRE(lava.has_value());
				CHECK(lava->Complete);
				lavaClearance[agent] = GetFootprintClearance(lava->Corners, Test::NavArenaLavaCenter, Test::NavArenaLavaHalfExtents);
				CAPTURE(lavaClearance[agent]);
				CHECK(lavaClearance[agent] >= clearance);
				bool aroundTheEnd = false;
				for (const glm::vec3& corner : lava->Corners)
					aroundTheEnd = aroundTheEnd || corner.x > Test::NavArenaLavaCenter.x + Test::NavArenaLavaHalfExtents.x;
				CHECK(aroundTheEnd);
				CHECK(lava->Corners.front().x == doctest::Approx(-6.0f));
				CHECK(lava->Corners.back().z == doctest::Approx(-2.0f));
			}
			// The large agent type keeps the larger clearance, more than the small one's radius.
			CHECK(pillarClearance[Large] > pillarClearance[Small]);
			CHECK(lavaClearance[Large] > lavaClearance[Small]);
			CHECK(pillarClearance[Large] > mesh.Agents[Small].Radius);
			CHECK(lavaClearance[Large] > mesh.Agents[Small].Radius);
		}

		TEST_CASE("NavigationWorld: a path avoids a costly area when going around it costs less")
		{
			const NavMeshData mesh = BakeArena();
			Scope<NavigationWorld> world = CreateWorld(mesh);
			// Straight through the mud would cost 3 m of floor and 2 m at ten times their length; around it about 6 m.
			const std::optional<NavigationPath> path = world->FindPath(Small, glm::vec3(5.0f, 0.0f, -3.5f), glm::vec3(5.0f, 0.0f, 1.5f), AnyCorners);
			REQUIRE(path.has_value());
			CHECK(path->Complete);
			// The mud's polygons, less a cell of the voxels' rounding, are never crossed.
			CHECK(GetFootprintClearance(path->Corners, Test::NavArenaCostCenter, Test::NavArenaCostHalfExtents - glm::vec3(mesh.Agents[Small].CellSize)) > 0.0f);
		}

		TEST_CASE("NavigationWorld: an unreachable goal gives the partial path to the closest reachable point")
		{
			const NavMeshData mesh = BakeArena(true);
			Scope<NavigationWorld> world = CreateWorld(mesh);
			const glm::vec3 islandTop(-8.0f, 1.5f, 8.0f);
			// The island's top is on the mesh, but no agent can climb onto it.
			// (Recast's detail surface lies up to a CellHeight above the geometry.)
			REQUIRE(world->FindClosestPoint(Small, islandTop, 0.5f).has_value());
			const std::optional<NavigationPath> path = world->FindPath(Small, glm::vec3(-6.0f, 0.0f, 0.0f), islandTop, AnyCorners);
			REQUIRE(path.has_value());
			CHECK_FALSE(path->Complete);
			const glm::vec3 end = path->Corners.back();
			CHECK(std::abs(end.y) < 0.3f);
			// The floor's point closest to the island: beside its footprint, at the agent's clearance.
			CHECK(Test::IsInsideFootprint(end, glm::vec3(-8.0f, 0.0f, 8.0f), glm::vec3(1.5f, 1.0f, 1.5f), mesh.Agents[Small].Radius + 2.0f * mesh.Agents[Small].CellSize));
			CHECK_FALSE(Test::IsInsideFootprint(end, glm::vec3(-8.0f, 0.0f, 8.0f), glm::vec3(1.5f, 1.0f, 1.5f), 0.0f));
		}

		TEST_CASE("NavigationWorld: a path ends where its corner cap cuts it, as an unreachable goal's does")
		{
			const NavMeshData mesh = BakeArena();
			Scope<NavigationWorld> world = CreateWorld(mesh);
			const std::optional<NavigationPath> full = world->FindPath(Small, glm::vec3(-6.0f, 0.0f, -8.0f), glm::vec3(-6.0f, 0.0f, -2.0f), AnyCorners);
			REQUIRE(full.has_value());
			REQUIRE(full->Corners.size() >= 3);
			const std::optional<NavigationPath> cut = world->FindPath(Small, glm::vec3(-6.0f, 0.0f, -8.0f), glm::vec3(-6.0f, 0.0f, -2.0f), 2);
			REQUIRE(cut.has_value());
			CHECK_FALSE(cut->Complete);
			REQUIRE(cut->Corners.size() == 2);
			CHECK(cut->Corners[0] == full->Corners[0]);
			CHECK(cut->Corners[1] == full->Corners[1]);
		}

		TEST_CASE("NavigationWorld: the closest point lies on the mesh within the distance, or there is none")
		{
			const NavMeshData mesh = BakeArena();
			Scope<NavigationWorld> world = CreateWorld(mesh);
			// Above open floor: straight down onto it.
			const std::optional<glm::vec3> below = world->FindClosestPoint(Small, glm::vec3(0.0f, 0.9f, 0.0f), NavigationEndpointSearchDistance);
			REQUIRE(below.has_value());
			CHECK(below->x == doctest::Approx(0.0f));
			CHECK(below->z == doctest::Approx(0.0f));
			CHECK(std::abs(below->y) < 0.25f);
			// Inside the pillar: the floor at its clearance.
			const std::optional<glm::vec3> pillar = world->FindClosestPoint(Small, glm::vec3(-4.0f, 0.0f, 0.0f), 3.0f);
			REQUIRE(pillar.has_value());
			CHECK_FALSE(Test::IsInsideFootprint(*pillar, Test::NavArenaPillarCenter, Test::NavArenaPillarHalfExtents, mesh.Agents[Small].Radius - mesh.Agents[Small].CellSize));
			// Above the pillar's top, which has no polygon, nothing within 40 cm.
			CHECK_FALSE(world->FindClosestPoint(Small, glm::vec3(-4.0f, 3.5f, 0.0f), 0.4f).has_value());
			// Far away: nothing, without walking Detour's tiles.
			CHECK_FALSE(world->FindClosestPoint(Small, glm::vec3(1.0e9f, 0.0f, -1.0e9f), MaxNavigationSearchDistance).has_value());
			CHECK_FALSE(world->FindPath(Small, glm::vec3(1.0e9f, 0.0f, 0.0f), glm::vec3(0.0f), AnyCorners).has_value());
			CHECK_FALSE(world->Raycast(Small, glm::vec3(0.0f, 0.0f, 1.0e9f), glm::vec3(0.0f)).has_value());
		}

		TEST_CASE("NavigationWorld: a raycast along the surface is clear across open floor and stops at a wall's clearance with its normal")
		{
			const NavMeshData mesh = BakeArena();
			Scope<NavigationWorld> world = CreateWorld(mesh);
			const NavMeshAgent& small = mesh.Agents[Small];
			const float wall = Test::NavArenaPillarCenter.x - Test::NavArenaPillarHalfExtents.x - small.Radius;

			const std::optional<NavigationRaycast> open = world->Raycast(Small, glm::vec3(0.0f, 0.0f, -2.0f), glm::vec3(0.0f, 0.0f, 2.0f));
			REQUIRE(open.has_value());
			CHECK(open->Clear);
			CHECK(open->Fraction == 1.0f);
			CHECK(open->Normal == glm::vec3(0.0f));
			CHECK(open->Point.x == doctest::Approx(0.0f));
			CHECK(open->Point.z == doctest::Approx(2.0f));

			const std::optional<NavigationRaycast> pillar = world->Raycast(Small, glm::vec3(-8.0f, 0.0f, 0.0f), glm::vec3(-1.0f, 0.0f, 0.0f));
			REQUIRE(pillar.has_value());
			CHECK_FALSE(pillar->Clear);
			CHECK(std::abs(pillar->Point.x - wall) <= small.CellSize);
			CHECK(pillar->Fraction == doctest::Approx((pillar->Point.x + 8.0f) / 7.0f).epsilon(0.01));
			// The wall's normal points back along the walk.
			CHECK(pillar->Normal.x < -0.99f);
			CHECK(pillar->Normal.y == 0.0f);

			const std::optional<NavigationRaycast> lava = world->Raycast(Small, glm::vec3(-6.0f, 0.0f, -2.0f), glm::vec3(-6.0f, 0.0f, -8.0f));
			REQUIRE(lava.has_value());
			CHECK_FALSE(lava->Clear);
			CHECK(std::abs(lava->Point.z - (Test::NavArenaLavaCenter.z + Test::NavArenaLavaHalfExtents.z + small.Radius)) <= small.CellSize);
			CHECK(lava->Normal.z > 0.99f);
		}

		TEST_CASE("NavigationWorld: random points cover the mesh, repeat with their seed and stay within a radius of their center")
		{
			const NavMeshData mesh = BakeArena();
			Scope<NavigationWorld> world = CreateWorld(mesh);
			const NavMeshAgent& small = mesh.Agents[Small];
			const float clearance = small.Radius - small.CellSize;

			Random random(42);
			std::vector<glm::vec3> points;
			bool onPlatform = false;
			for (int draw = 0; draw < 200; ++draw)
			{
				const std::optional<glm::vec3> point = world->FindRandomPoint(Small, random);
				REQUIRE(point.has_value());
				points.push_back(*point);
				// On the surface, and never where the agent cannot stand.
				const std::optional<glm::vec3> surface = world->FindClosestPoint(Small, *point, 0.05f);
				REQUIRE(surface.has_value());
				CHECK(glm::length(*surface - *point) < 1.0e-3f);
				CHECK_FALSE(Test::IsInsideFootprint(*point, Test::NavArenaPillarCenter, Test::NavArenaPillarHalfExtents, clearance));
				if (point->y < 0.5f)
					CHECK_FALSE(Test::IsInsideFootprint(*point, Test::NavArenaLavaCenter, Test::NavArenaLavaHalfExtents, clearance));
				onPlatform = onPlatform || point->y > 1.8f;
			}
			CHECK(onPlatform);

			const glm::vec3 center(-6.0f, 0.0f, 2.0f);
			for (int draw = 0; draw < 50; ++draw)
			{
				const std::optional<glm::vec3> point = world->FindRandomPointAround(Small, center, 3.0f, random);
				REQUIRE(point.has_value());
				const float dx = point->x - center.x;
				const float dz = point->z - center.z;
				CHECK(dx * dx + dz * dz <= 9.0f);
				points.push_back(*point);
			}

			// The same seed draws the same points and leaves the generator in the same state.
			Random again(42);
			std::vector<glm::vec3> repeated;
			for (int draw = 0; draw < 200; ++draw)
				repeated.push_back(*world->FindRandomPoint(Small, again));
			for (int draw = 0; draw < 50; ++draw)
				repeated.push_back(*world->FindRandomPointAround(Small, center, 3.0f, again));
			CHECK(repeated == points);
			CHECK(again.GetState() == random.GetState());
		}

		TEST_CASE("NavigationWorld: queries keep no state, so equal calls give equal results whatever ran before")
		{
			const NavMeshData mesh = BakeArena();
			Scope<NavigationWorld> world = CreateWorld(mesh);
			const glm::vec3 from(-6.0f, 0.0f, -8.0f);
			const glm::vec3 to(6.0f, 2.0f, 6.0f);
			const std::optional<NavigationPath> first = world->FindPath(Large, from, to, AnyCorners);
			REQUIRE(first.has_value());
			Random random(7);
			for (int query = 0; query < 20; ++query)
			{
				const std::optional<glm::vec3> a = world->FindRandomPoint(Large, random);
				const std::optional<glm::vec3> b = world->FindRandomPoint(Large, random);
				REQUIRE((a.has_value() && b.has_value()));
				static_cast<void>(world->FindPath(Large, *a, *b, AnyCorners));
				static_cast<void>(world->Raycast(Large, *a, *b));
			}
			const std::optional<NavigationPath> second = world->FindPath(Large, from, to, AnyCorners);
			REQUIRE(second.has_value());
			CHECK(second->Complete == first->Complete);
			CHECK(second->Corners == first->Corners);
		}

		TEST_CASE("NavigationWorld: an agent type without polygons finds nothing and draws nothing")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			Entity navigation = fixture.GetScene().CreateEntity("Navigation");
			navigation.AddComponent<NavigationComponent>(NavigationComponent{});
			const NavMeshData mesh = Test::BakeNavigationScene(fixture.GetScene(), assets.GetManager());
			REQUIRE(mesh.Agents.size() == 1);
			REQUIRE(mesh.Agents[0].GetPolygonCount() == 0);
			Scope<NavigationWorld> world = CreateWorld(mesh);
			CHECK_FALSE(world->FindPath(0, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), AnyCorners).has_value());
			CHECK_FALSE(world->FindClosestPoint(0, glm::vec3(0.0f), 1.0f).has_value());
			CHECK_FALSE(world->Raycast(0, glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f)).has_value());
			Random random(3);
			const Random::State state = random.GetState();
			CHECK_FALSE(world->FindRandomPoint(0, random).has_value());
			CHECK_FALSE(world->FindRandomPointAround(0, glm::vec3(0.0f), 5.0f, random).has_value());
			CHECK(random.GetState() == state);
		}

		TEST_CASE("NavigationWorld: a polygon of zero area is never a random point's and leaves the queries working")
		{
			{
				const NavMeshData mesh = MakeHandMadeMesh(true);
				Scope<NavigationWorld> world = CreateWorld(mesh);
				const std::optional<NavigationPath> path = world->FindPath(0, glm::vec3(0.5f, 0.0f, 1.0f), glm::vec3(3.5f, 0.0f, 1.0f), AnyCorners);
				REQUIRE(path.has_value());
				CHECK(path->Complete);
				const std::optional<NavigationRaycast> ray = world->Raycast(0, glm::vec3(0.5f, 0.0f, 1.0f), glm::vec3(3.5f, 0.0f, 1.0f));
				REQUIRE(ray.has_value());
				CHECK(ray->Clear);
				const std::optional<glm::vec3> edge = world->FindClosestPoint(0, glm::vec3(2.0f, 0.5f, 1.0f), 1.0f);
				REQUIRE(edge.has_value());
				CHECK(edge->y == doctest::Approx(0.0f));
				Random random(9);
				for (int draw = 0; draw < 100; ++draw)
				{
					const std::optional<glm::vec3> point = world->FindRandomPoint(0, random);
					REQUIRE(point.has_value());
					CHECK((point->x >= 0.0f && point->x <= 4.0f && point->z >= 0.0f && point->z <= 2.0f));
				}
			}
			// A mesh whose only polygon has no area: nothing to draw from, and no draw made.
			const NavMeshData degenerate = MakeHandMadeMesh(true, true);
			Scope<NavigationWorld> world = CreateWorld(degenerate);
			Random random(9);
			const Random::State state = random.GetState();
			CHECK_FALSE(world->FindRandomPoint(0, random).has_value());
			CHECK(random.GetState() == state);
			CHECK(world->FindClosestPoint(0, glm::vec3(2.0f, 0.0f, 1.0f), 1.0f).has_value());
			static_cast<void>(world->Raycast(0, glm::vec3(2.0f, 0.0f, 0.5f), glm::vec3(3.0f, 0.0f, 0.5f)));
			static_cast<void>(world->FindPath(0, glm::vec3(2.0f, 0.0f, 0.5f), glm::vec3(2.0f, 0.0f, 1.5f), AnyCorners));
		}

		TEST_CASE("NavigationWorld: an invalid payload is refused before Detour reads it, and the world frees every Detour allocation")
		{
			NavMeshData mesh = MakeHandMadeMesh(false);
			mesh.Agents[0].Polygons[0] = 40; // a vertex index past the mesh's six
			const Result<Scope<NavigationWorld>> refused = NavigationWorld::Create(mesh);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Validation);
			CHECK(GetLiveNavigationAllocationCount() == 0);
			{
				Scope<NavigationWorld> world = CreateWorld(MakeHandMadeMesh(false));
				CHECK(GetLiveNavigationAllocationCount() > 0);
			}
			CHECK(GetLiveNavigationAllocationCount() == 0);
		}
	}

}
