#include "TestsPCH.h"

#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TerrainGeometry.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/TerrainColliderComponent.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsComposition.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"
#include "Support/ExpectLog.h"
#include "Support/GlmApprox.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/PhysicsTestScene.h"
#include "Support/SceneTestFixture.h"

#include <algorithm>
#include <cstdint>
#include <format>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The TerrainCollider in the play session's physics (M26, C-TERR; Docs/Decisions/0037-m26-contract.md decision 21, stream
// T-B): a solid collider of the composition rules (Architecture §5.3) whose shape is its Terrain's height field, laid out by
// Asset/TerrainGeometry.h's rule; its own entity's implicit static body takes its layer and material; it rebuilds with its
// heightfield's versions; queries hit it as any body; and its body is a body of the physics block of the state hash.

namespace Engine {

	namespace {

		constexpr AssetHandle HillHandle{ 0x5e7a11u };
		constexpr AssetHandle FlatHandle{ 0x5e7a12u };
		constexpr AssetHandle MissingHandle{ 0x5e7a13u };
		constexpr float BallRadius = 0.25f;
		// Jolt's penetration slop (§9.1): a resting body sinks this far into what it rests on, along the contact normal.
		constexpr float PenetrationSlop = 0.02f;

		// A heightfield of `resolution` samples a side whose sample (x, z) is `rule(x, z)`.
		Ref<HeightfieldData> MakeHeightfield(uint32_t resolution, const std::function<uint16_t(uint32_t, uint32_t)>& rule)
		{
			Ref<HeightfieldData> heightfield = CreateRef<HeightfieldData>(MakeFlatHeightfield(resolution));
			for (uint32_t z = 0; z < resolution; ++z)
			{
				for (uint32_t x = 0; x < resolution; ++x)
					heightfield->Heights[x + resolution * z] = rule(x, z);
			}
			return heightfield;
		}

		// 17 x 17 samples rising along +X to 30,000, with a ridge along z = 8 that adds 6,000.
		Ref<HeightfieldData> MakeHill()
		{
			return MakeHeightfield(17, [](uint32_t x, uint32_t z)
			{
				return static_cast<uint16_t>(x * 1875u + (z == 8 ? 6000u : 0u));
			});
		}

		// An entity `name` at `position` and `scale` with a Terrain of `heightfield` over `size` metres, `heightScale` high, and a
		// TerrainCollider; the last root, or the last child of `parent` when it is valid.
		Entity AddTerrain(Scene& scene, std::string_view name, AssetHandle heightfield, const glm::vec2& size, float heightScale,
			const glm::vec3& position = glm::vec3(0.0f), const glm::vec3& scale = glm::vec3(1.0f), const TerrainColliderComponent& collider = {},
			Entity parent = {})
		{
			Entity entity = parent.IsValid() ? scene.CreateEntity(name, parent) : scene.CreateEntity(name);
			entity.Patch<TransformComponent>([&position, &scale](TransformComponent& transform)
			{
				transform.Translation = position;
				transform.Scale = scale;
			});
			TerrainComponent terrain;
			terrain.Heightfield.SetHandle(heightfield);
			terrain.Size = size;
			terrain.HeightScale = heightScale;
			entity.AddComponent<TerrainComponent>(terrain);
			entity.AddComponent<TerrainColliderComponent>(collider);
			return entity;
		}

		// A Dynamic sphere of BallRadius at `position` that only moves along Y when `fallOnly`.
		Entity AddBall(Scene& scene, const glm::vec3& position, bool fallOnly, float friction = 0.5f)
		{
			Entity ball = Test::AddSphereBody(scene, "Ball", position, BallRadius, BodyType::Dynamic);
			Test::PatchRigidBody(ball, [fallOnly, friction](RigidBodyComponent& body)
			{
				body.Friction = friction;
				if (!fallOnly)
					return;
				body.LockTranslation = glm::bvec3(true, false, true);
				body.LockRotation = glm::bvec3(true);
			});
			return ball;
		}

		// A session of `fixture`'s scene with `assets`, the layers Default and Ground colliding with each other.
		Scope<PlaySession> StartSession(Test::SceneTestFixture& fixture, Test::InMemoryAssetManager& assets, uint64_t seed = 42)
		{
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture, seed);
			specification.Assets = &assets;
			specification.Project.Physics.Layers = { "Default", "Ground" };
			specification.Project.Physics.Collisions = { { "Default", "Default" }, { "Default", "Ground" } };
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			return std::move(*session);
		}

		// The world surface of a terrain at `position` scaled by `scale` (no rotation) under the world point (x, z): its
		// height and unit normal, by Asset/TerrainGeometry.h's rule.
		struct WorldSurface
		{
			float Height = 0.0f;
			glm::vec3 Normal = glm::vec3(0.0f, 1.0f, 0.0f);
		};

		WorldSurface GetWorldSurface(const HeightfieldData& heightfield, const TerrainExtent& extent, const glm::vec3& position, const glm::vec3& scale,
			glm::vec2 worldXZ)
		{
			const glm::vec2 local((worldXZ.x - position.x) / scale.x, (worldXZ.y - position.z) / scale.z);
			const std::optional<float> height = SampleTerrainHeight(heightfield, extent, local);
			const std::optional<glm::vec3> normal = SampleTerrainNormal(heightfield, extent, local);
			REQUIRE(height.has_value());
			REQUIRE(normal.has_value());
			return WorldSurface{ .Height = position.y + scale.y * *height, .Normal = glm::normalize(*normal / scale) };
		}

		// The plan of `owner` with `origin`, or null.
		const PhysicsBodyPlan* FindPlan(const PhysicsComposition& composition, UUID owner, PhysicsBodyOrigin origin)
		{
			const auto found = std::find_if(composition.Bodies.begin(), composition.Bodies.end(), [owner, origin](const PhysicsBodyPlan& plan)
			{
				return plan.Owner == owner && plan.Origin == origin;
			});
			return found != composition.Bodies.end() ? &*found : nullptr;
		}

		// The highest the ball at `path` gets over ticks [from, to) of `session`.
		float GetHighestPoint(PlaySession& session, std::string_view path, uint32_t from, uint32_t to)
		{
			Test::RunTicks(session, from);
			float highest = Test::GetWorldPosition(session, path).y;
			for (uint32_t tick = from; tick < to; ++tick)
			{
				session.Tick();
				highest = std::max(highest, Test::GetWorldPosition(session, path).y);
			}
			return highest;
		}

	}

	TEST_SUITE("Scene")
	{
		// §5.3 as ADR 0037 decision 21 records it: a solid collider gathered as any; the departure, its layer on its own
		// entity's implicit static body; and a TerrainCollider without a heightfield makes no collider.
		TEST_CASE("TerrainCollider: a terrain is a solid collider whose own implicit static body takes its layer")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity hill = AddTerrain(scene, "Hill", HillHandle, glm::vec2(16.0f), 8.0f, glm::vec3(0.0f), glm::vec3(1.0f), { .Layer = "Ground" });
			const Entity rock = Test::AddBoxBody(scene, "Rock", glm::vec3(2.0f, 1.0f, 2.0f), glm::vec3(0.5f), std::nullopt, hill);
			const Entity bare = AddTerrain(scene, "Bare", AssetHandle(), glm::vec2(16.0f), 8.0f, glm::vec3(40.0f, 0.0f, 0.0f));
			Entity level = scene.CreateEntity("Level");
			level.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static, .Friction = 0.25f });
			const Entity gathered = AddTerrain(scene, "Gathered", FlatHandle, glm::vec2(8.0f), 1.0f, glm::vec3(-20.0f, 0.0f, 0.0f), glm::vec3(1.0f),
				{ .Layer = "Missing" }, level);

			const std::vector<std::string> layerNames = { "Default", "Ground" };
			const std::vector<std::vector<std::string>> collisions = { { "Default", "Default" }, { "Default", "Ground" } };
			Result<PhysicsLayerTable> layers = PhysicsLayerTable::Create(layerNames, collisions);
			REQUIRE(layers.has_value());
			const PhysicsComposition composition = ComposePhysicsBodies(scene, *layers);
			// The unused Layer of a gathered terrain names nothing, so its unknown name is not reported.
			CHECK(composition.Diagnostics.empty());
			REQUIRE(composition.Bodies.size() == 2);

			const PhysicsBodyPlan* own = FindPlan(composition, hill.GetUUID(), PhysicsBodyOrigin::ImplicitStatic);
			REQUIRE(own != nullptr);
			CHECK(own->MotionType == PhysicsMotionType::Static);
			CHECK(own->Layer == 1);
			// The terrain first (its entity's own colliders), then the collider-only child it gathers.
			REQUIRE(own->Colliders.size() == 2);
			CHECK(own->Colliders[0].Entity == hill.GetUUID());
			CHECK(own->Colliders[0].Type == PhysicsColliderType::Terrain);
			CHECK_FALSE(own->Colliders[0].IsTrigger);
			CHECK(own->Colliders[1].Entity == rock.GetUUID());

			const PhysicsBodyPlan* compound = FindPlan(composition, level.GetUUID(), PhysicsBodyOrigin::RigidBody);
			REQUIRE(compound != nullptr);
			CHECK(compound->Layer == 0);
			REQUIRE(compound->Colliders.size() == 1);
			CHECK(compound->Colliders[0].Entity == gathered.GetUUID());
			// No heightfield, no collider and no body.
			CHECK(FindPlan(composition, bare.GetUUID(), PhysicsBodyOrigin::ImplicitStatic) == nullptr);

			// An unknown layer on the body it applies to falls back to Default with its diagnostic on the TerrainCollider.
			hill.Patch<TerrainColliderComponent>([](TerrainColliderComponent& collider)
			{
				collider.Layer = "Missing";
			});
			const PhysicsComposition unknown = ComposePhysicsBodies(scene, *layers);
			REQUIRE(unknown.Diagnostics.size() == 1);
			CHECK(unknown.Diagnostics[0].Code == PhysicsUnknownLayerCode);
			CHECK(unknown.Diagnostics[0].Entity == hill.GetUUID());
			CHECK(unknown.Diagnostics[0].Component == "TerrainCollider");
			CHECK(unknown.Diagnostics[0].Field == "Layer");
			const PhysicsBodyPlan* fallback = FindPlan(unknown, hill.GetUUID(), PhysicsBodyOrigin::ImplicitStatic);
			REQUIRE(fallback != nullptr);
			CHECK(fallback->Layer == 0);
		}

		// A height field is not convex: a Dynamic RigidBody that would gather a terrain is refused, as with a triangle mesh.
		TEST_CASE("TerrainCollider: a Dynamic RigidBody cannot carry a terrain")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity raft = scene.CreateEntity("Raft");
			raft.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Dynamic });
			const Entity deck = AddTerrain(scene, "Deck", FlatHandle, glm::vec2(4.0f), 1.0f, glm::vec3(0.0f), glm::vec3(1.0f), {}, raft);
			const PhysicsComposition composition = ComposePhysicsBodies(scene, PhysicsLayerTable());
			REQUIRE(composition.Bodies.size() == 1);
			CHECK_FALSE(composition.Bodies[0].IsCreatable);
			REQUIRE(composition.Diagnostics.size() == 1);
			CHECK(composition.Diagnostics[0].Code == PhysicsNonconvexDynamicCode);
			CHECK(composition.Diagnostics[0].Entity == deck.GetUUID());
			CHECK(composition.Diagnostics[0].Component == "TerrainCollider");

			// A Kinematic one may, as with a triangle mesh.
			Test::PatchRigidBody(raft, [](RigidBodyComponent& body)
			{
				body.Type = BodyType::Kinematic;
			});
			const PhysicsComposition kinematic = ComposePhysicsBodies(scene, PhysicsLayerTable());
			CHECK(kinematic.Diagnostics.empty());
			REQUIRE(kinematic.Bodies.size() == 1);
			CHECK(kinematic.Bodies[0].IsCreatable);
		}

		// The rest-height acceptance in the session: the composition bakes the terrain entity's transform into the height
		// field (translation and a non-uniform scale here), so a sphere dropped on it rests at the analytic surface under it,
		// one radius less the slop along the normal above it.
		TEST_CASE("TerrainCollider: a sphere rests on a moved and scaled terrain at its analytic surface height")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			const Ref<HeightfieldData> hill = MakeHill();
			assets.Publish(HillHandle, hill, "Assets/Terrain/Hill.heightfield");
			const glm::vec3 position(5.0f, 1.0f, -3.0f);
			const glm::vec3 scale(2.0f, 0.5f, 1.0f);
			const TerrainExtent extent{ .Size = glm::vec2(16.0f), .HeightScale = 8.0f };
			static_cast<void>(AddTerrain(scene, "Hill", HillHandle, extent.Size, extent.HeightScale, position, scale));
			// Local (8.6, 7.3): inside cell (8, 7), below its diagonal, away from the ridge.
			const glm::vec2 point(position.x + 8.6f * scale.x, position.z + 7.3f * scale.z);
			const WorldSurface surface = GetWorldSurface(*hill, extent, position, scale, point);
			static_cast<void>(AddBall(scene, glm::vec3(point.x, surface.Height + 1.5f, point.y), true));

			Scope<PlaySession> session = StartSession(fixture, assets);
			CHECK(session->GetPhysics().GetDiagnostics().empty());
			Test::RunTicks(*session, 240);
			const float expected = surface.Height + (BallRadius - PenetrationSlop) / surface.Normal.y;
			CHECK(Test::GetWorldPosition(*session, "/Ball").y == doctest::Approx(expected).epsilon(0.01));
		}

		// Decision 21's material: the terrain's own body bounces and grips with the TerrainCollider's Restitution and Friction
		// (Jolt combines restitution by the larger, friction by the geometric mean); under a RigidBody the body's apply.
		TEST_CASE("TerrainCollider: a terrain's own body has its restitution and friction, which a RigidBody above it replaces")
		{
			Test::InMemoryAssetManager assets;
			assets.Publish(FlatHandle, CreateRef<HeightfieldData>(MakeFlatHeightfield(9)), "Assets/Terrain/Flat.heightfield");
			// A 20 degree slope: 8 m along X rising tan(20 degrees) * 8 = 2.91 m.
			assets.Publish(HillHandle, MakeHeightfield(9, [](uint32_t x, uint32_t /*z*/)
			{
				return static_cast<uint16_t>((x * 65535u) / 8u);
			}),
				"Assets/Terrain/Slope.heightfield");

			// A ball dropped 1.75 m onto the flat terrain: the highest it gets after its first contact (about tick 36).
			const auto bounce = [&assets](float restitution, bool underRigidBody)
			{
				Test::SceneTestFixture fixture;
				Scene& scene = fixture.GetScene();
				Entity parent;
				if (underRigidBody)
				{
					parent = scene.CreateEntity("Level");
					parent.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
				}
				static_cast<void>(AddTerrain(scene, "Ground", FlatHandle, glm::vec2(8.0f), 1.0f, glm::vec3(0.0f), glm::vec3(1.0f),
					{ .Restitution = restitution }, parent));
				static_cast<void>(AddBall(scene, glm::vec3(4.0f, 2.0f, 4.0f), true));
				Scope<PlaySession> session = StartSession(fixture, assets);
				return GetHighestPoint(*session, "/Ball", 45, 120);
			};
			CHECK(bounce(1.0f, false) > 1.5f);
			CHECK(bounce(0.0f, false) < 0.5f);
			CHECK(bounce(1.0f, true) < 0.5f);

			// A ball that cannot roll, set on the slope: how far it slides along X between ticks 30 and 90.
			const auto slide = [&assets](float friction)
			{
				Test::SceneTestFixture fixture;
				Scene& scene = fixture.GetScene();
				const float rise = 2.9117f; // tan(20 degrees) * 8
				static_cast<void>(AddTerrain(scene, "Slope", HillHandle, glm::vec2(8.0f), rise, glm::vec3(0.0f), glm::vec3(1.0f), { .Friction = friction }));
				Entity ball = AddBall(scene, glm::vec3(4.0f, rise * 0.5f + 0.3f, 4.0f), false, 1.0f);
				Test::PatchRigidBody(ball, [](RigidBodyComponent& body)
				{
					body.LockRotation = glm::bvec3(true);
				});
				Scope<PlaySession> session = StartSession(fixture, assets);
				Test::RunTicks(*session, 30);
				const float start = Test::GetWorldPosition(*session, "/Ball").x;
				Test::RunTicks(*session, 60);
				return start - Test::GetWorldPosition(*session, "/Ball").x;
			};
			CHECK(slide(1.0f) < 0.05f);
			CHECK(slide(0.0f) > 1.0f);
		}

		// Queries see the terrain's body as any body: the hit names the terrain as the collider and the body, at the surface.
		TEST_CASE("TerrainCollider: raycasts and overlaps hit the terrain at its surface")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			const Ref<HeightfieldData> hill = MakeHill();
			assets.Publish(HillHandle, hill, "Assets/Terrain/Hill.heightfield");
			const glm::vec3 position(-4.0f, 2.0f, 6.0f);
			const glm::vec3 scale(1.5f, 2.0f, 0.5f);
			const TerrainExtent extent{ .Size = glm::vec2(16.0f), .HeightScale = 8.0f };
			const Entity terrain = AddTerrain(scene, "Hill", HillHandle, extent.Size, extent.HeightScale, position, scale);
			Scope<PlaySession> session = StartSession(fixture, assets);
			const PhysicsSystem& physics = session->GetPhysics();

			const std::optional<PhysicsBodyReport> report = physics.GetBodyInfo(terrain.GetUUID());
			REQUIRE(report.has_value());
			CHECK(report->Origin == PhysicsBodyOrigin::ImplicitStatic);
			CHECK(report->MotionType == PhysicsMotionType::Static);
			CHECK(report->Colliders == std::vector<UUID>{ terrain.GetUUID() });
			// Jolt's bounds hold the terrain, and reach one cell past its far edges: it pads an odd sample count to its block
			// size with samples that collide with nothing.
			CHECK(report->Bounds.Min.x == doctest::Approx(position.x).epsilon(1.0e-4));
			CHECK(report->Bounds.Min.z == doctest::Approx(position.z).epsilon(1.0e-4));
			CHECK(report->Bounds.Max.x >= position.x + 16.0f * scale.x - 1.0e-3f);
			CHECK(report->Bounds.Max.x <= position.x + 17.0f * scale.x + 1.0e-3f);
			CHECK(report->Bounds.Max.z >= position.z + 16.0f * scale.z - 1.0e-3f);
			CHECK(report->Bounds.Max.z <= position.z + 17.0f * scale.z + 1.0e-3f);

			// Over the ridge (local (4.4, 8.25)) and on the ramp (local (12.3, 2.6)).
			for (const glm::vec2 local : { glm::vec2(4.4f, 8.25f), glm::vec2(12.3f, 2.6f) })
			{
				CAPTURE(local);
				const glm::vec2 point(position.x + local.x * scale.x, position.z + local.y * scale.z);
				const WorldSurface surface = GetWorldSurface(*hill, extent, position, scale, point);
				const Result<std::optional<PhysicsRaycastHit>> hit = physics.Raycast(glm::vec3(point.x, 50.0f, point.y), glm::vec3(0.0f, -1.0f, 0.0f), 100.0f);
				REQUIRE(hit.has_value());
				REQUIRE(hit->has_value());
				CHECK((*hit)->Entity == terrain.GetUUID());
				CHECK((*hit)->Body == terrain.GetUUID());
				CHECK((*hit)->Point.y == doctest::Approx(surface.Height).epsilon(1.0e-3));
				CHECK(Test::ApproxEqual((*hit)->Normal, surface.Normal, 1.0e-3f));

				const Result<std::vector<PhysicsOverlapHit>> touching = physics.OverlapSphere(glm::vec3(point.x, surface.Height + 0.1f, point.y), 0.2f);
				REQUIRE(touching.has_value());
				CHECK(*touching == std::vector<PhysicsOverlapHit>{ { .Entity = terrain.GetUUID(), .Body = terrain.GetUUID() } });
				const Result<std::vector<PhysicsOverlapHit>> above = physics.OverlapSphere(glm::vec3(point.x, surface.Height + 2.0f, point.y), 0.2f);
				REQUIRE(above.has_value());
				CHECK(above->empty());
			}
		}

		// An edit or a reimport of the heightfield (a new asset version) and a change of the Terrain's layout rebuild the
		// terrain's shape at the next step, in place.
		TEST_CASE("TerrainCollider: a new heightfield version or a Terrain change rebuilds the terrain's shape")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			assets.Publish(FlatHandle, CreateRef<HeightfieldData>(MakeFlatHeightfield(5)), "Assets/Terrain/Flat.heightfield");
			static_cast<void>(AddTerrain(scene, "Ground", FlatHandle, glm::vec2(8.0f), 4.0f));
			Scope<PlaySession> session = StartSession(fixture, assets);
			const auto surfaceAt = [&session]()
			{
				const Result<std::optional<PhysicsRaycastHit>> hit =
					session->GetPhysics().Raycast(glm::vec3(3.3f, 20.0f, 4.1f), glm::vec3(0.0f, -1.0f, 0.0f), 40.0f);
				REQUIRE(hit.has_value());
				REQUIRE(hit->has_value());
				return (*hit)->Point.y;
			};
			CHECK(surfaceAt() == doctest::Approx(0.0f));
			REQUIRE(session->GetPhysics().GetBodyInfo(Test::GetEntityId(session->GetScene(), "/Ground")).has_value());

			// Every sample at half the height scale: 2 m.
			assets.Publish(FlatHandle, CreateRef<HeightfieldData>(MakeFlatHeightfield(5, 32768)), "Assets/Terrain/Flat.heightfield");
			session->Tick();
			CHECK(surfaceAt() == doctest::Approx(4.0f * 32768.0f / 65535.0f).epsilon(1.0e-3));

			Entity ground = session->GetScene().FindEntityByPath("/Ground");
			REQUIRE(ground.IsValid());
			ground.Patch<TerrainComponent>([](TerrainComponent& terrain)
			{
				terrain.HeightScale = 8.0f;
			});
			session->Tick();
			CHECK(surfaceAt() == doctest::Approx(8.0f * 32768.0f / 65535.0f).epsilon(1.0e-3));
			CHECK(session->GetPhysics().GetDiagnostics().empty());
		}

		// A heightfield that does not load refuses the body that holds the terrain, with the TerrainCollider's diagnostic, as a
		// MeshCollider without a mesh is refused.
		TEST_CASE("TerrainCollider: a heightfield that does not load refuses the terrain's body")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			const Entity terrain = AddTerrain(scene, "Broken", MissingHandle, glm::vec2(8.0f), 1.0f);
			Test::ExpectLog refused(LogLevel::Error, PhysicsInvalidShapeCode);
			Scope<PlaySession> session = StartSession(fixture, assets);
			CHECK_FALSE(session->GetPhysics().GetBodyInfo(terrain.GetUUID()).has_value());
			const std::span<const PhysicsDiagnostic> diagnostics = session->GetPhysics().GetDiagnostics();
			REQUIRE(diagnostics.size() == 1);
			CHECK(diagnostics[0].Code == PhysicsInvalidShapeCode);
			CHECK(diagnostics[0].Entity == terrain.GetUUID());
			CHECK(diagnostics[0].Component == "TerrainCollider");

			// So does a description without an asset manager, or of an asset of another type, located at the terrain.
			const PhysicsComposition composition = ComposePhysicsBodies(scene, PhysicsLayerTable());
			REQUIRE(composition.Bodies.size() == 1);
			const Result<BodyShapeDescription> withoutAssets = DescribePhysicsBodyShape(scene, composition.Bodies[0], nullptr);
			REQUIRE_FALSE(withoutAssets.has_value());
			CHECK(GetPhysicsDiagnosticCode(withoutAssets.error()) == PhysicsInvalidShapeCode);
			CHECK(withoutAssets.error().GetLocation().Entity == terrain.GetUUID());
			assets.Publish(MissingHandle, CreateRef<MeshData>(), "Assets/Models/NotATerrain.glb");
			const Result<BodyShapeDescription> otherType = DescribePhysicsBodyShape(scene, composition.Bodies[0], &assets);
			REQUIRE_FALSE(otherType.has_value());
			CHECK(GetPhysicsDiagnosticCode(otherType.error()) == PhysicsInvalidShapeCode);
			CHECK(otherType.error().GetLocation().Entity == terrain.GetUUID());
		}

		// The terrain's body is a body of the physics block like any other (ADR 0028's phase-3 ledger: no block of its own),
		// so a ball rolled over a terrain gives one state hash in every configuration: this one, committed from Debug and
		// confirmed in Release.
		TEST_CASE("TerrainCollider: a ball rolled over a terrain hashes to its committed value in every configuration")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			assets.Publish(HillHandle, MakeHill(), "Assets/Terrain/Hill.heightfield");
			static_cast<void>(AddTerrain(scene, "Hill", HillHandle, glm::vec2(16.0f), 4.0f, glm::vec3(0.0f), glm::vec3(1.0f), { .Friction = 0.8f }));
			Entity ball = AddBall(scene, glm::vec3(14.0f, 3.0f, 2.0f), false);
			Test::PatchRigidBody(ball, [](RigidBodyComponent& body)
			{
				body.InitialLinearVelocity = glm::vec3(-1.0f, 0.0f, 3.0f);
			});
			Scope<PlaySession> session = StartSession(fixture, assets);
			Test::RunTicks(*session, 180);
			CHECK(std::format("{:016x}", session->ComputeStateHash()) == "35a92b5770fde15c");
		}
	}

}
