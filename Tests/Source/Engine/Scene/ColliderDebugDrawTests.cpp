#include "TestsPCH.h"

#include "Engine/Scene/ColliderDebugDraw.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Renderer/DebugDrawList.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/CapsuleColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/PhysicsTestScene.h"
#include "Support/RenderReference.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <set>
#include <span>
#include <tuple>
#include <utility>
#include <variant>

// Collider visualization (Architecture §8.10; Roadmap M11 "collider debug draw (edit and play)"): the records
// (Docs/Decisions/0014-m11-decisions.md decision 16), and, since the integration with M8, their mapping onto the debug draw
// list and the pixels the scene renderer's debug lines draw for them. The record cases cover the colour table, the
// categories of an edit scene (by the composition and by building each body's shape as the session would) and of a play
// session (sleeping bodies, bodies the session refused), world frames and scaled dimensions, the options, and mesh
// colliders.

namespace Engine {

	namespace {

		const ColliderDebugShape* FindShape(const std::vector<ColliderDebugShape>& shapes, UUID collider)
		{
			const auto found = std::find_if(shapes.begin(), shapes.end(), [collider](const ColliderDebugShape& shape)
			{
				return shape.Collider == collider;
			});
			return found == shapes.end() ? nullptr : &*found;
		}

		// An entity `name` at `position`, scaled by `scale`, with a MeshCollider of `mesh` (null: the MeshRenderer fallback).
		Entity AddMeshCollider(Scene& scene, std::string_view name, const glm::vec3& position, const glm::vec3& scale, AssetHandle mesh)
		{
			Entity entity = scene.CreateEntity(name);
			entity.Patch<TransformComponent>([&position, &scale](TransformComponent& transform)
			{
				transform.Translation = position;
				transform.Scale = scale;
			});
			entity.AddComponent<MeshColliderComponent>(MeshColliderComponent{}).Mesh.SetHandle(mesh);
			return entity;
		}

		// The 8-bit sRGB encoding of a linear colour, as the debug lines write it (after tonemapping, encoded, §8.10).
		std::array<int, 3> EncodeColor(const glm::vec4& linear)
		{
			const std::array<float, 3> channels = { linear.r, linear.g, linear.b };
			std::array<int, 3> encoded{};
			for (size_t channel = 0; channel < encoded.size(); ++channel)
				encoded[channel] = static_cast<int>(255.0 * Test::LinearToSrgb(static_cast<double>(channels[channel])) + 0.5);
			return encoded;
		}

		// The pixels of the RGBA8 `image` in columns [x0, x1) and rows [y0, y1) whose channels are each within 3 of `color`.
		uint32_t CountColor(const Image& image, const std::array<int, 3>& color, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
		{
			uint32_t count = 0;
			for (uint32_t row = y0; row < y1; ++row)
			{
				const std::span<const std::byte> pixels = image.GetRow(row);
				for (uint32_t column = x0; column < x1; ++column)
				{
					bool match = true;
					for (size_t channel = 0; channel < color.size(); ++channel)
						match = match && std::abs(std::to_integer<int>(pixels[static_cast<size_t>(column) * 4 + channel]) - color[channel]) <= 3;
					count += match ? 1u : 0u;
				}
			}
			return count;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("ColliderDebugDraw: every category has its own colour")
		{
			std::set<std::tuple<float, float, float, float>> colours;
			for (const ColliderDebugCategory category : { ColliderDebugCategory::Static, ColliderDebugCategory::Kinematic, ColliderDebugCategory::Dynamic,
					 ColliderDebugCategory::Sleeping, ColliderDebugCategory::Trigger, ColliderDebugCategory::Character, ColliderDebugCategory::Invalid })
			{
				const glm::vec4 colour = GetColliderDebugColor(category);
				CHECK(colour.a == 1.0f);
				colours.insert({ colour.r, colour.g, colour.b, colour.a });
			}
			CHECK(colours.size() == 7);
		}

		TEST_CASE("ColliderDebugDraw: an edit scene draws every collider with its body's category")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity ground = Test::AddGround(scene);
			const Entity crate = Test::AddBoxBody(scene, "Crate", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			const Entity platform = Test::AddBoxBody(scene, "Platform", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(1.0f, 0.1f, 1.0f), BodyType::Kinematic);
			Entity zone = Test::AddBoxBody(scene, "Zone", glm::vec3(-5.0f, 1.0f, 0.0f), glm::vec3(1.0f), std::nullopt);
			zone.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			const Entity loose = Test::AddBoxBody(scene, "Loose", glm::vec3(0.0f, 0.0f, 9.0f), glm::vec3(0.5f), std::nullopt);
			Entity player = scene.CreateEntity("Player");
			player.AddComponent<CharacterControllerComponent>(CharacterControllerComponent{});
			Entity locked = Test::AddBoxBody(scene, "Locked", glm::vec3(9.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			Test::PatchRigidBody(locked, [](RigidBodyComponent& body)
			{
				body.LockTranslation = glm::bvec3(true);
				body.LockRotation = glm::bvec3(true);
			});

			const std::vector<ColliderDebugShape> shapes = BuildColliderDebugDraw(scene, PhysicsLayerTable(), nullptr, nullptr);
			CHECK(shapes.size() == 7);
			const std::array<std::pair<Entity, ColliderDebugCategory>, 7> expected = { {
				{ ground, ColliderDebugCategory::Static },
				{ crate, ColliderDebugCategory::Dynamic },
				{ platform, ColliderDebugCategory::Kinematic },
				{ zone, ColliderDebugCategory::Trigger },
				{ loose, ColliderDebugCategory::Static },
				{ player, ColliderDebugCategory::Character },
				{ locked, ColliderDebugCategory::Invalid },
			} };
			for (const auto& [entity, category] : expected)
			{
				INFO("collider ", entity.GetName());
				const ColliderDebugShape* shape = FindShape(shapes, entity.GetUUID());
				REQUIRE(shape != nullptr);
				CHECK(shape->Category == category);
				CHECK(shape->Color == GetColliderDebugColor(category));
				CHECK(shape->Body == entity.GetUUID());
			}
			// The character's capsule stands on its base (the entity's origin): Height 1.8 and Radius 0.3 give a 0.6 m
			// cylinder half height around a centre 0.9 m up.
			const ColliderDebugShape* capsule = FindShape(shapes, player.GetUUID());
			REQUIRE(capsule != nullptr);
			CHECK(capsule->Type == ColliderDebugShapeType::Capsule);
			CHECK(Test::ApproxEqual(capsule->Position, glm::vec3(0.0f, 0.9f, 0.0f)));
			CHECK(capsule->Radius == doctest::Approx(0.3f));
			CHECK(capsule->HalfHeight == doctest::Approx(0.6f));
		}

		TEST_CASE("ColliderDebugDraw: a play session tells sleeping Dynamic bodies apart")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Sleeper", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			static_cast<void>(Test::AddBoxBody(scene, "Faller", glm::vec3(5.0f, 50.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
			REQUIRE(session.has_value());
			Test::RunTicks(**session, 120);
			const Scene& runtime = (*session)->GetScene();
			const std::vector<ColliderDebugShape> shapes = BuildColliderDebugDraw(runtime, PhysicsLayerTable(), &(*session)->GetPhysics(), nullptr);
			const ColliderDebugShape* sleeper = FindShape(shapes, Test::GetEntityId(runtime, "/Sleeper"));
			const ColliderDebugShape* faller = FindShape(shapes, Test::GetEntityId(runtime, "/Faller"));
			REQUIRE(sleeper != nullptr);
			REQUIRE(faller != nullptr);
			CHECK(sleeper->Category == ColliderDebugCategory::Sleeping);
			CHECK(sleeper->Color == GetColliderDebugColor(ColliderDebugCategory::Sleeping));
			CHECK(faller->Category == ColliderDebugCategory::Dynamic);
			// Play poses: the faller has fallen.
			CHECK(faller->Position.y < 45.0f);
		}

		TEST_CASE("ColliderDebugDraw: an edit scene draws a body the session would refuse as Invalid")
		{
			// The composition accepts these bodies; building them as the session does refuses them: a box under a scale that
			// makes it smaller than PhysicsShape::MinColliderSize, a character without a cylinder, and a Dynamic body of
			// MaxPhysicsMass in a box 90 km across (its inertia is too large for Jolt).
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity speck = Test::AddBoxBody(scene, "Speck", glm::vec3(0.0f), glm::vec3(0.5f), BodyType::Static);
			speck.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(1.0e-3f);
			});
			Entity squat = scene.CreateEntity("Squat");
			squat.AddComponent<CharacterControllerComponent>(CharacterControllerComponent{ .Height = 0.5f, .Radius = 0.3f });
			Entity vast = Test::AddBoxBody(scene, "Vast", glm::vec3(0.0f, 0.0f, 1.0e6f), glm::vec3(4.5e4f), BodyType::Dynamic);
			Test::PatchRigidBody(vast, [](RigidBodyComponent& body)
			{
				body.Mass = MaxPhysicsMass;
			});
			const Entity fine = Test::AddBoxBody(scene, "Fine", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			const std::vector<ColliderDebugShape> shapes = BuildColliderDebugDraw(scene, PhysicsLayerTable(), nullptr, nullptr);
			for (const UUID refused : { speck.GetUUID(), squat.GetUUID(), vast.GetUUID() })
			{
				const ColliderDebugShape* shape = FindShape(shapes, refused);
				REQUIRE(shape != nullptr);
				CHECK(shape->Category == ColliderDebugCategory::Invalid);
			}
			const ColliderDebugShape* fineShape = FindShape(shapes, fine.GetUUID());
			REQUIRE(fineShape != nullptr);
			CHECK(fineShape->Category == ColliderDebugCategory::Dynamic);
		}

		TEST_CASE("ColliderDebugDraw: a play session draws a body it did not create as Invalid")
		{
			// A body placed beyond MaxPhysicsCoordinate is refused by the world (a PHYSICS_INVALID_SHAPE diagnostic), though its
			// shape builds: the session has no body for it.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddBoxBody(scene, "Far", glm::vec3(0.0f, 0.0f, 2.0f * MaxPhysicsCoordinate), glm::vec3(0.5f), BodyType::Dynamic));
			static_cast<void>(Test::AddBoxBody(scene, "Near", glm::vec3(0.0f, 50.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			const Test::ExpectLog refused(LogLevel::Error, "PHYSICS_INVALID_SHAPE");
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, Test::MakePhysicsSessionSpecification(fixture));
			REQUIRE(session.has_value());
			Test::RunTicks(**session, 2);
			const Scene& runtime = (*session)->GetScene();
			const std::vector<ColliderDebugShape> shapes = BuildColliderDebugDraw(runtime, PhysicsLayerTable(), &(*session)->GetPhysics(), nullptr);
			const ColliderDebugShape* far = FindShape(shapes, Test::GetEntityId(runtime, "/Far"));
			const ColliderDebugShape* near = FindShape(shapes, Test::GetEntityId(runtime, "/Near"));
			REQUIRE(far != nullptr);
			REQUIRE(near != nullptr);
			CHECK(far->Category == ColliderDebugCategory::Invalid);
			CHECK(near->Category == ColliderDebugCategory::Dynamic);
		}

		TEST_CASE("ColliderDebugDraw: shapes carry world frames and scaled dimensions")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity parent = scene.CreateEntity("Parent");
			parent.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(10.0f, 0.0f, 0.0f);
				transform.Scale = glm::vec3(2.0f);
			});
			parent.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
			Entity box = Test::AddBoxBody(scene, "Box", glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.5f, 0.25f, 0.5f), std::nullopt, parent);
			box.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.Offset = glm::vec3(0.0f, 1.0f, 0.0f);
			});
			Entity capsule = scene.CreateEntity("Capsule");
			capsule.AddComponent<CapsuleColliderComponent>(CapsuleColliderComponent{ .Radius = 0.25f, .HalfHeight = 1.0f });
			capsule.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(1.0f, 3.0f, 1.0f);
			});
			const std::vector<ColliderDebugShape> shapes = BuildColliderDebugDraw(scene, PhysicsLayerTable(), nullptr, nullptr);
			const ColliderDebugShape* boxShape = FindShape(shapes, box.GetUUID());
			REQUIRE(boxShape != nullptr);
			CHECK(boxShape->Type == ColliderDebugShapeType::Box);
			CHECK(boxShape->Body == parent.GetUUID());
			CHECK(Test::ApproxEqual(boxShape->Position, glm::vec3(12.0f, 2.0f, 0.0f)));
			CHECK(Test::ApproxEqual(boxShape->HalfExtents, glm::vec3(1.0f, 0.5f, 1.0f)));
			// A capsule under a non-uniform scale uses its largest component, as the body's shape does.
			const ColliderDebugShape* capsuleShape = FindShape(shapes, capsule.GetUUID());
			REQUIRE(capsuleShape != nullptr);
			CHECK(capsuleShape->Type == ColliderDebugShapeType::Capsule);
			CHECK(capsuleShape->Radius == doctest::Approx(0.75f));
			CHECK(capsuleShape->HalfHeight == doctest::Approx(3.0f));
		}

		TEST_CASE("ColliderDebugDraw: options leave out triggers and characters")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity zone = Test::AddBoxBody(scene, "Zone", glm::vec3(0.0f), glm::vec3(1.0f), std::nullopt);
			zone.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			Entity player = scene.CreateEntity("Player");
			player.AddComponent<CharacterControllerComponent>(CharacterControllerComponent{});
			static_cast<void>(Test::AddGround(scene));
			const std::vector<ColliderDebugShape> all = BuildColliderDebugDraw(scene, PhysicsLayerTable(), nullptr, nullptr);
			CHECK(all.size() == 3);
			const std::vector<ColliderDebugShape> solidOnly =
				BuildColliderDebugDraw(scene, PhysicsLayerTable(), nullptr, nullptr, { .Triggers = false, .Characters = false });
			REQUIRE(solidOnly.size() == 1);
			CHECK(solidOnly[0].Collider == Test::GetEntityId(scene, "/Ground"));
		}

		TEST_CASE("ColliderDebugDraw: a mesh collider draws each edge of its mesh once, in world space")
		{
			Test::AssetTestFixture assets;
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity rock = AddMeshCollider(scene, "Rock", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(2.0f), BuiltinAssetHandles::CubeMesh);
			const std::vector<ColliderDebugShape> shapes = BuildColliderDebugDraw(scene, PhysicsLayerTable(), nullptr, &assets.GetManager());
			REQUIRE(shapes.size() == 1);
			const ColliderDebugShape& shape = shapes[0];
			CHECK(shape.Type == ColliderDebugShapeType::Lines);
			CHECK(shape.Category == ColliderDebugCategory::Static);
			CHECK(shape.Collider == rock.GetUUID());
			// The built-in cube has 24 vertices (flat faces) at 8 corners and 12 triangles: its 12 sides and the 6 face
			// diagonals, each once although every corner is split three ways.
			REQUIRE(shape.LineVertices.size() == 2 * 18);
			std::set<std::pair<std::tuple<float, float, float>, std::tuple<float, float, float>>> edges;
			for (size_t index = 0; index < shape.LineVertices.size(); index += 2)
			{
				const glm::vec3& a = shape.LineVertices[index];
				const glm::vec3& b = shape.LineVertices[index + 1];
				CHECK(a != b);
				const std::tuple<float, float, float> first(a.x, a.y, a.z);
				const std::tuple<float, float, float> second(b.x, b.y, b.z);
				edges.insert(first < second ? std::make_pair(first, second) : std::make_pair(second, first));
				// World space: the unit cube scaled by 2 around (0, 2, 0).
				for (const glm::vec3& corner : { a, b })
				{
					CHECK(std::abs(corner.x) == doctest::Approx(1.0f));
					CHECK(std::abs(corner.y - 2.0f) == doctest::Approx(1.0f));
					CHECK(std::abs(corner.z) == doctest::Approx(1.0f));
				}
			}
			CHECK(edges.size() == 18);
		}

		TEST_CASE("ColliderDebugDraw: a mesh collider beyond MaxMeshEdges draws its bounds, and one without a mesh draws nothing")
		{
			Test::AssetTestFixture assets;
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const Entity rock = AddMeshCollider(scene, "Rock", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(2.0f, 1.0f, 2.0f), BuiltinAssetHandles::CubeMesh);
			static_cast<void>(AddMeshCollider(scene, "Bare", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(1.0f), AssetHandle()));
			// A MeshCollider without a Mesh uses the entity's MeshRenderer mesh.
			Entity rendered = AddMeshCollider(scene, "Rendered", glm::vec3(-5.0f, 0.0f, 0.0f), glm::vec3(1.0f), AssetHandle());
			rendered.AddComponent<MeshRendererComponent>().Mesh.SetHandle(BuiltinAssetHandles::CubeMesh);

			const std::vector<ColliderDebugShape> shapes =
				BuildColliderDebugDraw(scene, PhysicsLayerTable(), nullptr, &assets.GetManager(), { .MaxMeshEdges = 17 });
			REQUIRE(shapes.size() == 2);
			const ColliderDebugShape* bounds = FindShape(shapes, rock.GetUUID());
			REQUIRE(bounds != nullptr);
			CHECK(bounds->Type == ColliderDebugShapeType::Box);
			CHECK(Test::ApproxEqual(bounds->Position, glm::vec3(0.0f, 2.0f, 0.0f)));
			CHECK(Test::ApproxEqual(bounds->HalfExtents, glm::vec3(1.0f, 0.5f, 1.0f)));
			CHECK(bounds->LineVertices.empty());
			const ColliderDebugShape* fallback = FindShape(shapes, rendered.GetUUID());
			REQUIRE(fallback != nullptr);
			CHECK(fallback->Type == ColliderDebugShapeType::Box);
			CHECK(Test::ApproxEqual(fallback->Position, glm::vec3(-5.0f, 0.0f, 0.0f)));

			// Without an asset manager no mesh loads, so no mesh collider draws.
			CHECK(BuildColliderDebugDraw(scene, PhysicsLayerTable(), nullptr, nullptr).empty());
		}

		TEST_CASE("ColliderDebugDraw: AppendColliderDebugDraw maps each record onto the debug draw list in order")
		{
			// M8 + M11 integration (Docs/Decisions/0014-m11-decisions.md decision 16): Box, Sphere and Capsule one to one, Lines
			// one Line per vertex pair, every command with the record's colour, duration 0 and Tested depth.
			const glm::quat quarterTurnZ(0.70710678f, 0.0f, 0.0f, 0.70710678f); // +90 degrees about Z: +Y becomes -X
			ColliderDebugShape box;
			box.Type = ColliderDebugShapeType::Box;
			box.Color = glm::vec4(0.1f, 0.2f, 0.3f, 1.0f);
			box.Position = glm::vec3(1.0f, 2.0f, 3.0f);
			box.Rotation = quarterTurnZ;
			box.HalfExtents = glm::vec3(0.5f, 1.0f, 1.5f);
			ColliderDebugShape sphere;
			sphere.Type = ColliderDebugShapeType::Sphere;
			sphere.Color = glm::vec4(0.4f, 0.5f, 0.6f, 1.0f);
			sphere.Position = glm::vec3(-1.0f, 0.0f, 2.0f);
			sphere.Radius = 0.75f;
			ColliderDebugShape capsule;
			capsule.Type = ColliderDebugShapeType::Capsule;
			capsule.Color = glm::vec4(0.7f, 0.8f, 0.9f, 1.0f);
			capsule.Position = glm::vec3(0.0f, 1.0f, 0.0f);
			capsule.Rotation = quarterTurnZ;
			capsule.Radius = 0.25f;
			capsule.HalfHeight = 0.5f;
			ColliderDebugShape lines;
			lines.Type = ColliderDebugShapeType::Lines;
			lines.Color = glm::vec4(1.0f, 0.0f, 0.5f, 1.0f);
			// The unpaired last vertex is ignored.
			lines.LineVertices = { glm::vec3(0.0f), glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.0f, 0.0f, 1.0f), glm::vec3(5.0f) };
			const std::array<ColliderDebugShape, 4> shapes = { box, sphere, capsule, lines };

			DebugDrawList list;
			list.AddArrow(glm::vec3(0.0f), glm::vec3(1.0f), 0.1f, glm::vec4(1.0f), 2.0f, DebugDepthMode::OnTop);
			AppendColliderDebugDraw(shapes, list);
			const std::span<const DebugDrawCommand> commands = list.GetCommands();
			REQUIRE(commands.size() == 1 + 5);
			CHECK(std::holds_alternative<DebugArrow>(commands[0].Shape)); // what the list held stays first
			const std::array<glm::vec4, 5> colours = { box.Color, sphere.Color, capsule.Color, lines.Color, lines.Color };
			for (size_t index = 1; index < commands.size(); ++index)
			{
				INFO("command ", index);
				CHECK(commands[index].Color == colours[index - 1]);
				CHECK(commands[index].Duration == 0.0f);
				CHECK(commands[index].Remaining == 0.0f);
				CHECK(commands[index].Depth == DebugDepthMode::Tested);
			}

			const DebugBox* drawnBox = std::get_if<DebugBox>(&commands[1].Shape);
			REQUIRE(drawnBox != nullptr);
			CHECK(drawnBox->Center == box.Position);
			CHECK(drawnBox->HalfExtents == box.HalfExtents);
			CHECK(drawnBox->Rotation == box.Rotation);
			const DebugSphere* drawnSphere = std::get_if<DebugSphere>(&commands[2].Shape);
			REQUIRE(drawnSphere != nullptr);
			CHECK(drawnSphere->Center == sphere.Position);
			CHECK(drawnSphere->Radius == sphere.Radius);
			// The capsule's axis is its frame's Y, here turned onto -X: the hemisphere centres lie HalfHeight either side.
			const DebugCapsule* drawnCapsule = std::get_if<DebugCapsule>(&commands[3].Shape);
			REQUIRE(drawnCapsule != nullptr);
			CHECK(Test::ApproxEqual(drawnCapsule->Start, glm::vec3(0.5f, 1.0f, 0.0f)));
			CHECK(Test::ApproxEqual(drawnCapsule->End, glm::vec3(-0.5f, 1.0f, 0.0f)));
			CHECK(drawnCapsule->Radius == capsule.Radius);
			const DebugLine* first = std::get_if<DebugLine>(&commands[4].Shape);
			const DebugLine* second = std::get_if<DebugLine>(&commands[5].Shape);
			REQUIRE(first != nullptr);
			REQUIRE(second != nullptr);
			CHECK(*first == DebugLine{ .From = glm::vec3(0.0f), .To = glm::vec3(1.0f, 0.0f, 0.0f) });
			CHECK(*second == DebugLine{ .From = glm::vec3(0.0f, 1.0f, 0.0f), .To = glm::vec3(0.0f, 0.0f, 1.0f) });

			// No records, no commands.
			DebugDrawList empty;
			AppendColliderDebugDraw({}, empty);
			CHECK(empty.IsEmpty());
		}

		TEST_CASE("ColliderDebugDraw: an edit scene's colliders render as debug lines in their categories' colours"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M8 + M11 integration (Docs/Decisions/0014-m11-decisions.md decision 16): the records of an edit scene, appended to
			// its game view's snapshot, drawn by the scene renderer's debug lines (§8.3 pass 13) through the path of
			// viewport.screenshot. A black view looking down -Z from (0, 1, 8) at a Static box on the left, a Dynamic sphere on
			// the right, a character in the middle and a mesh collider (the built-in cube, an implicit static body) above it;
			// no meshes, so every coloured pixel is a debug line.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Test::AssetTestFixture assets;
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity camera = scene.CreateEntity("Camera");
			camera.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 1.0f, 8.0f);
			});
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true, .Clear = ClearMode::Color, .ClearColor = glm::vec3(0.0f) });
			static_cast<void>(Test::AddBoxBody(scene, "Wall", glm::vec3(-4.0f, 1.0f, 0.0f), glm::vec3(0.75f), BodyType::Static));
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(4.0f, 1.0f, 0.0f), 0.75f, BodyType::Dynamic));
			scene.CreateEntity("Player").AddComponent<CharacterControllerComponent>(CharacterControllerComponent{});
			static_cast<void>(AddMeshCollider(scene, "Rock", glm::vec3(0.0f, 3.5f, 0.0f), glm::vec3(1.0f), BuiltinAssetHandles::CubeMesh));
			TransformSystem::Update(scene);

			const std::vector<ColliderDebugShape> shapes = BuildColliderDebugDraw(scene, PhysicsLayerTable(), nullptr, &assets.GetManager());
			REQUIRE(shapes.size() == 4);
			const std::array<int, 3> staticColor = EncodeColor(GetColliderDebugColor(ColliderDebugCategory::Static));
			const std::array<int, 3> dynamicColor = EncodeColor(GetColliderDebugColor(ColliderDebugCategory::Dynamic));
			const std::array<int, 3> characterColor = EncodeColor(GetColliderDebugColor(ColliderDebugCategory::Character));
			{
				GpuResourceCache cache(device, assets.GetManager());
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<ViewportCapture>> capture = ViewportCapture::CreateForScenes(device, **pipelines, cache, assets.GetManager());
				REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());

				const uint32_t width = DefaultViewportScreenshotWidth;
				const uint32_t height = DefaultViewportScreenshotHeight;
				const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = width, .Height = height });
				REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
				REQUIRE(snapshot->HasCamera);
				CHECK(snapshot->Meshes.empty());
				CHECK(snapshot->DebugDraw.IsEmpty());
				RenderSnapshot withColliders = *snapshot;
				AppendColliderDebugDraw(shapes, withColliders.DebugDraw);
				CHECK(withColliders.DebugDraw.GetSize() == 3 + 18); // a box, a sphere, a capsule and the cube's 18 edges

				const Result<Image> bare = (*capture)->Capture({}, *snapshot);
				REQUIRE_MESSAGE(bare.has_value(), bare.error().ToString());
				const Result<Image> drawn = (*capture)->Capture({}, withColliders);
				REQUIRE_MESSAGE(drawn.has_value(), drawn.error().ToString());
				REQUIRE(drawn->Width == width);
				REQUIRE(drawn->Height == height);

				// Without the records nothing has a collider colour.
				for (const std::array<int, 3>& color : { staticColor, dynamicColor, characterColor })
					CHECK(CountColor(*bare, color, 0, 0, width, height) == 0);
				// The box (x = -4) projects left of column 240, the sphere (x = 4) right of column 400, the character (x = 0,
				// y 0 to 1.8) between them below row 120 and the cube (y = 3.5) between them above it.
				CHECK(CountColor(*drawn, staticColor, 0, 0, 240, height) > 0);
				CHECK(CountColor(*drawn, dynamicColor, 0, 0, 240, height) == 0);
				CHECK(CountColor(*drawn, dynamicColor, 400, 0, width, height) > 0);
				CHECK(CountColor(*drawn, staticColor, 400, 0, width, height) == 0);
				CHECK(CountColor(*drawn, characterColor, 240, 120, 400, height) > 0);
				CHECK(CountColor(*drawn, characterColor, 0, 0, 240, height) == 0);
				CHECK(CountColor(*drawn, characterColor, 400, 0, width, height) == 0);
				CHECK(CountColor(*drawn, staticColor, 240, 0, 400, 120) > 0);
			}
			device.RunGarbageCollection();
		}
	}

}
