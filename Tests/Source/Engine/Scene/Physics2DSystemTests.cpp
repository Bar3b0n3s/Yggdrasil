#include "TestsPCH.h"

#include "Engine/Scene/Physics2DSystem.h"

#include "Engine/Asset/TilemapData.h"
#include "Engine/Core/Hash.h"
#include "Engine/Physics/Physics2DWorld.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Scene/Components/BoxCollider2DComponent.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CircleCollider2DComponent.h"
#include "Engine/Scene/Components/Light2DComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TilemapColliderComponent.h"
#include "Engine/Scene/Components/TilemapRendererComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Physics2DValidation.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/ExpectLog.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <map>
#include <optional>
#include <span>
#include <utility>
#include <vector>

// The play session's 2D physics (ADR 0028 decision 6; Docs/Decisions/0032-m21-contract.md decisions 9 to 12, stream P2;
// Roadmap M21 "2D contact events reach scripts in canonical order", "An entity holding both 2D and 3D physics components is
// a validation error").

namespace Engine {

	namespace {

		constexpr double FixedDelta2DSystem = 1.0 / 60.0;

		class Recording2DListener final : public IPhysics2DEventListener
		{
		public:
			void OnPhysics2DEvent(const PhysicsEvent& event) override { Events.push_back(event); }

			std::vector<PhysicsEvent> Events{};
		};

		Entity Create2DEntity(Scene& scene, std::string_view name, const glm::vec2& position)
		{
			Entity entity = scene.CreateEntity(name);
			entity.Patch<TransformComponent>([position](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(position, 0.0f);
			});
			return entity;
		}

		void StepBoth(Physics2DSystem& system, uint64_t tick)
		{
			const SimStep step = SimStep::FromTick(tick, FixedDelta2DSystem);
			system.PreStep(step);
			system.Step(step);
			system.PostStep(step);
		}

		// The transform update, then one step: what the play session runs around the 2D phases.
		void UpdateAndStep(Scene& scene, Physics2DSystem& system, uint64_t tick)
		{
			TransformSystem::Update(scene);
			StepBoth(system, tick);
		}

		// The events of `listener` that `self` received of `type`.
		std::vector<PhysicsEvent> FindEvents(const Recording2DListener& listener, UUID self, PhysicsEventType type)
		{
			std::vector<PhysicsEvent> found;
			std::ranges::copy_if(listener.Events, std::back_inserter(found), [self, type](const PhysicsEvent& event)
			{
				return event.Self == self && event.Type == type;
			});
			return found;
		}

		Entity Create2DBox(Scene& scene, std::string_view name, const glm::vec2& position, const glm::vec2& halfExtents, std::optional<BodyType> type)
		{
			Entity entity = Create2DEntity(scene, name, position);
			if (type.has_value())
				entity.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = *type });
			entity.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = halfExtents });
			return entity;
		}

		// Authored angles in (0, 3) rad whose planar angle the round trip of a write-back changes by an ulp: the body is
		// created at the twist of the owner's normalized rotation, laid as a rotation about Z (ToPhysicsPose) and read back
		// (ToPhysics2DPose); its write-back writes that rotation about Z, whose normalized twist, as PreStep computes a
		// target, is not the body's angle. These made a Transform write of the world Z alone teleport and wake a resting
		// Dynamic body (W21.1's review). The first `count` of a fixed sweep.
		std::vector<float> FindRoundTripAngles(size_t count)
		{
			const auto planar = [](const glm::quat& rotation)
			{
				return ToPhysics2DPose(PhysicsPose{ .Position = glm::vec3(0.0f), .Rotation = rotation }).Angle;
			};
			std::vector<float> angles;
			for (int step = 1; step < 3000 && angles.size() < count; ++step)
			{
				const float authored = 0.001f * static_cast<float>(step);
				const float created = planar(glm::normalize(ToPhysicsPose(Physics2DPose{ .Angle = authored }).Rotation));
				const float body = planar(ToPhysicsPose(Physics2DPose{ .Angle = created }).Rotation);
				const float written = planar(glm::normalize(ToPhysicsPose(Physics2DPose{ .Angle = body }).Rotation));
				if (std::bit_cast<uint32_t>(written) != std::bit_cast<uint32_t>(body))
					angles.push_back(authored);
			}
			return angles;
		}

		// Writes `z` as the entity's local Translation.z, nothing else.
		void WriteDepth(Entity entity, float z)
		{
			entity.Patch<TransformComponent>([z](TransformComponent& transform)
			{
				transform.Translation.z = z;
			});
		}

		void AppendHashFloat(std::vector<std::byte>& bytes, float value)
		{
			const auto bits = std::bit_cast<uint32_t>(value);
			for (uint32_t shift = 0; shift < 32; shift += 8)
				bytes.push_back(static_cast<std::byte>((bits >> shift) & 0xffu));
		}

		// A tilemap of `chunkSize`-cell chunks of `cellSize` cells holding `cells` (cell, value), its chunks built in canonical
		// order (sorted by (Y, X)), as tilemap.paint would leave them.
		Ref<TilemapData> MakeTestTilemap(const std::vector<std::pair<glm::ivec2, uint32_t>>& cells, uint32_t chunkSize, const glm::vec2& cellSize)
		{
			Ref<TilemapData> tilemap = CreateRef<TilemapData>();
			tilemap->CellSize = cellSize;
			tilemap->ChunkSize = chunkSize;
			tilemap->Palette = { TilemapTile{ .Solid = true }, TilemapTile{ .Solid = false } };
			const auto size = static_cast<int32_t>(chunkSize);
			std::map<std::pair<int32_t, int32_t>, TilemapChunk> chunks;
			for (const auto& [cell, tile] : cells)
			{
				const glm::ivec2 chunk = GetTilemapChunkCoordinate(cell, chunkSize);
				auto [found, created] = chunks.try_emplace(std::make_pair(chunk.y, chunk.x),
					TilemapChunk{ .X = chunk.x, .Y = chunk.y, .Tiles = std::vector<uint32_t>(static_cast<size_t>(chunkSize) * chunkSize, 0) });
				const glm::ivec2 local = cell - chunk * size;
				found->second.Tiles[static_cast<size_t>(local.y * size + local.x)] = tile;
			}
			for (auto& [key, chunk] : chunks)
				tilemap->Chunks.push_back(std::move(chunk));
			return tilemap;
		}

		// One body's entry of the physics2d hash block (Physics2DSystem::AppendStateHash) for a body at rest at `position`.
		void AppendRestingHashEntry(std::vector<std::byte>& bytes, UUID owner, const glm::vec2& position, bool sleeping)
		{
			for (uint32_t shift = 0; shift < 64; shift += 8)
				bytes.push_back(static_cast<std::byte>((owner.GetValue() >> shift) & 0xffu));
			AppendHashFloat(bytes, position.x);
			AppendHashFloat(bytes, position.y);
			AppendHashFloat(bytes, ToPhysics2DPose(PhysicsPose{ .Position = glm::vec3(position, 0.0f) }).Angle);
			for (int velocity = 0; velocity < 3; ++velocity)
				AppendHashFloat(bytes, 0.0f);
			bytes.push_back(static_cast<std::byte>(sleeping ? 1 : 0));
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("Physics2DSystem: contact events reach the listener sorted by the event order key, in the plane")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity floor = Create2DEntity(scene, "Floor", glm::vec2(0.0f, -0.5f));
			floor.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = BodyType::Static });
			floor.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(10.0f, 0.5f) });
			for (int index = 0; index < 6; ++index)
			{
				Entity crate = Create2DEntity(scene, "Crate", glm::vec2(static_cast<float>(index) - 2.5f, 0.6f + 0.2f * static_cast<float>(index)));
				crate.AddComponent<RigidBody2DComponent>();
				if (index % 2 == 0)
					crate.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(0.4f) });
				else
					crate.AddComponent<CircleCollider2DComponent>(CircleCollider2DComponent{ .Radius = 0.4f });
			}
			TransformSystem::Update(scene);

			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			Recording2DListener listener;
			(*system)->SetEventListener(&listener);
			for (uint64_t tick = 0; tick < 120; ++tick)
				StepBoth(**system, tick);
			(*system)->SetEventListener(nullptr);

			REQUIRE(listener.Events.size() >= 6);
			for (const PhysicsEvent& event : listener.Events)
			{
				CHECK(event.Contact.Point.z == 0.0f);
				CHECK(event.Contact.Normal.z == 0.0f);
			}
			// Within each PostStep (one tick), the events follow M20's canonical order key.
			for (size_t index = 1; index < listener.Events.size(); ++index)
			{
				if (listener.Events[index - 1].Tick == listener.Events[index].Tick)
					CHECK(GetPhysicsEventOrderKey(listener.Events[index - 1]) <= GetPhysicsEventOrderKey(listener.Events[index]));
			}
			// Every crate ends at rest on the floor, on the plane.
			for (const UUID id : scene.GetCanonicalOrder())
			{
				const ConstEntity entity = scene.FindEntityByID(id);
				CHECK(entity.GetComponent<TransformComponent>().Translation.z == 0.0f);
			}
		}

		TEST_CASE("Physics2DSystem: an entity holding 2D and 3D physics components keeps its 3D body only")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity mixed = Create2DEntity(scene, "Mixed", glm::vec2(0.0f, 2.0f));
			mixed.AddComponent<RigidBodyComponent>();
			mixed.AddComponent<BoxColliderComponent>();
			mixed.AddComponent<RigidBody2DComponent>();
			mixed.AddComponent<BoxCollider2DComponent>();
			TransformSystem::Update(scene);

			Result<Scope<PhysicsSystem>> physics = PhysicsSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(physics.has_value(), physics.error().ToString());
			// The system logs each diagnostic it raises once, an Error at Error.
			Test::ExpectLog mixedLogged(LogLevel::Error, "PHYSICS2D_MIXED_WORLDS");
			Result<Scope<Physics2DSystem>> physics2D = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(physics2D.has_value(), physics2D.error().ToString());
			CHECK((*physics)->GetBodyInfo(mixed.GetUUID()).has_value());
			CHECK_FALSE((*physics2D)->GetBodyBounds(mixed.GetUUID()).has_value());
			CHECK((*physics2D)->GetStats().BodyCount == 0);
			const std::span<const PhysicsDiagnostic> diagnostics = (*physics2D)->GetDiagnostics();
			REQUIRE(diagnostics.size() == 1);
			CHECK(diagnostics[0].Code == Physics2DMixedWorldsCode);
			CHECK(diagnostics[0].Severity == DiagnosticSeverity::Error);
			CHECK(diagnostics[0].Entity == mixed.GetUUID());
		}

		TEST_CASE("Physics2DSystem: the state hash block is empty without 2D bodies and covers them otherwise")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity box3D = Create2DEntity(scene, "Box3D", glm::vec2(0.0f));
			box3D.AddComponent<RigidBodyComponent>();
			box3D.AddComponent<BoxColliderComponent>();
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> empty = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(empty.has_value(), empty.error().ToString());
			XXH64Hasher untouched(0);
			XXH64Hasher appended(0);
			(*empty)->AppendStateHash(appended);
			CHECK(appended.Digest() == untouched.Digest());

			Entity ball = Create2DEntity(scene, "Ball", glm::vec2(0.0f, 3.0f));
			ball.AddComponent<RigidBody2DComponent>();
			ball.AddComponent<CircleCollider2DComponent>();
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> withBall = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE(withBall.has_value());
			XXH64Hasher covered(0);
			(*withBall)->AppendStateHash(covered);
			CHECK(covered.Digest() != untouched.Digest());
		}

		TEST_CASE("Physics2DSystem: the 2D world is created at the first 2D body, so a scene without one holds no second world")
		{
			// Every 3D game's session keeps its one PhysicsWorld ("PhysicsSystem: destroying the session destroys every body and
			// the world"): no second Jolt world, temp allocator or caches until a 2D body exists.
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity box3D = Create2DEntity(scene, "Box3D", glm::vec2(0.0f));
			box3D.AddComponent<RigidBodyComponent>();
			box3D.AddComponent<BoxColliderComponent>();
			TransformSystem::Update(scene);
			const uint32_t worldsBefore = PhysicsEngine::GetLiveWorldCount();
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene, .Gravity = glm::vec2(0.0f, -2.0f) });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			StepBoth(**system, 0);
			CHECK(PhysicsEngine::GetLiveWorldCount() == worldsBefore);
			// An empty world's answers.
			CHECK((*system)->GetStats().BodyCount == 0);
			const Result<std::optional<Physics2DRaycastHit>> miss = (*system)->Raycast(glm::vec2(-5.0f, 0.0f), glm::vec2(1.0f, 0.0f), 10.0f);
			REQUIRE(miss.has_value());
			CHECK_FALSE(miss->has_value());
			CHECK((*system)->GetGravity() == glm::vec2(0.0f, -2.0f));
			REQUIRE((*system)->SetGravity(glm::vec2(1.0f, -3.0f)).has_value());

			// The first 2D body creates the world, which takes the gravity set before.
			Entity ball = Create2DEntity(scene, "Ball", glm::vec2(4.0f, 3.0f));
			ball.AddComponent<RigidBody2DComponent>();
			ball.AddComponent<CircleCollider2DComponent>();
			TransformSystem::Update(scene);
			StepBoth(**system, 1);
			CHECK(PhysicsEngine::GetLiveWorldCount() == worldsBefore + 1);
			CHECK((*system)->GetStats().BodyCount == 1);
			CHECK((*system)->GetGravity() == glm::vec2(1.0f, -3.0f));
			system->reset();
			CHECK(PhysicsEngine::GetLiveWorldCount() == worldsBefore);
		}

		TEST_CASE("Physics2DSystem: a dynamic body writes back its plane position and its rotation about Z, keeping its world Z")
		{
			// Decision 9: the body takes the twist about Z of its owner's world rotation (PHYSICS2D_OFF_PLANE warns of the
			// swing), and the write-back keeps the entity's world Z (draw depth) and replaces the swing by none.
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity spinner = Create2DEntity(scene, "Spinner", glm::vec2(0.0f, 5.0f));
			spinner.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.z = 3.0f;
				transform.Rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(1.0f, 0.0f, 0.0f));
			});
			spinner.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .InitialAngularVelocity = 2.0f });
			spinner.AddComponent<BoxCollider2DComponent>();
			TransformSystem::Update(scene);

			const Test::ExpectLog offPlane(LogLevel::Warn, "PHYSICS2D_OFF_PLANE");
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			const std::span<const PhysicsDiagnostic> diagnostics = (*system)->GetDiagnostics();
			REQUIRE(diagnostics.size() == 1);
			CHECK(diagnostics[0].Code == Physics2DOffPlaneCode);
			CHECK(diagnostics[0].Severity == DiagnosticSeverity::Warning);
			for (uint64_t tick = 0; tick < 10; ++tick)
			{
				TransformSystem::Update(scene);
				StepBoth(**system, tick);
			}

			const TransformComponent& transform = spinner.GetComponent<TransformComponent>();
			CHECK(transform.Translation.z == 3.0f);
			CHECK(transform.Translation.y < 5.0f);
			CHECK(transform.Rotation.x == 0.0f);
			CHECK(transform.Rotation.y == 0.0f);
			CHECK(transform.Rotation.z > 0.0f);
		}

		TEST_CASE("Physics2DSystem: kinematic targets and teleports move bodies in the plane, and body functions refuse the wrong body")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity platform = Create2DEntity(scene, "Platform", glm::vec2(0.0f));
			platform.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = BodyType::Kinematic });
			platform.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(1.0f, 0.25f) });
			Entity crate = Create2DEntity(scene, "Crate", glm::vec2(-6.0f, 0.0f));
			crate.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.z = -2.0f;
			});
			crate.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .GravityFactor = 0.0f });
			crate.AddComponent<CircleCollider2DComponent>();
			Entity plain = Create2DEntity(scene, "Plain", glm::vec2(9.0f, 0.0f));
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene, .Gravity = glm::vec2(0.0f) });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());

			// A kinematic target is the entity's pose; the next steps carry the body there.
			REQUIRE((*system)->MoveKinematic(platform.GetUUID(), glm::vec2(2.0f, 1.0f), 90.0f).has_value());
			CHECK(platform.GetComponent<TransformComponent>().Translation == glm::vec3(2.0f, 1.0f, 0.0f));
			for (uint64_t tick = 0; tick < 2; ++tick)
			{
				TransformSystem::Update(scene);
				StepBoth(**system, tick);
			}
			const std::optional<Aabb> platformBounds = (*system)->GetBodyBounds(platform.GetUUID());
			REQUIRE(platformBounds.has_value());
			// Turned by 90 degrees about Z, the 2 x 0.5 platform stands 2 m tall around (2, 1).
			CHECK(platformBounds->GetCenter().x == doctest::Approx(2.0f).epsilon(0.001));
			CHECK(platformBounds->GetCenter().y == doctest::Approx(1.0f).epsilon(0.001));
			CHECK(platformBounds->Max.y - platformBounds->Min.y > platformBounds->Max.x - platformBounds->Min.x);

			// A teleport places the body at once, keeps the entity's Z and its velocity.
			REQUIRE((*system)->SetLinearVelocity(crate.GetUUID(), glm::vec2(1.0f, 0.0f)).has_value());
			REQUIRE((*system)->Teleport(crate.GetUUID(), glm::vec2(5.0f, 5.0f), std::nullopt).has_value());
			CHECK(crate.GetComponent<TransformComponent>().Translation == glm::vec3(5.0f, 5.0f, -2.0f));
			const std::optional<Aabb> crateBounds = (*system)->GetBodyBounds(crate.GetUUID());
			REQUIRE(crateBounds.has_value());
			CHECK(crateBounds->GetCenter().x == doctest::Approx(5.0f));
			CHECK(crateBounds->GetCenter().y == doctest::Approx(5.0f));
			const Result<glm::vec2> velocity = (*system)->GetLinearVelocity(crate.GetUUID());
			REQUIRE(velocity.has_value());
			CHECK(*velocity == glm::vec2(1.0f, 0.0f));

			// The wrong body: none (NotFound), or a motion type the operation does not act on (InvalidState).
			const Status noBody = (*system)->AddForce(plain.GetUUID(), glm::vec2(1.0f));
			REQUIRE_FALSE(noBody.has_value());
			CHECK(noBody.error().GetCode() == ErrorCode::NotFound);
			const Status kinematicForce = (*system)->AddForce(platform.GetUUID(), glm::vec2(1.0f));
			REQUIRE_FALSE(kinematicForce.has_value());
			CHECK(kinematicForce.error().GetCode() == ErrorCode::InvalidState);
			const Status dynamicTarget = (*system)->MoveKinematic(crate.GetUUID(), glm::vec2(0.0f), 0.0f);
			REQUIRE_FALSE(dynamicTarget.has_value());
			CHECK(dynamicTarget.error().GetCode() == ErrorCode::InvalidState);
			CHECK_FALSE((*system)->Raycast(glm::vec2(0.0f), glm::vec2(0.0f), 1.0f).has_value());
		}

		TEST_CASE("Physics2DSystem: destroying an entity ends its 2D pairs with synthesized exits")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity floor = Create2DEntity(scene, "Floor", glm::vec2(0.0f, -0.5f));
			floor.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(5.0f, 0.5f) });
			Entity ball = Create2DEntity(scene, "Ball", glm::vec2(0.0f, 1.0f));
			ball.AddComponent<RigidBody2DComponent>();
			ball.AddComponent<CircleCollider2DComponent>(CircleCollider2DComponent{ .Radius = 0.25f });
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			Recording2DListener listener;
			(*system)->SetEventListener(&listener);
			uint64_t tick = 0;
			for (; tick < 60 && listener.Events.empty(); ++tick)
			{
				TransformSystem::Update(scene);
				StepBoth(**system, tick);
			}
			REQUIRE(listener.Events.size() == 2);
			CHECK(listener.Events[0].Type == PhysicsEventType::CollisionEnter);
			CHECK((*system)->GetStats().ContactPairCount == 1);

			listener.Events.clear();
			scene.DestroyEntity(ball);
			(*system)->FlushDestroyed(tick);
			(*system)->SetEventListener(nullptr);
			// The surviving side gets its exit; the destroyed one gets none, and its body is gone.
			REQUIRE(listener.Events.size() == 1);
			CHECK(listener.Events[0].Type == PhysicsEventType::CollisionExit);
			CHECK(listener.Events[0].Self == floor.GetUUID());
			CHECK(listener.Events[0].Synthesized);
			CHECK((*system)->GetStats().BodyCount == 1);
			CHECK((*system)->GetStats().ContactPairCount == 0);
		}

		// W21.1's review (decision 9, Architecture §9.9): the world Z is free for draw depth, so a Transform write of the
		// world Z alone moves and wakes no 2D body, whatever its motion type and angle. Each body is compared with its owner's
		// planar pose at its last placement or write-back, never with its own pose, whose angle the round trip through the
		// quaternion may move by an ulp (FindRoundTripAngles' angles).
		TEST_CASE("Physics2DSystem: a Transform write of the world Z alone moves and wakes no 2D body, at any angle")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			const std::vector<float> angles = FindRoundTripAngles(4);
			REQUIRE(angles.size() == 4);
			CAPTURE(angles);

			SUBCASE("Dynamic bodies at rest at angles the round trip changes")
			{
				static_cast<void>(Create2DBox(scene, "Floor", glm::vec2(0.0f, -0.5f), glm::vec2(20.0f, 0.5f), BodyType::Static));
				std::vector<Entity> resting;
				for (size_t index = 0; index < angles.size(); ++index)
				{
					// Circles that cannot turn rest on the floor at their authored angle.
					Entity disc = Create2DEntity(scene, "Disc", glm::vec2(static_cast<float>(index) * 2.0f - 3.0f, 0.5f));
					const float angle = angles[index];
					disc.Patch<TransformComponent>([angle](TransformComponent& transform)
					{
						transform.Rotation = ToPhysicsPose(Physics2DPose{ .Angle = angle }).Rotation;
					});
					disc.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .FixedRotation = true });
					disc.AddComponent<CircleCollider2DComponent>(CircleCollider2DComponent{ .Radius = 0.5f });
					resting.push_back(disc);
				}
				TransformSystem::Update(scene);
				Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
				REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
				uint64_t tick = 0;
				for (; tick < 240; ++tick)
					UpdateAndStep(scene, **system, tick);
				std::vector<Aabb> bounds;
				for (const Entity disc : resting)
				{
					REQUIRE((*system)->IsSleeping(disc.GetUUID()).value_or(false));
					bounds.push_back((*system)->GetBodyBounds(disc.GetUUID()).value_or(Aabb{}));
				}
				for (int write = 1; write <= 10; ++write, ++tick)
				{
					for (const Entity disc : resting)
						WriteDepth(disc, 0.25f * static_cast<float>(write));
					UpdateAndStep(scene, **system, tick);
					for (size_t index = 0; index < resting.size(); ++index)
					{
						CAPTURE(index);
						CHECK((*system)->IsSleeping(resting[index].GetUUID()).value_or(false));
						CHECK((*system)->GetBodyBounds(resting[index].GetUUID()) == bounds[index]);
					}
				}
			}

			SUBCASE("A Static body under resting bodies")
			{
				Entity floor = Create2DBox(scene, "Floor", glm::vec2(0.0f, -0.5f), glm::vec2(20.0f, 0.5f), BodyType::Static);
				Entity disc = Create2DEntity(scene, "Disc", glm::vec2(0.0f, 0.5f));
				disc.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .FixedRotation = true });
				disc.AddComponent<CircleCollider2DComponent>(CircleCollider2DComponent{ .Radius = 0.5f });
				TransformSystem::Update(scene);
				Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
				REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
				Recording2DListener listener;
				(*system)->SetEventListener(&listener);
				uint64_t tick = 0;
				for (; tick < 240; ++tick)
					UpdateAndStep(scene, **system, tick);
				REQUIRE((*system)->IsSleeping(disc.GetUUID()).value_or(false));
				const std::optional<Aabb> floorBounds = (*system)->GetBodyBounds(floor.GetUUID());
				listener.Events.clear();
				for (int write = 1; write <= 10; ++write, ++tick)
				{
					WriteDepth(floor, -0.5f * static_cast<float>(write));
					UpdateAndStep(scene, **system, tick);
					CHECK((*system)->IsSleeping(disc.GetUUID()).value_or(false));
					CHECK((*system)->GetBodyBounds(floor.GetUUID()) == floorBounds);
				}
				CHECK(listener.Events.empty());
				(*system)->SetEventListener(nullptr);
			}

			SUBCASE("Kinematic bodies at rest, written outside the steps")
			{
				std::vector<Entity> platforms;
				for (size_t index = 0; index < angles.size(); ++index)
				{
					Entity platform = Create2DBox(scene, "Platform", glm::vec2(static_cast<float>(index) * 4.0f, 0.0f), glm::vec2(1.0f, 0.25f), BodyType::Kinematic);
					const float angle = angles[index];
					platform.Patch<TransformComponent>([angle](TransformComponent& transform)
					{
						transform.Rotation = ToPhysicsPose(Physics2DPose{ .Angle = angle }).Rotation;
					});
					platforms.push_back(platform);
				}
				TransformSystem::Update(scene);
				Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene, .Gravity = glm::vec2(0.0f) });
				REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
				uint64_t tick = 0;
				for (; tick < 240; ++tick)
					UpdateAndStep(scene, **system, tick);
				std::vector<Aabb> bounds;
				for (const Entity platform : platforms)
				{
					REQUIRE((*system)->IsSleeping(platform.GetUUID()).value_or(false));
					bounds.push_back((*system)->GetBodyBounds(platform.GetUUID()).value_or(Aabb{}));
				}
				for (int write = 1; write <= 10; ++write, ++tick)
				{
					// A write outside the steps (a frame-phase script, the editor) tags the entity as the play session does.
					for (const Entity platform : platforms)
					{
						WriteDepth(platform, 0.5f * static_cast<float>(write));
						scene.GetRegistry().emplace_or_replace<InterpolationResetTag>(platform.GetHandle());
					}
					UpdateAndStep(scene, **system, tick);
					for (const Entity platform : platforms)
						scene.GetRegistry().remove<InterpolationResetTag>(platform.GetHandle());
					for (size_t index = 0; index < platforms.size(); ++index)
					{
						CAPTURE(index);
						CHECK((*system)->IsSleeping(platforms[index].GetUUID()).value_or(false));
						CHECK((*system)->GetBodyBounds(platforms[index].GetUUID()) == bounds[index]);
					}
				}
			}
		}

		// W21.1's review (decision 10: a 3D game's session costs as before M21): without a world and a 2D physics component,
		// PreStep composes nothing, whatever entities are enabled or disabled; the first 2D body composes once.
		TEST_CASE("Physics2DSystem: a scene without 2D physics components composes nothing when entities are enabled or disabled")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity box3D = Create2DEntity(scene, "Box3D", glm::vec2(0.0f));
			box3D.AddComponent<RigidBodyComponent>();
			box3D.AddComponent<BoxColliderComponent>();
			Entity marker = Create2DEntity(scene, "Marker", glm::vec2(3.0f, 0.0f));
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			CHECK((*system)->GetStats().Compositions == 0);
			uint64_t tick = 0;
			for (const bool active : { false, true, false, true })
			{
				marker.SetActive(active);
				box3D.SetActive(active);
				UpdateAndStep(scene, **system, tick++);
				CHECK((*system)->GetStats().Compositions == 0);
			}

			Entity ball = Create2DEntity(scene, "Ball", glm::vec2(0.0f, 4.0f));
			ball.AddComponent<RigidBody2DComponent>();
			ball.AddComponent<CircleCollider2DComponent>();
			UpdateAndStep(scene, **system, tick++);
			CHECK((*system)->GetStats().Compositions == 1);
			CHECK((*system)->GetStats().BodyCount == 1);
		}

		// C21.2 (Docs/Decisions/0032-m21-contract.md decision 29): a 2D light and a tilemap renderer are rendering only, so a
		// scene that holds them and no 2D physics component composes nothing, creates no 2D world and appends no hash block,
		// whatever their tilemap holds. Live from the contract on, so it guards streams L, T1 and T2: only a TilemapCollider
		// makes a body from a tilemap.
		TEST_CASE("Physics2DSystem: lights and tilemap renderers make no 2D body, world or hash block")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity light = Create2DEntity(scene, "Torch", glm::vec2(1.0f, 2.0f));
			light.AddComponent<Light2DComponent>();
			Entity tiles = Create2DEntity(scene, "Ground", glm::vec2(0.0f));
			tiles.AddComponent<TilemapRendererComponent>(TilemapRendererComponent{ .Tilemap = TypedAssetHandle<AssetType::Tilemap>(AssetHandle(0x7a11ull)) });
			TransformSystem::Update(scene);
			const uint32_t worldsBefore = PhysicsEngine::GetLiveWorldCount();
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			uint64_t tick = 0;
			for (const bool active : { true, false, true })
			{
				tiles.SetActive(active);
				light.Patch<Light2DComponent>([](Light2DComponent& component)
				{
					component.Intensity += 1.0f;
				});
				UpdateAndStep(scene, **system, tick++);
			}
			CHECK((*system)->GetStats().Compositions == 0);
			CHECK((*system)->GetStats().BodyCount == 0);
			CHECK(PhysicsEngine::GetLiveWorldCount() == worldsBefore);
			XXH64Hasher untouched(0);
			XXH64Hasher appended(0);
			(*system)->AppendStateHash(appended);
			CHECK(appended.Digest() == untouched.Digest());
			CHECK((*system)->GetDiagnostics().empty());
		}

		// W21.1's review: Create refuses what the 2D world's creation would, so the world made at the first 2D body cannot fail
		// (ValidatePhysicsWorldLimits, PhysicsWorld::Create's checks).
		TEST_CASE("Physics2DSystem: Create refuses limits the 2D world would refuse")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			for (const PhysicsWorldLimits limits : {
					 PhysicsWorldLimits{ .MaxBodies = 0 },
					 PhysicsWorldLimits{ .MaxBodies = 8388609 },
					 PhysicsWorldLimits{ .MaxBodyPairs = 0 },
					 PhysicsWorldLimits{ .TempAllocatorBytes = size_t{ 1 } << 33 },
				 })
			{
				CAPTURE(limits.MaxBodies);
				const Result<Scope<Physics2DSystem>> refused = Physics2DSystem::Create({ .RuntimeScene = &scene, .Limits = limits });
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
			}
			const Result<Scope<Physics2DSystem>> largest = Physics2DSystem::Create({ .RuntimeScene = &scene, .Limits = PhysicsWorldLimits{ .MaxBodies = 8388608 } });
			CHECK(largest.has_value());
		}

		// Decision 9, PhysicsSystem's rule in the plane ("Physics: compound contact names the child collider"): a RigidBody2D
		// gathers its collider-only descendants into one body, whose contacts name the child collider that was hit.
		TEST_CASE("Physics2DSystem: a RigidBody2D gathers its collider-only children into one body, whose contacts name the child")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity level = Create2DEntity(scene, "Level", glm::vec2(0.0f));
			level.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = BodyType::Static });
			std::vector<Entity> pieces;
			for (int piece = 0; piece < 3; ++piece)
			{
				Entity child = Create2DBox(scene, "Piece", glm::vec2(static_cast<float>(piece), 0.0f), glm::vec2(0.5f), std::nullopt);
				REQUIRE(scene.SetParent(child, level).has_value());
				pieces.push_back(child);
			}
			Entity ball = Create2DEntity(scene, "Ball", glm::vec2(2.0f, 3.0f));
			ball.AddComponent<RigidBody2DComponent>();
			ball.AddComponent<CircleCollider2DComponent>(CircleCollider2DComponent{ .Radius = 0.25f });
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			Recording2DListener listener;
			(*system)->SetEventListener(&listener);
			for (uint64_t tick = 0; tick < 90; ++tick)
				UpdateAndStep(scene, **system, tick);
			(*system)->SetEventListener(nullptr);

			// One compound for the three pieces, and the ball's body.
			CHECK((*system)->GetStats().BodyCount == 2);
			CHECK((*system)->GetBodyBounds(level.GetUUID()).has_value());
			CHECK_FALSE((*system)->GetBodyBounds(pieces[2].GetUUID()).has_value());
			CHECK((*system)->GetColliderBounds(pieces[2].GetUUID()).has_value());
			const std::vector<PhysicsEvent> ballEnter = FindEvents(listener, ball.GetUUID(), PhysicsEventType::CollisionEnter);
			REQUIRE(ballEnter.size() == 1);
			// `other` is the body owner; the contact names the piece that was hit.
			CHECK(ballEnter[0].Other == level.GetUUID());
			CHECK(ballEnter[0].Contact.Collider == ball.GetUUID());
			CHECK(ballEnter[0].Contact.OtherCollider == pieces[2].GetUUID());
			CHECK(ballEnter[0].Contact.Normal.y < -0.9f);
			const std::vector<PhysicsEvent> levelEnter = FindEvents(listener, level.GetUUID(), PhysicsEventType::CollisionEnter);
			REQUIRE(levelEnter.size() == 1);
			CHECK(levelEnter[0].Contact.Collider == pieces[2].GetUUID());
			CHECK(levelEnter[0].Contact.OtherCollider == ball.GetUUID());
			CHECK(levelEnter[0].Contact.Normal.y > 0.9f);
		}

		// Decisions 9 and 11: an entity with solid and trigger 2D colliders and no RigidBody2D owns an implicit static body and
		// an implicit sensor body, each with its own pairs, and the physics2d hash block lists the solid one first.
		TEST_CASE("Physics2DSystem: solid and trigger 2D colliders of one entity make a static and a sensor body, hashed solid first")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity gate = Create2DBox(scene, "Gate", glm::vec2(0.0f), glm::vec2(0.5f), std::nullopt);
			gate.AddComponent<CircleCollider2DComponent>(CircleCollider2DComponent{ .Radius = 1.5f, .IsTrigger = true });
			TransformSystem::Update(scene);
			{
				Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
				REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
				CHECK((*system)->GetStats().BodyCount == 2);
				// The static body never wakes; the sensor body is kept active (§9.2 "Triggers").
				std::vector<std::byte> solidFirst;
				AppendRestingHashEntry(solidFirst, gate.GetUUID(), glm::vec2(0.0f), true);
				AppendRestingHashEntry(solidFirst, gate.GetUUID(), glm::vec2(0.0f), false);
				std::vector<std::byte> sensorFirst;
				AppendRestingHashEntry(sensorFirst, gate.GetUUID(), glm::vec2(0.0f), false);
				AppendRestingHashEntry(sensorFirst, gate.GetUUID(), glm::vec2(0.0f), true);
				XXH64Hasher block(0);
				(*system)->AppendStateHash(block);
				XXH64Hasher expected(0);
				expected.Update(solidFirst);
				XXH64Hasher reversed(0);
				reversed.Update(sensorFirst);
				CHECK(block.Digest() == expected.Digest());
				CHECK(block.Digest() != reversed.Digest());
			}

			Entity ball = Create2DEntity(scene, "Ball", glm::vec2(0.0f, 4.0f));
			ball.AddComponent<RigidBody2DComponent>();
			ball.AddComponent<CircleCollider2DComponent>(CircleCollider2DComponent{ .Radius = 0.25f });
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			Recording2DListener listener;
			(*system)->SetEventListener(&listener);
			for (uint64_t tick = 0; tick < 90; ++tick)
				UpdateAndStep(scene, **system, tick);
			(*system)->SetEventListener(nullptr);
			// The ball enters the sensor's circle, then lands on the solid box: one pair of each kind, both owned by the gate.
			const std::vector<PhysicsEvent> triggered = FindEvents(listener, gate.GetUUID(), PhysicsEventType::TriggerEnter);
			REQUIRE(triggered.size() == 1);
			CHECK(triggered[0].Other == ball.GetUUID());
			const std::vector<PhysicsEvent> landed = FindEvents(listener, gate.GetUUID(), PhysicsEventType::CollisionEnter);
			REQUIRE(landed.size() == 1);
			CHECK(landed[0].Other == ball.GetUUID());
			CHECK(FindEvents(listener, ball.GetUUID(), PhysicsEventType::TriggerEnter).size() == 1);
			CHECK(FindEvents(listener, ball.GetUUID(), PhysicsEventType::CollisionEnter).size() == 1);
			CHECK((*system)->GetStats().BodyCount == 3);
		}

		// PhysicsSystem's rule in the plane ("PhysicsSystem: a rebuild keeps the body's pairs and velocity, and a removal ends
		// its pairs with synthesized exits"): pairs are keyed by owner, so a collider change (a shape replaced in place) or a
		// RigidBody2D change (the body created again) neither ends nor restarts a touching pair.
		TEST_CASE("Physics2DSystem: a rebuild keeps a touching body's pair and velocity, and a removal ends its pairs with synthesized exits")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity ground = Create2DBox(scene, "Ground", glm::vec2(0.0f, -0.5f), glm::vec2(20.0f, 0.5f), BodyType::Static);
			Entity crate = Create2DBox(scene, "Crate", glm::vec2(0.0f, 0.5f), glm::vec2(0.5f), BodyType::Dynamic);
			Entity drifter = Create2DEntity(scene, "Drifter", glm::vec2(0.0f, 20.0f));
			drifter.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .LinearDamping = 0.0f, .GravityFactor = 0.0f, .AllowSleeping = false });
			drifter.AddComponent<CircleCollider2DComponent>();
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			Recording2DListener listener;
			(*system)->SetEventListener(&listener);
			uint64_t tick = 0;
			for (; tick < 30; ++tick)
				UpdateAndStep(scene, **system, tick);
			REQUIRE(FindEvents(listener, crate.GetUUID(), PhysicsEventType::CollisionEnter).size() == 1);
			listener.Events.clear();

			crate.Patch<BoxCollider2DComponent>([](BoxCollider2DComponent& collider)
			{
				collider.HalfExtents = glm::vec2(0.45f, 0.5f);
			});
			UpdateAndStep(scene, **system, tick++);
			crate.Patch<RigidBody2DComponent>([](RigidBody2DComponent& body)
			{
				body.Friction = 0.9f;
			});
			UpdateAndStep(scene, **system, tick++);
			UpdateAndStep(scene, **system, tick++);
			CHECK(listener.Events.empty());
			CHECK((*system)->GetStats().ContactPairCount == 1);

			// A body created again keeps its current velocity, not the RigidBody2D's InitialLinearVelocity.
			REQUIRE((*system)->SetLinearVelocity(drifter.GetUUID(), glm::vec2(3.0f, 0.0f)).has_value());
			drifter.Patch<RigidBody2DComponent>([](RigidBody2DComponent& body)
			{
				body.AngularDamping = 0.1f;
			});
			UpdateAndStep(scene, **system, tick++);
			CHECK((*system)->GetLinearVelocity(drifter.GetUUID()).value_or(glm::vec2(0.0f)).x == doctest::Approx(3.0f));

			// Removing the crate's only collider removes its body: the pair ends with a synthesized exit for both sides.
			crate.RemoveComponent<BoxCollider2DComponent>();
			UpdateAndStep(scene, **system, tick++);
			(*system)->SetEventListener(nullptr);
			CHECK_FALSE((*system)->GetBodyBounds(crate.GetUUID()).has_value());
			const std::vector<PhysicsEvent> groundExits = FindEvents(listener, ground.GetUUID(), PhysicsEventType::CollisionExit);
			REQUIRE(groundExits.size() == 1);
			CHECK(groundExits[0].Other == crate.GetUUID());
			CHECK(groundExits[0].Synthesized);
			CHECK(FindEvents(listener, crate.GetUUID(), PhysicsEventType::CollisionExit).size() == 1);
			CHECK((*system)->GetStats().ContactPairCount == 0);
		}

		// PhysicsSystem's rule in the plane ("Physics: destroying or disabling a partner synthesizes exits"): a disabled entity's
		// body leaves the world at the flush, its partners get synthesized exits and it gets none, and enabled again its body
		// is back at the next PreStep.
		TEST_CASE("Physics2DSystem: disabling an entity ends its 2D pairs with synthesized exits, and enabling it brings its body back")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			static_cast<void>(Create2DBox(scene, "Ground", glm::vec2(0.0f, -0.5f), glm::vec2(20.0f, 0.5f), BodyType::Static));
			Entity napper = Create2DBox(scene, "Napper", glm::vec2(3.0f, 0.5f), glm::vec2(0.5f), BodyType::Dynamic);
			Entity zone = Create2DEntity(scene, "Zone", glm::vec2(3.0f, 0.5f));
			zone.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(1.5f), .IsTrigger = true });
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			Recording2DListener listener;
			(*system)->SetEventListener(&listener);
			uint64_t tick = 0;
			for (; tick < 30; ++tick)
				UpdateAndStep(scene, **system, tick);
			REQUIRE(FindEvents(listener, zone.GetUUID(), PhysicsEventType::TriggerEnter).size() == 1);
			listener.Events.clear();

			napper.SetActive(false);
			(*system)->FlushDestroyed(tick);
			const std::vector<PhysicsEvent> zoneExits = FindEvents(listener, zone.GetUUID(), PhysicsEventType::TriggerExit);
			REQUIRE(zoneExits.size() == 1);
			CHECK(zoneExits[0].Other == napper.GetUUID());
			CHECK(zoneExits[0].Synthesized);
			CHECK(FindEvents(listener, napper.GetUUID(), PhysicsEventType::TriggerExit).empty());
			CHECK(FindEvents(listener, napper.GetUUID(), PhysicsEventType::CollisionExit).empty());
			UpdateAndStep(scene, **system, tick++);
			CHECK_FALSE((*system)->GetBodyBounds(napper.GetUUID()).has_value());

			napper.SetActive(true);
			listener.Events.clear();
			UpdateAndStep(scene, **system, tick++);
			UpdateAndStep(scene, **system, tick++);
			(*system)->SetEventListener(nullptr);
			CHECK((*system)->GetBodyBounds(napper.GetUUID()).has_value());
			CHECK(FindEvents(listener, zone.GetUUID(), PhysicsEventType::TriggerEnter).size() == 1);
		}

		// PhysicsSystem's rule in the plane ("PhysicsSystem: a body falling asleep keeps its pairs, and moving it away ends
		// them"): Jolt reports a sleeping pair's contacts as removed while they still touch; the system keeps them dormant until
		// a step that began with one of the bodies awake does not report them again.
		TEST_CASE("Physics2DSystem: a 2D body falling asleep keeps its pairs, and moving it away ends them")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity ground = Create2DBox(scene, "Ground", glm::vec2(0.0f, -0.5f), glm::vec2(20.0f, 0.5f), BodyType::Static);
			Entity box = Create2DBox(scene, "Box", glm::vec2(0.0f, 0.5f), glm::vec2(0.5f), BodyType::Dynamic);
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			Recording2DListener listener;
			(*system)->SetEventListener(&listener);
			uint64_t tick = 0;
			for (; tick < 180; ++tick)
				UpdateAndStep(scene, **system, tick);
			REQUIRE((*system)->IsSleeping(box.GetUUID()).value_or(false));
			CHECK(FindEvents(listener, box.GetUUID(), PhysicsEventType::CollisionEnter).size() == 1);
			CHECK(FindEvents(listener, box.GetUUID(), PhysicsEventType::CollisionExit).empty());
			CHECK((*system)->GetStats().ContactPairCount == 1);

			// Lifted away, the box is awake for the next step, which reports no contact: a real exit, once per side.
			listener.Events.clear();
			REQUIRE((*system)->Teleport(box.GetUUID(), glm::vec2(0.0f, 10.0f), std::nullopt).has_value());
			UpdateAndStep(scene, **system, tick++);
			UpdateAndStep(scene, **system, tick++);
			(*system)->SetEventListener(nullptr);
			for (const UUID self : { box.GetUUID(), ground.GetUUID() })
			{
				const std::vector<PhysicsEvent> exits = FindEvents(listener, self, PhysicsEventType::CollisionExit);
				REQUIRE(exits.size() == 1);
				CHECK_FALSE(exits[0].Synthesized);
			}
			CHECK((*system)->GetStats().ContactPairCount == 0);
		}

		// The RigidBody2D proxy checks a body function's motion type with it before it acts (W21.1's review).
		TEST_CASE("Physics2DSystem: GetMotionType reports the simulated motion type of a RigidBody2D's body")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity crate = Create2DBox(scene, "Crate", glm::vec2(0.0f), glm::vec2(0.5f), BodyType::Dynamic);
			Entity lift = Create2DBox(scene, "Lift", glm::vec2(3.0f, 0.0f), glm::vec2(0.5f), BodyType::Kinematic);
			Entity sensor = Create2DEntity(scene, "Sensor", glm::vec2(6.0f, 0.0f));
			sensor.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = BodyType::Static });
			sensor.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .IsTrigger = true });
			Entity wall = Create2DBox(scene, "Wall", glm::vec2(9.0f, 0.0f), glm::vec2(0.5f), std::nullopt);
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene, .Gravity = glm::vec2(0.0f) });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			CHECK((*system)->GetMotionType(crate.GetUUID()).value_or(PhysicsMotionType::Static) == PhysicsMotionType::Dynamic);
			CHECK((*system)->GetMotionType(lift.GetUUID()).value_or(PhysicsMotionType::Static) == PhysicsMotionType::Kinematic);
			// A Static RigidBody2D whose colliders are triggers is a Kinematic sensor, as in the 3D world.
			CHECK((*system)->GetMotionType(sensor.GetUUID()).value_or(PhysicsMotionType::Static) == PhysicsMotionType::Kinematic);
			// An implicit static body is no RigidBody2D body.
			const Result<PhysicsMotionType> implicit = (*system)->GetMotionType(wall.GetUUID());
			REQUIRE_FALSE(implicit.has_value());
			CHECK(implicit.error().GetCode() == ErrorCode::NotFound);
		}

		// C21.2 (Docs/Decisions/0032-m21-contract.md decision 25, stream T2): the TilemapCollider's tilemap, a ground row in two
		// chunks of 4 x 4 unit cells with a non-solid cell above it: its boxes, [0, 4] x [0, 1] and [4, 8] x [0, 1] (never merged
		// across the chunks' edge), form the implicit static body of the tilemap's entity.
		TEST_CASE("Physics2DSystem: a TilemapCollider makes one static compound of its merged boxes in canonical order")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			const AssetHandle tilemapHandle = UUID(8960);
			Ref<TilemapData> tilemap = CreateRef<TilemapData>();
			tilemap->ChunkSize = 4;
			tilemap->Palette = { TilemapTile{ .Solid = true }, TilemapTile{ .Solid = false } };
			tilemap->Chunks = {
				TilemapChunk{ .X = 0, .Y = 0, .Tiles = { 1, 1, 1, 1, 0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
				TilemapChunk{ .X = 1, .Y = 0, .Tiles = { 1, 1, 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 } },
			};
			assets.Publish(tilemapHandle, tilemap, "Assets/Tilemaps/Ground.tilemap");
			Entity ground = Create2DEntity(scene, "Ground", glm::vec2(0.0f));
			ground.AddComponent<TilemapColliderComponent>(TilemapColliderComponent{ .Tilemap = TypedAssetHandle<AssetType::Tilemap>(tilemapHandle) });
			// A ball falls through the non-solid cell (2, 1) onto the ground, and rolls across the seam at x = 4.
			Entity ball = Create2DEntity(scene, "Ball", glm::vec2(2.5f, 3.0f));
			ball.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .EnhancedInternalEdgeRemoval = true, .InitialLinearVelocity = glm::vec2(2.0f, 0.0f) });
			ball.AddComponent<CircleCollider2DComponent>(CircleCollider2DComponent{ .Radius = 0.25f });
			TransformSystem::Update(scene);
			Recording2DListener listener;
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			(*system)->SetEventListener(&listener);
			CHECK((*system)->GetDiagnostics().empty());
			CHECK((*system)->GetStats().BodyCount == 2);
			const std::optional<Aabb> bounds = (*system)->GetBodyBounds(ground.GetUUID());
			REQUIRE(bounds.has_value());
			CHECK(bounds->Min.x == doctest::Approx(0.0f));
			CHECK(bounds->Min.y == doctest::Approx(0.0f));
			CHECK(bounds->Max.x == doctest::Approx(8.0f));
			CHECK(bounds->Max.y == doctest::Approx(1.0f));

			for (uint64_t tick = 0; tick < 120; ++tick)
				UpdateAndStep(scene, **system, tick);
			// It rests on the ground's top, past the seam, without catching on it.
			const glm::vec3 rest = ball.GetComponent<TransformComponent>().Translation;
			CHECK(rest.y == doctest::Approx(1.25f).epsilon(0.01));
			CHECK(rest.x > 4.5f);
			// The ball's enter names the tilemap's entity as the collider it touched.
			const std::vector<PhysicsEvent> enters = FindEvents(listener, ball.GetUUID(), PhysicsEventType::CollisionEnter);
			REQUIRE(enters.size() == 1);
			CHECK(enters[0].Other == ground.GetUUID());
			CHECK(enters[0].Contact.OtherCollider == ground.GetUUID());
			// A ray down at x = 6 hits the second chunk's box, the tilemap's entity, at the ground's top.
			const Result<std::optional<Physics2DRaycastHit>> hit = (*system)->Raycast(glm::vec2(6.0f, 5.0f), glm::vec2(0.0f, -1.0f), 10.0f);
			REQUIRE(hit.has_value());
			REQUIRE(hit->has_value());
			CHECK((*hit)->Entity == ground.GetUUID());
			CHECK((*hit)->Body == ground.GetUUID());
			CHECK((*hit)->Point.y == doctest::Approx(1.0f));
		}

		// C21.2 (decision 25, stream T2): a new asset version of the tilemap (a tilemap.paint edit) rebuilds the shape in place at
		// the next pre-step, as a mesh collider's hot reload; so does a new TilemapRenderer.Tilemap on an entity whose
		// TilemapCollider has none, as a MeshRenderer's new Mesh does for a MeshCollider without one (C21.2's review).
		TEST_CASE("Physics2DSystem: a tilemap's new version rebuilds its collider's shape in place")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			const AssetHandle tilemapHandle = UUID(8961);
			const AssetHandle lowHandle = UUID(8962);
			const AssetHandle highHandle = UUID(8963);
			const auto publish = [&assets](AssetHandle handle, std::vector<uint32_t> tiles)
			{
				Ref<TilemapData> tilemap = CreateRef<TilemapData>();
				tilemap->ChunkSize = 2;
				tilemap->Palette = { TilemapTile{ .Solid = true } };
				tilemap->Chunks = { TilemapChunk{ .X = 0, .Y = 0, .Tiles = std::move(tiles) } };
				assets.Publish(handle, tilemap, "Assets/Tilemaps/" + handle.ToString() + ".tilemap");
			};
			publish(tilemapHandle, { 1, 1, 0, 0 });
			publish(lowHandle, { 1, 1, 0, 0 });
			publish(highHandle, { 1, 1, 1, 1 });
			Entity ground = Create2DEntity(scene, "Ground", glm::vec2(0.0f));
			ground.AddComponent<TilemapColliderComponent>(TilemapColliderComponent{ .Tilemap = TypedAssetHandle<AssetType::Tilemap>(tilemapHandle) });
			// A collider without a Tilemap takes its entity's TilemapRenderer's.
			Entity ledge = Create2DEntity(scene, "Ledge", glm::vec2(10.0f, 0.0f));
			ledge.AddComponent<TilemapRendererComponent>(TilemapRendererComponent{ .Tilemap = TypedAssetHandle<AssetType::Tilemap>(lowHandle) });
			ledge.AddComponent<TilemapColliderComponent>();
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			const auto topAt = [&system](float x)
			{
				const Result<std::optional<Physics2DRaycastHit>> hit = (*system)->Raycast(glm::vec2(x, 5.0f), glm::vec2(0.0f, -1.0f), 10.0f);
				REQUIRE(hit.has_value());
				return hit->has_value() ? (*hit)->Point.y : -1.0f;
			};
			CHECK(topAt(1.5f) == doctest::Approx(1.0f));
			CHECK(topAt(10.5f) == doctest::Approx(1.0f));
			CHECK((*system)->GetStats().BodyCount == 2);
			uint64_t compositions = (*system)->GetStats().Compositions;
			// Painting cell (1, 1) raises the right column to 2; the body keeps its identity (no new body, one more composition).
			publish(tilemapHandle, { 1, 1, 0, 1 });
			UpdateAndStep(scene, **system, 0);
			CHECK(topAt(1.5f) == doctest::Approx(2.0f));
			CHECK(topAt(0.5f) == doctest::Approx(1.0f));
			CHECK((*system)->GetStats().BodyCount == 2);
			CHECK((*system)->GetStats().Compositions == compositions + 1);
			// Switching the ledge renderer's tilemap rebuilds the ledge's collider in place: both columns rise to 2.
			compositions = (*system)->GetStats().Compositions;
			ledge.Patch<TilemapRendererComponent>([highHandle](TilemapRendererComponent& renderer)
			{
				renderer.Tilemap = TypedAssetHandle<AssetType::Tilemap>(highHandle);
			});
			UpdateAndStep(scene, **system, 1);
			CHECK(topAt(10.5f) == doctest::Approx(2.0f));
			CHECK(topAt(11.5f) == doctest::Approx(2.0f));
			CHECK((*system)->GetStats().BodyCount == 2);
			CHECK((*system)->GetStats().Compositions == compositions + 1);
			CHECK((*system)->GetDiagnostics().empty());
			// Erasing every cell of the ground's tilemap leaves it no shape: its body is removed, without a diagnostic.
			publish(tilemapHandle, { 0, 0, 0, 0 });
			UpdateAndStep(scene, **system, 2);
			CHECK((*system)->GetStats().BodyCount == 1);
			CHECK((*system)->GetDiagnostics().empty());
			// Painting a solid cell again creates the body once more (stream T2: the body its tilemap left without a shape).
			publish(tilemapHandle, { 0, 0, 1, 0 });
			UpdateAndStep(scene, **system, 3);
			CHECK((*system)->GetStats().BodyCount == 2);
			CHECK(topAt(0.5f) == doctest::Approx(2.0f));
			CHECK(topAt(1.5f) == -1.0f);
			CHECK((*system)->GetDiagnostics().empty());
		}

		// Stream T2 (decision 25): merging changes how many sub-shapes a tilemap's body has, never where it is solid. A stepped
		// terrain of half-unit cells in 4-cell chunks, with a hole and non-solid cells on top, collided once by a TilemapCollider
		// (merged boxes) and once by a static RigidBody2D gathering one BoxCollider2D per solid cell (the tiles' unmerged boxes):
		// every cell centre overlaps the ground exactly where a cell is solid, rays down meet the same tops, and bodies dropped
		// on it come to rest at the same places.
		TEST_CASE("Physics2DSystem: bodies collide with a TilemapCollider's merged boxes as with the tiles' unmerged boxes")
		{
			// Column x (0 to 15) is solid up to row 1 + (x / 3) % 3, except the hole at column 7; every fourth column carries a
			// non-solid cell on its top.
			constexpr float CellSide = 0.5f;
			constexpr int32_t Columns = 16;
			const auto heightOf = [](int32_t x)
			{
				return x == 7 ? 0 : 1 + (x / 3) % 3;
			};
			std::vector<std::pair<glm::ivec2, uint32_t>> cells;
			for (int32_t x = 0; x < Columns; ++x)
			{
				for (int32_t y = 0; y < heightOf(x); ++y)
					cells.emplace_back(glm::ivec2(x, y), 1u);
				if (x % 4 == 0)
					cells.emplace_back(glm::ivec2(x, heightOf(x)), 2u);
			}
			const Ref<TilemapData> tilemap = MakeTestTilemap(cells, 4, glm::vec2(CellSide));
			const glm::vec2 origin(-3.0f, -2.0f);
			const auto cellCenter = [&origin](int32_t x, int32_t y)
			{
				return origin + (glm::vec2(static_cast<float>(x), static_cast<float>(y)) + 0.5f) * CellSide;
			};

			struct Outcome
			{
				std::vector<bool> Solid{};
				std::vector<float> Tops{};
				std::vector<glm::vec2> Rests{};
			};
			const auto run = [&](bool merged)
			{
				Test::SceneTestFixture fixture(1, true);
				Scene& scene = fixture.GetScene();
				Test::InMemoryAssetManager assets;
				const AssetHandle tilemapHandle = UUID(8964);
				assets.Publish(tilemapHandle, tilemap, "Assets/Tilemaps/Steps.tilemap");
				Entity ground = Create2DEntity(scene, "Ground", origin);
				if (merged)
				{
					ground.AddComponent<TilemapColliderComponent>(TilemapColliderComponent{ .Tilemap = TypedAssetHandle<AssetType::Tilemap>(tilemapHandle) });
				}
				else
				{
					ground.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = BodyType::Static });
					for (const auto& [cell, tile] : cells)
					{
						if (tile != 1)
							continue;
						Entity piece = Create2DEntity(scene, "Tile", (glm::vec2(cell) + 0.5f) * CellSide);
						piece.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(0.5f * CellSide) });
						REQUIRE(scene.SetParent(piece, ground, std::nullopt, false).has_value());
					}
				}
				// A ball over the middle of every solid column, and a crate over the flat top of columns 9 to 11 across the chunk
				// edge at column 12's left.
				std::vector<Entity> dropped;
				for (int32_t x = 0; x < Columns; ++x)
				{
					if (heightOf(x) == 0)
						continue;
					Entity ball = Create2DEntity(scene, "Ball", cellCenter(x, 5));
					ball.AddComponent<RigidBody2DComponent>();
					ball.AddComponent<CircleCollider2DComponent>(CircleCollider2DComponent{ .Radius = 0.2f });
					dropped.push_back(ball);
				}
				Entity crate = Create2DEntity(scene, "Crate", cellCenter(10, 7));
				crate.AddComponent<RigidBody2DComponent>();
				crate.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(0.6f, 0.2f) });
				dropped.push_back(crate);
				TransformSystem::Update(scene);
				Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene, .Assets = &assets });
				REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
				CHECK((*system)->GetDiagnostics().empty());

				Outcome outcome;
				// Every cell centre of the map and a margin around it: solid where the ground is.
				for (int32_t y = -1; y < 5; ++y)
				{
					for (int32_t x = -1; x <= Columns; ++x)
					{
						const Result<std::vector<PhysicsOverlapHit>> hits = (*system)->OverlapCircle(cellCenter(x, y), 0.1f * CellSide);
						REQUIRE(hits.has_value());
						outcome.Solid.push_back(std::ranges::any_of(*hits, [&ground](const PhysicsOverlapHit& hit)
						{
							return hit.Body == ground.GetUUID();
						}));
					}
				}
				// The ground's top under every column's centre and every seam between columns.
				for (int32_t step = 0; step < 2 * Columns; ++step)
				{
					const float x = origin.x + static_cast<float>(step) * 0.5f * CellSide + 0.25f * CellSide;
					const Result<std::vector<Physics2DRaycastHit>> hits = (*system)->RaycastAll(glm::vec2(x, 10.0f), glm::vec2(0.0f, -1.0f), 20.0f);
					REQUIRE(hits.has_value());
					const auto top = std::ranges::find_if(*hits, [&ground](const Physics2DRaycastHit& hit)
					{
						return hit.Body == ground.GetUUID();
					});
					outcome.Tops.push_back(top != hits->end() ? top->Point.y : -100.0f);
				}
				for (uint64_t tick = 0; tick < 150; ++tick)
					UpdateAndStep(scene, **system, tick);
				for (const Entity body : dropped)
					outcome.Rests.push_back(glm::vec2(body.GetComponent<TransformComponent>().Translation));
				return outcome;
			};

			const Outcome merged = run(true);
			const Outcome unmerged = run(false);
			CHECK(merged.Solid == unmerged.Solid);
			// The overlaps are the cells: solid exactly where a solid cell is.
			size_t index = 0;
			for (int32_t y = -1; y < 5; ++y)
			{
				for (int32_t x = -1; x <= Columns; ++x)
				{
					CAPTURE(x);
					CAPTURE(y);
					const bool solid = x >= 0 && x < Columns && y >= 0 && y < heightOf(x);
					CHECK(merged.Solid[index++] == solid);
				}
			}
			REQUIRE(merged.Tops.size() == unmerged.Tops.size());
			for (size_t column = 0; column < merged.Tops.size(); ++column)
			{
				CAPTURE(column);
				CHECK(merged.Tops[column] == doctest::Approx(unmerged.Tops[column]).epsilon(1e-5));
			}
			REQUIRE(merged.Rests.size() == unmerged.Rests.size());
			for (size_t body = 0; body < merged.Rests.size(); ++body)
			{
				CAPTURE(body);
				CHECK(merged.Rests[body].x == doctest::Approx(unmerged.Rests[body].x).epsilon(1e-3));
				CHECK(merged.Rests[body].y == doctest::Approx(unmerged.Rests[body].y).epsilon(1e-3));
			}
			// Each ball rests on its column's top, within Jolt's penetration slop (2 cm).
			size_t ball = 0;
			for (int32_t x = 0; x < Columns; ++x)
			{
				if (heightOf(x) == 0)
					continue;
				CAPTURE(x);
				const float top = origin.y + static_cast<float>(heightOf(x)) * CellSide;
				CHECK(std::abs(merged.Rests[ball].y - (top + 0.2f)) <= 0.025f);
				CHECK(merged.Rests[ball].x == doctest::Approx(cellCenter(x, 0).x));
				++ball;
			}
		}

		// C21.2 (decision 27, stream P3): physics2d.bodyInfo's report, PhysicsSystem::GetBodyInfo's in the plane.
		TEST_CASE("Physics2DSystem: GetBodyInfo reports a 2D body's pose, colliders and contacts")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity floor = Create2DBox(scene, "Floor", glm::vec2(0.0f, -0.5f), glm::vec2(5.0f, 0.5f), BodyType::Static);
			Entity crate = Create2DBox(scene, "Crate", glm::vec2(1.0f, 0.5f), glm::vec2(0.5f), BodyType::Dynamic);
			// A collider-only child gathered into the crate's body, 0.6 above its centre (its local Transform kept).
			Entity lid = Create2DBox(scene, "Lid", glm::vec2(0.0f, 0.6f), glm::vec2(0.5f, 0.1f), std::nullopt);
			REQUIRE(scene.SetParent(lid, crate, {}, false).has_value());
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene, .Gravity = glm::vec2(0.0f, -9.81f) });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			for (uint64_t tick = 0; tick < 30; ++tick)
				UpdateAndStep(scene, **system, tick);

			const std::optional<Physics2DBodyReport> report = (*system)->GetBodyInfo(lid.GetUUID());
			REQUIRE(report.has_value());
			CHECK(report->Owner == crate.GetUUID());
			CHECK(report->Origin == PhysicsBodyOrigin::RigidBody);
			CHECK(report->MotionType == PhysicsMotionType::Dynamic);
			CHECK_FALSE(report->IsSensor);
			CHECK(report->LayerName == "Default");
			CHECK(report->Colliders == std::vector<UUID>{ crate.GetUUID(), lid.GetUUID() });
			CHECK(report->Pose.Position.x == doctest::Approx(1.0f).epsilon(0.01));
			CHECK(report->Pose.Position.y == doctest::Approx(0.5f).epsilon(0.02));
			CHECK(report->BoundsMin.x == doctest::Approx(0.5f).epsilon(0.02));
			CHECK(report->BoundsMax.y == doctest::Approx(1.2f).epsilon(0.02));
			REQUIRE(report->Contacts.size() == 1);
			CHECK(report->Contacts[0].Other == floor.GetUUID());
			CHECK_FALSE(report->Contacts[0].IsTrigger);

			const std::optional<Physics2DBodyReport> floorReport = (*system)->GetBodyInfo(floor.GetUUID());
			REQUIRE(floorReport.has_value());
			CHECK(floorReport->MotionType == PhysicsMotionType::Static);
			CHECK(floorReport->LinearVelocity == glm::vec2(0.0f));
			// An entity without a 2D body, or one disabled, reports none.
			Entity marker = Create2DEntity(scene, "Marker", glm::vec2(3.0f, 3.0f));
			CHECK_FALSE((*system)->GetBodyInfo(marker.GetUUID()).has_value());
			crate.SetActive(false);
			CHECK_FALSE((*system)->GetBodyInfo(crate.GetUUID()).has_value());
		}

		// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3): a pre-step visits the owners of kinematic bodies and the
		// owners whose transforms changed since the last one, its shape check the bodies that bake a written entity in, and the
		// write-back the owners of dynamic bodies, whatever static scenery the scene holds; a Transform written outside the
		// steps (a script's Entity::Patch, a teleport) still places its body, and a world-change list that overflowed visits
		// every body owner, as every pre-step did before.
		TEST_CASE("Physics2DSystem: a tick visits only the moving bodies and the bodies whose transforms changed")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			// Static scenery, Static RigidBody2Ds and implicit static bodies, and props without a body, away from the moving
			// bodies.
			std::vector<Entity> walls;
			const auto addWall = [&scene, &walls](int index)
			{
				std::optional<BodyType> type;
				if (index % 2 == 0)
					type = BodyType::Static;
				walls.push_back(Create2DBox(scene, "Wall", glm::vec2(3.0f * static_cast<float>(index), -10.0f), glm::vec2(0.5f), type));
			};
			for (int index = 0; index < 12; ++index)
				addWall(index);
			std::vector<Entity> props;
			for (int index = 0; index < 8; ++index)
				props.push_back(Create2DEntity(scene, "Prop", glm::vec2(3.0f * static_cast<float>(index), 20.0f)));
			static_cast<void>(Create2DBox(scene, "Platform", glm::vec2(0.0f, 10.0f), glm::vec2(1.0f, 0.25f), BodyType::Kinematic));
			// A ball that never stops, so it writes its pose back every step.
			Entity ball = Create2DEntity(scene, "Ball", glm::vec2(-20.0f, 0.0f));
			ball.AddComponent<RigidBody2DComponent>(
				RigidBody2DComponent{ .LinearDamping = 0.0f, .GravityFactor = 0.0f, .AllowSleeping = false, .InitialLinearVelocity = glm::vec2(1.0f, 0.0f) });
			ball.AddComponent<CircleCollider2DComponent>(CircleCollider2DComponent{ .Radius = 0.25f });
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene, .Gravity = glm::vec2(0.0f) });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			REQUIRE((*system)->GetStats().BodyCount == 14);
			// The transform update, then one step: the entities the step's passes visited.
			const auto stepVisits = [&scene, &system](uint64_t tick)
			{
				TransformSystem::Update(scene);
				const uint64_t visited = scene.GetEntitiesVisited();
				StepBoth(**system, tick);
				return scene.GetEntitiesVisited() - visited;
			};
			const auto centerOf = [&system](const Entity entity)
			{
				const std::optional<Aabb> bounds = (*system)->GetBodyBounds(entity.GetUUID());
				REQUIRE(bounds.has_value());
				return glm::vec2(bounds->GetCenter());
			};

			// The first step: the platform's move and the ball's write-back. From then on the Transform the ball wrote back adds
			// its shape check and its pre-step visit: four visits a step, none for the walls and the props.
			CHECK(stepVisits(0) == 2);
			uint64_t tick = 1;
			for (; tick < 4; ++tick)
				CHECK(stepVisits(tick) == 4);
			// Thirty more walls: the step that creates their bodies visits them (their Transforms are new), the next ones four
			// entities again.
			for (int index = 12; index < 42; ++index)
				addWall(index);
			static_cast<void>(stepVisits(tick++));
			REQUIRE((*system)->GetStats().BodyCount == 44);
			CHECK(stepVisits(tick++) == 4);

			// A script's write of a wall's Transform: the wall's shape check and pre-step visit, which places it.
			walls[3].Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.y = -5.0f;
			});
			CHECK(stepVisits(tick++) == 6);
			CHECK(centerOf(walls[3]).x == doctest::Approx(9.0f));
			CHECK(centerOf(walls[3]).y == doctest::Approx(-5.0f));
			CHECK(stepVisits(tick++) == 4);

			// A teleport of another wall, and a write that teleports the ball: each body is where its Transform is.
			REQUIRE((*system)->Teleport(walls[6].GetUUID(), glm::vec2(18.0f, -2.0f), std::nullopt).has_value());
			ball.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.y = 4.0f;
			});
			CHECK(stepVisits(tick++) == 6);
			CHECK(centerOf(walls[6]).x == doctest::Approx(18.0f));
			CHECK(centerOf(walls[6]).y == doctest::Approx(-2.0f));
			CHECK(centerOf(ball).y == doctest::Approx(4.0f));
			CHECK(stepVisits(tick++) == 4);

			// Transform updates without a step that change more world matrices than the list holds (eight props each, past
			// 4,096 entries): the next pre-step checks every body's shape (44) and visits every body owner (44), the ball's
			// write-back one more, and still places the wall written meanwhile.
			for (int update = 0; update < 600; ++update)
			{
				for (const Entity prop : props)
				{
					prop.Patch<TransformComponent>([update](TransformComponent& transform)
					{
						transform.Translation.z = static_cast<float>(update);
					});
				}
				TransformSystem::Update(scene);
			}
			walls[9].Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation.y = -3.0f;
			});
			CHECK(stepVisits(tick++) == 89);
			CHECK(centerOf(walls[9]).y == doctest::Approx(-3.0f));
			CHECK(stepVisits(tick++) == 4);
		}

		// M43: the physics2d hash block lists the bodies in canonical order of their owners (a parent before its children, then
		// the next root), whatever the order of their UUIDs.
		TEST_CASE("Physics2DSystem: the state hash block lists the bodies in canonical order of their owners")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			// Canonical order First, Child, Second; their UUIDs in the reverse order.
			Entity first = scene.CreateEntityWithID(UUID(300), "First");
			Entity second = scene.CreateEntityWithID(UUID(100), "Second");
			Entity child = scene.CreateEntityWithID(UUID(200), "Child", first);
			const auto addStaticBox = [](const Entity entity, const glm::vec2& position)
			{
				entity.Patch<TransformComponent>([&position](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(position, 0.0f);
				});
				entity.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = BodyType::Static });
				entity.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(0.5f) });
			};
			addStaticBox(first, glm::vec2(0.0f));
			addStaticBox(second, glm::vec2(-6.0f, 0.0f));
			addStaticBox(child, glm::vec2(6.0f, 0.0f));
			TransformSystem::Update(scene);
			Result<Scope<Physics2DSystem>> system = Physics2DSystem::Create({ .RuntimeScene = &scene });
			REQUIRE_MESSAGE(system.has_value(), system.error().ToString());
			REQUIRE((*system)->GetStats().BodyCount == 3);

			std::vector<std::byte> canonical;
			AppendRestingHashEntry(canonical, first.GetUUID(), glm::vec2(0.0f), true);
			AppendRestingHashEntry(canonical, child.GetUUID(), glm::vec2(6.0f, 0.0f), true);
			AppendRestingHashEntry(canonical, second.GetUUID(), glm::vec2(-6.0f, 0.0f), true);
			std::vector<std::byte> byUuid;
			AppendRestingHashEntry(byUuid, second.GetUUID(), glm::vec2(-6.0f, 0.0f), true);
			AppendRestingHashEntry(byUuid, child.GetUUID(), glm::vec2(6.0f, 0.0f), true);
			AppendRestingHashEntry(byUuid, first.GetUUID(), glm::vec2(0.0f), true);
			XXH64Hasher block(0);
			(*system)->AppendStateHash(block);
			XXH64Hasher expected(0);
			expected.Update(canonical);
			XXH64Hasher reversed(0);
			reversed.Update(byUuid);
			CHECK(block.Digest() == expected.Digest());
			CHECK(block.Digest() != reversed.Digest());
		}
	}

}
