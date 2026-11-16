#include "TestsPCH.h"
#include "Engine/Scene/NavigationDebugDraw.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/NavMeshData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Renderer/DebugDrawList.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/NavigationComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderAnnotations.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/AutomationTestClient.h"
#include "Support/NavigationTestScene.h"

#include <doctest/doctest.h>

#include <nlohmann/json.hpp>

#include <cstddef>
#include <variant>

// The navigation view (Scene/NavigationDebugDraw.h; Docs/Decisions/0045-m30-navigation.md decision 12): the baked mesh's
// polygon edges, and their way into a view through RenderViewFlags::Navigation and AppendRenderAnnotations.

namespace Engine {

	namespace {

		// Bakes the arena into the active scene and returns its mesh.
		AssetRef<NavMeshData> BakeArena(Test::AutomationFixture& setup)
		{
			static_cast<void>(Test::AddNavigationArena(setup.GetEditor().GetScene()));
			const Result<Json> baked = setup.Call("navigation.bake", Json::object());
			REQUIRE_MESSAGE(baked.has_value(), baked.error().ToString());
			const AssetHandle handle = setup.GetEditor().GetScene().FindEntityByPath("/Navigation").GetComponent<NavigationComponent>().NavMesh.GetHandle();
			const Result<AssetRef<Asset>> loaded = setup.GetEditor().GetAssets().Load(handle);
			REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
			AssetRef<NavMeshData> mesh = AssetCast<NavMeshData>(*loaded);
			REQUIRE(mesh != nullptr);
			return mesh;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("NavigationDebugDraw: the view draws every polygon edge of each agent type once, above its vertices, in its agent type's colour")
		{
			Test::AutomationFixture setup("NavigationDebugDrawEdges");
			const AssetRef<NavMeshData> mesh = BakeArena(setup);
			// Recast's polygons name each other across a shared edge, so the view draws every border edge and half of the
			// edges between polygons.
			size_t expected = 0;
			for (const NavMeshAgent& agent : mesh->Agents)
			{
				size_t borders = 0;
				size_t shared = 0;
				for (size_t polygon = 0; polygon < agent.GetPolygonCount(); ++polygon)
					for (uint32_t edge = 0; edge < GetNavMeshPolygonVertexCount(agent, polygon); ++edge)
						(agent.Polygons[polygon * 2 * agent.VerticesPerPolygon + agent.VerticesPerPolygon + edge] == NavMeshNullIndex ? borders : shared) += 1;
				REQUIRE(shared % 2 == 0);
				expected += borders + shared / 2;
			}

			DebugDrawList debug;
			const size_t lines = AppendNavigationDebugDraw(setup.GetEditor().GetScene(), setup.GetEditor().GetAssets(), 0.5f, debug);
			CHECK(lines == expected);
			REQUIRE(debug.GetSize() == expected);
			// The first line: polygon 0's first edge of the first agent type, lifted, a border at half the opacity.
			const NavMeshAgent& small = mesh->Agents[0];
			const DebugLine* first = std::get_if<DebugLine>(&debug.GetCommands()[0].Shape);
			REQUIRE(first != nullptr);
			CHECK(first->From == GetNavMeshVertexPosition(small, small.Polygons[0]) + glm::vec3(0.0f, NavigationDebugLift, 0.0f));
			CHECK(first->To == GetNavMeshVertexPosition(small, small.Polygons[1]) + glm::vec3(0.0f, NavigationDebugLift, 0.0f));
			size_t agentLines[2] = {};
			for (const DebugDrawCommand& command : debug.GetCommands())
			{
				REQUIRE(std::holds_alternative<DebugLine>(command.Shape));
				CHECK(command.Duration == 0.0f);
				CHECK(command.Depth == DebugDepthMode::Tested);
				const bool smallColor = glm::vec3(command.Color) == glm::vec3(GetNavigationDebugColor(0, true));
				CHECK((smallColor || glm::vec3(command.Color) == glm::vec3(GetNavigationDebugColor(1, true))));
				CHECK((command.Color.a == 0.5f * GetNavigationDebugColor(0, true).a || command.Color.a == 0.5f * GetNavigationDebugColor(0, false).a));
				++agentLines[smallColor ? 0 : 1];
			}
			CHECK(agentLines[0] > 0);
			CHECK(agentLines[1] > 0);
			CHECK(GetNavigationDebugColor(0, true) != GetNavigationDebugColor(1, true));
		}

		TEST_CASE("NavigationDebugDraw: nothing is drawn without a Navigation component, without a mesh or for a disabled one")
		{
			Test::AutomationFixture setup("NavigationDebugDrawNothing");
			Scene& scene = setup.GetEditor().GetScene();
			AssetManager& assets = setup.GetEditor().GetAssets();
			DebugDrawList debug;
			CHECK(AppendNavigationDebugDraw(scene, assets, 1.0f, debug) == 0);
			static_cast<void>(BakeArena(setup));
			Entity navigation = scene.FindEntityByPath("/Navigation");
			navigation.SetActive(false);
			CHECK(AppendNavigationDebugDraw(scene, assets, 1.0f, debug) == 0);
			navigation.SetActive(true);
			navigation.Patch<NavigationComponent>([](NavigationComponent& component)
			{
				component.NavMesh.SetHandle(AssetHandle{});
			});
			CHECK(AppendNavigationDebugDraw(scene, assets, 1.0f, debug) == 0);
			CHECK(debug.IsEmpty());
		}

		TEST_CASE("RenderAnnotations: the Navigation flag appends the navigation mesh, and the extraction accepts it")
		{
			Test::AutomationFixture setup("NavigationDebugDrawAnnotations");
			static_cast<void>(BakeArena(setup));
			Scene& scene = setup.GetEditor().GetScene();
			Entity camera = scene.CreateEntity("Camera");
			camera.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 18.0f, 22.0f);
			});
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			TransformSystem::Update(scene);
			DebugDrawList lines;
			const size_t expected = AppendNavigationDebugDraw(scene, setup.GetEditor().GetAssets(), 1.0f, lines);
			REQUIRE(expected > 0);

			for (const bool shown : { false, true })
			{
				CAPTURE(shown);
				RenderExtractionRequest request{ .Width = 64, .Height = 48 };
				request.Flags = shown ? RenderViewFlags::Navigation : RenderViewFlags::None;
				Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, request);
				REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
				REQUIRE(snapshot->HasCamera);
				REQUIRE(AppendRenderAnnotations(scene, setup.GetEditor().GetAssets(), PhysicsLayerTable(), nullptr, *snapshot).has_value());
				CHECK(snapshot->AnnotationDraw.GetSize() == (shown ? expected : 0));
			}
			// A flag past Navigation is still unknown.
			RenderExtractionRequest unknown{ .Width = 64, .Height = 48 };
			unknown.Flags = static_cast<RenderViewFlags>(1U << 8);
			CHECK_FALSE(ExtractRenderSnapshot(scene, unknown).has_value());
		}
	}

}
