#include "TestsPCH.h"

#include "Engine/Scene/PhysicsComposition.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Scene.h"
#include "Support/AssetTestFixture.h"
#include "Support/PhysicsTestScene.h"

#include <set>

// The physics composition rules (Architecture §5.3 "Physics composition rules", §9.2; Docs/Decisions/0014-m11-decisions.md
// decision 10), the shape descriptions they give and the mesh shape cache.

namespace Engine {

	namespace {

		// The diagnostics of `composition` with `code`.
		std::vector<PhysicsDiagnostic> DiagnosticsWithCode(const PhysicsComposition& composition, std::string_view code)
		{
			std::vector<PhysicsDiagnostic> found;
			for (const PhysicsDiagnostic& diagnostic : composition.Diagnostics)
			{
				if (diagnostic.Code == code)
					found.push_back(diagnostic);
			}
			return found;
		}

		// The plan whose owner is `owner` and whose origin is `origin`, or null.
		const PhysicsBodyPlan* FindPlan(const PhysicsComposition& composition, UUID owner, PhysicsBodyOrigin origin)
		{
			for (const PhysicsBodyPlan& plan : composition.Bodies)
			{
				if (plan.Owner == owner && plan.Origin == origin)
					return &plan;
			}
			return nullptr;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("PhysicsBodyOrigin: every origin has its enumerator name")
		{
			CHECK(PhysicsBodyOriginToString(PhysicsBodyOrigin::RigidBody) == "RigidBody");
			CHECK(PhysicsBodyOriginToString(PhysicsBodyOrigin::ImplicitStatic) == "ImplicitStatic");
			CHECK(PhysicsBodyOriginToString(PhysicsBodyOrigin::ImplicitSensor) == "ImplicitSensor");
			CHECK(PhysicsBodyOriginToString(PhysicsBodyOrigin::Character) == "Character");
		}

		TEST_CASE("PhysicsComposition: a RigidBody gathers its collider-only descendants into one compound")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// The shared-static-body pattern (§5.3): a level root with a Static RigidBody and track pieces below it.
			Entity level = scene.CreateEntity("Level");
			level.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
			for (int piece = 0; piece < 3; ++piece)
				static_cast<void>(Test::AddBoxBody(scene, "Piece", glm::vec3(static_cast<float>(piece), 0.0f, 0.0f), glm::vec3(0.5f), std::nullopt, level));
			// A piece with its own body starts its own body, and its subtree belongs to it.
			Entity platform = Test::AddBoxBody(scene, "Platform", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Kinematic, level);
			static_cast<void>(Test::AddBoxBody(scene, "Rail", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f, 0.1f, 0.1f), std::nullopt, platform));

			const PhysicsComposition composition = ComposePhysicsBodies(scene, PhysicsLayerTable());
			CHECK(composition.Diagnostics.empty());
			REQUIRE(composition.Bodies.size() == 2);
			const PhysicsBodyPlan& track = composition.Bodies[0];
			CHECK(track.Owner == level.GetUUID());
			CHECK(track.Origin == PhysicsBodyOrigin::RigidBody);
			CHECK(track.MotionType == PhysicsMotionType::Static);
			REQUIRE(track.Colliders.size() == 3);
			// Sub-shape user data is the index: the pieces in canonical order.
			const std::span<const UUID> pieces = level.GetChildren();
			REQUIRE(pieces.size() == 4);
			for (size_t index = 0; index < 3; ++index)
				CHECK(track.Colliders[index].Entity == pieces[index]);
			const PhysicsBodyPlan& moving = composition.Bodies[1];
			CHECK(moving.Owner == platform.GetUUID());
			CHECK(moving.MotionType == PhysicsMotionType::Kinematic);
			REQUIRE(moving.Colliders.size() == 2);
			CHECK(moving.Colliders[0].Entity == platform.GetUUID());
			CHECK(moving.Colliders[1].Entity == Test::GetEntityId(scene, "/Level/Platform/Rail"));
		}

		TEST_CASE("PhysicsComposition: solid colliders without a RigidBody form implicit static bodies")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity a = Test::AddBoxBody(scene, "A", glm::vec3(0.0f), glm::vec3(0.5f), std::nullopt);
			Entity child = Test::AddBoxBody(scene, "Child", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.5f), std::nullopt, a);
			child.AddComponent<SphereColliderComponent>(SphereColliderComponent{});
			const PhysicsComposition composition = ComposePhysicsBodies(scene, PhysicsLayerTable());
			// One implicit static body for A that gathers its collider-only child, like a RigidBody would.
			REQUIRE(composition.Bodies.size() == 1);
			const PhysicsBodyPlan& body = composition.Bodies[0];
			CHECK(body.Owner == a.GetUUID());
			CHECK(body.Origin == PhysicsBodyOrigin::ImplicitStatic);
			CHECK(body.MotionType == PhysicsMotionType::Static);
			CHECK(body.Layer == 0);
			REQUIRE(body.Colliders.size() == 3);
			CHECK(body.Colliders[1].Type == PhysicsColliderType::Box);
			CHECK(body.Colliders[2].Type == PhysicsColliderType::Sphere);
		}

		TEST_CASE("PhysicsComposition: trigger colliders without their own RigidBody form implicit sensor bodies")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity level = scene.CreateEntity("Level");
			level.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static, .Layer = "Default" });
			Entity checkpoint = Test::AddBoxBody(scene, "Checkpoint", glm::vec3(3.0f, 1.0f, 0.0f), glm::vec3(1.0f), std::nullopt, level);
			checkpoint.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			// A trigger on a moving ball: the ball's RigidBody is on the same entity, so it must agree (here: solid sphere,
			// trigger child).
			Entity ball = Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f, 5.0f, 0.0f), 0.5f, BodyType::Dynamic);
			Entity pickup = Test::AddSphereBody(scene, "Pickup", glm::vec3(0.0f), 2.0f, std::nullopt, ball);
			pickup.Patch<SphereColliderComponent>([](SphereColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});

			const PhysicsComposition composition = ComposePhysicsBodies(scene, PhysicsLayerTable());
			CHECK(composition.Diagnostics.empty());
			const PhysicsBodyPlan* sensor = FindPlan(composition, checkpoint.GetUUID(), PhysicsBodyOrigin::ImplicitSensor);
			REQUIRE(sensor != nullptr);
			CHECK(sensor->IsSensor);
			CHECK(sensor->MotionType == PhysicsMotionType::Kinematic);
			REQUIRE(sensor->Colliders.size() == 1);
			// The level's compound does not take the trigger; left without a collider, it makes no body.
			const PhysicsBodyPlan* track = FindPlan(composition, level.GetUUID(), PhysicsBodyOrigin::RigidBody);
			REQUIRE(track != nullptr);
			CHECK(track->Colliders.empty());
			CHECK_FALSE(track->IsCreatable);
			const PhysicsBodyPlan* aura = FindPlan(composition, pickup.GetUUID(), PhysicsBodyOrigin::ImplicitSensor);
			REQUIRE(aura != nullptr);
			const PhysicsBodyPlan* body = FindPlan(composition, ball.GetUUID(), PhysicsBodyOrigin::RigidBody);
			REQUIRE(body != nullptr);
			CHECK(body->Colliders.size() == 1);
		}

		TEST_CASE("PhysicsComposition: a Static RigidBody with trigger colliders is planned as a Kinematic sensor")
		{
			// A goal authored as a Static RigidBody with a trigger: triggers are Kinematic and kept active (§9.2), so it also
			// detects sleeping bodies.
			Test::SceneTestFixture fixture;
			Entity goal = Test::AddBoxBody(fixture.GetScene(), "Goal", glm::vec3(0.0f), glm::vec3(1.0f), BodyType::Static);
			goal.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			const PhysicsComposition composition = ComposePhysicsBodies(fixture.GetScene(), PhysicsLayerTable());
			CHECK(composition.Diagnostics.empty());
			const PhysicsBodyPlan* plan = FindPlan(composition, goal.GetUUID(), PhysicsBodyOrigin::RigidBody);
			REQUIRE(plan != nullptr);
			CHECK(plan->IsSensor);
			CHECK(plan->MotionType == PhysicsMotionType::Kinematic);
			CHECK(plan->IsCreatable);
		}

		TEST_CASE("PhysicsComposition: a body's collision group is the nearest RigidBody or CharacterController owner at or above it")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// A ball with a trigger child, a character with a pickup radius, and loose static geometry with a trigger.
			Entity ball = Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f, 5.0f, 0.0f), 0.5f, BodyType::Dynamic);
			Entity aura = Test::AddSphereBody(scene, "Aura", glm::vec3(0.0f), 1.0f, std::nullopt, ball);
			Entity player = scene.CreateEntity("Player");
			player.AddComponent<CharacterControllerComponent>(CharacterControllerComponent{});
			Entity pickup = Test::AddSphereBody(scene, "Pickup", glm::vec3(0.0f, 1.0f, 0.0f), 1.5f, std::nullopt, player);
			for (Entity trigger : { aura, pickup })
			{
				trigger.Patch<SphereColliderComponent>([](SphereColliderComponent& collider)
				{
					collider.IsTrigger = true;
				});
			}
			Entity rock = Test::AddBoxBody(scene, "Rock", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.5f), std::nullopt);
			Entity zone = Test::AddBoxBody(scene, "Zone", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(1.0f), std::nullopt, rock);
			zone.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});

			const PhysicsComposition composition = ComposePhysicsBodies(scene, PhysicsLayerTable());
			const auto groupOf = [&composition](UUID owner, PhysicsBodyOrigin origin)
			{
				const PhysicsBodyPlan* plan = FindPlan(composition, owner, origin);
				return plan != nullptr ? std::optional<UUID>(plan->CollisionGroup) : std::nullopt;
			};
			CHECK(groupOf(ball.GetUUID(), PhysicsBodyOrigin::RigidBody) == std::optional<UUID>(ball.GetUUID()));
			CHECK(groupOf(aura.GetUUID(), PhysicsBodyOrigin::ImplicitSensor) == std::optional<UUID>(ball.GetUUID()));
			CHECK(groupOf(player.GetUUID(), PhysicsBodyOrigin::Character) == std::optional<UUID>(player.GetUUID()));
			CHECK(groupOf(pickup.GetUUID(), PhysicsBodyOrigin::ImplicitSensor) == std::optional<UUID>(player.GetUUID()));
			// Without a RigidBody or a character above them, bodies have no group.
			CHECK(groupOf(rock.GetUUID(), PhysicsBodyOrigin::ImplicitStatic) == std::optional<UUID>(UUID()));
			CHECK(groupOf(zone.GetUUID(), PhysicsBodyOrigin::ImplicitSensor) == std::optional<UUID>(UUID()));
		}

		TEST_CASE("PhysicsComposition: bodies are listed in canonical order whatever order components were added in")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity first = scene.CreateEntity("First");
			Entity second = scene.CreateEntity("Second");
			// Components added in reverse entity order.
			second.AddComponent<SphereColliderComponent>(SphereColliderComponent{});
			second.AddComponent<RigidBodyComponent>(RigidBodyComponent{});
			first.AddComponent<RigidBodyComponent>(RigidBodyComponent{});
			first.AddComponent<BoxColliderComponent>(BoxColliderComponent{});
			const PhysicsComposition composition = ComposePhysicsBodies(scene, PhysicsLayerTable());
			REQUIRE(composition.Bodies.size() == 2);
			CHECK(composition.Bodies[0].Owner == first.GetUUID());
			CHECK(composition.Bodies[1].Owner == second.GetUUID());
		}

		TEST_CASE("PhysicsComposition: mixed, Dynamic-trigger, non-convex Dynamic and all-DOF-locked bodies are refused with their codes")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity mixed = Test::AddBoxBody(scene, "Mixed", glm::vec3(0.0f), glm::vec3(0.5f), BodyType::Kinematic);
			mixed.AddComponent<SphereColliderComponent>(SphereColliderComponent{ .IsTrigger = true });
			Entity trigger = Test::AddBoxBody(scene, "DynamicTrigger", glm::vec3(2.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			trigger.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			Entity mesh = scene.CreateEntity("Mesh");
			// A non-convex mesh collider (Convex defaults to false) on a Dynamic body.
			mesh.AddComponent<MeshColliderComponent>(MeshColliderComponent{});
			mesh.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Dynamic });
			Entity locked = Test::AddBoxBody(scene, "Locked", glm::vec3(4.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			Test::PatchRigidBody(locked, [](RigidBodyComponent& body)
			{
				body.LockTranslation = glm::bvec3(true);
				body.LockRotation = glm::bvec3(true);
			});

			const PhysicsComposition composition = ComposePhysicsBodies(scene, PhysicsLayerTable());
			const auto refused = [&composition](UUID owner, std::string_view code)
			{
				const std::vector<PhysicsDiagnostic> found = DiagnosticsWithCode(composition, code);
				const bool reported = std::any_of(found.begin(), found.end(), [owner](const PhysicsDiagnostic& diagnostic)
				{
					return diagnostic.Entity == owner && diagnostic.Severity == DiagnosticSeverity::Error;
				});
				const PhysicsBodyPlan* plan = FindPlan(composition, owner, PhysicsBodyOrigin::RigidBody);
				return reported && plan != nullptr && !plan->IsCreatable;
			};
			CHECK(refused(mixed.GetUUID(), PhysicsMixedTriggerCode));
			CHECK(refused(trigger.GetUUID(), PhysicsDynamicTriggerCode));
			CHECK(refused(mesh.GetUUID(), PhysicsNonconvexDynamicCode));
			CHECK(refused(locked.GetUUID(), PhysicsAllDofsLockedCode));
			CHECK(composition.Diagnostics.size() == 4);
		}

		TEST_CASE("PhysicsComposition: an unknown layer resolves to Default with PHYSICS_UNKNOWN_LAYER")
		{
			Test::SceneTestFixture fixture;
			Entity ball = Test::AddSphereBody(fixture.GetScene(), "Ball", glm::vec3(0.0f), 0.5f, BodyType::Dynamic);
			Test::PatchRigidBody(ball, [](RigidBodyComponent& body)
			{
				body.Layer = "Bal";
			});
			const PhysicsComposition composition = ComposePhysicsBodies(fixture.GetScene(), PhysicsLayerTable());
			REQUIRE(composition.Bodies.size() == 1);
			CHECK(composition.Bodies[0].Layer == 0);
			CHECK(composition.Bodies[0].IsCreatable);
			const std::vector<PhysicsDiagnostic> unknown = DiagnosticsWithCode(composition, PhysicsUnknownLayerCode);
			REQUIRE(unknown.size() == 1);
			CHECK(unknown[0].Entity == ball.GetUUID());
			CHECK(unknown[0].Component == "RigidBody");
			CHECK(unknown[0].Field == "Layer");
		}

		TEST_CASE("PhysicsComposition: a sphere under a non-uniform scale raises PHYSICS_NONUNIFORM_SCALE")
		{
			Test::SceneTestFixture fixture;
			Entity ball = Test::AddSphereBody(fixture.GetScene(), "Ball", glm::vec3(0.0f), 0.5f, BodyType::Dynamic);
			ball.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(1.0f, 2.0f, 1.0f);
			});
			const PhysicsComposition composition = ComposePhysicsBodies(fixture.GetScene(), PhysicsLayerTable());
			const std::vector<PhysicsDiagnostic> nonuniform = DiagnosticsWithCode(composition, PhysicsNonuniformScaleCode);
			REQUIRE(nonuniform.size() == 1);
			CHECK(nonuniform[0].Severity == DiagnosticSeverity::Warning);
			CHECK(composition.Bodies.at(0).IsCreatable);
		}

		TEST_CASE("PhysicsComposition: a Dynamic body under a moving parent raises PHYSICS_DYNAMIC_UNDER_MOVING_PARENT")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity platform = Test::AddBoxBody(scene, "Platform", glm::vec3(0.0f), glm::vec3(2.0f, 0.1f, 2.0f), BodyType::Kinematic);
			Entity crate = Test::AddBoxBody(scene, "Crate", glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic, platform);
			// Under a static parent there is nothing to warn about.
			Entity shelf = Test::AddBoxBody(scene, "Shelf", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(1.0f), BodyType::Static);
			static_cast<void>(Test::AddBoxBody(scene, "Book", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.2f), BodyType::Dynamic, shelf));
			const PhysicsComposition composition = ComposePhysicsBodies(scene, PhysicsLayerTable());
			const std::vector<PhysicsDiagnostic> moving = DiagnosticsWithCode(composition, PhysicsDynamicUnderMovingParentCode);
			REQUIRE(moving.size() == 1);
			CHECK(moving[0].Entity == crate.GetUUID());
			CHECK(moving[0].Severity == DiagnosticSeverity::Warning);
		}

		TEST_CASE("PhysicsComposition: bodies beyond the limit are refused with PHYSICS_LIMIT_EXCEEDED")
		{
			Test::SceneTestFixture fixture;
			for (int body = 0; body < 5; ++body)
				static_cast<void>(Test::AddBoxBody(fixture.GetScene(), "Box", glm::vec3(static_cast<float>(body) * 2.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			const PhysicsComposition composition = ComposePhysicsBodies(fixture.GetScene(), PhysicsLayerTable(), 3);
			REQUIRE(composition.Bodies.size() == 5);
			CHECK(composition.Bodies[2].IsCreatable);
			CHECK_FALSE(composition.Bodies[3].IsCreatable);
			CHECK_FALSE(composition.Bodies[4].IsCreatable);
			// One diagnostic per refused entity (§9.1), each on its owner.
			const std::vector<PhysicsDiagnostic> limit = DiagnosticsWithCode(composition, PhysicsLimitExceededCode);
			REQUIRE(limit.size() == 2);
			const std::set<UUID> refused = { limit[0].Entity, limit[1].Entity };
			CHECK(refused == std::set<UUID>{ composition.Bodies[3].Owner, composition.Bodies[4].Owner });
			for (const PhysicsDiagnostic& diagnostic : limit)
			{
				CHECK(diagnostic.Severity == DiagnosticSeverity::Error);
				CHECK(diagnostic.Subject == "bodies");
			}
		}

		TEST_CASE("PhysicsComposition: an owner with two refused bodies gets one limit diagnostic")
		{
			// An entity with solid colliders and trigger colliders of its own and no RigidBody owns an implicit static body and
			// an implicit sensor body; both beyond the limit make one diagnostic, so its (code, entity, subject) stays unique.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddBoxBody(scene, "First", glm::vec3(0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Entity mixed = Test::AddBoxBody(scene, "Mixed", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.5f), std::nullopt);
			mixed.AddComponent<SphereColliderComponent>(SphereColliderComponent{ .Radius = 2.0f, .IsTrigger = true });
			const PhysicsComposition composition = ComposePhysicsBodies(scene, PhysicsLayerTable(), 1);
			REQUIRE(composition.Bodies.size() == 3);
			CHECK_FALSE(composition.Bodies[1].IsCreatable);
			CHECK_FALSE(composition.Bodies[2].IsCreatable);
			const std::vector<PhysicsDiagnostic> limit = DiagnosticsWithCode(composition, PhysicsLimitExceededCode);
			REQUIRE(limit.size() == 1);
			CHECK(limit[0].Entity == mixed.GetUUID());
		}

		TEST_CASE("PhysicsComposition: disabled entities and their subtrees make no bodies")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity root = Test::AddBoxBody(scene, "Root", glm::vec3(0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			static_cast<void>(Test::AddBoxBody(scene, "Child", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic, root));
			root.SetActive(false);
			CHECK(ComposePhysicsBodies(scene, PhysicsLayerTable()).Bodies.empty());
		}

		TEST_CASE("PhysicsComposition: implicit sensor bodies take the nearest ancestor RigidBody's layer, characters their own")
		{
			const std::vector<std::string> layers = { "Default", "Track" };
			const std::vector<std::vector<std::string>> collisions = { { "Default", "Track" } };
			Result<PhysicsLayerTable> table = PhysicsLayerTable::Create(layers, collisions);
			REQUIRE(table.has_value());
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity level = scene.CreateEntity("Level");
			level.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static, .Layer = "Track" });
			Entity goal = Test::AddBoxBody(scene, "Goal", glm::vec3(0.0f), glm::vec3(1.0f), std::nullopt, level);
			goal.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			Entity player = scene.CreateEntity("Player");
			player.AddComponent<CharacterControllerComponent>(CharacterControllerComponent{ .Layer = "Track" });
			const PhysicsComposition composition = ComposePhysicsBodies(scene, *table);
			const PhysicsBodyPlan* sensor = FindPlan(composition, goal.GetUUID(), PhysicsBodyOrigin::ImplicitSensor);
			REQUIRE(sensor != nullptr);
			CHECK(sensor->Layer == 1);
			const PhysicsBodyPlan* character = FindPlan(composition, player.GetUUID(), PhysicsBodyOrigin::Character);
			REQUIRE(character != nullptr);
			CHECK(character->Layer == 1);
			CHECK(character->MotionType == PhysicsMotionType::Kinematic);
			CHECK(character->Colliders.empty());
		}

		TEST_CASE("PhysicsComposition: DescribePhysicsBodyShape places gathered colliders relative to the owner with scale baked in")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity owner = scene.CreateEntity("Owner");
			owner.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(10.0f, 0.0f, 0.0f);
				transform.Scale = glm::vec3(2.0f);
			});
			owner.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
			Entity piece = Test::AddBoxBody(scene, "Piece", glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.5f), std::nullopt, owner);
			piece.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.Offset = glm::vec3(0.0f, 0.25f, 0.0f);
			});
			const PhysicsComposition composition = ComposePhysicsBodies(scene, PhysicsLayerTable());
			REQUIRE(composition.Bodies.size() == 1);
			Result<BodyShapeDescription> description = DescribePhysicsBodyShape(scene, composition.Bodies[0], nullptr);
			REQUIRE_MESSAGE(description.has_value(), description.error().ToString());
			REQUIRE(description->Colliders.size() == 1);
			const ColliderShapeDescription& collider = description->Colliders[0];
			// In the owner's body space (its world pose without scale): the child's offset and position, both scaled by 2.
			CHECK(Test::ApproxEqual(collider.Position, glm::vec3(2.0f, 0.5f, 0.0f)));
			CHECK(Test::ApproxEqual(collider.Scale, glm::vec3(2.0f)));
			CHECK(collider.UserData == 0);
			REQUIRE(std::holds_alternative<BoxShapeGeometry>(collider.Geometry));

			// A mesh collider without an asset manager is an invalid shape, never a crash.
			Entity mesh = scene.CreateEntity("Mesh");
			mesh.AddComponent<MeshColliderComponent>(MeshColliderComponent{});
			const PhysicsComposition withMesh = ComposePhysicsBodies(scene, PhysicsLayerTable());
			const auto meshPlan = std::find_if(withMesh.Bodies.begin(), withMesh.Bodies.end(), [&mesh](const PhysicsBodyPlan& plan)
			{
				return plan.Owner == mesh.GetUUID();
			});
			REQUIRE(meshPlan != withMesh.Bodies.end());
			Result<BodyShapeDescription> missing = DescribePhysicsBodyShape(scene, *meshPlan, nullptr);
			REQUIRE_FALSE(missing.has_value());
			CHECK(GetPhysicsDiagnosticCode(missing.error()) == PhysicsInvalidShapeCode);
		}

		TEST_CASE("PhysicsMeshShapeCache: mesh colliders share one shape per mesh, version, convexity and scale")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// A level root whose three pieces are the built-in cube mesh, two at scale 1 and one at scale 2.
			Entity level = scene.CreateEntity("Level");
			level.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
			for (int piece = 0; piece < 3; ++piece)
			{
				Entity entity = scene.CreateEntity("Piece", level);
				entity.Patch<TransformComponent>([piece](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(static_cast<float>(piece) * 3.0f, 0.0f, 0.0f);
					transform.Scale = glm::vec3(piece == 2 ? 2.0f : 1.0f);
				});
				entity.AddComponent<MeshColliderComponent>(MeshColliderComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh) });
			}
			const PhysicsComposition composition = ComposePhysicsBodies(scene, PhysicsLayerTable());
			REQUIRE(composition.Bodies.size() == 1);
			PhysicsMeshShapeCache cache;
			Result<BodyShapeDescription> description = DescribePhysicsBodyShape(scene, composition.Bodies[0], &assets.GetManager(), &cache);
			REQUIRE_MESSAGE(description.has_value(), description.error().ToString());
			REQUIRE(description->Colliders.size() == 3);
			const auto sharedOf = [&description](size_t index) -> Ref<const PhysicsShape>
			{
				const ColliderShapeDescription& collider = description->Colliders[index];
				const SharedShapeGeometry* shared = std::get_if<SharedShapeGeometry>(&collider.Geometry);
				return shared != nullptr && collider.Scale == glm::vec3(1.0f) ? shared->Shape : nullptr;
			};
			Ref<const PhysicsShape> first = sharedOf(0);
			REQUIRE(first != nullptr);
			CHECK(sharedOf(1) == first);
			Ref<const PhysicsShape> scaled = sharedOf(2);
			REQUIRE(scaled != nullptr);
			CHECK(scaled != first);
			CHECK(cache.GetSize() == 2);
			// The shapes build into the body's compound; once nothing holds them, Prune drops them.
			Result<Ref<const PhysicsShape>> body = PhysicsShape::Create(*description);
			REQUIRE_MESSAGE(body.has_value(), body.error().ToString());
			CHECK((*body)->GetColliderCount() == 3);
			cache.Prune();
			CHECK(cache.GetSize() == 2);
			description = BodyShapeDescription{};
			body = Ref<const PhysicsShape>();
			first.reset();
			scaled.reset();
			cache.Prune();
			CHECK(cache.GetSize() == 0);
		}
	}

}
