#include "TestsPCH.h"
#include "Engine/Scene/NavigationBakeInput.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Asset/TerrainGeometry.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CapsuleColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Components/TerrainColliderComponent.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/AssetTestFixture.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/NavigationTestScene.h"
#include "Support/PhysicsTestScene.h"
#include "Support/SceneTestFixture.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cmath>
#include <optional>
#include <string>
#include <vector>

// The navigation bake's input (Scene/NavigationBakeInput.h; Docs/Decisions/0045-m30-navigation.md decision 6): what the bake
// reads of a scene, its triangulation of every collider kind, its modifiers and its hash.

namespace Engine {

	namespace {

		// A Navigation component on its own entity, with `layers`.
		void AddNavigation(Scene& scene, std::vector<std::string> layers = {})
		{
			Entity entity = scene.CreateEntity("Navigation");
			NavigationComponent navigation;
			navigation.Layers = std::move(layers);
			entity.AddComponent<NavigationComponent>(navigation);
		}

		// A layer table of "Default" and "Props", every pair colliding.
		PhysicsLayerTable MakeLayers()
		{
			const std::vector<std::string> names = { "Default", "Props" };
			const std::vector<std::vector<std::string>> pairs = { { "Default", "Default" }, { "Default", "Props" }, { "Props", "Props" } };
			Result<PhysicsLayerTable> layers = PhysicsLayerTable::Create(names, pairs);
			REQUIRE_MESSAGE(layers.has_value(), layers.error().ToString());
			return std::move(*layers);
		}

		// The unit normal of triangle `triangle` of `input` (cross(b - a, c - a)).
		glm::vec3 TriangleNormal(const NavigationBakeInput& input, size_t triangle)
		{
			const glm::vec3& a = input.Vertices[input.Indices[triangle * 3]];
			const glm::vec3& b = input.Vertices[input.Indices[triangle * 3 + 1]];
			const glm::vec3& c = input.Vertices[input.Indices[triangle * 3 + 2]];
			return glm::normalize(glm::cross(b - a, c - a));
		}

		// The input's triangles' outward check: every triangle's normal points away from `center`.
		bool FacesOutward(const NavigationBakeInput& input, const glm::vec3& center)
		{
			for (size_t triangle = 0; triangle < input.Indices.size() / 3; ++triangle)
			{
				const glm::vec3 centroid = (input.Vertices[input.Indices[triangle * 3]] + input.Vertices[input.Indices[triangle * 3 + 1]]
											   + input.Vertices[input.Indices[triangle * 3 + 2]])
					/ 3.0f;
				if (glm::dot(TriangleNormal(input, triangle), centroid - center) <= 0.0f)
					return false;
			}
			return true;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("NavigationBakeInput: a scene without a Navigation component or with settings out of range is refused")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			const Result<NavigationBakeInput> none = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
			REQUIRE_FALSE(none.has_value());
			CHECK(none.error().GetCode() == ErrorCode::NotFound);

			// A disabled Navigation is no Navigation.
			AddNavigation(scene);
			Entity navigation = scene.FindEntityByPath("/Navigation");
			navigation.SetActive(false);
			CHECK(CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager()).error().GetCode() == ErrorCode::NotFound);
			navigation.SetActive(true);
			REQUIRE(CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager()).has_value());

			// A layer the project does not define, and settings out of range.
			navigation.Patch<NavigationComponent>([](NavigationComponent& settings)
			{
				settings.Layers = { "Lava" };
			});
			CHECK(CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager()).error().GetCode() == ErrorCode::Validation);
			navigation.Patch<NavigationComponent>([](NavigationComponent& settings)
			{
				settings.Layers.clear();
				settings.AgentTypes.clear();
			});
			CHECK(CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager()).error().GetCode() == ErrorCode::Validation);
			navigation.Patch<NavigationComponent>([](NavigationComponent& settings)
			{
				settings.AgentTypes = { NavAgentType{}, NavAgentType{} };
			});
			CHECK(CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager()).error().GetCode() == ErrorCode::Validation);
			navigation.Patch<NavigationComponent>([](NavigationComponent& settings)
			{
				settings.AgentTypes = { NavAgentType{ .Radius = 50.0f } };
			});
			CHECK(CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager()).error().GetCode() == ErrorCode::Validation);
			navigation.Patch<NavigationComponent>([](NavigationComponent& settings)
			{
				settings.AgentTypes = { NavAgentType{} };
				settings.CellSize = 0.0f;
			});
			CHECK(CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager()).error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("NavigationBakeInput: only static, solid, creatable bodies on the named layers are read")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddNavigation(scene);
			// An implicit static box: read (12 triangles).
			static_cast<void>(Test::AddBoxBody(scene, "Wall", glm::vec3(0.0f), glm::vec3(1.0f), std::nullopt));
			const Result<NavigationBakeInput> wall = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
			REQUIRE_MESSAGE(wall.has_value(), wall.error().ToString());
			CHECK(wall->Indices.size() == 36);
			CHECK(wall->Vertices.size() == 8);
			// Dynamic and Kinematic bodies, a trigger and a character: none is read.
			static_cast<void>(Test::AddBoxBody(scene, "Crate", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			static_cast<void>(Test::AddBoxBody(scene, "Lift", glm::vec3(-5.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Kinematic));
			Entity trigger = scene.CreateEntity("Trigger");
			trigger.AddComponent<BoxColliderComponent>(BoxColliderComponent{ .IsTrigger = true });
			Entity hero = scene.CreateEntity("Hero");
			hero.AddComponent<CharacterControllerComponent>();
			const Result<NavigationBakeInput> moving = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
			REQUIRE(moving.has_value());
			CHECK(moving->Indices == wall->Indices);
			CHECK(moving->Vertices == wall->Vertices);
			// A Static RigidBody on the Props layer: read with every layer, left out when the Navigation names only Default.
			Entity shelf = Test::AddBoxBody(scene, "Shelf", glm::vec3(0.0f, 3.0f, 0.0f), glm::vec3(0.5f), BodyType::Static);
			Test::PatchRigidBody(shelf, [](RigidBodyComponent& body)
			{
				body.Layer = "Props";
			});
			const Result<NavigationBakeInput> every = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
			REQUIRE(every.has_value());
			CHECK(every->Indices.size() == 72);
			scene.FindEntityByPath("/Navigation").Patch<NavigationComponent>([](NavigationComponent& settings)
			{
				settings.Layers = { "Default" };
			});
			const Result<NavigationBakeInput> named = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
			REQUIRE(named.has_value());
			CHECK(named->Indices.size() == 36);
			CHECK(named->SkippedBodies.empty());
		}

		TEST_CASE("NavigationBakeInput: boxes, spheres, capsules and meshes are triangulated in world space, facing outward")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddNavigation(scene);
			// A box turned a quarter turn about Y and moved: its corners in world space.
			Entity box = Test::AddBoxBody(scene, "Box", glm::vec3(2.0f, 1.0f, 3.0f), glm::vec3(1.0f, 0.5f, 2.0f), std::nullopt);
			box.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			});
			const Result<NavigationBakeInput> boxed = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
			REQUIRE_MESSAGE(boxed.has_value(), boxed.error().ToString());
			REQUIRE(boxed->Vertices.size() == 8);
			glm::vec3 low(1.0e9f);
			glm::vec3 high(-1.0e9f);
			for (const glm::vec3& vertex : boxed->Vertices)
			{
				low = glm::min(low, vertex);
				high = glm::max(high, vertex);
			}
			// The quarter turn swaps the box's X and Z extents.
			CHECK(low.x == doctest::Approx(0.0f).epsilon(1e-5));
			CHECK(high.x == doctest::Approx(4.0f).epsilon(1e-5));
			CHECK(low.y == doctest::Approx(0.5f).epsilon(1e-5));
			CHECK(high.z == doctest::Approx(4.0f).epsilon(1e-5));
			CHECK(FacesOutward(*boxed, glm::vec3(2.0f, 1.0f, 3.0f)));
			scene.DestroyEntity(box);

			// A sphere of radius 2: every vertex on its surface, both poles, every triangle outward.
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f, 5.0f, 0.0f), 2.0f, std::nullopt));
			const Result<NavigationBakeInput> sphere = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
			REQUIRE(sphere.has_value());
			CHECK(sphere->Vertices.size() == 2 + (NavBakeSphereRings - 1) * NavBakeSphereSegments);
			CHECK(sphere->Indices.size() == 3 * 2 * NavBakeSphereSegments * (NavBakeSphereRings - 1));
			for (const glm::vec3& vertex : sphere->Vertices)
				CHECK(glm::length(vertex - glm::vec3(0.0f, 5.0f, 0.0f)) == doctest::Approx(2.0f).epsilon(1e-5));
			CHECK(FacesOutward(*sphere, glm::vec3(0.0f, 5.0f, 0.0f)));
			scene.DestroyEntity(scene.FindEntityByPath("/Ball"));

			// A capsule of radius 0.5 and half height 1 along Y: its top pole 1.5 m above its centre, every triangle outward.
			Entity capsule = scene.CreateEntity("Post");
			capsule.AddComponent<CapsuleColliderComponent>(CapsuleColliderComponent{ .Radius = 0.5f, .HalfHeight = 1.0f });
			const Result<NavigationBakeInput> post = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
			REQUIRE(post.has_value());
			CHECK(post->Vertices.size() == 2 + NavBakeSphereRings * NavBakeSphereSegments);
			float top = -1.0e9f;
			for (const glm::vec3& vertex : post->Vertices)
				top = std::max(top, vertex.y);
			CHECK(top == doctest::Approx(1.5f).epsilon(1e-5));
			CHECK(FacesOutward(*post, glm::vec3(0.0f)));
			scene.DestroyEntity(capsule);

			// A mesh collider, convex or not, is its mesh's triangles: the built-in cube's 12.
			for (const bool convex : { false, true })
			{
				CAPTURE(convex);
				Entity mesh = scene.CreateEntity("Rock");
				mesh.AddComponent<MeshColliderComponent>(
					MeshColliderComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh), .Convex = convex });
				const Result<NavigationBakeInput> rock = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
				REQUIRE_MESSAGE(rock.has_value(), rock.error().ToString());
				CHECK(rock->Indices.size() == 36);
				CHECK(FacesOutward(*rock, glm::vec3(0.0f)));
				scene.DestroyEntity(mesh);
			}
		}

		// The height field's cells, two triangles each, split along the diagonal and facing up as the terrain's surface is
		// (Asset/TerrainGeometry.h): every triangle lies in the surface's plane at its centroid, its normal the surface's.
		TEST_CASE("NavigationBakeInput: a terrain collider is its height field's cells, split and facing as the terrain's surface")
		{
			constexpr AssetHandle Bowl{ 0x4e41u };
			constexpr uint32_t Samples = 5;
			Test::InMemoryAssetManager assets;
			// Bowed along both axes, so a cell's two triangles are different planes and a wrong split would leave the surface.
			auto heightfield = CreateRef<HeightfieldData>(MakeFlatHeightfield(Samples));
			for (uint32_t z = 0; z < Samples; ++z)
			{
				for (uint32_t x = 0; x < Samples; ++x)
					heightfield->Heights[x + Samples * z] = static_cast<uint16_t>(x * x * 2000 + z * z * 1500);
			}
			assets.Publish(Bowl, heightfield, "Assets/Terrain/Bowl.heightfield");
			const TerrainExtent extent{ .Size = glm::vec2(8.0f, 6.0f), .HeightScale = 4.0f };
			const glm::vec3 origin(1.0f, 2.0f, 3.0f);

			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddNavigation(scene);
			Entity terrain = scene.CreateEntity("Terrain");
			terrain.Patch<TransformComponent>([&origin](TransformComponent& transform)
			{
				transform.Translation = origin;
			});
			terrain.AddComponent<TerrainComponent>(TerrainComponent{
				.Heightfield = TypedAssetHandle<AssetType::Heightfield>(Bowl), .Size = extent.Size, .HeightScale = extent.HeightScale });
			terrain.AddComponent<TerrainColliderComponent>();
			const Result<NavigationBakeInput> input = CollectNavigationBakeInput(scene, MakeLayers(), assets);
			REQUIRE_MESSAGE(input.has_value(), input.error().ToString());
			CHECK(input->SkippedBodies.empty());
			CHECK(input->Vertices.size() == Samples * Samples);
			REQUIRE(input->Indices.size() == 3 * 2 * (Samples - 1) * (Samples - 1));
			for (size_t triangle = 0; triangle < input->Indices.size() / 3; ++triangle)
			{
				CAPTURE(triangle);
				const glm::vec3 centroid = (input->Vertices[input->Indices[triangle * 3]] + input->Vertices[input->Indices[triangle * 3 + 1]]
											   + input->Vertices[input->Indices[triangle * 3 + 2]])
					/ 3.0f;
				const glm::vec2 local(centroid.x - origin.x, centroid.z - origin.z);
				const std::optional<float> height = SampleTerrainHeight(*heightfield, extent, local);
				const std::optional<glm::vec3> normal = SampleTerrainNormal(*heightfield, extent, local);
				REQUIRE(height.has_value());
				REQUIRE(normal.has_value());
				CHECK(centroid.y - origin.y == doctest::Approx(*height).epsilon(1e-4));
				const glm::vec3 facing = TriangleNormal(*input, triangle);
				CHECK(facing.y > 0.0f);
				CHECK(glm::dot(facing, *normal) == doctest::Approx(1.0f).epsilon(1e-4));
			}
		}

		TEST_CASE("NavigationBakeInput: a body whose shape cannot be described adds no triangle and is listed as skipped")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddNavigation(scene);
			static_cast<void>(Test::AddBoxBody(scene, "Wall", glm::vec3(0.0f), glm::vec3(1.0f), std::nullopt));
			// A MeshCollider without a mesh, on an entity without a MeshRenderer: its shape cannot be described.
			Entity rock = scene.CreateEntity("Rock");
			rock.AddComponent<MeshColliderComponent>();
			const Result<NavigationBakeInput> input = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
			REQUIRE_MESSAGE(input.has_value(), input.error().ToString());
			CHECK(input->Indices.size() == 36);
			REQUIRE(input->SkippedBodies.size() == 1);
			CHECK(input->SkippedBodies[0] == rock.GetUUID());
		}

		TEST_CASE("NavigationBakeInput: a collider under a scaled and rotated parent is placed by its world transform, and a disabled one is not read")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddNavigation(scene);
			Entity parent = scene.CreateEntity("Parent");
			parent.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(3.0f, 1.0f, -2.0f);
				transform.Rotation = glm::angleAxis(glm::radians(30.0f), glm::normalize(glm::vec3(0.0f, 1.0f, 1.0f)));
				transform.Scale = glm::vec3(2.0f, 2.0f, 2.0f);
			});
			Entity child = Test::AddBoxBody(scene, "Child", glm::vec3(1.0f, 0.0f, 0.5f), glm::vec3(0.5f, 0.25f, 1.0f), std::nullopt, parent);
			child.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = glm::angleAxis(glm::radians(45.0f), glm::vec3(1.0f, 0.0f, 0.0f));
			});
			const Result<NavigationBakeInput> input = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
			REQUIRE_MESSAGE(input.has_value(), input.error().ToString());
			REQUIRE(input->Vertices.size() == 8);
			// Every corner of the box, placed by the child's world matrix, is a vertex of the input.
			const glm::mat4 world = TransformSystem::ComputeWorldMatrix(child);
			for (uint32_t corner = 0; corner < 8; ++corner)
			{
				CAPTURE(corner);
				const glm::vec3 local((corner & 1U) != 0 ? 0.5f : -0.5f, (corner & 2U) != 0 ? 0.25f : -0.25f, (corner & 4U) != 0 ? 1.0f : -1.0f);
				const glm::vec3 expected(world * glm::vec4(local, 1.0f));
				CHECK(std::any_of(input->Vertices.begin(), input->Vertices.end(), [&expected](const glm::vec3& vertex)
				{
					return glm::length(vertex - expected) < 1.0e-4f;
				}));
			}
			CHECK(FacesOutward(*input, glm::vec3(world[3])));

			// Disabling the child, or its parent, leaves it out.
			child.SetActive(false);
			const Result<NavigationBakeInput> disabled = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
			REQUIRE(disabled.has_value());
			CHECK(disabled->Vertices.empty());
			child.SetActive(true);
			parent.SetActive(false);
			const Result<NavigationBakeInput> hidden = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
			REQUIRE(hidden.has_value());
			CHECK(hidden->Vertices.empty());
		}

		// A world pose that overflows (valid content: two far translations add up past float's range) would hand Recast
		// infinite coordinates; the input refuses it, naming the entity.
		TEST_CASE("NavigationBakeInput: a body or a NavModifier whose world placement is not finite is refused, naming its entity")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddNavigation(scene);
			Entity far = scene.CreateEntity("Far");
			far.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(3.0e38f, 0.0f, 0.0f);
			});
			Entity wall = Test::AddBoxBody(scene, "Wall", glm::vec3(3.0e38f, 0.0f, 0.0f), glm::vec3(1.0f), std::nullopt, far);
			const Result<NavigationBakeInput> body = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
			REQUIRE_FALSE(body.has_value());
			CHECK(body.error().GetCode() == ErrorCode::Validation);
			CHECK(body.error().GetMessageText().find(wall.GetUUID().ToString()) != std::string::npos);
			scene.DestroyEntity(wall);

			Entity hole = scene.CreateEntity("Hole", far);
			hole.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(3.0e38f, 0.0f, 0.0f);
			});
			hole.AddComponent<NavModifierComponent>();
			const Result<NavigationBakeInput> modifier = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
			REQUIRE_FALSE(modifier.has_value());
			CHECK(modifier.error().GetCode() == ErrorCode::Validation);
			CHECK(modifier.error().GetMessageText().find(hole.GetUUID().ToString()) != std::string::npos);
		}

		TEST_CASE("NavigationBakeInput: a mirrored collider keeps its triangles facing outward")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddNavigation(scene);
			Entity box = Test::AddBoxBody(scene, "Mirrored", glm::vec3(0.0f), glm::vec3(1.0f), std::nullopt);
			box.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(-1.0f, 1.0f, 1.0f);
			});
			const Result<NavigationBakeInput> input = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
			REQUIRE_MESSAGE(input.has_value(), input.error().ToString());
			REQUIRE(input->Indices.size() == 36);
			CHECK(FacesOutward(*input, glm::vec3(0.0f)));
		}

		TEST_CASE("NavigationBakeInput: a NavModifier is its box's yawed footprint and vertical extent, its pitch and roll ignored")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddNavigation(scene);
			Entity hole = scene.CreateEntity("Hole");
			hole.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(1.0f, 2.0f, 3.0f);
				transform.Scale = glm::vec3(2.0f, 1.0f, 1.0f);
				// A quarter turn of yaw and some pitch: the footprint turns, the sides stay vertical.
				transform.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f))
					* glm::angleAxis(glm::radians(20.0f), glm::vec3(0.0f, 0.0f, 1.0f));
			});
			hole.AddComponent<NavModifierComponent>(NavModifierComponent{ .Kind = NavModifierKind::Cost, .Cost = 3.0f, .HalfExtents = glm::vec3(1.0f, 0.5f, 0.25f) });
			const Result<NavigationBakeInput> input = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
			REQUIRE_MESSAGE(input.has_value(), input.error().ToString());
			REQUIRE(input->Modifiers.size() == 1);
			const NavBakeModifier& modifier = input->Modifiers[0];
			CHECK(modifier.Entity == hole.GetUUID());
			CHECK(modifier.Kind == NavModifierKind::Cost);
			CHECK(modifier.Cost == 3.0f);
			CHECK(modifier.MinY == doctest::Approx(1.5f));
			CHECK(modifier.MaxY == doctest::Approx(2.5f));
			// The scaled box is 4 m along its X axis and 0.5 m along its Z; the quarter turn of yaw lays its X axis along -Z.
			glm::vec2 low(1.0e9f);
			glm::vec2 high(-1.0e9f);
			for (const glm::vec2& corner : modifier.Corners)
			{
				low = glm::min(low, corner);
				high = glm::max(high, corner);
			}
			CHECK(high.x - low.x == doctest::Approx(0.5f).epsilon(1e-4));
			CHECK(high.y - low.y == doctest::Approx(4.0f).epsilon(1e-4));
			CHECK((low + high).x / 2.0f == doctest::Approx(1.0f).epsilon(1e-4));
			CHECK((low + high).y / 2.0f == doctest::Approx(3.0f).epsilon(1e-4));
		}

		// The acceptance's half (Roadmap M30: "moving a wall makes project.validate report the bake as stale"): the hash follows
		// the static geometry, the modifiers and the settings, and nothing else.
		TEST_CASE("NavigationBakeInput: the hash follows the static geometry, the modifiers and the settings, and nothing else")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddNavigationArena(scene));
			const auto hash = [&scene, &assets]()
			{
				const Result<NavigationBakeInput> input = CollectNavigationBakeInput(scene, MakeLayers(), assets.GetManager());
				REQUIRE_MESSAGE(input.has_value(), input.error().ToString());
				return HashNavigationBakeInput(*input);
			};
			const uint64_t baked = hash();
			CHECK(hash() == baked);

			// The mesh reference, a dynamic body and an entity without colliders change nothing.
			Entity navigation = scene.FindEntityByPath("/Navigation");
			navigation.Patch<NavigationComponent>([](NavigationComponent& settings)
			{
				settings.NavMesh = TypedAssetHandle<AssetType::NavMesh>(AssetHandle(0x1234));
			});
			static_cast<void>(Test::AddBoxBody(scene, "Crate", glm::vec3(1.0f, 3.0f, 1.0f), glm::vec3(0.5f), BodyType::Dynamic));
			static_cast<void>(scene.CreateEntity("Marker"));
			CHECK(hash() == baked);
			// Nor does a NotWalkable box's Cost, which the bake never reads.
			scene.FindEntityByPath("/Lava").Patch<NavModifierComponent>([](NavModifierComponent& modifier)
			{
				modifier.Cost = 7.0f;
			});
			CHECK(hash() == baked);

			// Moving a wall, changing a modifier or a setting each changes it.
			Entity pillar = scene.FindEntityByPath("/Pillar");
			pillar.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.x += 0.5f;
			});
			const uint64_t moved = hash();
			CHECK(moved != baked);
			scene.FindEntityByPath("/Mud").Patch<NavModifierComponent>([](NavModifierComponent& modifier)
			{
				modifier.Cost = 20.0f;
			});
			const uint64_t costlier = hash();
			CHECK(costlier != moved);
			navigation.Patch<NavigationComponent>([](NavigationComponent& settings)
			{
				settings.AgentTypes[1].Radius = 1.2f;
			});
			CHECK(hash() != costlier);
		}
	}

}
