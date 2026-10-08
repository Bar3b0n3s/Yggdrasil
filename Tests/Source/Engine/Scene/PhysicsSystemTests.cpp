#include "TestsPCH.h"

#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Jobs/MainThreadQueue.h"
#include "Engine/Core/Random.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Physics/PhysicsShape.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/CapsuleColliderComponent.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/MeshColliderComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/SphereColliderComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/AssetTestFixture.h"
#include "Support/ExpectLog.h"
#include "Support/PhysicsTestScene.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <tuple>
#include <vector>

// The play session's physics (Architecture §9.2 to §9.4, §5.7 steps 5 to 8; §9.7 and Roadmap M11 acceptance): composition
// into bodies, the PreStep/PostStep sync, sorted events and synthesized exits, through PlaySession (lockstep ticks, Play
// mode), and the play-session fuzz test (§9.1, §15.2: no Jolt assert is reachable from content or from the body functions).
// The names starting "Physics:" are Roadmap acceptance names, used verbatim; the others name their unit (the
// trigger-detects-a-character acceptance is in PhysicsSystemCharactersTests.cpp).

namespace Engine {

	namespace {

		// Starts a lockstep-style session of `fixture`'s scene and installs `listener`.
		Scope<PlaySession> StartSession(Test::SceneTestFixture& fixture, Test::RecordingPhysicsListener* listener = nullptr, IPlaySessionObserver* observer = nullptr)
		{
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture);
			specification.Observer = observer;
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			if (listener != nullptr)
				(*session)->GetPhysics().SetEventListener(listener);
			return std::move(*session);
		}

		// Calls `action` at the start of every phase `phase` of a tick in [first, last].
		class PhaseAction final : public IPlaySessionObserver
		{
		public:
			PhaseAction(PlaySessionPhase phase, uint64_t first, uint64_t last, std::function<void(PlaySession&, uint64_t)> action)
				: m_Phase(phase), m_First(first), m_Last(last), m_Action(std::move(action))
			{
			}

			void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t tick) override
			{
				if (phase == m_Phase && tick >= m_First && tick <= m_Last)
					m_Action(session, tick);
			}
		private:
			PlaySessionPhase m_Phase = PlaySessionPhase::FixedUpdate;
			uint64_t m_First = 0;
			uint64_t m_Last = 0;
			std::function<void(PlaySession&, uint64_t)> m_Action{};
		};

		// A track of `count` adjacent 1 m boxes along +X (centres x = 0 .. count - 1, top face y = 0.5): one static compound
		// under a level root when `shared`, else `count` separate static bodies.
		void AddTrack(Scene& scene, int count, bool shared)
		{
			Entity level = scene.CreateEntity("Level");
			if (shared)
				level.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
			for (int piece = 0; piece < count; ++piece)
			{
				static_cast<void>(Test::AddBoxBody(scene, "Piece", glm::vec3(static_cast<float>(piece), 0.0f, 0.0f), glm::vec3(0.5f),
					shared ? std::nullopt : std::optional<BodyType>(BodyType::Static), level));
			}
		}

		// The largest |v.y| of "/Ball" while a 0.5 m sphere rolls along the track at 6 m/s (§9.2 "Seams"). It REQUIREs, so
		// callers keep it out of CHECK expressions.
		float MeasureRollingBump(bool shared)
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddTrack(scene, 20, shared);
			Entity ball = Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f, 1.0f, 0.0f), 0.5f, BodyType::Dynamic);
			Test::PatchRigidBody(ball, [](RigidBodyComponent& body)
			{
				body.MotionQuality = MotionQuality::LinearCast;
				body.EnhancedInternalEdgeRemoval = true;
				body.Friction = 0.8f;
				body.MaxAngularVelocity = 120.0f;
				body.AllowSleeping = false;
			});
			Scope<PlaySession> session = StartSession(fixture);
			PhysicsSystem& physics = session->GetPhysics();
			const UUID id = ball.GetUUID();
			// Settle on the first piece, then roll.
			Test::RunTicks(*session, 30);
			REQUIRE(physics.SetLinearVelocity(id, glm::vec3(6.0f, 0.0f, 0.0f)).has_value());
			REQUIRE(physics.SetAngularVelocity(id, glm::vec3(0.0f, 0.0f, -12.0f)).has_value());
			float largest = 0.0f;
			for (int tick = 0; tick < 240 && Test::GetWorldPosition(*session, "/Ball").x < 18.5f; ++tick)
			{
				session->Tick();
				largest = std::max(largest, std::abs(physics.GetLinearVelocity(id).value_or(glm::vec3(0.0f)).y));
			}
			CHECK(Test::GetWorldPosition(*session, "/Ball").x >= 18.5f);
			return largest;
		}

		// An asset manager over meshes the test publishes, each publication a new version (a hot reload, §7.5).
		class PublishingAssetManager final : public AssetManager
		{
		public:
			explicit PublishingAssetManager(JobSystem& jobs)
				: m_Jobs(&jobs)
			{
			}

			void Publish(AssetHandle handle, AssetRef<Asset> asset)
			{
				m_Assets[handle] = std::move(asset);
				BumpVersion(handle);
			}

			[[nodiscard]] Result<AssetRef<Asset>> Load(AssetHandle handle) override
			{
				if (Result<AssetRef<Asset>> builtin = GetProceduralBuiltin(handle))
					return builtin;
				const auto found = m_Assets.find(handle);
				if (found == m_Assets.end())
					return MakeError(ErrorCode::NotFound, "no asset {}", handle.ToString());
				return found->second;
			}

			[[nodiscard]] JobHandle<AssetRef<Asset>> LoadAsync(AssetHandle handle) override
			{
				Result<AssetRef<Asset>> loaded = Load(handle);
				return m_Jobs->Submit([loaded]() -> Result<AssetRef<Asset>>
				{
					return loaded;
				});
			}

			[[nodiscard]] AssetState GetState(AssetHandle handle) const override
			{
				return m_Assets.contains(handle) ? AssetState::Loaded : AssetState::Unloaded;
			}

			[[nodiscard]] const AssetMetadata* GetMetadata(AssetHandle /*handle*/) const override { return nullptr; }

			[[nodiscard]] AssetType GetAssetType(AssetHandle handle) const override
			{
				const auto found = m_Assets.find(handle);
				return found == m_Assets.end() ? AssetType::None : found->second->GetAssetType();
			}

			[[nodiscard]] std::optional<AssetHandle> Resolve(std::string_view /*reference*/) const override { return std::nullopt; }
			[[nodiscard]] std::string GetReferencePath(AssetHandle handle) const override { return handle.ToString(); }
			void WaitIdle() override {}
		private:
			JobSystem* m_Jobs = nullptr; // not owned
			std::map<AssetHandle, AssetRef<Asset>> m_Assets;
		};

		// A closed box mesh of `halfExtent` (its 8 corners and 12 triangles; the physics shapes read positions and indices).
		AssetRef<MeshData> MakeBoxMesh(float halfExtent)
		{
			Ref<MeshData> mesh = CreateRef<MeshData>();
			for (int corner = 0; corner < 8; ++corner)
			{
				const glm::vec3 position((corner & 1) != 0 ? halfExtent : -halfExtent, (corner & 2) != 0 ? halfExtent : -halfExtent,
					(corner & 4) != 0 ? halfExtent : -halfExtent);
				mesh->Vertices.push_back(MeshVertex{ .Position = position });
			}
			mesh->Indices = { 0, 2, 1, 1, 2, 3, 4, 5, 6, 5, 7, 6, 0, 1, 4, 1, 5, 4, 2, 6, 3, 3, 6, 7, 0, 4, 2, 2, 4, 6, 1, 3, 5, 3, 7, 5 };
			return mesh;
		}

		// A MeshRenderer showing `mesh`.
		MeshRendererComponent MakeMeshRenderer(AssetHandle mesh)
		{
			MeshRendererComponent renderer;
			renderer.Mesh = TypedAssetHandle<AssetType::Mesh>(mesh);
			return renderer;
		}

		// The fuzz test's random values (§15.2 "NaN, ±Inf, 0, negative, huge"): every content value stays within its registry
		// rule (the only way content reaches the system), every function argument is anything a binding could pass.
		class PhysicsFuzzer
		{
		public:
			explicit PhysicsFuzzer(uint64_t seed)
				: m_Random(seed)
			{
			}

			[[nodiscard]] float Range(float minimum, float maximum)
			{
				return static_cast<float>(m_Random.RangeDouble(static_cast<double>(minimum), static_cast<double>(maximum)));
			}
			[[nodiscard]] int Pick(int count) { return static_cast<int>(m_Random.RangeInt(0, count - 1)); }
			[[nodiscard]] bool Chance(int percent) { return Pick(100) < percent; }
			// A dimension within the registry's minimum, sometimes at the minimum itself and sometimes enormous, around and far
			// beyond the largest collider (the registry gives dimensions no maximum; PhysicsShape::Create takes colliders up to
			// PhysicsShape::MaxColliderSize across).
			[[nodiscard]] float Dimension()
			{
				if (Chance(10))
					return 0.001f;
				if (Chance(2))
					return Chance(50) ? Range(1.0e4f, PhysicsShape::MaxColliderSize / 2.0f) : Range(PhysicsShape::MaxColliderSize / 2.0f, 1.0e8f);
				return Range(0.001f, 6.0f);
			}
			// A mass within the registry's rule, sometimes at either end of it.
			[[nodiscard]] float Mass()
			{
				if (Chance(10))
					return 0.001f;
				return Chance(5) ? MaxPhysicsMass : Range(0.001f, 100.0f);
			}
			[[nodiscard]] glm::vec3 Vector(float magnitude)
			{
				return glm::vec3(Range(-magnitude, magnitude), Range(-magnitude, magnitude), Range(-magnitude, magnitude));
			}
			[[nodiscard]] glm::quat Rotation()
			{
				return glm::normalize(glm::quat(Range(0.1f, 1.0f), Range(-1.0f, 1.0f), Range(-1.0f, 1.0f), Range(-1.0f, 1.0f)));
			}
			// A scale whose components have a magnitude of at least MinTransformScaleMagnitude, sometimes negative, sometimes
			// non-uniform.
			[[nodiscard]] glm::vec3 Scale()
			{
				if (Chance(60))
					return glm::vec3(1.0f);
				const float uniform = Chance(15) ? 1.0e-4f : Range(0.1f, 3.0f);
				glm::vec3 scale = Chance(50) ? glm::vec3(uniform) : glm::vec3(Range(0.1f, 3.0f), Range(0.1f, 3.0f), Range(0.1f, 3.0f));
				if (Chance(15))
					scale.x = -scale.x;
				return scale;
			}
			// Any float a script or automation call could pass: the special values, and values just inside the guards (positions
			// and sizes near MaxPhysicsCoordinate, forces, velocities and gravity near the body functions' 1e12, which is
			// MaxPhysicsGravity) that the functions accept.
			[[nodiscard]] float Wild()
			{
				constexpr std::array<float, 12> Special = { 0.0f, -0.0f, std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::infinity(),
					-std::numeric_limits<float>::infinity(), std::numeric_limits<float>::max(), -1.0e20f, 1.0e-30f, 0.9f * MaxPhysicsCoordinate,
					-0.9f * MaxPhysicsCoordinate, 0.9f * MaxPhysicsGravity, -0.9f * MaxPhysicsGravity };
				return Chance(25) ? Special[static_cast<size_t>(Pick(static_cast<int>(Special.size())))] : Range(-50.0f, 50.0f);
			}
			[[nodiscard]] glm::vec3 WildVector() { return glm::vec3(Wild(), Wild(), Wild()); }
			[[nodiscard]] glm::quat WildRotation() { return Chance(70) ? Rotation() : glm::quat(Wild(), Wild(), Wild(), Wild()); }
		private:
			Random m_Random;
		};

		// Adds random physics components to `entity`, each within its registry rules.
		void AddRandomPhysics(PhysicsFuzzer& fuzzer, Entity entity, bool meshes)
		{
			entity.Patch<TransformComponent>([&fuzzer](TransformComponent& transform)
			{
				transform.Translation = fuzzer.Vector(15.0f);
				transform.Rotation = fuzzer.Chance(50) ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f) : fuzzer.Rotation();
				transform.Scale = fuzzer.Scale();
			});
			if (fuzzer.Chance(55))
			{
				entity.AddComponent<BoxColliderComponent>(BoxColliderComponent{ .HalfExtents = glm::vec3(fuzzer.Dimension(), fuzzer.Dimension(), fuzzer.Dimension()),
					.Offset = fuzzer.Vector(1.0f),
					.Rotation = fuzzer.Rotation(),
					.IsTrigger = fuzzer.Chance(20) });
			}
			if (fuzzer.Chance(35))
			{
				entity.AddComponent<SphereColliderComponent>(
					SphereColliderComponent{ .Radius = fuzzer.Dimension(), .Offset = fuzzer.Vector(1.0f), .IsTrigger = fuzzer.Chance(20) });
			}
			if (fuzzer.Chance(25))
			{
				entity.AddComponent<CapsuleColliderComponent>(CapsuleColliderComponent{ .Radius = fuzzer.Dimension(),
					.HalfHeight = fuzzer.Dimension(),
					.Offset = fuzzer.Vector(1.0f),
					.Rotation = fuzzer.Rotation(),
					.IsTrigger = fuzzer.Chance(20) });
			}
			if (meshes && fuzzer.Chance(20))
			{
				const std::array<AssetHandle, 4> handles = { BuiltinAssetHandles::CubeMesh, BuiltinAssetHandles::SphereMesh, BuiltinAssetHandles::PlaneMesh,
					AssetHandle() };
				const AssetHandle mesh = handles[static_cast<size_t>(fuzzer.Pick(static_cast<int>(handles.size())))];
				entity.AddComponent<MeshColliderComponent>(
					MeshColliderComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(mesh), .Convex = fuzzer.Chance(50), .IsTrigger = fuzzer.Chance(20) });
				if (!mesh.IsValid() && fuzzer.Chance(50))
					entity.AddComponent<MeshRendererComponent>(MakeMeshRenderer(BuiltinAssetHandles::CubeMesh));
			}
			const int body = fuzzer.Pick(10);
			if (body < 5)
			{
				RigidBodyComponent component;
				component.Type = static_cast<BodyType>(fuzzer.Pick(3));
				component.Mass = fuzzer.Mass();
				component.Friction = fuzzer.Range(0.0f, 2.0f);
				component.Restitution = fuzzer.Range(0.0f, 1.0f);
				component.LinearDamping = fuzzer.Chance(20) ? 0.0f : fuzzer.Range(0.0f, 5.0f);
				component.AngularDamping = fuzzer.Range(0.0f, 5.0f);
				component.GravityFactor = fuzzer.Range(-3.0f, 3.0f);
				component.MotionQuality = fuzzer.Chance(30) ? MotionQuality::LinearCast : MotionQuality::Discrete;
				component.AllowSleeping = fuzzer.Chance(70);
				component.LockTranslation = glm::bvec3(fuzzer.Chance(30), fuzzer.Chance(30), fuzzer.Chance(30));
				component.LockRotation = glm::bvec3(fuzzer.Chance(30), fuzzer.Chance(30), fuzzer.Chance(30));
				component.Layer = fuzzer.Chance(10) ? "Missing" : (fuzzer.Chance(50) ? "Default" : "Track");
				component.MaxLinearVelocity = fuzzer.Chance(10) ? 0.0f : fuzzer.Range(0.0f, 1000.0f);
				component.MaxAngularVelocity = fuzzer.Chance(10) ? 0.0f : fuzzer.Range(0.0f, 200.0f);
				component.EnhancedInternalEdgeRemoval = fuzzer.Chance(50);
				component.InitialLinearVelocity = fuzzer.Vector(fuzzer.Chance(10) ? 5000.0f : 20.0f);
				component.InitialAngularVelocity = fuzzer.Vector(fuzzer.Chance(10) ? 5000.0f : 20.0f);
				entity.AddComponent<RigidBodyComponent>(component);
			}
			else if (body == 5)
			{
				entity.AddComponent<CharacterControllerComponent>(CharacterControllerComponent{ .Height = fuzzer.Dimension() * 2.0f,
					.Radius = fuzzer.Dimension(),
					.MaxSlopeAngle = fuzzer.Range(0.0f, 90.0f),
					.StepHeight = fuzzer.Range(0.0f, 1.0f),
					.Mass = fuzzer.Mass(),
					.GravityFactor = fuzzer.Chance(5) ? 1.0e30f : fuzzer.Range(-2.0f, 2.0f),
					.Layer = fuzzer.Chance(10) ? "Missing" : "Default" });
			}
		}

		// One random edit or body function call, as a script's OnFixedUpdate or an automation request would make it. The
		// function results are success or a located error; nothing asserts.
		void MakeRandomCall(PhysicsFuzzer& fuzzer, PlaySession& session)
		{
			Scene& scene = session.GetScene();
			const std::vector<UUID> order(scene.GetCanonicalOrder().begin(), scene.GetCanonicalOrder().end());
			if (order.empty())
				return;
			// Sometimes an id that names nothing (a destroyed entity, a typo).
			const UUID id = fuzzer.Chance(10) ? UUID(0x5eed) : order[static_cast<size_t>(fuzzer.Pick(static_cast<int>(order.size())))];
			PhysicsSystem& physics = session.GetPhysics();
			const Entity entity = scene.FindEntityByID(id);
			const int action = fuzzer.Pick(29);
			if (action == 0)
				static_cast<void>(physics.AddForce(id, fuzzer.WildVector()));
			else if (action == 1)
				static_cast<void>(physics.AddForceAtPosition(id, fuzzer.WildVector(), fuzzer.WildVector()));
			else if (action == 2)
				static_cast<void>(physics.AddTorque(id, fuzzer.WildVector()));
			else if (action == 3)
				static_cast<void>(physics.AddImpulse(id, fuzzer.WildVector()));
			else if (action == 4)
				static_cast<void>(physics.AddAngularImpulse(id, fuzzer.WildVector()));
			else if (action == 5)
				static_cast<void>(physics.SetLinearVelocity(id, fuzzer.WildVector()));
			else if (action == 6)
				static_cast<void>(physics.SetAngularVelocity(id, fuzzer.WildVector()));
			else if (action == 7)
				static_cast<void>(physics.MoveKinematic(id, fuzzer.WildVector(), fuzzer.WildRotation()));
			else if (action == 8)
				static_cast<void>(physics.Teleport(id, fuzzer.WildVector(), fuzzer.Chance(50) ? std::optional<glm::quat>(fuzzer.WildRotation()) : std::nullopt));
			else if (action == 9)
				static_cast<void>(physics.WakeUp(id));
			else if (action == 10)
				static_cast<void>(physics.SetGravity(fuzzer.Chance(80) ? glm::vec3(0.0f, fuzzer.Range(-30.0f, 0.0f), 0.0f) : fuzzer.WildVector()));
			else if (action == 11)
				static_cast<void>(physics.MoveCharacter(id, fuzzer.WildVector()));
			else if (action == 22)
				static_cast<void>(physics.RaycastAll(fuzzer.WildVector(), fuzzer.WildVector(), fuzzer.Wild()));
			else if (action == 23)
				static_cast<void>(physics.SphereCast(fuzzer.WildVector(), fuzzer.Wild(), fuzzer.WildVector(), fuzzer.Wild()));
			else if (action == 24)
				static_cast<void>(physics.OverlapSphere(fuzzer.WildVector(), fuzzer.Wild()));
			else if (action == 25)
				static_cast<void>(physics.OverlapBox(fuzzer.WildVector(), fuzzer.WildVector(), fuzzer.WildRotation()));
			else if (action == 26)
				static_cast<void>(physics.Raycast(fuzzer.WildVector(), fuzzer.WildVector(), fuzzer.Wild(), fuzzer.Chance(50) ? AllPhysicsLayers : 1u));
			else if (action == 27)
			{
				static_cast<void>(physics.GetBodyBounds(id));
				static_cast<void>(physics.GetColliderBounds(id));
			}
			else if (action == 28)
				static_cast<void>(physics.GetCharacterState(id));
			else if (!entity.IsValid())
				static_cast<void>(physics.GetBodyInfo(id));
			else if (action == 12)
				scene.DestroyEntity(entity);
			else if (action == 13)
				entity.SetActive(!entity.IsActiveSelf());
			else if (action == 14)
				entity.GetComponent<TransformComponent>().Translation = fuzzer.Vector(20.0f); // a script's Transform write: a teleport
			else if (action == 15)
			{
				const Entity parent = fuzzer.Chance(20) ? Entity() : scene.FindEntityByID(order[static_cast<size_t>(fuzzer.Pick(static_cast<int>(order.size())))]);
				static_cast<void>(scene.SetParent(entity, parent));
			}
			else if (action == 16 && entity.HasComponent<RigidBodyComponent>())
			{
				const auto type = static_cast<BodyType>(fuzzer.Pick(3));
				const bool locked = fuzzer.Chance(30);
				entity.Patch<RigidBodyComponent>([type, locked](RigidBodyComponent& body)
				{
					body.Type = type;
					body.LockTranslation = glm::bvec3(locked);
					body.LockRotation = glm::bvec3(locked);
				});
			}
			else if (action == 17 && entity.HasComponent<BoxColliderComponent>())
			{
				const glm::vec3 extents(fuzzer.Dimension(), fuzzer.Dimension(), fuzzer.Dimension());
				const bool trigger = fuzzer.Chance(30);
				entity.Patch<BoxColliderComponent>([&extents, trigger](BoxColliderComponent& box)
				{
					box.HalfExtents = extents;
					box.IsTrigger = trigger;
				});
			}
			else if (action == 17)
				entity.AddComponent<BoxColliderComponent>(BoxColliderComponent{ .HalfExtents = glm::vec3(fuzzer.Dimension()) });
			else if (action == 18 && entity.HasComponent<SphereColliderComponent>())
				entity.RemoveComponent<SphereColliderComponent>();
			else if (action == 18 && entity.HasComponent<RigidBodyComponent>())
				entity.RemoveComponent<RigidBodyComponent>();
			else if (action == 19)
			{
				Result<Entity> created = session.CreateEntity("Spawned");
				if (created)
					AddRandomPhysics(fuzzer, *created, false);
			}
			else if (action == 20)
			{
				const glm::vec3 scale = fuzzer.Scale();
				entity.Patch<TransformComponent>([&scale](TransformComponent& transform)
				{
					transform.Scale = scale;
				});
			}
			else
			{
				static_cast<void>(physics.GetLinearVelocity(id));
				static_cast<void>(physics.IsSleeping(id));
				static_cast<void>(physics.GetBodyInfo(id));
			}
		}

		// Makes `calls` random calls in every fixed update (a script's OnFixedUpdate).
		class FuzzingObserver final : public IPlaySessionObserver
		{
		public:
			FuzzingObserver(PhysicsFuzzer& fuzzer, int calls)
				: m_Fuzzer(fuzzer), m_Calls(calls)
			{
			}

			void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t /*tick*/) override
			{
				if (phase != PlaySessionPhase::FixedUpdate)
					return;
				for (int call = 0; call < m_Calls; ++call)
					MakeRandomCall(m_Fuzzer, session);
			}
		private:
			PhysicsFuzzer& m_Fuzzer; // not owned: the test case's, outliving the session
			int m_Calls = 0;
		};

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("PhysicsEventType: every type has its enumerator name")
		{
			CHECK(PhysicsEventTypeToString(PhysicsEventType::CollisionEnter) == "CollisionEnter");
			CHECK(PhysicsEventTypeToString(PhysicsEventType::CollisionExit) == "CollisionExit");
			CHECK(PhysicsEventTypeToString(PhysicsEventType::TriggerEnter) == "TriggerEnter");
			CHECK(PhysicsEventTypeToString(PhysicsEventType::TriggerExit) == "TriggerExit");
		}

		TEST_CASE("PhysicsSystem: the session's physics follows the project's gravity")
		{
			Test::SceneTestFixture fixture;
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture);
			specification.Project.Physics.Gravity = glm::vec3(0.0f, -20.0f, 1.0f);
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			CHECK(Test::ApproxEqual((*session)->GetPhysics().GetGravity(), glm::vec3(0.0f, -20.0f, 1.0f)));
		}

		TEST_CASE("PhysicsSystem: Create refuses a missing scene, a zero FixedHz and invalid layers")
		{
			CHECK(PhysicsSystem::Create({}).error().GetCode() == ErrorCode::InvalidArgument);
			Test::SceneTestFixture fixture;
			PhysicsSystemSpecification zeroHz;
			zeroHz.RuntimeScene = &fixture.GetScene();
			zeroHz.FixedHz = 0;
			CHECK(PhysicsSystem::Create(zeroHz).error().GetCode() == ErrorCode::InvalidArgument);
			PhysicsSystemSpecification badLayers;
			badLayers.RuntimeScene = &fixture.GetScene();
			badLayers.Layers = { "Track" };
			CHECK(PhysicsSystem::Create(badLayers).error().GetCode() == ErrorCode::Validation);
		}

		TEST_CASE("PhysicsSystem: bodies exist from tick 0 and Dynamic bodies fall and write their pose back")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			// A dynamic box under a moved and rotated static parent: the write-back goes through the parent's inverse.
			Entity parent = scene.CreateEntity("Parent");
			parent.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(10.0f, 0.0f, 0.0f);
				transform.Rotation = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			});
			static_cast<void>(Test::AddBoxBody(scene, "Box", glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic, parent));
			Scope<PlaySession> session = StartSession(fixture);
			const UUID box = Test::GetEntityId(session->GetScene(), "/Parent/Box");
			REQUIRE(session->GetPhysics().GetBodyInfo(box).has_value());
			CHECK(session->GetPhysics().GetStats().BodyCount == 2);

			Test::RunTicks(*session, 240);
			const std::optional<PhysicsBodyReport> info = session->GetPhysics().GetBodyInfo(box);
			REQUIRE(info.has_value());
			CHECK(info->Pose.Position.y == doctest::Approx(0.48f).epsilon(0.02));
			// The entity's world pose is the body's; its local translation is relative to the rotated parent.
			CHECK(Test::ApproxEqual(Test::GetWorldPosition(*session, "/Parent/Box"), info->Pose.Position, 1.0e-4f));
			const Entity entity = session->GetScene().FindEntityByID(box);
			CHECK(entity.GetComponent<TransformComponent>().Translation.y == doctest::Approx(0.48f).epsilon(0.02));
		}

		TEST_CASE("Physics: trigger detects a sleeping body")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Sleeper", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			PhysicsSystem& physics = session->GetPhysics();
			const UUID sleeper = Test::GetEntityId(session->GetScene(), "/Sleeper");
			Test::RunTicks(*session, 300);
			REQUIRE(physics.IsSleeping(sleeper).value_or(false));

			// A checkpoint appears around the sleeping box: an implicit sensor body, created at the next PreStep.
			Result<Entity> checkpoint = session->CreateEntity("Checkpoint");
			REQUIRE(checkpoint.has_value());
			checkpoint->AddComponent<BoxColliderComponent>(BoxColliderComponent{ .HalfExtents = glm::vec3(2.0f), .IsTrigger = true });
			listener.Events.clear();
			Test::RunTicks(*session, 2);
			const UUID trigger = checkpoint->GetUUID();
			CHECK(listener.Find(trigger, PhysicsEventType::TriggerEnter).size() == 1);
			const std::vector<PhysicsEvent> entered = listener.Find(sleeper, PhysicsEventType::TriggerEnter);
			REQUIRE(entered.size() == 1);
			CHECK(entered[0].Other == trigger);
			// Detecting it does not wake it.
			CHECK(physics.IsSleeping(sleeper).value_or(false));
		}

		TEST_CASE("PhysicsSystem: a Static RigidBody trigger detects a sleeping body")
		{
			// The variant of "Physics: trigger detects a sleeping body" with an authored Static RigidBody: it is planned as a
			// Kinematic sensor, kept active (§9.2), so it sees the sleeping box too.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Sleeper", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			PhysicsSystem& physics = session->GetPhysics();
			const UUID sleeper = Test::GetEntityId(session->GetScene(), "/Sleeper");
			Test::RunTicks(*session, 300);
			REQUIRE(physics.IsSleeping(sleeper).value_or(false));

			Result<Entity> goal = session->CreateEntity("Goal");
			REQUIRE(goal.has_value());
			goal->AddComponent<BoxColliderComponent>(BoxColliderComponent{ .HalfExtents = glm::vec3(2.0f), .IsTrigger = true });
			goal->AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
			listener.Events.clear();
			Test::RunTicks(*session, 2);
			const std::vector<PhysicsEvent> entered = listener.Find(sleeper, PhysicsEventType::TriggerEnter);
			REQUIRE(entered.size() == 1);
			CHECK(entered[0].Other == goal->GetUUID());
			const std::optional<PhysicsBodyReport> report = physics.GetBodyInfo(goal->GetUUID());
			REQUIRE(report.has_value());
			CHECK(report->MotionType == PhysicsMotionType::Kinematic);
			CHECK(report->IsSensor);
			CHECK(physics.IsSleeping(sleeper).value_or(false));
		}

		TEST_CASE("PhysicsSystem: an attached trigger never reports its own body")
		{
			// A trigger child of a moving ball (§5.3) overlaps the ball all the time; their shared collision group keeps the pair
			// out, so the trigger reports only others.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			Entity ball = Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f, 0.5f, 0.0f), 0.5f, BodyType::Dynamic);
			Entity aura = Test::AddSphereBody(scene, "Aura", glm::vec3(0.0f), 2.0f, std::nullopt, ball);
			aura.Patch<SphereColliderComponent>([](SphereColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			static_cast<void>(Test::AddBoxBody(scene, "Crate", glm::vec3(1.5f, 0.25f, 0.0f), glm::vec3(0.25f), BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			Test::RunTicks(*session, 30);
			const UUID crate = Test::GetEntityId(session->GetScene(), "/Crate");
			const std::vector<PhysicsEvent> auraEnters = listener.Find(aura.GetUUID(), PhysicsEventType::TriggerEnter);
			REQUIRE(auraEnters.size() == 1);
			CHECK(auraEnters[0].Other == crate);
			CHECK(listener.Find(ball.GetUUID(), PhysicsEventType::TriggerEnter).empty());
		}

		TEST_CASE("PhysicsSystem: trigger enter and exit are reported exactly once for both entities")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity gate = Test::AddBoxBody(scene, "Gate", glm::vec3(0.0f, 5.0f, 0.0f), glm::vec3(2.0f, 0.5f, 2.0f), std::nullopt);
			gate.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f, 10.0f, 0.0f), 0.25f, BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			Test::RunTicks(*session, 120);
			const UUID gateId = gate.GetUUID();
			const UUID ballId = Test::GetEntityId(session->GetScene(), "/Ball");
			for (const UUID self : { gateId, ballId })
			{
				CHECK(listener.Find(self, PhysicsEventType::TriggerEnter).size() == 1);
				CHECK(listener.Find(self, PhysicsEventType::TriggerExit).size() == 1);
				CHECK(listener.Find(self, PhysicsEventType::CollisionEnter).empty());
			}
			const std::vector<PhysicsEvent> exits = listener.Find(ballId, PhysicsEventType::TriggerExit);
			REQUIRE(exits.size() == 1);
			CHECK_FALSE(exits[0].Synthesized);
		}

		TEST_CASE("Physics: kinematic platform carries a resting box")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddBoxBody(scene, "Platform", glm::vec3(0.0f), glm::vec3(2.0f, 0.1f, 2.0f), BodyType::Kinematic));
			static_cast<void>(Test::AddBoxBody(scene, "Crate", glm::vec3(0.0f, 0.6f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			// The platform moves along +X at 1 m/s, driven like a script's RigidBody:MoveKinematic in OnFixedUpdate.
			PhaseAction mover(PlaySessionPhase::FixedUpdate, 60, 179, [](PlaySession& session, uint64_t tick)
			{
				const UUID platform = Test::GetEntityId(session.GetScene(), "/Platform");
				const float x = static_cast<float>(tick - 59) / 60.0f;
				REQUIRE(session.GetPhysics().MoveKinematic(platform, glm::vec3(x, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)).has_value());
			});
			Scope<PlaySession> session = StartSession(fixture, nullptr, &mover);
			Test::RunTicks(*session, 60);
			const float restX = Test::GetWorldPosition(*session, "/Crate").x;
			Test::RunTicks(*session, 120);
			CHECK(Test::GetWorldPosition(*session, "/Platform").x == doctest::Approx(2.0f).epsilon(1.0e-3));
			// The crate rode along (within the friction's slip) and stayed on top.
			CHECK(Test::GetWorldPosition(*session, "/Crate").x - restX == doctest::Approx(2.0f).epsilon(0.1));
			CHECK(Test::GetWorldPosition(*session, "/Crate").y > 0.5f);
		}

		TEST_CASE("Physics: destroy inside a contact callback is safe")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f, 2.0f, 0.0f), 0.5f, BodyType::Dynamic));
			static_cast<void>(Test::AddSphereBody(scene, "Other", glm::vec3(3.0f, 2.0f, 0.0f), 0.5f, BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			PlaySession* sessionPointer = session.get();
			const UUID ball = Test::GetEntityId(session->GetScene(), "/Ball");
			const UUID ground = Test::GetEntityId(session->GetScene(), "/Ground");
			// The ball destroys itself when it lands, as a script's OnCollisionEnter would (deferred, §5.7).
			listener.OnEvent = [sessionPointer, ball](const PhysicsEvent& event)
			{
				if (event.Type == PhysicsEventType::CollisionEnter && event.Self == ball)
				{
					Entity entity = sessionPointer->GetScene().FindEntityByID(ball);
					if (entity.IsValid())
						sessionPointer->GetScene().DestroyEntity(entity);
				}
			};
			Test::RunTicks(*session, 90);
			CHECK_FALSE(session->GetScene().FindEntityByID(ball).IsValid());
			CHECK_FALSE(session->GetPhysics().GetBodyInfo(ball).has_value());
			// The ground received the landing's enter (its Self stayed alive), and the destroy flush closed the pair: one
			// synthesized exit for the ground.
			const std::vector<PhysicsEvent> exits = listener.Find(ground, PhysicsEventType::CollisionExit);
			REQUIRE(exits.size() == 1);
			CHECK(exits[0].Other == ball);
			CHECK(exits[0].Synthesized);
			// The rest of the scene keeps simulating.
			CHECK(session->GetPhysics().GetBodyInfo(Test::GetEntityId(session->GetScene(), "/Other")).has_value());
			CHECK(session->GetPhysics().GetStats().BodyCount == 2);
		}

		TEST_CASE("Physics: sphere rolling across 20 boxes of one compound keeps |vy| < 0.05")
		{
			const float bump = MeasureRollingBump(true);
			CHECK(bump < 0.05f);
		}

		TEST_CASE("PhysicsSystem: a roll across 20 separate static bodies bumps at the seams")
		{
			// The control case of §9.2: what the shared static compound avoids.
			const float bump = MeasureRollingBump(false);
			CHECK(bump >= 0.05f);
		}

		TEST_CASE("Physics: compound contact names the child collider")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddTrack(scene, 3, true);
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(2.0f, 3.0f, 0.0f), 0.25f, BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			Test::RunTicks(*session, 90);
			const Scene& runtime = session->GetScene();
			const UUID ball = Test::GetEntityId(runtime, "/Ball");
			const UUID level = Test::GetEntityId(runtime, "/Level");
			const UUID third = Test::GetEntityId(runtime, "/Level/Piece[2]");
			const std::vector<PhysicsEvent> ballEnter = listener.Find(ball, PhysicsEventType::CollisionEnter);
			REQUIRE(ballEnter.size() == 1);
			// `other` is the body owner; the contact names the piece that was hit (§9.4).
			CHECK(ballEnter[0].Other == level);
			CHECK(ballEnter[0].Contact.Collider == ball);
			CHECK(ballEnter[0].Contact.OtherCollider == third);
			CHECK(ballEnter[0].Contact.Normal.y < -0.9f);
			const std::vector<PhysicsEvent> levelEnter = listener.Find(level, PhysicsEventType::CollisionEnter);
			REQUIRE(levelEnter.size() == 1);
			CHECK(levelEnter[0].Contact.Collider == third);
			CHECK(levelEnter[0].Contact.OtherCollider == ball);
			CHECK(levelEnter[0].Contact.Normal.y > 0.9f);
		}

		TEST_CASE("Physics: destroying or disabling a partner synthesizes exits")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Doomed", glm::vec3(-3.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			static_cast<void>(Test::AddBoxBody(scene, "Napper", glm::vec3(3.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Entity zone = Test::AddBoxBody(scene, "Zone", glm::vec3(3.0f, 0.5f, 0.0f), glm::vec3(1.5f), std::nullopt);
			zone.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			Scene& runtime = session->GetScene();
			const UUID ground = Test::GetEntityId(runtime, "/Ground");
			const UUID doomed = Test::GetEntityId(runtime, "/Doomed");
			const UUID napper = Test::GetEntityId(runtime, "/Napper");
			const UUID zoneId = Test::GetEntityId(runtime, "/Zone");
			Test::RunTicks(*session, 30);
			REQUIRE(listener.Find(zoneId, PhysicsEventType::TriggerEnter).size() == 1);
			listener.Events.clear();

			// Destroyed: the ground receives OnCollisionExit with an invalid `other`.
			runtime.DestroyEntity(runtime.FindEntityByID(doomed));
			// Disabled: the zone receives OnTriggerExit with an inactive `other`.
			runtime.FindEntityByID(napper).SetActive(false);
			session->Tick();
			const std::vector<PhysicsEvent> groundExits = listener.Find(ground, PhysicsEventType::CollisionExit);
			REQUIRE(groundExits.size() == 2);
			const auto doomedExit = std::find_if(groundExits.begin(), groundExits.end(), [doomed](const PhysicsEvent& event)
			{
				return event.Other == doomed;
			});
			REQUIRE(doomedExit != groundExits.end());
			CHECK(doomedExit->Synthesized);
			CHECK_FALSE(runtime.FindEntityByID(doomed).IsValid());
			const std::vector<PhysicsEvent> zoneExits = listener.Find(zoneId, PhysicsEventType::TriggerExit);
			REQUIRE(zoneExits.size() == 1);
			CHECK(zoneExits[0].Other == napper);
			CHECK(zoneExits[0].Synthesized);
			CHECK_FALSE(runtime.FindEntityByID(napper).IsActive());
			// No callback goes to the destroyed or disabled entity itself.
			CHECK(listener.Find(doomed, PhysicsEventType::CollisionExit).empty());
			CHECK(listener.Find(napper, PhysicsEventType::TriggerExit).empty());
			CHECK_FALSE(session->GetPhysics().GetBodyInfo(napper).has_value());

			// Enabled again, the body is back at the next PreStep and the zone sees it enter once more.
			runtime.FindEntityByID(napper).SetActive(true);
			listener.Events.clear();
			Test::RunTicks(*session, 2);
			CHECK(session->GetPhysics().GetBodyInfo(napper).has_value());
			CHECK(listener.Find(zoneId, PhysicsEventType::TriggerEnter).size() == 1);
		}

		TEST_CASE("Physics: Transform write in OnFixedUpdate teleports the body in the same tick")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddSphereBody(fixture.GetScene(), "Ball", glm::vec3(0.0f, 10.0f, 0.0f), 0.5f, BodyType::Dynamic));
			// In the fixed update of tick 30 a script writes the ball's Transform.
			PhaseAction writer(PlaySessionPhase::FixedUpdate, 30, 30, [](PlaySession& session, uint64_t /*tick*/)
			{
				Entity ball = session.GetScene().FindEntityByPath("/Ball");
				ball.GetComponent<TransformComponent>().Translation = glm::vec3(5.0f, 20.0f, 0.0f);
			});
			Scope<PlaySession> session = StartSession(fixture, nullptr, &writer);
			Test::RunTicks(*session, 30);
			const UUID ball = Test::GetEntityId(session->GetScene(), "/Ball");
			const glm::vec3 velocityBefore = session->GetPhysics().GetLinearVelocity(ball).value_or(glm::vec3(0.0f));
			session->Tick();
			// The body moved to the written pose in tick 30 itself (then fell for one step), keeping its velocity.
			const glm::vec3 position = session->GetPhysics().GetBodyInfo(ball).value_or(PhysicsBodyReport{}).Pose.Position;
			CHECK(position.x == doctest::Approx(5.0f));
			CHECK(position.y == doctest::Approx(20.0f + (velocityBefore.y - 9.81f / 60.0f) / 60.0f).epsilon(1.0e-3));
			CHECK(session->GetPhysics().GetLinearVelocity(ball).value_or(glm::vec3(0.0f)).y < velocityBefore.y);
		}

		TEST_CASE("PhysicsSystem: positions beyond MaxPhysicsCoordinate never reach the world")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f, 10.0f, 0.0f), 0.5f, BodyType::Dynamic));
			static_cast<void>(Test::AddSphereBody(scene, "Far", glm::vec3(0.0f, 0.0f, 2.0f * MaxPhysicsCoordinate), 0.5f, BodyType::Dynamic));
			// A script writes a Transform far beyond the range in the fixed update of tick 5.
			PhaseAction writer(PlaySessionPhase::FixedUpdate, 5, 5, [](PlaySession& session, uint64_t /*tick*/)
			{
				session.GetScene().FindEntityByPath("/Ball").GetComponent<TransformComponent>().Translation = glm::vec3(1.0e30f, 0.0f, 0.0f);
			});
			Test::ExpectLog refused(LogLevel::Error, "PHYSICS_INVALID_SHAPE");
			Scope<PlaySession> session = StartSession(fixture, nullptr, &writer);
			PhysicsSystem& physics = session->GetPhysics();
			// A body placed beyond the range is refused with a diagnostic; the rest of the scene plays.
			const UUID far = Test::GetEntityId(session->GetScene(), "/Far");
			CHECK_FALSE(physics.GetBodyInfo(far).has_value());
			REQUIRE(physics.GetDiagnostics().size() == 1);
			CHECK(physics.GetDiagnostics()[0].Entity == far);
			// The written Transform is no teleport: the body keeps falling where it was, and its write-back replaces the value.
			Test::RunTicks(*session, 10);
			const UUID ball = Test::GetEntityId(session->GetScene(), "/Ball");
			const glm::vec3 position = physics.GetBodyInfo(ball).value_or(PhysicsBodyReport{}).Pose.Position;
			CHECK(position.x == doctest::Approx(0.0f));
			CHECK(position.y < 10.0f);
			CHECK(Test::GetWorldPosition(*session, "/Ball").x == doctest::Approx(0.0f));
		}

		TEST_CASE("Physics: invalid shapes, all-DOF-locked and Dynamic-trigger bodies give diagnostics, never asserts")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			// A mesh collider without a mesh (no Mesh and no MeshRenderer): PHYSICS_INVALID_SHAPE.
			Entity mesh = scene.CreateEntity("NoMesh");
			mesh.AddComponent<MeshColliderComponent>(MeshColliderComponent{}).Convex = true;
			mesh.AddComponent<RigidBodyComponent>(RigidBodyComponent{});
			// A character whose capsule has no cylinder.
			Entity squat = scene.CreateEntity("Squat");
			squat.AddComponent<CharacterControllerComponent>(CharacterControllerComponent{ .Height = 0.4f, .Radius = 0.3f });
			Entity locked = Test::AddBoxBody(scene, "Locked", glm::vec3(0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			Test::PatchRigidBody(locked, [](RigidBodyComponent& body)
			{
				body.LockTranslation = glm::bvec3(true);
				body.LockRotation = glm::bvec3(true);
			});
			Entity trigger = Test::AddBoxBody(scene, "DynamicTrigger", glm::vec3(3.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			trigger.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			static_cast<void>(Test::AddSphereBody(scene, "Fine", glm::vec3(6.0f, 0.0f, 0.0f), 0.5f, BodyType::Dynamic));
			// Settings Jolt itself would assert on, which are valid content and simulate (§9.1): a Kinematic mesh body and an
			// implicit mesh trigger in the same place (Jolt has no mesh-versus-mesh collision), a Kinematic body with every
			// degree of freedom locked, and a starting velocity above the maximum.
			const TypedAssetHandle<AssetType::Mesh> cube(BuiltinAssetHandles::CubeMesh);
			Entity kinematicMesh = scene.CreateEntity("KinematicMesh");
			kinematicMesh.AddComponent<MeshColliderComponent>(MeshColliderComponent{ .Mesh = cube });
			kinematicMesh.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Kinematic });
			Entity meshTrigger = scene.CreateEntity("MeshTrigger");
			meshTrigger.AddComponent<MeshColliderComponent>(MeshColliderComponent{ .Mesh = cube, .IsTrigger = true });
			Entity lockedKinematic = Test::AddBoxBody(scene, "LockedKinematic", glm::vec3(9.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Kinematic);
			Test::PatchRigidBody(lockedKinematic, [](RigidBodyComponent& body)
			{
				body.LockTranslation = glm::bvec3(true);
				body.LockRotation = glm::bvec3(true);
			});
			Entity fast = Test::AddSphereBody(scene, "Fast", glm::vec3(12.0f, 0.0f, 0.0f), 0.5f, BodyType::Dynamic);
			Test::PatchRigidBody(fast, [](RigidBodyComponent& body)
			{
				body.GravityFactor = 0.0f;
				body.LinearDamping = 0.0f;
				body.InitialLinearVelocity = glm::vec3(600.0f, 0.0f, 0.0f);
			});

			Test::ExpectLog invalidShape(LogLevel::Error, "PHYSICS_INVALID_SHAPE");
			Test::ExpectLog allLocked(LogLevel::Error, "PHYSICS_ALL_DOFS_LOCKED");
			Test::ExpectLog dynamicTrigger(LogLevel::Error, "PHYSICS_DYNAMIC_TRIGGER");
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture);
			specification.Assets = &assets.GetManager();
			Result<Scope<PlaySession>> started = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
			Scope<PlaySession> session = std::move(*started);
			Test::RunTicks(*session, 10);
			const PhysicsSystem& physics = session->GetPhysics();
			const auto hasDiagnostic = [&physics](UUID entity, std::string_view code)
			{
				const std::span<const PhysicsDiagnostic> diagnostics = physics.GetDiagnostics();
				return std::any_of(diagnostics.begin(), diagnostics.end(), [entity, code](const PhysicsDiagnostic& diagnostic)
				{
					return diagnostic.Entity == entity && diagnostic.Code == code;
				});
			};
			CHECK(hasDiagnostic(mesh.GetUUID(), PhysicsInvalidShapeCode));
			CHECK(hasDiagnostic(squat.GetUUID(), PhysicsInvalidShapeCode));
			CHECK(hasDiagnostic(locked.GetUUID(), PhysicsAllDofsLockedCode));
			CHECK(hasDiagnostic(trigger.GetUUID(), PhysicsDynamicTriggerCode));
			for (const UUID refused : { mesh.GetUUID(), squat.GetUUID(), locked.GetUUID(), trigger.GetUUID() })
				CHECK_FALSE(physics.GetBodyInfo(refused).has_value());
			// Each diagnostic is raised once, however many ticks run; the valid body simulates.
			CHECK(invalidShape.GetMatchCount() == 2);
			CHECK(allLocked.GetMatchCount() == 1);
			CHECK(dynamicTrigger.GetMatchCount() == 1);
			CHECK(physics.GetBodyInfo(Test::GetEntityId(session->GetScene(), "/Fine")).has_value());
			// The bodies Jolt would have asserted on exist, without a diagnostic; the fast start was clamped to the maximum.
			for (const UUID valid : { kinematicMesh.GetUUID(), meshTrigger.GetUUID(), lockedKinematic.GetUUID(), fast.GetUUID() })
			{
				CHECK(physics.GetBodyInfo(valid).has_value());
				CHECK_FALSE(hasDiagnostic(valid, PhysicsInvalidShapeCode));
				CHECK_FALSE(hasDiagnostic(valid, PhysicsAllDofsLockedCode));
			}
			CHECK(physics.GetLinearVelocity(fast.GetUUID()).value_or(glm::vec3(0.0f)).x == doctest::Approx(500.0f));
		}

		TEST_CASE("PhysicsSystem: changing a RigidBody or a collider rebuilds the body at the next PreStep")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddBoxBody(fixture.GetScene(), "Box", glm::vec3(0.0f, 10.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Scope<PlaySession> session = StartSession(fixture);
			PhysicsSystem& physics = session->GetPhysics();
			Entity box = session->GetScene().FindEntityByPath("/Box");
			const UUID id = box.GetUUID();
			box.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.HalfExtents = glm::vec3(2.0f);
			});
			session->Tick();
			const std::optional<Aabb> bounds = physics.GetBodyBounds(id);
			REQUIRE(bounds.has_value());
			CHECK(bounds->GetSize().x == doctest::Approx(4.0f).epsilon(1.0e-3));
			box.Patch<RigidBodyComponent>([](RigidBodyComponent& body)
			{
				body.Type = BodyType::Static;
			});
			session->Tick();
			const float y = Test::GetWorldPosition(*session, "/Box").y;
			Test::RunTicks(*session, 30);
			CHECK(Test::GetWorldPosition(*session, "/Box").y == doctest::Approx(y));
			CHECK(physics.GetBodyInfo(id).value_or(PhysicsBodyReport{}).MotionType == PhysicsMotionType::Static);
			// Removing the RigidBody leaves the collider as an implicit static body (§5.3).
			box.RemoveComponent<RigidBodyComponent>();
			session->Tick();
			CHECK(physics.GetBodyInfo(id).value_or(PhysicsBodyReport{}).Origin == PhysicsBodyOrigin::ImplicitStatic);
		}

		TEST_CASE("PhysicsSystem: reparenting and scale changes rebuild the bodies they affect")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddTrack(scene, 1, true);
			static_cast<void>(Test::AddBoxBody(scene, "Loose", glm::vec3(5.0f, 0.0f, 0.0f), glm::vec3(0.5f), std::nullopt));
			Scope<PlaySession> session = StartSession(fixture);
			PhysicsSystem& physics = session->GetPhysics();
			Scene& runtime = session->GetScene();
			Entity level = runtime.FindEntityByPath("/Level");
			Entity loose = runtime.FindEntityByPath("/Loose");
			REQUIRE(physics.GetBodyInfo(loose.GetUUID()).value_or(PhysicsBodyReport{}).Owner == loose.GetUUID());
			// Moved under the level root, the loose piece joins its compound.
			REQUIRE(runtime.SetParent(loose, level).has_value());
			session->Tick();
			const std::optional<PhysicsBodyReport> joined = physics.GetBodyInfo(loose.GetUUID());
			REQUIRE(joined.has_value());
			CHECK(joined->Owner == level.GetUUID());
			CHECK(joined->Colliders.size() == 2);
			// Scale is baked into shapes: scaling the root rebuilds its compound twice as large (x from -1 to 11).
			level.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(2.0f);
			});
			session->Tick();
			const std::optional<Aabb> bounds = physics.GetBodyBounds(level.GetUUID());
			REQUIRE(bounds.has_value());
			CHECK(bounds->GetSize().x == doctest::Approx(12.0f).epsilon(1.0e-3));
		}

		TEST_CASE("PhysicsSystem: a rebuild keeps the body's pairs and velocity, and a removal ends its pairs with synthesized exits")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Crate", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Entity drifter = Test::AddSphereBody(scene, "Drifter", glm::vec3(0.0f, 20.0f, 0.0f), 0.5f, BodyType::Dynamic);
			Test::PatchRigidBody(drifter, [](RigidBodyComponent& body)
			{
				body.GravityFactor = 0.0f;
				body.LinearDamping = 0.0f;
				body.AllowSleeping = false;
			});
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			PhysicsSystem& physics = session->GetPhysics();
			Scene& runtime = session->GetScene();
			Entity crate = runtime.FindEntityByPath("/Crate");
			const UUID ground = Test::GetEntityId(runtime, "/Ground");
			Test::RunTicks(*session, 30);
			REQUIRE(listener.Find(crate.GetUUID(), PhysicsEventType::CollisionEnter).size() == 1);
			listener.Events.clear();

			// A collider change replaces the shape in place; a RigidBody change creates the body again. Neither ends nor
			// restarts the pair (pairs are keyed by owner, §9.4).
			crate.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.HalfExtents = glm::vec3(0.5f, 0.5f, 0.45f);
			});
			session->Tick();
			Test::PatchRigidBody(crate, [](RigidBodyComponent& body)
			{
				body.Friction = 0.9f;
			});
			Test::RunTicks(*session, 2);
			CHECK(listener.Events.empty());
			const std::optional<PhysicsBodyReport> rebuilt = physics.GetBodyInfo(crate.GetUUID());
			REQUIRE(rebuilt.has_value());
			CHECK(rebuilt->Contacts.size() == 1);

			// A body created again keeps its current velocity, not the RigidBody's InitialLinearVelocity.
			const UUID drifterId = drifter.GetUUID();
			REQUIRE(physics.SetLinearVelocity(drifterId, glm::vec3(3.0f, 0.0f, 0.0f)).has_value());
			Test::PatchRigidBody(runtime.FindEntityByID(drifterId), [](RigidBodyComponent& body)
			{
				body.AngularDamping = 0.1f;
			});
			session->Tick();
			CHECK(physics.GetLinearVelocity(drifterId).value_or(glm::vec3(0.0f)).x == doctest::Approx(3.0f));

			// Removing the crate's only collider removes its body: no contact re-establishes the pair, which ends with a
			// synthesized exit for both sides.
			crate.RemoveComponent<BoxColliderComponent>();
			session->Tick();
			CHECK_FALSE(physics.GetBodyInfo(crate.GetUUID()).has_value());
			const std::vector<PhysicsEvent> groundExits = listener.Find(ground, PhysicsEventType::CollisionExit);
			REQUIRE(groundExits.size() == 1);
			CHECK(groundExits[0].Other == crate.GetUUID());
			CHECK(groundExits[0].Synthesized);
			CHECK(listener.Find(crate.GetUUID(), PhysicsEventType::CollisionExit).size() == 1);
		}

		TEST_CASE("PhysicsSystem: an exit callback that destroys an entity in contact still synthesizes that entity's exits")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Doomed", glm::vec3(-3.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			static_cast<void>(Test::AddBoxBody(scene, "Chained", glm::vec3(3.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			Scene& runtime = session->GetScene();
			const UUID ground = Test::GetEntityId(runtime, "/Ground");
			const UUID doomed = Test::GetEntityId(runtime, "/Doomed");
			const UUID chained = Test::GetEntityId(runtime, "/Chained");
			Test::RunTicks(*session, 30);
			listener.Events.clear();
			// The ground's exit from the doomed box destroys the chained box, which also rests on the ground, as a script's
			// OnCollisionExit would: the same flush must close that pair too.
			Scene* runtimePointer = &runtime;
			listener.OnEvent = [runtimePointer, doomed, chained](const PhysicsEvent& event)
			{
				if (event.Type != PhysicsEventType::CollisionExit || event.Other != doomed)
					return;
				Entity entity = runtimePointer->FindEntityByID(chained);
				if (entity.IsValid())
					runtimePointer->DestroyEntity(entity);
			};
			runtime.DestroyEntity(runtime.FindEntityByID(doomed));
			session->Tick();
			const std::vector<PhysicsEvent> exits = listener.Find(ground, PhysicsEventType::CollisionExit);
			REQUIRE(exits.size() == 2);
			CHECK(std::all_of(exits.begin(), exits.end(), [](const PhysicsEvent& event)
			{
				return event.Synthesized;
			}));
			CHECK(std::any_of(exits.begin(), exits.end(), [chained](const PhysicsEvent& event)
			{
				return event.Other == chained;
			}));
			CHECK_FALSE(session->GetPhysics().GetBodyInfo(chained).has_value());
			CHECK(session->GetPhysics().GetStats().BodyCount == 1);
		}

		TEST_CASE("PhysicsSystem: an entity destroyed earlier in the tick gets no enter and leaves no exit")
		{
			// The ball starts inside the zone; OnFixedUpdate of tick 0 destroys it, so the step's enter has a pending entity:
			// dropped for both sides, and nothing is synthesized at the flush (§9.4 step 3).
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity zone = Test::AddBoxBody(scene, "Zone", glm::vec3(0.0f), glm::vec3(2.0f), std::nullopt);
			zone.Patch<BoxColliderComponent>([](BoxColliderComponent& collider)
			{
				collider.IsTrigger = true;
			});
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f), 0.5f, BodyType::Dynamic));
			PhaseAction destroyer(PlaySessionPhase::FixedUpdate, 0, 0, [](PlaySession& session, uint64_t /*tick*/)
			{
				session.GetScene().DestroyEntity(session.GetScene().FindEntityByPath("/Ball"));
			});
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener, &destroyer);
			const UUID ball = Test::GetEntityId(session->GetScene(), "/Ball");
			session->Tick();
			CHECK(listener.Events.empty());
			CHECK_FALSE(session->GetPhysics().GetBodyInfo(ball).has_value());
		}

		TEST_CASE("PhysicsSystem: a rotated Dynamic body comes to rest and falls asleep")
		{
			// The teleport check compares the local values PostStep wrote, bit for bit; a comparison through the world matrix
			// would see rounding in the decomposed rotation and teleport (and wake) the box every tick.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			Entity box = Test::AddBoxBody(scene, "Box", glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
			box.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = glm::angleAxis(glm::radians(30.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			});
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 300);
			CHECK(session->GetPhysics().IsSleeping(box.GetUUID()).value_or(false));
		}

		TEST_CASE("PhysicsSystem: a Dynamic body under a moving parent stays where it is simulated")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity platform = Test::AddBoxBody(scene, "Platform", glm::vec3(0.0f), glm::vec3(1.0f, 0.1f, 1.0f), BodyType::Kinematic);
			Entity drone = Test::AddSphereBody(scene, "Drone", glm::vec3(0.0f, 5.0f, 0.0f), 0.25f, BodyType::Dynamic, platform);
			Test::PatchRigidBody(drone, [](RigidBodyComponent& body)
			{
				body.GravityFactor = 0.0f;
			});
			PhaseAction mover(PlaySessionPhase::FixedUpdate, 1, 120, [](PlaySession& session, uint64_t tick)
			{
				const UUID id = Test::GetEntityId(session.GetScene(), "/Platform");
				REQUIRE(session.GetPhysics().MoveKinematic(id, glm::vec3(static_cast<float>(tick) / 60.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)).has_value());
			});
			Test::ExpectLog movingParent(LogLevel::Warn, "PHYSICS_DYNAMIC_UNDER_MOVING_PARENT");
			Scope<PlaySession> session = StartSession(fixture, nullptr, &mover);
			Test::RunTicks(*session, 121);
			CHECK(Test::GetWorldPosition(*session, "/Platform").x == doctest::Approx(2.0f).epsilon(1.0e-3));
			// The parent's motion never moved the drone, and PostStep kept its entity where its (sleeping) body is.
			const glm::vec3 position = Test::GetWorldPosition(*session, "/Platform/Drone");
			CHECK(Test::ApproxEqual(position, glm::vec3(0.0f, 5.0f, 0.0f), 1.0e-4f));
			CHECK(Test::ApproxEqual(position, session->GetPhysics().GetBodyInfo(drone.GetUUID()).value_or(PhysicsBodyReport{}).Pose.Position, 1.0e-4f));
		}

		TEST_CASE("PhysicsSystem: a teleported Kinematic body is placed, not swept")
		{
			// A script teleports the platform in OnFixedUpdate (Transform write plus PlaySession::MarkTeleported, as
			// Transform:Teleport does): PreStep places it with SetPose, so the crate resting on it is not flung.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Platform", glm::vec3(0.0f, 1.0f, 0.0f), glm::vec3(2.0f, 0.1f, 2.0f), BodyType::Kinematic));
			static_cast<void>(Test::AddBoxBody(scene, "Crate", glm::vec3(0.0f, 1.6f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			PhaseAction teleporter(PlaySessionPhase::FixedUpdate, 60, 60, [](PlaySession& session, uint64_t /*tick*/)
			{
				Entity platform = session.GetScene().FindEntityByPath("/Platform");
				platform.GetComponent<TransformComponent>().Translation = glm::vec3(50.0f, 1.0f, 0.0f);
				session.MarkTeleported(platform);
			});
			Scope<PlaySession> session = StartSession(fixture, nullptr, &teleporter);
			Test::RunTicks(*session, 61);
			CHECK(Test::GetWorldPosition(*session, "/Platform").x == doctest::Approx(50.0f));
			const UUID crate = Test::GetEntityId(session->GetScene(), "/Crate");
			CHECK(std::abs(session->GetPhysics().GetLinearVelocity(crate).value_or(glm::vec3(100.0f)).x) < 0.1f);
			CHECK(std::abs(Test::GetWorldPosition(*session, "/Crate").x) < 0.1f);
		}

		TEST_CASE("PhysicsSystem: the listener hears each diagnostic raised at run time once")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddGround(fixture.GetScene()));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			// An entity created during play with every degree of freedom locked (M13 turns this into a script error).
			Result<Entity> locked = session->CreateEntity("Locked");
			REQUIRE(locked.has_value());
			locked->AddComponent<BoxColliderComponent>(BoxColliderComponent{});
			locked->AddComponent<RigidBodyComponent>(RigidBodyComponent{});
			Test::PatchRigidBody(*locked, [](RigidBodyComponent& body)
			{
				body.LockTranslation = glm::bvec3(true);
				body.LockRotation = glm::bvec3(true);
			});
			Test::ExpectLog allLocked(LogLevel::Error, "PHYSICS_ALL_DOFS_LOCKED");
			Test::RunTicks(*session, 3);
			REQUIRE(listener.Diagnostics.size() == 1);
			CHECK(listener.Diagnostics[0].Code == PhysicsAllDofsLockedCode);
			CHECK(listener.Diagnostics[0].Entity == locked->GetUUID());
			CHECK(session->GetPhysics().GetDiagnostics().size() == 1);
		}

		TEST_CASE("PhysicsSystem: events are sorted by UUID pair and type and reach both entities, lower UUID first")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			for (int ball = 0; ball < 8; ++ball)
				static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(static_cast<float>(ball) * 1.5f, 1.0f, 0.0f), 0.5f, BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			Test::RunTicks(*session, 60);
			REQUIRE(listener.Events.size() >= 16);
			// Events come in pairs (Self = lower UUID first), and the pairs in (lower, higher, type) order within a tick.
			for (size_t index = 0; index + 1 < listener.Events.size(); index += 2)
			{
				const PhysicsEvent& first = listener.Events[index];
				const PhysicsEvent& second = listener.Events[index + 1];
				CHECK(first.Self < first.Other);
				CHECK(second.Self == first.Other);
				CHECK(second.Other == first.Self);
				CHECK(second.Type == first.Type);
				if (index >= 2 && listener.Events[index - 2].Tick == first.Tick)
				{
					const PhysicsEvent& previous = listener.Events[index - 2];
					CHECK(std::tie(previous.Self, previous.Other, previous.Type) < std::tie(first.Self, first.Other, first.Type));
				}
			}
		}

		TEST_CASE("PhysicsSystem: body functions refuse missing bodies, wrong motion types and non-finite values")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f, 2.0f, 0.0f), 0.5f, BodyType::Dynamic));
			static_cast<void>(scene.CreateEntity("Empty"));
			Scope<PlaySession> session = StartSession(fixture);
			PhysicsSystem& physics = session->GetPhysics();
			const UUID ball = Test::GetEntityId(session->GetScene(), "/Ball");
			const UUID ground = Test::GetEntityId(session->GetScene(), "/Ground");
			const UUID empty = Test::GetEntityId(session->GetScene(), "/Empty");
			CHECK(physics.AddForce(empty, glm::vec3(1.0f)).error().GetCode() == ErrorCode::NotFound);
			CHECK(physics.AddForce(ground, glm::vec3(1.0f)).error().GetCode() == ErrorCode::InvalidState);
			CHECK(physics.MoveKinematic(ball, glm::vec3(0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)).error().GetCode() == ErrorCode::InvalidState);
			CHECK(physics.SetLinearVelocity(ground, glm::vec3(1.0f)).error().GetCode() == ErrorCode::InvalidState);
			const float nan = std::numeric_limits<float>::quiet_NaN();
			CHECK(physics.AddImpulse(ball, glm::vec3(nan)).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.Teleport(ball, glm::vec3(std::numeric_limits<float>::infinity()), std::nullopt).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.SetGravity(glm::vec3(nan)).error().GetCode() == ErrorCode::InvalidArgument);
			// Valid calls act.
			REQUIRE(physics.AddImpulse(ball, glm::vec3(2.0f, 0.0f, 0.0f)).has_value());
			CHECK(physics.GetLinearVelocity(ball).value_or(glm::vec3(0.0f)).x == doctest::Approx(2.0f));
			REQUIRE(physics.Teleport(ball, glm::vec3(0.0f, 8.0f, 0.0f), std::nullopt).has_value());
			CHECK(Test::GetWorldPosition(*session, "/Ball").y == doctest::Approx(8.0f));
			REQUIRE(physics.WakeUp(ball).has_value());
			CHECK_FALSE(physics.IsSleeping(ball).value_or(true));
		}

		TEST_CASE("PhysicsSystem: GetBodyInfo reports a compound child's owner, the layer and the active contacts")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddTrack(scene, 3, true);
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(1.0f, 1.0f, 0.0f), 0.5f, BodyType::Dynamic));
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 30);
			const Scene& runtime = session->GetScene();
			const UUID piece = Test::GetEntityId(runtime, "/Level/Piece[1]");
			const UUID level = Test::GetEntityId(runtime, "/Level");
			const UUID ball = Test::GetEntityId(runtime, "/Ball");
			const std::optional<PhysicsBodyReport> track = session->GetPhysics().GetBodyInfo(piece);
			REQUIRE(track.has_value());
			CHECK(track->Owner == level);
			CHECK(track->Colliders.size() == 3);
			CHECK(track->LayerName == "Default");
			REQUIRE(track->Contacts.size() == 1);
			CHECK(track->Contacts[0].Other == ball);
			CHECK(track->Contacts[0].Collider == piece);
			CHECK_FALSE(track->Contacts[0].IsTrigger);
			const std::optional<PhysicsBodyReport> rolling = session->GetPhysics().GetBodyInfo(ball);
			REQUIRE(rolling.has_value());
			CHECK(rolling->MotionType == PhysicsMotionType::Dynamic);
			CHECK_FALSE(rolling->Character.has_value());
		}

		TEST_CASE("PhysicsSystem: a body falling asleep keeps its pairs, and moving it away ends them")
		{
			// Jolt reports a sleeping pair's contacts as removed although they still touch; the system keeps them (dormant) until
			// a step that began with one of the bodies awake does not report them again.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Box", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			PhysicsSystem& physics = session->GetPhysics();
			const UUID box = Test::GetEntityId(session->GetScene(), "/Box");
			const UUID ground = Test::GetEntityId(session->GetScene(), "/Ground");
			Test::RunTicks(*session, 180);
			REQUIRE(physics.IsSleeping(box).value_or(false));
			CHECK(listener.Find(box, PhysicsEventType::CollisionEnter).size() == 1);
			CHECK(listener.Find(box, PhysicsEventType::CollisionExit).empty());
			const std::optional<PhysicsBodyReport> resting = physics.GetBodyInfo(box);
			REQUIRE(resting.has_value());
			REQUIRE(resting->Contacts.size() == 1);
			CHECK(resting->Contacts[0].Other == ground);
			CHECK(physics.GetStats().ContactPairCount == 1);

			// Lifted away, the box is awake for the next step, which reports no contact: a real exit, once per side.
			listener.Events.clear();
			REQUIRE(physics.Teleport(box, glm::vec3(0.0f, 10.0f, 0.0f), std::nullopt).has_value());
			Test::RunTicks(*session, 2);
			for (const UUID self : { box, ground })
			{
				const std::vector<PhysicsEvent> exits = listener.Find(self, PhysicsEventType::CollisionExit);
				REQUIRE(exits.size() == 1);
				CHECK_FALSE(exits[0].Synthesized);
			}
			CHECK(physics.GetStats().ContactPairCount == 0);
		}

		TEST_CASE("PhysicsSystem: moving a gathered collider or changing the MeshRenderer mesh it falls back to rebuilds the compound")
		{
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			AddTrack(scene, 1, true);
			Entity level = scene.FindEntityByPath("/Level");
			// A mesh piece without a Mesh of its own: it uses its MeshRenderer's unit cube, 5 m up.
			Entity mesh = scene.CreateEntity("MeshPiece", level);
			mesh.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(3.0f, 5.0f, 0.0f);
			});
			mesh.AddComponent<MeshRendererComponent>(MakeMeshRenderer(BuiltinAssetHandles::CubeMesh));
			mesh.AddComponent<MeshColliderComponent>(MeshColliderComponent{});
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture);
			specification.Assets = &assets.GetManager();
			Result<Scope<PlaySession>> started = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
			Scope<PlaySession> session = std::move(*started);
			PhysicsSystem& physics = session->GetPhysics();
			Scene& runtime = session->GetScene();
			const UUID levelId = level.GetUUID();
			const std::optional<Aabb> before = physics.GetBodyBounds(levelId);
			REQUIRE(before.has_value());
			CHECK(before->Min.x == doctest::Approx(-0.5f).epsilon(1.0e-3));
			CHECK(before->Max.y == doctest::Approx(5.5f).epsilon(1.0e-3));

			// A script moves the box piece: the compound follows at the next PreStep.
			runtime.FindEntityByPath("/Level/Piece").GetComponent<TransformComponent>().Translation = glm::vec3(-5.0f, 0.0f, 0.0f);
			session->Tick();
			const std::optional<Aabb> moved = physics.GetBodyBounds(levelId);
			REQUIRE(moved.has_value());
			CHECK(moved->Min.x == doctest::Approx(-5.5f).epsilon(1.0e-3));

			// The MeshRenderer now shows the flat unit plane: the mesh piece's top drops to y = 5.
			runtime.FindEntityByPath("/Level/MeshPiece").Patch<MeshRendererComponent>([](MeshRendererComponent& renderer)
			{
				renderer.Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::PlaneMesh);
			});
			session->Tick();
			const std::optional<Aabb> flattened = physics.GetBodyBounds(levelId);
			REQUIRE(flattened.has_value());
			CHECK(flattened->Max.y == doctest::Approx(5.0f).epsilon(1.0e-3));
			CHECK(physics.GetBodyInfo(levelId).value_or(PhysicsBodyReport{}).Colliders.size() == 2);
		}

		TEST_CASE("PhysicsSystem: a new version of a collider's mesh rebuilds its shape")
		{
			// §7.5 hot reload: the mesh shape cache keys shapes by asset version, and PreStep sees the version change.
			MainThreadQueue queue;
			JobSystem jobs(0, queue);
			PublishingAssetManager assets(jobs);
			const AssetHandle rockMesh(0x6d65736800000001ull);
			assets.Publish(rockMesh, MakeBoxMesh(0.5f));
			Test::SceneTestFixture fixture;
			Entity rock = fixture.GetScene().CreateEntity("Rock");
			rock.AddComponent<MeshColliderComponent>(MeshColliderComponent{ .Mesh = TypedAssetHandle<AssetType::Mesh>(rockMesh) });
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture);
			specification.Assets = &assets;
			Result<Scope<PlaySession>> started = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
			Scope<PlaySession> session = std::move(*started);
			PhysicsSystem& physics = session->GetPhysics();
			const std::optional<Aabb> before = physics.GetBodyBounds(rock.GetUUID());
			REQUIRE(before.has_value());
			CHECK(before->GetSize().x == doctest::Approx(1.0f).epsilon(1.0e-3));

			assets.Publish(rockMesh, MakeBoxMesh(2.0f));
			session->Tick();
			const std::optional<Aabb> reloaded = physics.GetBodyBounds(rock.GetUUID());
			REQUIRE(reloaded.has_value());
			CHECK(reloaded->GetSize().x == doctest::Approx(4.0f).epsilon(1.0e-3));
			CHECK(physics.GetBodyInfo(rock.GetUUID()).value_or(PhysicsBodyReport{}).Origin == PhysicsBodyOrigin::ImplicitStatic);
		}

		TEST_CASE("PhysicsSystem: bodies beyond the world's body limit are refused with PHYSICS_LIMIT_EXCEEDED")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			for (int body = 0; body < 5; ++body)
				static_cast<void>(Test::AddBoxBody(scene, "Box", glm::vec3(static_cast<float>(body) * 2.0f, 0.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			TransformSystem::Update(scene);
			PhysicsSystemSpecification specification;
			specification.RuntimeScene = &scene;
			specification.Limits.MaxBodies = 3;
			Test::ExpectLog limit(LogLevel::Error, "PHYSICS_LIMIT_EXCEEDED");
			Result<Scope<PhysicsSystem>> physics = PhysicsSystem::Create(specification);
			REQUIRE_MESSAGE(physics.has_value(), physics.error().ToString());
			CHECK((*physics)->GetStats().BodyCount == 3);
			// Each refused entity has its diagnostic (§9.1); only the first is logged.
			const std::span<const PhysicsDiagnostic> diagnostics = (*physics)->GetDiagnostics();
			REQUIRE(diagnostics.size() == 2);
			const std::vector<UUID> roots(scene.GetRootEntities().begin(), scene.GetRootEntities().end());
			for (const PhysicsDiagnostic& diagnostic : diagnostics)
			{
				CHECK(diagnostic.Code == PhysicsLimitExceededCode);
				CHECK(diagnostic.Severity == DiagnosticSeverity::Error);
				CHECK(diagnostic.Subject == "bodies");
			}
			CHECK(std::is_permutation(roots.begin() + 3, roots.end(), std::vector<UUID>{ diagnostics[0].Entity, diagnostics[1].Entity }.begin()));
			CHECK(limit.GetMatchCount() == 1);
		}

		TEST_CASE("PhysicsSystem: body functions refuse out-of-range values and non-unit rotations")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f, 2.0f, 0.0f), 0.5f, BodyType::Dynamic));
			static_cast<void>(Test::AddBoxBody(scene, "Platform", glm::vec3(0.0f), glm::vec3(1.0f, 0.1f, 1.0f), BodyType::Kinematic));
			Scope<PlaySession> session = StartSession(fixture);
			PhysicsSystem& physics = session->GetPhysics();
			const UUID ball = Test::GetEntityId(session->GetScene(), "/Ball");
			const UUID platform = Test::GetEntityId(session->GetScene(), "/Platform");
			CHECK(physics.AddForce(ball, glm::vec3(1.0e13f, 0.0f, 0.0f)).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.SetLinearVelocity(ball, glm::vec3(0.0f, -1.0e13f, 0.0f)).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.Teleport(ball, glm::vec3(2.0e9f, 0.0f, 0.0f), std::nullopt).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.Teleport(ball, glm::vec3(0.0f), glm::quat(2.0f, 0.0f, 0.0f, 0.0f)).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.MoveKinematic(platform, glm::vec3(1.0f, 0.0f, 0.0f), glm::quat(0.0f, 0.0f, 0.0f, 0.0f)).error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(physics.SetGravity(glm::vec3(0.0f, -1.0e13f, 0.0f)).error().GetCode() == ErrorCode::InvalidArgument);
			// A MoveKinematic sets the entity's pose at once; the body follows at the next step.
			REQUIRE(physics.MoveKinematic(platform, glm::vec3(1.0f, 0.0f, 0.0f), glm::quat(1.0f, 0.0f, 0.0f, 0.0f)).has_value());
			CHECK(Test::GetWorldPosition(*session, "/Platform").x == doctest::Approx(1.0f));
			session->Tick();
			CHECK(physics.GetBodyInfo(platform).value_or(PhysicsBodyReport{}).Pose.Position.x == doctest::Approx(1.0f).epsilon(1.0e-4));
			CHECK(physics.SetGravity(glm::vec3(0.0f, -1.62f, 0.0f)).has_value());
			CHECK(Test::ApproxEqual(physics.GetGravity(), glm::vec3(0.0f, -1.62f, 0.0f)));
		}

		TEST_CASE("PhysicsSystem: destroying the session destroys every body and the world")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Box", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			const uint32_t bodiesBefore = PhysicsEngine::GetLiveBodyCount();
			const uint32_t worldsBefore = PhysicsEngine::GetLiveWorldCount();
			{
				Scope<PlaySession> session = StartSession(fixture);
				Test::RunTicks(*session, 5);
				CHECK(PhysicsEngine::GetLiveBodyCount() == bodiesBefore + 2);
				CHECK(PhysicsEngine::GetLiveWorldCount() == worldsBefore + 1);
			}
			CHECK(PhysicsEngine::GetLiveBodyCount() == bodiesBefore);
			CHECK(PhysicsEngine::GetLiveWorldCount() == worldsBefore);
		}

		TEST_CASE("PhysicsSystem: a project gravity beyond MaxPhysicsGravity is refused when the session starts")
		{
			// The registry bounds Physics.Gravity; a value past it (a project file edited by hand) must not reach a step, where
			// one tick's velocity change would overflow and Jolt would assert.
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddSphereBody(fixture.GetScene(), "Ball", glm::vec3(0.0f, 5.0f, 0.0f), 0.5f, BodyType::Dynamic));
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture);
			specification.Project.Physics.Gravity = glm::vec3(0.0f, -1.0e37f, 0.0f);
			const Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_FALSE(session.has_value());
			CHECK(session.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(session.error().ToString().contains("Physics.Gravity"));
		}

		TEST_CASE("PhysicsSystem: a limit's warning and its error are both reported")
		{
			// Nine boxes rest on the ground (nine contacts, 90 % of the limit of ten: the warning), then three more (the contact
			// constraints overflow: the error). Both are PHYSICS_LIMIT_EXCEEDED on the same subject; the warning must not hide
			// the error.
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			for (int box = 0; box < 9; ++box)
				static_cast<void>(Test::AddBoxBody(scene, "Box", glm::vec3(static_cast<float>(box) * 2.0f - 20.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			TransformSystem::Update(scene);
			PhysicsSystemSpecification specification;
			specification.RuntimeScene = &scene;
			specification.Limits.MaxContactConstraints = 10;
			Result<Scope<PhysicsSystem>> created = PhysicsSystem::Create(specification);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PhysicsSystem& physics = **created;
			Test::RecordingPhysicsListener listener;
			physics.SetEventListener(&listener);
			const Test::ExpectLog warning(LogLevel::Warn, "PHYSICS_LIMIT_EXCEEDED");
			const Test::ExpectLog error(LogLevel::Error, "PHYSICS_LIMIT_EXCEEDED");
			uint64_t tick = 0;
			const auto run = [&scene, &physics, &tick](uint32_t ticks)
			{
				for (uint32_t index = 0; index < ticks; ++index, ++tick)
				{
					const SimStep step = SimStep::FromTick(tick, 1.0 / 60.0);
					TransformSystem::Update(scene);
					physics.PreStep(step);
					physics.Step(step);
					physics.PostStep(step);
					physics.FlushDestroyed(tick);
				}
			};
			run(5);
			for (int box = 9; box < 12; ++box)
				static_cast<void>(Test::AddBoxBody(scene, "Box", glm::vec3(static_cast<float>(box) * 2.0f - 20.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			run(5);

			const auto find = [](std::span<const PhysicsDiagnostic> diagnostics, DiagnosticSeverity severity, std::string_view subject)
			{
				return std::find_if(diagnostics.begin(), diagnostics.end(), [severity, subject](const PhysicsDiagnostic& diagnostic)
				{
					return diagnostic.Code == PhysicsLimitExceededCode && diagnostic.Severity == severity && (subject.empty() || diagnostic.Subject == subject);
				});
			};
			const std::span<const PhysicsDiagnostic> diagnostics = physics.GetDiagnostics();
			const auto overflow = find(diagnostics, DiagnosticSeverity::Error, {});
			REQUIRE(overflow != diagnostics.end());
			const std::string subject = overflow->Subject;
			CAPTURE(subject);
			CHECK(subject == "contactConstraints");
			CHECK(find(diagnostics, DiagnosticSeverity::Warning, subject) != diagnostics.end());
			const std::span<const PhysicsDiagnostic> heard = listener.Diagnostics;
			CHECK(find(heard, DiagnosticSeverity::Warning, subject) != heard.end());
			CHECK(find(heard, DiagnosticSeverity::Error, subject) != heard.end());
			CHECK(error.GetMatchCount() == 1);
			physics.SetEventListener(nullptr);
		}

		TEST_CASE("PhysicsSystem: a Static body moved or created onto a sleeping body wakes it, and their pair enters")
		{
			// A static body cannot wake, so the world wakes what it now touches (§9.3 "teleported and woken").
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Box", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			static_cast<void>(Test::AddBoxBody(scene, "Wall", glm::vec3(10.0f, 2.0f, 0.0f), glm::vec3(0.5f, 2.0f, 2.0f), BodyType::Static));
			Test::RecordingPhysicsListener listener;
			Scope<PlaySession> session = StartSession(fixture, &listener);
			PhysicsSystem& physics = session->GetPhysics();
			Scene& runtime = session->GetScene();
			const UUID box = Test::GetEntityId(runtime, "/Box");
			const UUID wall = Test::GetEntityId(runtime, "/Wall");
			Test::RunTicks(*session, 240);
			REQUIRE(physics.IsSleeping(box).value_or(false));
			listener.Events.clear();

			UUID mover;
			SUBCASE("a Transform write")
			{
				runtime.FindEntityByID(wall).GetComponent<TransformComponent>().Translation = glm::vec3(0.8f, 2.0f, 0.0f);
				mover = wall;
			}
			SUBCASE("Teleport")
			{
				REQUIRE(physics.Teleport(wall, glm::vec3(0.8f, 2.0f, 0.0f), std::nullopt).has_value());
				mover = wall;
			}
			SUBCASE("a new Static body")
			{
				Result<Entity> spawned = session->CreateEntity("Spawned");
				REQUIRE(spawned.has_value());
				spawned->GetComponent<TransformComponent>().Translation = glm::vec3(0.8f, 2.0f, 0.0f);
				spawned->AddComponent<BoxColliderComponent>(BoxColliderComponent{ .HalfExtents = glm::vec3(0.5f, 2.0f, 2.0f) });
				spawned->AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
				mover = spawned->GetUUID();
			}
			session->Tick();
			CHECK_FALSE(physics.IsSleeping(box).value_or(true));
			Test::RunTicks(*session, 30);
			CHECK(Test::GetWorldPosition(*session, "/Box").x < -0.1f);
			const std::vector<PhysicsEvent> entered = listener.Find(box, PhysicsEventType::CollisionEnter);
			CHECK(std::any_of(entered.begin(), entered.end(), [mover](const PhysicsEvent& event)
			{
				return event.Other == mover;
			}));
		}

		TEST_CASE("PhysicsSystem: a Kinematic body takes no velocity: it follows its Transform")
		{
			// Every PreStep moves a Kinematic body to its Transform (MoveKinematic), which sets its velocities: a velocity set
			// by a script would be overwritten before the step, so it is refused, and Initial* velocities do not apply.
			Test::SceneTestFixture fixture;
			Entity platform = Test::AddBoxBody(fixture.GetScene(), "Platform", glm::vec3(0.0f), glm::vec3(2.0f, 0.1f, 2.0f), BodyType::Kinematic);
			Test::PatchRigidBody(platform, [](RigidBodyComponent& body)
			{
				body.InitialLinearVelocity = glm::vec3(5.0f, 0.0f, 0.0f);
				body.InitialAngularVelocity = glm::vec3(0.0f, 3.0f, 0.0f);
			});
			Scope<PlaySession> session = StartSession(fixture);
			PhysicsSystem& physics = session->GetPhysics();
			const UUID id = platform.GetUUID();
			Test::RunTicks(*session, 30);
			CHECK(Test::ApproxEqual(Test::GetWorldPosition(*session, "/Platform"), glm::vec3(0.0f)));
			CHECK(physics.GetLinearVelocity(id).value_or(glm::vec3(1.0f)) == glm::vec3(0.0f));
			for (const Status& refused : { physics.SetLinearVelocity(id, glm::vec3(1.0f, 0.0f, 0.0f)), physics.SetAngularVelocity(id, glm::vec3(0.0f, 1.0f, 0.0f)) })
			{
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidState);
				CHECK(refused.error().GetHint().contains("MoveKinematic"));
			}
		}

		TEST_CASE("PhysicsSystem: a Transform written beyond the physics range is put back to the body's pose")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Sleeper", glm::vec3(0.0f, 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			static_cast<void>(Test::AddBoxBody(scene, "Platform", glm::vec3(10.0f, 3.0f, 0.0f), glm::vec3(1.0f, 0.1f, 1.0f), BodyType::Kinematic));
			// The platform moves along +X at 1 m/s from tick 240 on, until a script writes its Transform out of range at tick
			// 260; the sleeping box's Transform is written out of range at tick 250.
			PhaseAction script(PlaySessionPhase::FixedUpdate, 240, 260, [](PlaySession& session, uint64_t tick)
			{
				Entity platform = session.GetScene().FindEntityByPath("/Platform");
				if (tick == 260)
				{
					platform.GetComponent<TransformComponent>().Translation = glm::vec3(2.0f * MaxPhysicsCoordinate, 3.0f, 0.0f);
					return;
				}
				const glm::vec3 target(10.0f + static_cast<float>(tick - 239) / 60.0f, 3.0f, 0.0f);
				REQUIRE(session.GetPhysics().MoveKinematic(platform.GetUUID(), target, glm::quat(1.0f, 0.0f, 0.0f, 0.0f)).has_value());
				if (tick == 250)
					session.GetScene().FindEntityByPath("/Sleeper").GetComponent<TransformComponent>().Translation = glm::vec3(0.0f, -3.0e9f, 0.0f);
			});
			Scope<PlaySession> session = StartSession(fixture, nullptr, &script);
			PhysicsSystem& physics = session->GetPhysics();
			const UUID sleeper = Test::GetEntityId(session->GetScene(), "/Sleeper");
			const UUID platform = Test::GetEntityId(session->GetScene(), "/Platform");
			Test::RunTicks(*session, 240);
			REQUIRE(physics.IsSleeping(sleeper).value_or(false));
			const glm::vec3 resting = physics.GetBodyInfo(sleeper).value_or(PhysicsBodyReport{}).Pose.Position;

			// The sleeping body's write is not applied: it keeps sleeping where it was, and its entity shows it there again.
			Test::RunTicks(*session, 11);
			CHECK(physics.IsSleeping(sleeper).value_or(false));
			CHECK(physics.GetBodyInfo(sleeper).value_or(PhysicsBodyReport{}).Pose.Position == resting);
			CHECK(Test::ApproxEqual(Test::GetWorldPosition(*session, "/Sleeper"), resting, 1.0e-3f));

			// The moving platform's write is not applied either: it stops where its body is and stays there.
			Test::RunTicks(*session, 10);
			const glm::vec3 stopped = physics.GetBodyInfo(platform).value_or(PhysicsBodyReport{}).Pose.Position;
			CHECK(stopped.x == doctest::Approx(10.0f + 20.0f / 60.0f).epsilon(1.0e-3));
			CHECK(Test::ApproxEqual(Test::GetWorldPosition(*session, "/Platform"), stopped, 1.0e-4f));
			Test::RunTicks(*session, 30);
			CHECK(Test::ApproxEqual(physics.GetBodyInfo(platform).value_or(PhysicsBodyReport{}).Pose.Position, stopped, 1.0e-4f));
			CHECK(glm::length(physics.GetLinearVelocity(platform).value_or(glm::vec3(1.0f))) < 1.0e-4f);
		}

		TEST_CASE("PhysicsSystem: Teleport refuses a position its parent would carry beyond the range, changing nothing")
		{
			// The requested position is within MaxPhysicsCoordinate, but the local values written under a scaled and rotated
			// parent give back a world position that rounds past it (floats are 64 m apart there). Each case is either placed
			// within the range or refused with the Transform as it was; some of these parents round past the range.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity frame = scene.CreateEntity("Frame");
			static_cast<void>(Test::AddSphereBody(scene, "Ball", glm::vec3(0.0f), 0.5f, BodyType::Dynamic, frame));
			Scope<PlaySession> session = StartSession(fixture);
			PhysicsSystem& physics = session->GetPhysics();
			Scene& runtime = session->GetScene();
			Entity parent = runtime.FindEntityByPath("/Frame");
			Entity ball = runtime.FindEntityByPath("/Frame/Ball");
			uint32_t refusals = 0;
			for (int index = 0; index < 24; ++index)
			{
				CAPTURE(index);
				TransformComponent& transform = parent.GetComponent<TransformComponent>();
				transform.Scale = glm::vec3(1.0f + 0.37f * static_cast<float>(index));
				transform.Rotation = glm::angleAxis(glm::radians(7.0f * static_cast<float>(index)), glm::normalize(glm::vec3(0.0f, 0.3f, 1.0f)));
				const TransformComponent before = ball.GetComponent<TransformComponent>();
				const Status teleported = physics.Teleport(ball.GetUUID(), glm::vec3(MaxPhysicsCoordinate, 0.0f, 0.0f), std::nullopt);
				if (teleported.has_value())
				{
					CHECK(IsWithinPhysicsRange(physics.GetBodyInfo(ball.GetUUID()).value_or(PhysicsBodyReport{}).Pose.Position));
					continue;
				}
				++refusals;
				CHECK(teleported.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(ball.GetComponent<TransformComponent>().Translation == before.Translation);
				CHECK(ball.GetComponent<TransformComponent>().Rotation == before.Rotation);
			}
			CHECK(refusals > 0);
			// The session keeps simulating.
			Test::RunTicks(*session, 2);
			CHECK(physics.GetBodyInfo(ball.GetUUID()).has_value());
		}

		TEST_CASE("PhysicsSystem: a character falling past the physics range stops at its edge")
		{
			// At 5 Hz a character near the bottom of the range falls 100 m per tick once it reaches its terminal speed, which
			// would carry it past MaxPhysicsCoordinate; it stops at the edge instead and its inner body stays in the world.
			Test::SceneTestFixture fixture;
			Entity character = fixture.GetScene().CreateEntity("Character");
			character.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, -999999990.0f, 0.0f);
			});
			character.AddComponent<CharacterControllerComponent>(CharacterControllerComponent{});
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture);
			specification.Project.Simulation.FixedHz = 5;
			Result<Scope<PlaySession>> started = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
			Scope<PlaySession> session = std::move(*started);
			const Test::ExpectLog stopped(LogLevel::Warn, "would leave the");
			Test::RunTicks(*session, 150);
			CHECK(stopped.GetMatchCount() == 1);
			const std::optional<PhysicsBodyReport> report = session->GetPhysics().GetBodyInfo(character.GetUUID());
			REQUIRE(report.has_value());
			CHECK(IsWithinPhysicsRange(report->Pose.Position));
			CHECK(IsWithinPhysicsRange(Test::GetWorldPosition(*session, "/Character")));
		}

		TEST_CASE("PhysicsSystem: seeded random content, edits and calls never reach a Jolt assert")
		{
			// The play-session fuzz test of §9.1 and §15.2 for what M11 has: random scenes within the registry's rules (their
			// extremes included: the smallest and enormous colliders, the smallest and largest masses), random edits, body
			// function, query and bounds calls with NaN, infinities, zeros, huge values, values just inside the guards and stale
			// ids in every fixed update, and entities destroyed or disabled from inside physics callbacks. A Jolt assert ends the process (it reaches the
			// engine's assert handler), so passing is the proof; every function either succeeds or returns an error.
			Test::AssetTestFixture assets;
			assets.OpenProject();
			Test::ExpectLog diagnostics(LogLevel::Error, "PHYSICS_");
			for (uint64_t seed = 1; seed <= 24; ++seed)
			{
				CAPTURE(seed);
				PhysicsFuzzer fuzzer(seed);
				Test::SceneTestFixture fixture(seed);
				Scene& scene = fixture.GetScene();
				std::vector<Entity> entities;
				for (int index = 0; index < 28; ++index)
				{
					const bool child = !entities.empty() && fuzzer.Chance(45);
					Entity parent = child ? entities[static_cast<size_t>(fuzzer.Pick(static_cast<int>(entities.size())))] : Entity();
					Entity entity = parent.IsValid() ? scene.CreateEntity("Entity", parent) : scene.CreateEntity("Entity");
					AddRandomPhysics(fuzzer, entity, true);
					if (fuzzer.Chance(10))
						entity.SetActive(false);
					entities.push_back(entity);
				}
				// An unknown layer, so every run raises at least one Error diagnostic (the ExpectLog above).
				Entity unknown = Test::AddBoxBody(scene, "Unknown", glm::vec3(0.0f, -30.0f, 0.0f), glm::vec3(0.5f), BodyType::Static);
				Test::PatchRigidBody(unknown, [](RigidBodyComponent& body)
				{
					body.Layer = "Missing";
				});

				FuzzingObserver observer(fuzzer, 6);
				PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture, seed);
				specification.Observer = &observer;
				specification.Assets = &assets.GetManager();
				specification.Project.Physics.Layers = { "Default", "Track" };
				specification.Project.Physics.Collisions = { { "Default", "Default" }, { "Default", "Track" } };
				Result<Scope<PlaySession>> started = Test::StartPhysicsSession(fixture, specification);
				REQUIRE_MESSAGE(started.has_value(), started.error().ToString());
				Scope<PlaySession> session = std::move(*started);

				// Callbacks destroy and disable entities, as scripts may (§9.7 "destroy inside a callback is safe").
				Test::RecordingPhysicsListener listener;
				PlaySession* sessionPointer = session.get();
				listener.OnEvent = [sessionPointer, &fuzzer](const PhysicsEvent& event)
				{
					Entity self = sessionPointer->GetScene().FindEntityByID(event.Self);
					CHECK(self.IsValid());
					if (!self.IsValid())
						return;
					CHECK(self.IsActive());
					if (fuzzer.Chance(4))
						sessionPointer->GetScene().DestroyEntity(self);
					else if (fuzzer.Chance(4))
						self.SetActive(false);
				};
				session->GetPhysics().SetEventListener(&listener);
				Test::RunTicks(*session, 120);

				for (const PhysicsDiagnostic& diagnostic : session->GetPhysics().GetDiagnostics())
					CHECK(std::find(PhysicsDiagnosticCodes.begin(), PhysicsDiagnosticCodes.end(), diagnostic.Code) != PhysicsDiagnosticCodes.end());
				// After the flushes no body, character or pair remains for an entity that left play.
				for (const UUID id : session->GetScene().GetCanonicalOrder())
				{
					const std::optional<PhysicsBodyReport> report = session->GetPhysics().GetBodyInfo(id);
					if (!report.has_value())
						continue;
					CHECK(session->GetScene().FindEntityByID(report->Owner).IsValid());
					for (const PhysicsContactInfo& contact : report->Contacts)
						CHECK(session->GetScene().FindEntityByID(contact.Other).IsValid());
				}
				session->GetPhysics().SetEventListener(nullptr);
			}
		}
	}

}
