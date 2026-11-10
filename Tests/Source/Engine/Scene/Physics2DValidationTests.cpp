#include "TestsPCH.h"

#include "Engine/Scene/Physics2DValidation.h"

#include "Engine/Asset/SpriteSheetData.h"
#include "Engine/Asset/TilemapCollision.h"
#include "Engine/Asset/TilemapData.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Scene/Components/BoxCollider2DComponent.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CircleCollider2DComponent.h"
#include "Engine/Scene/Components/PolygonCollider2DComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/TilemapColliderComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Physics2DSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/ExpectLog.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The 2D physics validation codes (Docs/Decisions/0032-m21-contract.md decision 12, stream P2; Roadmap M21 "An entity holding
// both 2D and 3D physics components is a validation error").

namespace Engine {

	namespace {

		Entity CreatePlanarEntity(Scene& scene, std::string_view name, const glm::vec2& position)
		{
			Entity entity = scene.CreateEntity(name);
			entity.Patch<TransformComponent>([position](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(position, 0.0f);
			});
			return entity;
		}

		Entity CreatePlanarBody(Scene& scene, std::string_view name, const glm::vec2& position, BodyType type)
		{
			Entity entity = CreatePlanarEntity(scene, name, position);
			entity.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = type });
			return entity;
		}

		PhysicsLayerTable MakeDefaultLayers()
		{
			Result<PhysicsLayerTable> layers = PhysicsLayerTable::Create(std::vector<std::string>{ "Default" }, std::vector<std::vector<std::string>>{ { "Default", "Default" } });
			REQUIRE(layers.has_value());
			return std::move(*layers);
		}

		// The one diagnostic of `diagnostics` with `code` on `entity`, or nullopt (none, or more than one).
		std::optional<PhysicsDiagnostic> FindDiagnostic(std::span<const PhysicsDiagnostic> diagnostics, std::string_view code, UUID entity)
		{
			std::optional<PhysicsDiagnostic> found;
			for (const PhysicsDiagnostic& diagnostic : diagnostics)
			{
				if (diagnostic.Code != code || diagnostic.Entity != entity)
					continue;
				if (found.has_value())
					return std::nullopt;
				found = diagnostic;
			}
			return found;
		}

		// Checks that `diagnostics` holds one diagnostic of `code` on `entity`, with `severity`, `component` and `field`.
		void CheckReported(std::span<const PhysicsDiagnostic> diagnostics, std::string_view code, UUID entity, DiagnosticSeverity severity,
			std::string_view component, std::string_view field)
		{
			CAPTURE(code);
			const std::optional<PhysicsDiagnostic> found = FindDiagnostic(diagnostics, code, entity);
			REQUIRE(found.has_value());
			CHECK(found->Severity == severity);
			CHECK(found->Component == component);
			CHECK(found->Field == field);
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("Physics2DValidation: a mixed entity and an off-plane body are reported")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity mixed = CreatePlanarEntity(scene, "Mixed", glm::vec2(0.0f));
			mixed.AddComponent<RigidBody2DComponent>();
			mixed.AddComponent<CircleCollider2DComponent>();
			mixed.AddComponent<BoxColliderComponent>();
			Entity tilted = CreatePlanarEntity(scene, "Tilted", glm::vec2(4.0f, 0.0f));
			tilted.AddComponent<RigidBody2DComponent>();
			tilted.AddComponent<BoxCollider2DComponent>();
			tilted.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(1.0f, 0.0f, 0.0f));
			});
			Entity spun = CreatePlanarEntity(scene, "Spun", glm::vec2(8.0f, 0.0f));
			spun.AddComponent<RigidBody2DComponent>();
			spun.AddComponent<BoxCollider2DComponent>();
			spun.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 0.0f, 1.0f));
			});
			TransformSystem::Update(scene);

			Result<PhysicsLayerTable> layers = PhysicsLayerTable::Create(std::vector<std::string>{ "Default" }, std::vector<std::vector<std::string>>{ { "Default", "Default" } });
			REQUIRE(layers.has_value());
			const std::vector<PhysicsDiagnostic> diagnostics = ValidateScenePhysics2D(scene, *layers, nullptr);
			REQUIRE(diagnostics.size() == 2);
			CHECK(diagnostics[0].Code == Physics2DMixedWorldsCode);
			CHECK(diagnostics[0].Entity == mixed.GetUUID());
			CHECK(diagnostics[1].Code == Physics2DOffPlaneCode);
			CHECK(diagnostics[1].Entity == tilted.GetUUID());
			CHECK(diagnostics[1].Severity == DiagnosticSeverity::Warning);
		}

		// Decision 12 (W21.1's review): the 2D world reports the rules it shares with the 3D one under the 3D codes, naming the
		// 2D component and field, both in project.validate's check (ValidateScenePhysics2D) and in the play session's system,
		// which refuses the bodies the 3D world would refuse.
		TEST_CASE("Physics2DValidation: the shared PHYSICS_* rules are reported for 2D bodies, naming the 2D component")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			// An undeclared layer: reported on the RigidBody2D's Layer; the body is made on layer 0.
			Entity lost = CreatePlanarBody(scene, "Lost", glm::vec2(0.0f), BodyType::Dynamic);
			lost.Patch<RigidBody2DComponent>([](RigidBody2DComponent& body)
			{
				body.Layer = "Nowhere";
			});
			lost.AddComponent<BoxCollider2DComponent>();
			// Colliders that disagree on IsTrigger: refused.
			Entity mixed = CreatePlanarBody(scene, "Mixed", glm::vec2(4.0f, 0.0f), BodyType::Static);
			mixed.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .IsTrigger = true });
			mixed.AddComponent<CircleCollider2DComponent>();
			// A Dynamic body of triggers: refused; a Static one is a Kinematic sensor.
			Entity sensor = CreatePlanarBody(scene, "Sensor", glm::vec2(8.0f, 0.0f), BodyType::Dynamic);
			sensor.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .IsTrigger = true });
			Entity gate = CreatePlanarBody(scene, "Gate", glm::vec2(12.0f, 0.0f), BodyType::Static);
			gate.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .IsTrigger = true });
			// A Dynamic body under a Kinematic one: a warning, the body simulates in world space.
			Entity platform = CreatePlanarBody(scene, "Platform", glm::vec2(16.0f, 0.0f), BodyType::Kinematic);
			platform.AddComponent<BoxCollider2DComponent>();
			Entity crate = CreatePlanarBody(scene, "Crate", glm::vec2(0.0f, 2.0f), BodyType::Dynamic);
			crate.AddComponent<BoxCollider2DComponent>();
			REQUIRE(scene.SetParent(crate, platform, {}, false).has_value());
			// A circle under a scale whose X and Y differ: a warning naming the CircleCollider2D; a scale that differs only in Z
			// is uniform in the plane.
			Entity egg = CreatePlanarBody(scene, "Egg", glm::vec2(20.0f, 0.0f), BodyType::Static);
			egg.AddComponent<CircleCollider2DComponent>();
			egg.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(2.0f, 1.0f, 1.0f);
			});
			Entity ball = CreatePlanarBody(scene, "Ball", glm::vec2(24.0f, 0.0f), BodyType::Static);
			ball.AddComponent<CircleCollider2DComponent>();
			ball.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(2.0f, 2.0f, 7.0f);
			});
			// A polygon whose points lie on one line has no hull: an invalid shape, named on its collider entity.
			Entity sliver = CreatePlanarBody(scene, "Sliver", glm::vec2(28.0f, 0.0f), BodyType::Static);
			Entity edge = CreatePlanarEntity(scene, "Edge", glm::vec2(0.0f));
			edge.AddComponent<PolygonCollider2DComponent>(PolygonCollider2DComponent{ .Points = { glm::vec2(-1.0f, 0.0f), glm::vec2(0.0f), glm::vec2(1.0f, 0.0f) } });
			REQUIRE(scene.SetParent(edge, sliver, {}, false).has_value());
			TransformSystem::Update(scene);

			const PhysicsLayerTable layers = MakeDefaultLayers();
			const std::vector<PhysicsDiagnostic> validated = ValidateScenePhysics2D(scene, layers, nullptr);
			CheckReported(validated, PhysicsUnknownLayerCode, lost.GetUUID(), DiagnosticSeverity::Error, "RigidBody2D", "Layer");
			CheckReported(validated, PhysicsMixedTriggerCode, mixed.GetUUID(), DiagnosticSeverity::Error, "", "IsTrigger");
			CheckReported(validated, PhysicsDynamicTriggerCode, sensor.GetUUID(), DiagnosticSeverity::Error, "RigidBody2D", "Type");
			CheckReported(validated, PhysicsDynamicUnderMovingParentCode, crate.GetUUID(), DiagnosticSeverity::Warning, "RigidBody2D", "Type");
			CheckReported(validated, PhysicsNonuniformScaleCode, egg.GetUUID(), DiagnosticSeverity::Warning, "CircleCollider2D", "");
			CheckReported(validated, PhysicsInvalidShapeCode, edge.GetUUID(), DiagnosticSeverity::Error, "PolygonCollider2D", "");
			CHECK_FALSE(FindDiagnostic(validated, PhysicsNonuniformScaleCode, ball.GetUUID()).has_value());
			CHECK(validated.size() == 6);

			// The play session's system reports the same rules and creates the bodies the rules allow.
			const Test::ExpectLog unknownLogged(LogLevel::Error, PhysicsUnknownLayerCode);
			const Test::ExpectLog mixedLogged(LogLevel::Error, PhysicsMixedTriggerCode);
			const Test::ExpectLog triggerLogged(LogLevel::Error, PhysicsDynamicTriggerCode);
			const Test::ExpectLog shapeLogged(LogLevel::Error, PhysicsInvalidShapeCode);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			const std::span<const PhysicsDiagnostic> raised = (*system)->GetDiagnostics();
			CheckReported(raised, PhysicsUnknownLayerCode, lost.GetUUID(), DiagnosticSeverity::Error, "RigidBody2D", "Layer");
			CheckReported(raised, PhysicsMixedTriggerCode, mixed.GetUUID(), DiagnosticSeverity::Error, "", "IsTrigger");
			CheckReported(raised, PhysicsDynamicTriggerCode, sensor.GetUUID(), DiagnosticSeverity::Error, "RigidBody2D", "Type");
			CheckReported(raised, PhysicsDynamicUnderMovingParentCode, crate.GetUUID(), DiagnosticSeverity::Warning, "RigidBody2D", "Type");
			CheckReported(raised, PhysicsNonuniformScaleCode, egg.GetUUID(), DiagnosticSeverity::Warning, "CircleCollider2D", "");
			CheckReported(raised, PhysicsInvalidShapeCode, edge.GetUUID(), DiagnosticSeverity::Error, "PolygonCollider2D", "");
			CHECK((*system)->GetBodyBounds(lost.GetUUID()).has_value());
			CHECK_FALSE((*system)->GetBodyBounds(mixed.GetUUID()).has_value());
			CHECK_FALSE((*system)->GetBodyBounds(sensor.GetUUID()).has_value());
			CHECK((*system)->GetMotionType(gate.GetUUID()).value_or(PhysicsMotionType::Static) == PhysicsMotionType::Kinematic);
			CHECK((*system)->GetBodyBounds(crate.GetUUID()).has_value());
			CHECK((*system)->GetBodyBounds(egg.GetUUID()).has_value());
			CHECK_FALSE((*system)->GetBodyBounds(sliver.GetUUID()).has_value());
		}

		// M21's milestone review: the validator checks every Dynamic RigidBody2D's built shape as the session's system does before
		// it creates the body (PhysicsShape::CheckDynamicBody), so a body the session refuses is reported at edit time, on the
		// same component and field: here a body of the largest Mass whose two collider-only circles lie 40 km either side of it,
		// whose inertia (about m d^2 = 1.6e18 kg m^2) exceeds what Jolt takes; the same colliders under a 1 kg body are valid.
		TEST_CASE("Physics2DValidation: a Dynamic RigidBody2D whose shape the world refuses is reported as the session reports it")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			const auto addWheels = [&scene](Entity body, float distance)
			{
				for (const float side : { -1.0f, 1.0f })
				{
					Entity wheel = CreatePlanarEntity(scene, "Wheel", glm::vec2(side * distance, 0.0f));
					wheel.AddComponent<CircleCollider2DComponent>();
					REQUIRE(scene.SetParent(wheel, body, {}, false).has_value());
				}
			};
			Entity heavy = CreatePlanarBody(scene, "Heavy", glm::vec2(0.0f), BodyType::Dynamic);
			heavy.Patch<RigidBody2DComponent>([](RigidBody2DComponent& body)
			{
				body.Mass = MaxPhysicsMass;
			});
			addWheels(heavy, 40000.0f);
			Entity light = CreatePlanarBody(scene, "Light", glm::vec2(0.0f, 100.0f), BodyType::Dynamic);
			addWheels(light, 40000.0f);
			TransformSystem::Update(scene);

			const PhysicsLayerTable layers = MakeDefaultLayers();
			const std::vector<PhysicsDiagnostic> validated = ValidateScenePhysics2D(scene, layers, nullptr);
			CheckReported(validated, PhysicsInvalidShapeCode, heavy.GetUUID(), DiagnosticSeverity::Error, "RigidBody2D", "Mass");
			CHECK(validated.size() == 1);

			const Test::ExpectLog refused(LogLevel::Error, PhysicsInvalidShapeCode);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			const std::span<const PhysicsDiagnostic> raised = (*system)->GetDiagnostics();
			CheckReported(raised, PhysicsInvalidShapeCode, heavy.GetUUID(), DiagnosticSeverity::Error, "RigidBody2D", "Mass");
			CHECK(raised.size() == validated.size());
			CHECK_FALSE((*system)->GetBodyBounds(heavy.GetUUID()).has_value());
			CHECK((*system)->GetBodyBounds(light.GetUUID()).has_value());
		}

		// Decision 12 (W21.1's review): the 2D bodies beyond the world's limit are refused in canonical order, one
		// PHYSICS_LIMIT_EXCEEDED Error per refused owner, subject "bodies", as the 3D world's.
		TEST_CASE("Physics2DValidation: 2D bodies beyond the body limit are refused with PHYSICS_LIMIT_EXCEEDED in canonical order")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			std::vector<Entity> boxes;
			for (int index = 0; index < 5; ++index)
			{
				Entity box = CreatePlanarBody(scene, "Box", glm::vec2(static_cast<float>(index) * 2.0f, 0.0f), BodyType::Dynamic);
				box.AddComponent<BoxCollider2DComponent>();
				boxes.push_back(box);
			}
			TransformSystem::Update(scene);
			const Test::ExpectLog limitLogged(LogLevel::Error, PhysicsLimitExceededCode);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene, .Limits = PhysicsWorldLimits{ .MaxBodies = 3 } });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			CHECK((*system)->GetStats().BodyCount == 3);
			for (size_t index = 0; index < boxes.size(); ++index)
			{
				CAPTURE(index);
				const std::optional<PhysicsDiagnostic> limit = FindDiagnostic((*system)->GetDiagnostics(), PhysicsLimitExceededCode, boxes[index].GetUUID());
				CHECK((*system)->GetBodyBounds(boxes[index].GetUUID()).has_value() == (index < 3));
				REQUIRE(limit.has_value() == (index >= 3));
				if (limit.has_value())
				{
					CHECK(limit->Severity == DiagnosticSeverity::Error);
					CHECK(limit->Subject == "bodies");
				}
			}
		}

		// C21.2 (Docs/Decisions/0032-m21-contract.md decision 25, stream T2): a TilemapCollider loads its tilemap through the
		// validator's asset manager as the session's system does, so project.validate and the session agree.
		TEST_CASE("Physics2DValidation: a TilemapCollider without a tilemap is PHYSICS_INVALID_SHAPE, and one without a solid cell is quiet")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			const AssetHandle emptyHandle = UUID(8970);
			Ref<TilemapData> empty = CreateRef<TilemapData>();
			empty->Palette = { TilemapTile{ .Solid = false } };
			empty->ChunkSize = 1;
			empty->Chunks = { TilemapChunk{ .X = 0, .Y = 0, .Tiles = { 1 } } };
			assets.Publish(emptyHandle, empty, "Assets/Tilemaps/Decor.tilemap");
			// Neither a Tilemap nor a TilemapRenderer to take one from.
			Entity bare = CreatePlanarEntity(scene, "Bare", glm::vec2(0.0f));
			bare.AddComponent<TilemapColliderComponent>();
			// A tilemap that is not registered.
			Entity missing = CreatePlanarEntity(scene, "Missing", glm::vec2(4.0f, 0.0f));
			missing.AddComponent<TilemapColliderComponent>(TilemapColliderComponent{ .Tilemap = TypedAssetHandle<AssetType::Tilemap>(AssetHandle(0x89700000ull)) });
			// Only non-solid cells: no shape and no body, without a diagnostic.
			Entity decor = CreatePlanarEntity(scene, "Decor", glm::vec2(8.0f, 0.0f));
			decor.AddComponent<TilemapColliderComponent>(TilemapColliderComponent{ .Tilemap = TypedAssetHandle<AssetType::Tilemap>(emptyHandle) });
			// A TilemapCollider is a 2D physics component: with a 3D one beside it, the entity is in both worlds.
			Entity mixed = CreatePlanarEntity(scene, "Mixed", glm::vec2(12.0f, 0.0f));
			mixed.AddComponent<TilemapColliderComponent>(TilemapColliderComponent{ .Tilemap = TypedAssetHandle<AssetType::Tilemap>(emptyHandle) });
			mixed.AddComponent<BoxColliderComponent>();
			TransformSystem::Update(scene);

			const PhysicsLayerTable layers = MakeDefaultLayers();
			const std::vector<PhysicsDiagnostic> diagnostics = ValidateScenePhysics2D(scene, layers, &assets);
			const std::optional<PhysicsDiagnostic> bareShape = FindDiagnostic(diagnostics, PhysicsInvalidShapeCode, bare.GetUUID());
			REQUIRE(bareShape.has_value());
			CHECK(bareShape->Component == "TilemapCollider");
			CHECK(bareShape->Field == "Tilemap");
			CHECK(FindDiagnostic(diagnostics, PhysicsInvalidShapeCode, missing.GetUUID()).has_value());
			CHECK(FindDiagnostic(diagnostics, Physics2DMixedWorldsCode, mixed.GetUUID()).has_value());
			CHECK(std::ranges::none_of(diagnostics, [&decor](const PhysicsDiagnostic& diagnostic)
			{
				return diagnostic.Entity == decor.GetUUID();
			}));
			// Without an asset manager every tilemap collider is PHYSICS_INVALID_SHAPE, as a mesh collider is in the 3D world.
			const std::vector<PhysicsDiagnostic> withoutAssets = ValidateScenePhysics2D(scene, layers, nullptr);
			CHECK(FindDiagnostic(withoutAssets, PhysicsInvalidShapeCode, decor.GetUUID()).has_value());

			// The session reports the same and creates none of these bodies.
			const Test::ExpectLog shapeLogged(LogLevel::Error, PhysicsInvalidShapeCode);
			const Test::ExpectLog mixedLogged(LogLevel::Error, Physics2DMixedWorldsCode);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			CHECK((*system)->GetStats().BodyCount == 0);
			CHECK(FindDiagnostic((*system)->GetDiagnostics(), PhysicsInvalidShapeCode, bare.GetUUID()).has_value());
		}

		// Stream T2 (decision 25): a TilemapCollider's boxes are several sub-shapes of one collider, so a shape the 2D world
		// refuses is named by the collider it comes from, not by its place among the sub-shapes: a polygon gathered after a
		// tilemap's three boxes names its PolygonCollider2D, and a tilemap box too large for the world (a 64-cell row of
		// 1 km cells) names the TilemapCollider and its Tilemap field, in project.validate and in the session alike.
		TEST_CASE("Physics2DValidation: a shape refused after a tilemap's boxes names its own collider, and a refused box the TilemapCollider")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			const AssetHandle dotsHandle = UUID(8971);
			Ref<TilemapData> dots = CreateRef<TilemapData>();
			dots->ChunkSize = 4;
			dots->Palette = { TilemapTile{ .Solid = true } };
			dots->Chunks = { TilemapChunk{ .X = 0, .Y = 0, .Tiles = { 1, 0, 1, 0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 0 } } };
			assets.Publish(dotsHandle, dots, "Assets/Tilemaps/Dots.tilemap");
			const AssetHandle wideHandle = UUID(8972);
			Ref<TilemapData> wide = CreateRef<TilemapData>();
			wide->CellSize = glm::vec2(MaxTilemapCellSize);
			wide->ChunkSize = MaxTilemapChunkSize;
			wide->Palette = { TilemapTile{ .Solid = true } };
			TilemapChunk row{ .X = 0, .Y = 0, .Tiles = std::vector<uint32_t>(static_cast<size_t>(MaxTilemapChunkSize) * MaxTilemapChunkSize, 0) };
			std::fill_n(row.Tiles.begin(), MaxTilemapChunkSize, 1u);
			wide->Chunks = { std::move(row) };
			assets.Publish(wideHandle, wide, "Assets/Tilemaps/Wide.tilemap");

			// The terrain's implicit static body: its tilemap's three boxes, then the gathered polygon whose points lie on a line.
			Entity terrain = CreatePlanarEntity(scene, "Terrain", glm::vec2(0.0f));
			terrain.AddComponent<TilemapColliderComponent>(TilemapColliderComponent{ .Tilemap = TypedAssetHandle<AssetType::Tilemap>(dotsHandle) });
			Entity sliver = CreatePlanarEntity(scene, "Sliver", glm::vec2(0.0f, 6.0f));
			sliver.AddComponent<PolygonCollider2DComponent>(
				PolygonCollider2DComponent{ .Points = { glm::vec2(-1.0f, 0.0f), glm::vec2(0.0f), glm::vec2(1.0f, 0.0f) } });
			REQUIRE(scene.SetParent(sliver, terrain, {}, false).has_value());
			// A box, then the tilemap's one 64 km box.
			Entity plain = CreatePlanarEntity(scene, "Plain", glm::vec2(0.0f, -100.0f));
			plain.AddComponent<BoxCollider2DComponent>();
			plain.AddComponent<TilemapColliderComponent>(TilemapColliderComponent{ .Tilemap = TypedAssetHandle<AssetType::Tilemap>(wideHandle) });
			TransformSystem::Update(scene);

			const std::vector<PhysicsDiagnostic> validated = ValidateScenePhysics2D(scene, MakeDefaultLayers(), &assets);
			CheckReported(validated, PhysicsInvalidShapeCode, sliver.GetUUID(), DiagnosticSeverity::Error, "PolygonCollider2D", "");
			CheckReported(validated, PhysicsInvalidShapeCode, plain.GetUUID(), DiagnosticSeverity::Error, "TilemapCollider", "Tilemap");
			CHECK_FALSE(FindDiagnostic(validated, PhysicsInvalidShapeCode, terrain.GetUUID()).has_value());

			const Test::ExpectLog shapeLogged(LogLevel::Error, PhysicsInvalidShapeCode);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			const std::span<const PhysicsDiagnostic> raised = (*system)->GetDiagnostics();
			CheckReported(raised, PhysicsInvalidShapeCode, sliver.GetUUID(), DiagnosticSeverity::Error, "PolygonCollider2D", "");
			CheckReported(raised, PhysicsInvalidShapeCode, plain.GetUUID(), DiagnosticSeverity::Error, "TilemapCollider", "Tilemap");
			CHECK((*system)->GetStats().BodyCount == 0);
		}

		// Decision 25 (W21.2's review): one TilemapCollider holds at most MaxTilemapColliderBoxes merged boxes, and its Tilemap
		// must load as a tilemap. A map of exactly that many boxes makes its body; one more box, or a Tilemap that names a sprite
		// sheet, is PHYSICS_INVALID_SHAPE on the TilemapCollider's Tilemap and makes none, in project.validate and in the session.
		TEST_CASE("Physics2DValidation: a TilemapCollider past MaxTilemapColliderBoxes boxes or naming another asset type is PHYSICS_INVALID_SHAPE")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			// A 64 x 64 checkerboard chunk merges into 2,048 boxes of one cell, so 32 of them make exactly the limit.
			constexpr uint32_t Side = MaxTilemapChunkSize;
			constexpr int32_t FullChunks = 32;
			static_assert(static_cast<uint32_t>(FullChunks) * Side * Side / 2 == MaxTilemapColliderBoxes);
			std::vector<uint32_t> checkerboard(static_cast<size_t>(Side) * Side, 0);
			for (uint32_t y = 0; y < Side; ++y)
			{
				for (uint32_t x = 0; x < Side; ++x)
					checkerboard[static_cast<size_t>(y) * Side + x] = (x + y) % 2 == 0 ? 1u : 0u;
			}
			Ref<TilemapData> full = CreateRef<TilemapData>();
			full->ChunkSize = Side;
			full->Palette = { TilemapTile{ .Solid = true } };
			for (int32_t x = 0; x < FullChunks; ++x)
				full->Chunks.push_back(TilemapChunk{ .X = x, .Y = 0, .Tiles = checkerboard });
			REQUIRE(MergeTilemapBoxes(*full).size() == MaxTilemapColliderBoxes);
			Ref<TilemapData> over = CreateRef<TilemapData>(*full);
			TilemapChunk extra{ .X = FullChunks, .Y = 0, .Tiles = std::vector<uint32_t>(static_cast<size_t>(Side) * Side, 0) };
			extra.Tiles.front() = 1;
			over->Chunks.push_back(std::move(extra));
			REQUIRE(MergeTilemapBoxes(*over).size() == MaxTilemapColliderBoxes + 1);
			const AssetHandle fullHandle = UUID(8973);
			const AssetHandle overHandle = UUID(8974);
			const AssetHandle sheetHandle = UUID(8975);
			assets.Publish(fullHandle, full, "Assets/Tilemaps/Full.tilemap");
			assets.Publish(overHandle, over, "Assets/Tilemaps/Over.tilemap");
			assets.Publish(sheetHandle, CreateRef<SpriteSheetData>(), "Assets/Sprites/Tiles.spritesheet");

			Entity atLimit = CreatePlanarEntity(scene, "AtLimit", glm::vec2(0.0f));
			atLimit.AddComponent<TilemapColliderComponent>(TilemapColliderComponent{ .Tilemap = TypedAssetHandle<AssetType::Tilemap>(fullHandle) });
			Entity beyond = CreatePlanarEntity(scene, "Beyond", glm::vec2(0.0f, -100.0f));
			beyond.AddComponent<TilemapColliderComponent>(TilemapColliderComponent{ .Tilemap = TypedAssetHandle<AssetType::Tilemap>(overHandle) });
			Entity sheet = CreatePlanarEntity(scene, "Sheet", glm::vec2(0.0f, -200.0f));
			sheet.AddComponent<TilemapColliderComponent>(TilemapColliderComponent{ .Tilemap = TypedAssetHandle<AssetType::Tilemap>(sheetHandle) });
			TransformSystem::Update(scene);

			const std::vector<PhysicsDiagnostic> validated = ValidateScenePhysics2D(scene, MakeDefaultLayers(), &assets);
			CheckReported(validated, PhysicsInvalidShapeCode, beyond.GetUUID(), DiagnosticSeverity::Error, "TilemapCollider", "Tilemap");
			CheckReported(validated, PhysicsInvalidShapeCode, sheet.GetUUID(), DiagnosticSeverity::Error, "TilemapCollider", "Tilemap");
			CHECK(std::ranges::none_of(validated, [&atLimit](const PhysicsDiagnostic& diagnostic)
			{
				return diagnostic.Entity == atLimit.GetUUID();
			}));

			const Test::ExpectLog shapeLogged(LogLevel::Error, PhysicsInvalidShapeCode);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			const std::span<const PhysicsDiagnostic> raised = (*system)->GetDiagnostics();
			CheckReported(raised, PhysicsInvalidShapeCode, beyond.GetUUID(), DiagnosticSeverity::Error, "TilemapCollider", "Tilemap");
			CheckReported(raised, PhysicsInvalidShapeCode, sheet.GetUUID(), DiagnosticSeverity::Error, "TilemapCollider", "Tilemap");
			// Only the map at the limit makes a body.
			CHECK((*system)->GetStats().BodyCount == 1);
		}
	}

}
