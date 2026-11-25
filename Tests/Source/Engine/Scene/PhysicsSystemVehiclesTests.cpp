#include "TestsPCH.h"

#include "Engine/Scene/PhysicsSystem.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/HeightfieldData.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Random.h"
#include "Engine/Physics/PhysicsDiagnostics.h"
#include "Engine/Physics/PhysicsEngine.h"
#include "Engine/Renderer/RenderStaticSet.h"
#include "Engine/Scene/Components/BoxColliderComponent.h"
#include "Engine/Scene/Components/ColliderSurfaceComponent.h"
#include "Engine/Scene/Components/HingeJointComponent.h"
#include "Engine/Scene/Components/MassPropertiesComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/StaticComponent.h"
#include "Engine/Scene/Components/TerrainColliderComponent.h"
#include "Engine/Scene/Components/TerrainComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/VehicleWheelComponent.h"
#include "Engine/Scene/Components/WheeledVehicleComponent.h"
#include "Engine/Scene/PhysicsVehicles.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"
#include "Support/ExpectLog.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/PhysicsTestScene.h"
#include "Support/TestTimeout.h"

#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <format>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The play session's vehicles (M44, Docs/Decisions/0062-m44-wheeled-vehicles.md decision 3; Architecture §9.11;
// PhysicsSystem.h's M44 paragraph): wheel poses (marked written for M43's change tracking since the landing), driver input
// and waking, kept wheel frames, settings in place, every body removal path, the grip by surface, PHYSICS_VEHICLE_INVALID at
// run time, the state hash's vehicle part, two histories stepping alike, the visits of a step (M43's EntitiesVisited, since
// the landing), and random vehicles and edits never reaching a Jolt assert. Through PlaySession (lockstep ticks, Play mode),
// but for the visits, counted on a PhysicsSystem stepped alone.

namespace Engine {

	namespace {

		constexpr std::array<std::string_view, 4> WheelNames = { "FL", "FR", "RL", "RR" };
		constexpr AssetHandle FlatHandle{ 0x5e7a44u };

		// A buggy: a 1200 kg box chassis (1.8 x 0.5 x 4 m) on four wheels of radius 0.4 m, front left, front right, rear left
		// and rear right (the front at -Z, the left at -X), each hanging 0.5 m (its SuspensionMaxLength) below the chassis's
		// centre height; the front ones steer 35 degrees, the rear ones are driven through one differential and hold the
		// hand brake, and both axles have an anti-roll bar.
		struct BuggyOptions
		{
			glm::vec3 Position = glm::vec3(0.0f, 0.8f, 0.0f);
			glm::vec3 Velocity = glm::vec3(0.0f);
			VehicleWheelCast Cast = VehicleWheelCast::Cylinder;
			VehicleTransmissionMode Transmission = VehicleTransmissionMode::Auto;
			bool AntiRollBars = true;
		};

		Entity AddBuggy(Scene& scene, std::string_view name, const BuggyOptions& options = {})
		{
			Entity body = Test::AddBoxBody(scene, name, options.Position, glm::vec3(0.9f, 0.25f, 2.0f), BodyType::Dynamic);
			Test::PatchRigidBody(body, [&options](RigidBodyComponent& rigidBody)
			{
				rigidBody.Mass = 1200.0f;
				rigidBody.InitialLinearVelocity = options.Velocity;
			});
			std::array<UUID, 4> wheels{};
			for (size_t index = 0; index < WheelNames.size(); ++index)
			{
				const bool front = index < 2;
				Entity wheel = scene.CreateEntity(WheelNames[index], body);
				wheel.Patch<TransformComponent>([index, front](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(index % 2 == 0 ? -0.9f : 0.9f, -0.5f, front ? -1.4f : 1.4f);
				});
				wheel.AddComponent<VehicleWheelComponent>(
					VehicleWheelComponent{ .Radius = 0.4f, .Width = 0.3f, .MaxSteerAngle = front ? 35.0f : 0.0f, .MaxHandBrakeTorque = front ? 0.0f : 4000.0f });
				wheels[index] = wheel.GetUUID();
			}
			WheeledVehicleComponent vehicle;
			vehicle.Transmission = options.Transmission;
			vehicle.WheelCast = options.Cast;
			vehicle.Differentials = { VehicleDifferential{ .LeftWheel = wheels[2], .RightWheel = wheels[3] } };
			if (options.AntiRollBars)
				vehicle.AntiRollBars = { VehicleAntiRollBar{ .LeftWheel = wheels[0], .RightWheel = wheels[1] }, VehicleAntiRollBar{ .LeftWheel = wheels[2], .RightWheel = wheels[3] } };
			body.AddComponent<WheeledVehicleComponent>(vehicle);
			return body;
		}

		Scope<PlaySession> StartSession(Test::SceneTestFixture& fixture, Test::InMemoryAssetManager* assets = nullptr)
		{
			PlaySessionSpecification specification = Test::MakePhysicsSessionSpecification(fixture);
			specification.Assets = assets;
			Result<Scope<PlaySession>> session = Test::StartPhysicsSession(fixture, specification);
			REQUIRE_MESSAGE(session.has_value(), session.error().ToString());
			return std::move(*session);
		}

		Entity Find(PlaySession& session, std::string_view path)
		{
			Entity entity = session.GetScene().FindEntityByPath(path);
			REQUIRE(entity.IsValid());
			return entity;
		}

		PhysicsVehicleReport GetVehicle(PlaySession& session, std::string_view path)
		{
			const std::optional<PhysicsVehicleReport> report = session.GetPhysics().GetVehicleInfo(Find(session, path).GetUUID());
			REQUIRE(report.has_value());
			return *report;
		}

		PhysicsBodyReport GetBody(PlaySession& session, std::string_view path)
		{
			const std::optional<PhysicsBodyReport> report = session.GetPhysics().GetBodyInfo(Find(session, path).GetUUID());
			REQUIRE(report.has_value());
			return *report;
		}

		void PatchVehicle(PlaySession& session, std::string_view path, const std::function<void(WheeledVehicleComponent&)>& patch)
		{
			Find(session, path).Patch<WheeledVehicleComponent>([&patch](WheeledVehicleComponent& vehicle)
			{
				patch(vehicle);
			});
		}

		// The digest of the physics block alone (PhysicsSystem::AppendStateHash).
		uint64_t HashPhysics(const PlaySession& session)
		{
			XXH64Hasher hasher(0);
			session.GetPhysics().AppendStateHash(hasher);
			return hasher.Digest();
		}

		bool SameBits(const glm::vec3& a, const glm::vec3& b)
		{
			return std::bit_cast<uint32_t>(a.x) == std::bit_cast<uint32_t>(b.x) && std::bit_cast<uint32_t>(a.y) == std::bit_cast<uint32_t>(b.y)
				&& std::bit_cast<uint32_t>(a.z) == std::bit_cast<uint32_t>(b.z);
		}

		bool SameBits(const glm::quat& a, const glm::quat& b)
		{
			return std::bit_cast<uint32_t>(a.w) == std::bit_cast<uint32_t>(b.w) && SameBits(glm::vec3(a.x, a.y, a.z), glm::vec3(b.x, b.y, b.z));
		}

		// The number of `code` diagnostics the session raised for the entity at `path` with `subject`.
		size_t CountDiagnostics(PlaySession& session, std::string_view path, std::string_view code, std::string_view subject)
		{
			const UUID id = Find(session, path).GetUUID();
			const std::span<const PhysicsDiagnostic> diagnostics = session.GetPhysics().GetDiagnostics();
			return static_cast<size_t>(std::count_if(diagnostics.begin(), diagnostics.end(), [id, code, subject](const PhysicsDiagnostic& diagnostic)
			{
				return diagnostic.Entity == id && diagnostic.Code == code && diagnostic.Subject == subject;
			}));
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("PhysicsVehicles: wheel entities take their wheel's suspension, steer and spin, written only when they change")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddGround(fixture.GetScene()));
			static_cast<void>(AddBuggy(fixture.GetScene(), "Buggy"));
			// The wheels' spin damped to nothing at every step: Jolt turns a wheel by its last spin while its vehicle sleeps, so
			// only a wheel that stops turning rests in one pose.
			const auto setDamping = [](Scene& scene, float damping)
			{
				for (const std::string_view name : WheelNames)
				{
					scene.FindEntityByPath(std::format("/Buggy/{}", name)).Patch<VehicleWheelComponent>([damping](VehicleWheelComponent& wheel)
					{
						wheel.AngularDamping = damping;
					});
				}
			};
			setDamping(fixture.GetScene(), 1000.0f);
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 600);
			REQUIRE(GetBody(*session, "/Buggy").IsSleeping);

			// At rest: each wheel hangs its suspension's length below its attachment point (the chassis's centre height), its
			// rotation its authored one with its spin.
			PhysicsVehicleReport vehicle = GetVehicle(*session, "/Buggy");
			REQUIRE(vehicle.Created);
			REQUIRE(vehicle.Wheels.size() == 4);
			for (size_t index = 0; index < WheelNames.size(); ++index)
			{
				INFO(WheelNames[index]);
				const PhysicsVehicleWheelReport& wheel = vehicle.Wheels[index];
				CHECK(wheel.Entity == Find(*session, std::format("/Buggy/{}", WheelNames[index])).GetUUID());
				CHECK(wheel.Touching);
				CHECK(wheel.SuspensionLength < 0.45f);
				const TransformComponent& transform = Find(*session, std::format("/Buggy/{}", WheelNames[index])).GetComponent<TransformComponent>();
				CHECK(transform.Translation.y == doctest::Approx(-wheel.SuspensionLength).epsilon(1.0e-4));
				CHECK(transform.Translation.x == doctest::Approx(index % 2 == 0 ? -0.9f : 0.9f).epsilon(1.0e-4));
				CHECK(transform.Translation.z == doctest::Approx(index < 2 ? -1.4f : 1.4f).epsilon(1.0e-4));
			}

			// Resting, the wheels' poses do not change, so nothing is written again: their bits stay. (The writes are direct, as
			// the bodies' write-back is; from M44's merge M43's change tracking marks only those that differ.)
			std::array<TransformComponent, 4> resting{};
			for (size_t index = 0; index < WheelNames.size(); ++index)
				resting[index] = Find(*session, std::format("/Buggy/{}", WheelNames[index])).GetComponent<TransformComponent>();
			Test::RunTicks(*session, 30);
			for (size_t index = 0; index < WheelNames.size(); ++index)
			{
				const TransformComponent& transform = Find(*session, std::format("/Buggy/{}", WheelNames[index])).GetComponent<TransformComponent>();
				CHECK(SameBits(transform.Translation, resting[index].Translation));
				CHECK(SameBits(transform.Rotation, resting[index].Rotation));
			}
			// The wheels free to spin again (a wheel's field: the vehicle is made again, at its kept frames).
			setDamping(session->GetScene(), 0.2f);
			Test::RunTicks(*session, 1);
			REQUIRE(GetVehicle(*session, "/Buggy").Created);

			// Full right lock: the front wheels turn 35 degrees to the right (their forward, local -Z, towards +X), the rear ones not.
			PatchVehicle(*session, "/Buggy", [](WheeledVehicleComponent& component)
			{
				component.Steering = 1.0f;
			});
			Test::RunTicks(*session, 2);
			vehicle = GetVehicle(*session, "/Buggy");
			for (size_t index = 0; index < WheelNames.size(); ++index)
			{
				INFO(WheelNames[index]);
				const float steer = index < 2 ? 35.0f : 0.0f;
				CHECK(vehicle.Wheels[index].SteerAngle == doctest::Approx(steer).epsilon(1.0e-3));
				const glm::quat rotation = Find(*session, std::format("/Buggy/{}", WheelNames[index])).GetComponent<TransformComponent>().Rotation;
				const glm::vec3 forward = rotation * glm::vec3(0.0f, 0.0f, -1.0f);
				const float turned = glm::degrees(std::atan2(forward.x, -forward.z));
				CHECK(turned == doctest::Approx(steer).epsilon(2.0e-3));
			}

			// Throttle: the driven rear wheels spin forward, and their entities turn with them about their axle.
			PatchVehicle(*session, "/Buggy", [](WheeledVehicleComponent& component)
			{
				component.Steering = 0.0f;
				component.Throttle = 1.0f;
			});
			Test::RunTicks(*session, 30);
			const glm::quat before = Find(*session, "/Buggy/RL").GetComponent<TransformComponent>().Rotation;
			const float spinBefore = GetVehicle(*session, "/Buggy").Wheels[2].Rotation;
			Test::RunTicks(*session, 1);
			vehicle = GetVehicle(*session, "/Buggy");
			CHECK(vehicle.Wheels[2].AngularVelocity > 0.0f);
			CHECK(vehicle.Wheels[3].AngularVelocity > 0.0f);
			CHECK(vehicle.Speed > 0.0f);
			CHECK(vehicle.Wheels[2].Rotation != spinBefore);
			CHECK_FALSE(SameBits(Find(*session, "/Buggy/RL").GetComponent<TransformComponent>().Rotation, before));
			// The spin is about the axle (local X): the wheel's X axis stays the chassis's.
			const glm::vec3 axle = Find(*session, "/Buggy/RL").GetComponent<TransformComponent>().Rotation * glm::vec3(1.0f, 0.0f, 0.0f);
			CHECK(axle.x == doctest::Approx(1.0f).epsilon(1.0e-3));
		}

		TEST_CASE("PhysicsVehicles: a wheel pose written in PostStep reaches the world-change list and the static cache")
		{
			// The landing of M44 over M43 (Docs/Decisions/0061-m43-static-scenery.md's W43.1 record; ADR 0060's ledger, the
			// PhysicsSystem.cpp row): PostStep writes the wheels' Transforms directly and marks each write
			// (Scene::MarkTransformWritten), so the next update recomputes the wheel's world matrix, a world-change list sees it
			// and the render static cache moves its item. A sleeping vehicle is not written back, but Jolt still turns its wheels
			// by their last spin: those ticks move the wheels alone, which only the wheels' own marks reveal.
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddGround(fixture.GetScene()));
			Entity buggy = AddBuggy(fixture.GetScene(), "Buggy");
			buggy.AddComponent<StaticComponent>();
			for (const std::string_view name : WheelNames)
				fixture.GetScene().FindEntityByPath(std::format("/Buggy/{}", name)).AddComponent<MeshRendererComponent>().Mesh = TypedAssetHandle<AssetType::Mesh>(BuiltinAssetHandles::CubeMesh);
			Scope<PlaySession> session = StartSession(fixture);
			Scene& scene = session->GetScene();
			const uint32_t list = scene.OpenWorldChangeList();
			Test::RunTicks(*session, 600);
			REQUIRE(GetBody(*session, "/Buggy").IsSleeping);

			// The static item of `wheel` (its MeshRenderer, held by the static set) and its world matrix there.
			const auto staticWorld = [&scene](const Entity& wheel) -> std::optional<glm::mat4>
			{
				const Ref<const RenderStaticSet> set = scene.RefreshStaticSet();
				REQUIRE(set != nullptr);
				for (const Ref<const RenderStaticCell>& cell : set->Cells)
				{
					for (size_t index = 0; index < cell->PickTable.size(); ++index)
					{
						if (cell->PickTable[index] == wheel.GetUUID())
							return cell->Items[index].World;
					}
				}
				return std::nullopt;
			};

			std::vector<entt::entity> changed;
			static_cast<void>(scene.TakeWorldChanges(list, changed));
			const TransformComponent chassis = Find(*session, "/Buggy").GetComponent<TransformComponent>();
			std::array<TransformComponent, 4> before{};
			for (size_t index = 0; index < WheelNames.size(); ++index)
				before[index] = Find(*session, std::format("/Buggy/{}", WheelNames[index])).GetComponent<TransformComponent>();
			Test::RunTicks(*session, 1);
			REQUIRE(GetBody(*session, "/Buggy").IsSleeping);
			const TransformComponent& after = Find(*session, "/Buggy").GetComponent<TransformComponent>();
			REQUIRE(SameBits(after.Translation, chassis.Translation));
			REQUIRE(SameBits(after.Rotation, chassis.Rotation));

			changed.clear();
			REQUIRE(scene.TakeWorldChanges(list, changed));
			size_t turned = 0;
			for (size_t index = 0; index < WheelNames.size(); ++index)
			{
				INFO(WheelNames[index]);
				const Entity wheel = Find(*session, std::format("/Buggy/{}", WheelNames[index]));
				const TransformComponent& transform = wheel.GetComponent<TransformComponent>();
				const bool moved = !SameBits(transform.Translation, before[index].Translation) || !SameBits(transform.Rotation, before[index].Rotation);
				// A wheel written this tick is in the list; its world matrix and its static item are the walked one's.
				CHECK((std::ranges::find(changed, wheel.GetHandle()) != changed.end()) == moved);
				const glm::mat4 walked = TransformSystem::ComputeWorldMatrix(wheel);
				CHECK(wheel.GetComponent<WorldTransformComponent>().Matrix == walked);
				const std::optional<glm::mat4> item = staticWorld(wheel);
				REQUIRE(item.has_value());
				CHECK(*item == walked);
				turned += moved ? 1 : 0;
			}
			// The ticks this case is about: the chassis rests while a wheel turns.
			CHECK(turned > 0);
			scene.CloseWorldChangeList(list);
		}

		// Phase 8's landing (ADR 0060's ledger, "Rules for every pass a later lander adds"; the merge review's finding 1): item 4b
		// and the wheel poses visit the vehicles alone (State::CollectVehicleOwners), as M43's "PhysicsSystem: a tick visits only
		// the moving bodies and the bodies whose transforms changed" counts the other passes. A braked buggy and three boxes asleep
		// on the ground: a box's step visits it once (the write-back of every Dynamic body's owner), the buggy's three times (the
		// write-back, 4b and its wheel poses), and the four wheels, the ground and plain entities never.
		TEST_CASE("PhysicsVehicles: a step visits each vehicle once in 4b and once in its wheel poses")
		{
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			Entity buggy = AddBuggy(scene, "Buggy");
			buggy.Patch<WheeledVehicleComponent>([](WheeledVehicleComponent& vehicle)
			{
				vehicle.Brake = 1.0f;
			});
			constexpr int BoxCount = 3;
			std::vector<UUID> sleepers = { buggy.GetUUID() };
			for (int index = 0; index < BoxCount; ++index)
				sleepers.push_back(Test::AddBoxBody(scene, "Box", glm::vec3(6.0f + 3.0f * static_cast<float>(index), 0.5f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic).GetUUID());
			for (int index = 0; index < 40; ++index)
			{
				Entity prop = scene.CreateEntity("Prop");
				prop.Patch<TransformComponent>([index](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(2.0f * static_cast<float>(index), 5.0f, 10.0f);
				});
			}
			TransformSystem::Update(scene);
			PhysicsSystemSpecification specification;
			specification.RuntimeScene = &scene;
			Result<Scope<PhysicsSystem>> created = PhysicsSystem::Create(specification);
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			PhysicsSystem& physics = **created;
			// The transform update, then one step: the entities the step's passes visited.
			const auto stepVisits = [&scene, &physics](uint64_t tick)
			{
				TransformSystem::Update(scene);
				const uint64_t visited = scene.GetEntitiesVisited();
				const SimStep step = SimStep::FromTick(tick, 1.0 / 60.0);
				physics.PreStep(step);
				physics.Step(step);
				physics.PostStep(step);
				return scene.GetEntitiesVisited() - visited;
			};
			const auto asleep = [&physics, &sleepers]()
			{
				return std::ranges::all_of(sleepers, [&physics](const UUID id)
				{
					const std::optional<PhysicsBodyReport> report = physics.GetBodyInfo(id);
					return report.has_value() && report->IsSleeping;
				});
			};

			uint64_t tick = 0;
			for (; tick < 1800 && !asleep(); ++tick)
				static_cast<void>(stepVisits(tick));
			REQUIRE(asleep());
			const std::optional<PhysicsVehicleReport> vehicle = physics.GetVehicleInfo(buggy.GetUUID());
			REQUIRE(vehicle.has_value());
			REQUIRE(vehicle->Created);
			for (int quiet = 0; quiet < 3; ++quiet)
				CHECK(stepVisits(tick++) == BoxCount + 3);
		}

		TEST_CASE("PhysicsVehicles: a driver-input write wakes a sleeping vehicle and composes nothing")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddGround(fixture.GetScene()));
			static_cast<void>(AddBuggy(fixture.GetScene(), "Buggy"));
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 600);
			REQUIRE(GetBody(*session, "/Buggy").IsSleeping);

			// A steering write alone (no throttle) is new input: the body wakes for it.
			PatchVehicle(*session, "/Buggy", [](WheeledVehicleComponent& component)
			{
				component.Steering = 0.5f;
			});
			Test::RunTicks(*session, 1);
			CHECK_FALSE(GetBody(*session, "/Buggy").IsSleeping);
			Test::RunTicks(*session, 600);
			REQUIRE(GetBody(*session, "/Buggy").IsSleeping);

			// A throttle wakes it and drives it forward (local -Z).
			const glm::vec3 start = Test::GetWorldPosition(*session, "/Buggy");
			PatchVehicle(*session, "/Buggy", [](WheeledVehicleComponent& component)
			{
				component.Throttle = 1.0f;
			});
			Test::RunTicks(*session, 60);
			CHECK(Test::GetWorldPosition(*session, "/Buggy").z < start.z - 1.0f);
			CHECK(GetVehicle(*session, "/Buggy").Speed > 2.0f);

			// The same input written once or at every tick ends in one state: a write of driver input changes the live vehicle
			// alone, never makes it again.
			const auto drive = [](bool everyTick)
			{
				Test::SceneTestFixture runFixture;
				static_cast<void>(Test::AddGround(runFixture.GetScene()));
				static_cast<void>(AddBuggy(runFixture.GetScene(), "Buggy"));
				Scope<PlaySession> run = StartSession(runFixture);
				for (int tick = 0; tick < 180; ++tick)
				{
					if (everyTick || tick == 0)
					{
						PatchVehicle(*run, "/Buggy", [](WheeledVehicleComponent& component)
						{
							component.Throttle = 0.7f;
							component.Steering = 0.25f;
						});
					}
					Test::RunTicks(*run, 1);
				}
				return run->ComputeStateHash();
			};
			CHECK(drive(true) == drive(false));
		}

		TEST_CASE("PhysicsVehicles: a vehicle keeps its wheels' frames when its body is created again")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddGround(fixture.GetScene()));
			static_cast<void>(AddBuggy(fixture.GetScene(), "Buggy"));
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 240);
			const float restHeight = Test::GetWorldPosition(*session, "/Buggy").y;
			REQUIRE(GetVehicle(*session, "/Buggy").Created);

			// Driving writes the wheels' Transforms (compressed, spun). A RigidBody change makes the body and its vehicle again
			// (here at the synchronization of a start flush, so no step runs in between), which keeps the frames formed at the start
			// and carries the RPM, the gear, the clutch and each wheel's spin and angle over exactly.
			PatchVehicle(*session, "/Buggy", [](WheeledVehicleComponent& component)
			{
				component.Throttle = 1.0f;
			});
			Test::RunTicks(*session, 60);
			const PhysicsVehicleReport driving = GetVehicle(*session, "/Buggy");
			REQUIRE(driving.Wheels[2].AngularVelocity > 100.0f);
			Test::PatchRigidBody(Find(*session, "/Buggy"), [](RigidBodyComponent& body)
			{
				body.AngularDamping = 0.06f;
			});
			session->GetPhysics().SynchronizeBodies();
			const PhysicsVehicleReport remade = GetVehicle(*session, "/Buggy");
			CHECK(remade.Created);
			CHECK(remade.EngineRpm == driving.EngineRpm);
			CHECK(remade.Gear == driving.Gear);
			CHECK(remade.Clutch == driving.Clutch);
			for (size_t index = 0; index < 4; ++index)
			{
				INFO(WheelNames[index]);
				CHECK(remade.Wheels[index].AngularVelocity == driving.Wheels[index].AngularVelocity);
				CHECK(remade.Wheels[index].Rotation == driving.Wheels[index].Rotation);
			}
			Test::RunTicks(*session, 1);
			CHECK(GetVehicle(*session, "/Buggy").EngineRpm > 0.5f * driving.EngineRpm);

			// Stopped, it rests at the height it rested at before: the wheels hang where they were authored, not where the engine
			// last wrote them (which would lift the chassis by the suspension's compression).
			PatchVehicle(*session, "/Buggy", [](WheeledVehicleComponent& component)
			{
				component.Throttle = 0.0f;
				component.Brake = 1.0f;
			});
			Test::RunTicks(*session, 300);
			CHECK(Test::GetWorldPosition(*session, "/Buggy").y == doctest::Approx(restHeight).epsilon(0.002));

			// Disabled and enabled again, it comes back at the same frames, and the body at its rest height.
			Find(*session, "/Buggy").SetActive(false);
			Test::RunTicks(*session, 2);
			CHECK_FALSE(GetVehicle(*session, "/Buggy").Created);
			Find(*session, "/Buggy").SetActive(true);
			Test::RunTicks(*session, 300);
			CHECK(GetVehicle(*session, "/Buggy").Created);
			CHECK(Test::GetWorldPosition(*session, "/Buggy").y == doctest::Approx(restHeight).epsilon(0.002));
		}

		TEST_CASE("PhysicsVehicles: a wheel keeps its frame through a reparent and its components added back")
		{
			// The W44.2 review's finding 1 and the lead's amendment of decision 3: a wheel's kept frame lasts the session while the
			// wheel belongs to its vehicle, so a frame is never formed again from the Transform the engine wrote (its spin, steer and
			// compression). Each path happens while the rear left wheel's spin reads between 100 and 260 degrees, where a frame formed
			// from its written pose would point its forward backward, and its suspension is compressed.
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddGround(fixture.GetScene()));
			static_cast<void>(AddBuggy(fixture.GetScene(), "Buggy"));
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 240);
			const float restHeight = Test::GetWorldPosition(*session, "/Buggy").y;
			const glm::vec3 heading = GetBody(*session, "/Buggy").Pose.Rotation * glm::vec3(0.0f, 0.0f, -1.0f);
			PatchVehicle(*session, "/Buggy", [](WheeledVehicleComponent& component)
			{
				component.Throttle = 1.0f;
			});
			bool turned = false;
			for (int tick = 0; tick < 600 && !turned; ++tick)
			{
				Test::RunTicks(*session, 1);
				const float rotation = GetVehicle(*session, "/Buggy").Wheels[2].Rotation;
				turned = rotation > 100.0f && rotation < 260.0f;
			}
			REQUIRE(turned);
			REQUIRE(GetVehicle(*session, "/Buggy").Wheels[2].SuspensionLength < 0.49f);

			SUBCASE("a reparent under a new axle of the vehicle")
			{
				Entity axle = session->GetScene().CreateEntity("Axle", Find(*session, "/Buggy"));
				Entity wheel = Find(*session, "/Buggy/RL");
				REQUIRE(session->GetScene().SetParent(wheel, axle, {}, false).has_value());
				// A reparent below the vehicle changes no body; a write to the wheel then makes the vehicle again, which keeps the
				// wheel's frame rather than form one from its written pose under the new chain.
				wheel.Patch<VehicleWheelComponent>([](VehicleWheelComponent& wheel)
				{
					wheel.AngularDamping = 0.25f;
				});
				Test::RunTicks(*session, 1);
			}
			SUBCASE("its VehicleWheel removed and added back")
			{
				const Test::ExpectLog invalid(LogLevel::Error, "PHYSICS_VEHICLE_INVALID");
				Entity wheel = Find(*session, "/Buggy/RL");
				const VehicleWheelComponent component = wheel.GetComponent<VehicleWheelComponent>();
				wheel.RemoveComponent<VehicleWheelComponent>();
				// Without it, the differential and the bar name an entity that is not a wheel: the vehicle is refused meanwhile.
				Test::RunTicks(*session, 1);
				CHECK_FALSE(GetVehicle(*session, "/Buggy").Created);
				wheel.AddComponent<VehicleWheelComponent>(component);
				Test::RunTicks(*session, 1);
			}
			SUBCASE("the vehicle's WheeledVehicle removed and added back")
			{
				Entity buggy = Find(*session, "/Buggy");
				const WheeledVehicleComponent component = buggy.GetComponent<WheeledVehicleComponent>();
				buggy.RemoveComponent<WheeledVehicleComponent>();
				Test::RunTicks(*session, 1);
				buggy.AddComponent<WheeledVehicleComponent>(component);
				Test::RunTicks(*session, 1);
			}
			REQUIRE(GetVehicle(*session, "/Buggy").Created);
			REQUIRE(GetVehicle(*session, "/Buggy").Wheels.size() == 4);

			// Braked to rest, the chassis rides at its first rest height: no wheel hangs from a compressed attachment.
			PatchVehicle(*session, "/Buggy", [](WheeledVehicleComponent& component)
			{
				component.Throttle = 0.0f;
				component.Brake = 1.0f;
			});
			Test::RunTicks(*session, 300);
			CHECK(Test::GetWorldPosition(*session, "/Buggy").y == doctest::Approx(restHeight).epsilon(0.002));
			// Then at full throttle it drives forward, straight: no wheel's drive points backward or is toed.
			PatchVehicle(*session, "/Buggy", [](WheeledVehicleComponent& component)
			{
				component.Throttle = 1.0f;
				component.Brake = 0.0f;
			});
			Test::RunTicks(*session, 60);
			CHECK(GetVehicle(*session, "/Buggy").Speed > 2.0f);
			const glm::vec3 now = GetBody(*session, "/Buggy").Pose.Rotation * glm::vec3(0.0f, 0.0f, -1.0f);
			CHECK(glm::dot(now, heading) > std::cos(glm::radians(5.0f)));
		}

		TEST_CASE("PhysicsVehicles: wheels are placed where the body holds them, under a scaled parent and through an axle")
		{
			// The W44.2 review's finding 2: the body bakes its entity's world scale, so the wheels' frames and their placement take
			// the vehicle's world scale, not its local one. A buggy under a group scaled 2, and one whose front left wheel hangs
			// below an axle entity; ray casts put each contact straight under its wheel.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			Entity group = scene.CreateEntity("Group");
			group.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Scale = glm::vec3(2.0f);
			});
			Entity scaled = AddBuggy(scene, "Scaled", BuggyOptions{ .Position = glm::vec3(0.0f, 0.7f, 0.0f), .Cast = VehicleWheelCast::Ray, .AntiRollBars = false });
			REQUIRE(scene.SetParent(scaled, group, {}, false).has_value());
			Entity axled = AddBuggy(scene, "Axled", BuggyOptions{ .Position = glm::vec3(20.0f, 0.8f, 0.0f), .Cast = VehicleWheelCast::Ray, .AntiRollBars = false });
			Entity axle = scene.CreateEntity("Axle", axled);
			axle.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, -0.25f, -1.4f);
			});
			Entity front = scene.FindEntityByPath("/Axled/FL");
			REQUIRE(scene.SetParent(front, axle, {}, false).has_value());
			front.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(-0.9f, -0.25f, 0.0f);
			});
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 240);

			for (const std::string_view vehicle : { "/Group/Scaled", "/Axled" })
			{
				INFO(vehicle);
				const PhysicsVehicleReport report = GetVehicle(*session, vehicle);
				REQUIRE(report.Created);
				const PhysicsPose pose = GetBody(*session, vehicle).Pose;
				const float scale = vehicle == "/Group/Scaled" ? 2.0f : 1.0f;
				for (const PhysicsVehicleWheelReport& wheel : report.Wheels)
				{
					const ConstEntity entity = session->GetScene().FindEntityByID(wheel.Entity);
					INFO(entity.GetName());
					REQUIRE(wheel.Touching);
					// The wheel entity's world centre stands on its contact, one Radius above it, at its authored place scaled by the
					// vehicle's world scale.
					const glm::vec3 centre = glm::vec3(TransformSystem::ComputeWorldMatrix(entity)[3]);
					CHECK(centre.x == doctest::Approx(wheel.Point.x).epsilon(1.0e-4));
					CHECK(centre.z == doctest::Approx(wheel.Point.z).epsilon(1.0e-4));
					CHECK(centre.y - wheel.Point.y == doctest::Approx(0.4f).epsilon(1.0e-3));
					const glm::vec3 local = glm::inverse(pose.Rotation) * (centre - pose.Position);
					CHECK(std::abs(local.x) == doctest::Approx(0.9f * scale).epsilon(1.0e-3));
					CHECK(std::abs(local.z) == doctest::Approx(1.4f * scale).epsilon(1.0e-3));
				}
			}
		}

		TEST_CASE("PhysicsVehicles: a vehicle whose disabled wheels leave it undriven raises PHYSICS_VEHICLE_INVALID in play")
		{
			// The W44.2 review's finding 3: a vehicle never silently fails to exist. Disabling the driven rear wheels leaves no
			// wheel the differential drives; the session reports it once, naming them, and the vehicle returns with its wheels.
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddGround(fixture.GetScene()));
			static_cast<void>(AddBuggy(fixture.GetScene(), "Buggy"));
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 30);
			REQUIRE(GetVehicle(*session, "/Buggy").Created);
			{
				const Test::ExpectLog undriven(LogLevel::Error, "its driven wheels 'RL' and 'RR' are disabled");
				Find(*session, "/Buggy/RL").SetActive(false);
				Find(*session, "/Buggy/RR").SetActive(false);
				Test::RunTicks(*session, 30);
				CHECK_FALSE(GetVehicle(*session, "/Buggy").Created);
				CHECK(CountDiagnostics(*session, "/Buggy", PhysicsVehicleInvalidCode, "differential") == 1);
			}
			Find(*session, "/Buggy/RL").SetActive(true);
			Find(*session, "/Buggy/RR").SetActive(true);
			Test::RunTicks(*session, 2);
			CHECK(GetVehicle(*session, "/Buggy").Created);
			CHECK(GetVehicle(*session, "/Buggy").Wheels.size() == 4);
		}

		TEST_CASE("PhysicsVehicles: an anti-roll bar's stiffness changes in place, keeping the vehicle's motion, at one hash with 0, 1 and 8 Jolt threads")
		{
			// The lead's rule after the W44.1 review (finding 2): a bar's Stiffness on the same wheels is written in place
			// (PhysicsWorld::SetVehicleSettings), so the body's velocity and the wheels' spin carry on, and what follows is the same
			// at every Jolt thread count. Twenty-four buggies, since Jolt steps vehicles in jobs of eight (decision 4.7).
			Test::SetTestCaseTimeLimit(240.0);
			const auto run = [](uint32_t threads, bool check)
			{
				const Test::ScopedWorkerThreadCount count(threads);
				Test::SceneTestFixture fixture;
				Scene& scene = fixture.GetScene();
				static_cast<void>(Test::AddBoxBody(scene, "Ground", glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(200.0f, 0.5f, 200.0f), BodyType::Static));
				for (int index = 0; index < 24; ++index)
				{
					static_cast<void>(AddBuggy(scene, std::format("Buggy{}", index),
						BuggyOptions{ .Position = glm::vec3(static_cast<float>(index % 6) * 12.0f - 30.0f, 0.8f, static_cast<float>(index / 6) * 20.0f - 30.0f) }));
				}
				Scope<PlaySession> session = StartSession(fixture);
				for (int index = 0; index < 24; ++index)
				{
					PatchVehicle(*session, std::format("/Buggy{}", index), [index](WheeledVehicleComponent& component)
					{
						component.Throttle = 1.0f;
						component.Steering = index % 2 == 0 ? 0.6f : -0.6f;
					});
				}
				Test::RunTicks(*session, 90);
				std::vector<PhysicsVehicleReport> before;
				std::vector<glm::vec3> velocities;
				for (int index = 0; index < 24; ++index)
				{
					before.push_back(GetVehicle(*session, std::format("/Buggy{}", index)));
					velocities.push_back(GetBody(*session, std::format("/Buggy{}", index)).LinearVelocity);
					PatchVehicle(*session, std::format("/Buggy{}", index), [](WheeledVehicleComponent& component)
					{
						for (VehicleAntiRollBar& bar : component.AntiRollBars)
							bar.Stiffness = 20000.0f;
					});
				}
				// Applied at a synchronization (a start flush's: no step runs in between), the change leaves every vehicle as it
				// was: its body's velocity, its engine and each wheel's spin read exactly as before.
				session->GetPhysics().SynchronizeBodies();
				if (check)
				{
					for (size_t index = 0; index < 24; ++index)
					{
						INFO("buggy " << index);
						const PhysicsVehicleReport after = GetVehicle(*session, std::format("/Buggy{}", index));
						CHECK(after.Created);
						CHECK(GetBody(*session, std::format("/Buggy{}", index)).LinearVelocity == velocities[index]);
						CHECK(after.EngineRpm == before[index].EngineRpm);
						CHECK(after.Gear == before[index].Gear);
						for (size_t wheel = 0; wheel < 4; ++wheel)
							CHECK(after.Wheels[wheel].AngularVelocity == before[index].Wheels[wheel].AngularVelocity);
					}
				}
				Test::RunTicks(*session, 60);
				return session->ComputeStateHash();
			};
			const uint64_t none = run(0, true);
			CHECK(run(1, false) == none);
			CHECK(run(8, false) == none);
		}

		TEST_CASE("PhysicsVehicles: a removed anti-roll bar makes the vehicle again and leaves no stale force")
		{
			// The W44.1 review's finding 2: Jolt writes a wheel's anti-roll impulse only while a bar covers the wheel, and keeps it
			// otherwise. A buggy turning hard on stiff bars, whose bars are then removed, is made again without them: stopped on
			// flat ground, it rests level, each axle's two suspensions alike, as one that never had bars.
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddBoxBody(fixture.GetScene(), "Ground", glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(200.0f, 0.5f, 200.0f), BodyType::Static));
			Entity buggy = AddBuggy(fixture.GetScene(), "Buggy");
			buggy.Patch<WheeledVehicleComponent>([](WheeledVehicleComponent& vehicle)
			{
				for (VehicleAntiRollBar& bar : vehicle.AntiRollBars)
					bar.Stiffness = 50000.0f;
			});
			// Its centre of mass lowered, so it turns without rolling over.
			buggy.AddComponent<MassPropertiesComponent>(MassPropertiesComponent{ .CenterOfMassOffset = glm::vec3(0.0f, -0.5f, 0.0f) });
			Scope<PlaySession> session = StartSession(fixture);
			PatchVehicle(*session, "/Buggy", [](WheeledVehicleComponent& component)
			{
				component.Throttle = 0.6f;
				component.Steering = 0.4f;
			});
			// Turning, the stiff bars carry much of the roll while every wheel touches: their impulses would stay on the wheels,
			// stale, unless the vehicle is made again. Removed at the first such tick, once it is moving.
			bool leaning = false;
			for (int tick = 0; tick < 600 && !leaning; ++tick)
			{
				Test::RunTicks(*session, 1);
				const PhysicsVehicleReport turning = GetVehicle(*session, "/Buggy");
				const bool touching = std::all_of(turning.Wheels.begin(), turning.Wheels.end(), [](const PhysicsVehicleWheelReport& wheel)
				{
					return wheel.Touching;
				});
				leaning = turning.Speed > 2.0f && touching && std::abs(turning.Wheels[0].SuspensionLength - turning.Wheels[1].SuspensionLength) > 0.005f;
			}
			REQUIRE(leaning);
			PatchVehicle(*session, "/Buggy", [](WheeledVehicleComponent& component)
			{
				component.AntiRollBars.clear();
				component.Throttle = 0.0f;
				component.Steering = 0.0f;
				component.Brake = 1.0f;
			});
			Test::RunTicks(*session, 1);
			// Made again with its motion: still moving, its driven wheels still turning.
			CHECK(GetVehicle(*session, "/Buggy").Created);
			CHECK(GetVehicle(*session, "/Buggy").Speed > 1.0f);
			Test::RunTicks(*session, 400);
			const PhysicsVehicleReport resting = GetVehicle(*session, "/Buggy");
			CHECK(std::abs(resting.Speed) < 0.05f);
			CHECK(resting.Wheels[0].SuspensionLength == doctest::Approx(resting.Wheels[1].SuspensionLength).epsilon(0.005));
			CHECK(resting.Wheels[2].SuspensionLength == doctest::Approx(resting.Wheels[3].SuspensionLength).epsilon(0.005));
			const glm::vec3 up = GetBody(*session, "/Buggy").Pose.Rotation * glm::vec3(0.0f, 1.0f, 0.0f);
			CHECK(up.y > 0.9999f);
		}

		TEST_CASE("PhysicsVehicles: every path that removes a vehicle's body removes its vehicle first")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddGround(fixture.GetScene()));
			static_cast<void>(AddBuggy(fixture.GetScene(), "Buggy"));
			Scope<PlaySession> session = StartSession(fixture);
			PatchVehicle(*session, "/Buggy", [](WheeledVehicleComponent& component)
			{
				component.Throttle = 0.5f;
			});
			Test::RunTicks(*session, 30);
			REQUIRE(GetVehicle(*session, "/Buggy").Created);

			SUBCASE("a RigidBody change makes the body and its vehicle again")
			{
				Test::PatchRigidBody(Find(*session, "/Buggy"), [](RigidBodyComponent& body)
				{
					body.Mass = 1000.0f;
				});
				Test::RunTicks(*session, 30);
				CHECK(GetVehicle(*session, "/Buggy").Created);
				CHECK(GetVehicle(*session, "/Buggy").Speed > 0.5f);
			}
			SUBCASE("a body refused after an edit loses its vehicle until it is accepted again")
			{
				const Test::ExpectLog refused(LogLevel::Error, "PHYSICS_ALL_DOFS_LOCKED");
				Test::PatchRigidBody(Find(*session, "/Buggy"), [](RigidBodyComponent& body)
				{
					body.LockTranslation = glm::bvec3(true);
					body.LockRotation = glm::bvec3(true);
				});
				Test::RunTicks(*session, 30);
				CHECK_FALSE(GetVehicle(*session, "/Buggy").Created);
				Test::PatchRigidBody(Find(*session, "/Buggy"), [](RigidBodyComponent& body)
				{
					body.LockTranslation = glm::bvec3(false);
					body.LockRotation = glm::bvec3(false);
				});
				Test::RunTicks(*session, 30);
				CHECK(GetVehicle(*session, "/Buggy").Created);
			}
			SUBCASE("a disabled vehicle goes at the flush and comes back")
			{
				Find(*session, "/Buggy").SetActive(false);
				Test::RunTicks(*session, 30);
				CHECK_FALSE(GetVehicle(*session, "/Buggy").Created);
				Find(*session, "/Buggy").SetActive(true);
				Test::RunTicks(*session, 30);
				CHECK(GetVehicle(*session, "/Buggy").Created);
				// A disabled wheel leaves the vehicle, made again on three wheels, and returns to it.
				Find(*session, "/Buggy/FL").SetActive(false);
				Test::RunTicks(*session, 2);
				CHECK(GetVehicle(*session, "/Buggy").Created);
				CHECK(GetVehicle(*session, "/Buggy").Wheels.size() == 3);
				Find(*session, "/Buggy/FL").SetActive(true);
				Test::RunTicks(*session, 2);
				CHECK(GetVehicle(*session, "/Buggy").Wheels.size() == 4);
			}
			SUBCASE("a destroyed vehicle goes at the flush")
			{
				const UUID id = Find(*session, "/Buggy").GetUUID();
				session->GetScene().DestroyEntity(Find(*session, "/Buggy"));
				Test::RunTicks(*session, 30);
				CHECK_FALSE(session->GetPhysics().GetVehicleInfo(id).has_value());
			}
			SUBCASE("a destroyed wheel a differential names makes its vehicle invalid")
			{
				const Test::ExpectLog dangling(LogLevel::Error, "PHYSICS_VEHICLE_INVALID");
				session->GetScene().DestroyEntity(Find(*session, "/Buggy/RL"));
				Test::RunTicks(*session, 2);
				CHECK_FALSE(GetVehicle(*session, "/Buggy").Created);
				CHECK(CountDiagnostics(*session, "/Buggy", PhysicsVehicleInvalidCode, "differential") == 1);
			}
			SUBCASE("a destroyed wheel nothing names leaves its vehicle made again on the other wheels")
			{
				// The W44.2 review's nit 12: a buggy without anti-roll bars, whose front left wheel no differential or bar names.
				static_cast<void>(AddBuggy(session->GetScene(), "Bare", BuggyOptions{ .Position = glm::vec3(10.0f, 0.8f, 0.0f), .AntiRollBars = false }));
				Test::RunTicks(*session, 2);
				REQUIRE(GetVehicle(*session, "/Bare").Created);
				REQUIRE(GetVehicle(*session, "/Bare").Wheels.size() == 4);
				session->GetScene().DestroyEntity(Find(*session, "/Bare/FL"));
				Test::RunTicks(*session, 2);
				const PhysicsVehicleReport bare = GetVehicle(*session, "/Bare");
				CHECK(bare.Created);
				CHECK(bare.Wheels.size() == 3);
				CHECK(CountDiagnostics(*session, "/Bare", PhysicsVehicleInvalidCode, "differential") == 0);
			}
			SUBCASE("the session's end destroys the vehicles before the bodies")
			{
				const uint32_t bodies = PhysicsEngine::GetLiveBodyCount();
				const uint32_t worlds = PhysicsEngine::GetLiveWorldCount();
				REQUIRE(bodies >= 2);
				session.reset();
				CHECK(PhysicsEngine::GetLiveBodyCount() == bodies - 2);
				CHECK(PhysicsEngine::GetLiveWorldCount() == worlds - 1);
			}
		}

		TEST_CASE("PhysicsVehicles: the same buggy stops shorter on a rough surface than on a slick one, a ColliderSurface's and a TerrainCollider's alike")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Test::InMemoryAssetManager assets;
			assets.Publish(FlatHandle, CreateRef<HeightfieldData>(MakeFlatHeightfield(9)), "Assets/Terrain/Flat.heightfield");
			// Four lanes 8 m wide and 60 m long, their tops at y = 0: two static boxes with ColliderSurfaces and two flat
			// terrains, slick (0.3) and rough (1.0) each.
			const std::array<float, 4> lanes = { -18.0f, -6.0f, 6.0f, 18.0f };
			for (size_t index = 0; index < 2; ++index)
			{
				Entity lane = Test::AddBoxBody(scene, std::format("Box{}", index), glm::vec3(lanes[index], -0.5f, -20.0f), glm::vec3(4.0f, 0.5f, 30.0f), BodyType::Static);
				lane.AddComponent<ColliderSurfaceComponent>(ColliderSurfaceComponent{ .Friction = index == 0 ? 0.3f : 1.0f });
			}
			for (size_t index = 2; index < 4; ++index)
			{
				Entity lane = scene.CreateEntity(std::format("Terrain{}", index));
				lane.Patch<TransformComponent>([&lanes, index](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(lanes[index] - 4.0f, 0.0f, -50.0f);
				});
				TerrainComponent terrain;
				terrain.Heightfield.SetHandle(FlatHandle);
				terrain.Size = glm::vec2(8.0f, 60.0f);
				terrain.HeightScale = 1.0f;
				lane.AddComponent<TerrainComponent>(terrain);
				lane.AddComponent<TerrainColliderComponent>(TerrainColliderComponent{ .Friction = index == 2 ? 0.3f : 1.0f });
			}
			for (size_t index = 0; index < 4; ++index)
			{
				Entity buggy = AddBuggy(scene, std::format("Buggy{}", index),
					BuggyOptions{ .Position = glm::vec3(lanes[index], 0.8f, 0.0f), .Velocity = glm::vec3(0.0f, 0.0f, -15.0f) });
				buggy.Patch<WheeledVehicleComponent>([](WheeledVehicleComponent& vehicle)
				{
					vehicle.Brake = 1.0f;
				});
			}
			Scope<PlaySession> session = StartSession(fixture, &assets);
			Test::RunTicks(*session, 300);
			std::array<float, 4> distances{};
			for (size_t index = 0; index < 4; ++index)
			{
				CHECK(GetVehicle(*session, std::format("/Buggy{}", index)).Speed == doctest::Approx(0.0f).epsilon(0.05));
				distances[index] = -Test::GetWorldPosition(*session, std::format("/Buggy{}", index)).z;
			}
			INFO("box slick ", distances[0], " box rough ", distances[1], " terrain slick ", distances[2], " terrain rough ", distances[3]);
			// Locked wheels slide with the tyre's grip at full slip (1.0) combined with the ground's by the geometric mean: about
			// 15^2 / (2 g sqrt(mu)), 21 m on the slick lanes and 11.5 m on the rough ones.
			CHECK(distances[1] < 0.75f * distances[0]);
			CHECK(distances[3] < 0.75f * distances[2]);
			CHECK(distances[2] == doctest::Approx(distances[0]).epsilon(0.1));
			CHECK(distances[3] == doctest::Approx(distances[1]).epsilon(0.1));
		}

		TEST_CASE("PhysicsVehicles: one state reached through two histories steps identically")
		{
			// History B removes the WheeledVehicle of the buggy whose vehicle was made first and adds it back before the first step,
			// so Jolt holds its constraint last; the constraints' priorities (the joints', then the vehicles' ranks in UUID order)
			// keep the solve order, so both reach one state. Jolt sorts constraints by priority, then by index, within an island
			// only, and the order matters only between constraints that share a Dynamic body (the W44.2 review's finding 4): the
			// buggies stand on one Dynamic deck, which every vehicle constraint pushes on, so the three are in one island and their
			// order shows. Without the vehicles' ranks, the case fails.
			const auto run = [](bool readd)
			{
				Test::SceneTestFixture fixture;
				Scene& scene = fixture.GetScene();
				static_cast<void>(Test::AddGround(scene));
				Entity deck = Test::AddBoxBody(scene, "Deck", glm::vec3(6.0f, 0.25f, 0.0f), glm::vec3(16.0f, 0.25f, 16.0f), BodyType::Dynamic);
				Test::PatchRigidBody(deck, [](RigidBodyComponent& body)
				{
					body.Mass = 20000.0f;
				});
				for (int index = 0; index < 3; ++index)
				{
					static_cast<void>(AddBuggy(scene, std::format("Buggy{}", index), BuggyOptions{ .Position = glm::vec3(static_cast<float>(index) * 6.0f, 1.3f, 0.0f) }));
				}
				Entity door = Test::AddBoxBody(scene, "Door", glm::vec3(-6.0f, 1.0f, 0.0f), glm::vec3(0.5f, 1.0f, 0.05f), BodyType::Dynamic);
				door.AddComponent<HingeJointComponent>(HingeJointComponent{ .Anchor = glm::vec3(-0.5f, 0.0f, 0.0f), .Axis = glm::vec3(0.0f, 1.0f, 0.0f) });
				Scope<PlaySession> session = StartSession(fixture);
				if (readd)
				{
					Entity first = Find(*session, "/Buggy0");
					for (const std::string_view path : { "/Buggy1", "/Buggy2" })
					{
						if (Find(*session, path).GetUUID() < first.GetUUID())
							first = Find(*session, path);
					}
					const WheeledVehicleComponent vehicle = first.GetComponent<WheeledVehicleComponent>();
					first.RemoveComponent<WheeledVehicleComponent>();
					session->GetPhysics().SynchronizeBodies();
					first.AddComponent<WheeledVehicleComponent>(vehicle);
				}
				for (int index = 0; index < 3; ++index)
				{
					PatchVehicle(*session, std::format("/Buggy{}", index), [index](WheeledVehicleComponent& component)
					{
						component.Throttle = 0.4f + 0.2f * static_cast<float>(index);
						component.Steering = 0.3f;
					});
				}
				Test::RunTicks(*session, 120);
				return session->ComputeStateHash();
			};
			CHECK(run(false) == run(true));
		}

		TEST_CASE("PhysicsVehicles: a scene without vehicles hashes as before")
		{
			// The physics block of a scene without a vehicle in force or kept frames is M37's: wheels without a vehicle, and a
			// vehicle that cannot be made (a Static body), add nothing to it. The committed hashes of the physics and joint
			// determinism cases (PhysicsDeterminismTests.cpp, JointDeterminismTests.cpp) pin that layout.
			const auto run = [](int variant)
			{
				Test::SceneTestFixture fixture;
				Scene& scene = fixture.GetScene();
				static_cast<void>(Test::AddGround(scene));
				Entity box = Test::AddBoxBody(scene, "Box", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic);
				Entity post = Test::AddBoxBody(scene, "Post", glm::vec3(3.0f, 1.0f, 0.0f), glm::vec3(0.25f), BodyType::Static);
				Entity wheel = scene.CreateEntity("Wheel", box);
				if (variant >= 1)
					wheel.AddComponent<VehicleWheelComponent>();
				if (variant == 2)
				{
					static_cast<void>(scene.CreateEntity("PostWheel", post).AddComponent<VehicleWheelComponent>());
					post.AddComponent<WheeledVehicleComponent>();
				}
				Scope<PlaySession> session = StartSession(fixture);
				Test::RunTicks(*session, 60);
				return HashPhysics(*session);
			};
			const uint64_t plain = run(0);
			CHECK(run(1) == plain);
			const Test::ExpectLog invalid(LogLevel::Error, "PHYSICS_VEHICLE_INVALID");
			CHECK(run(2) == plain);
		}

		TEST_CASE("PhysicsSystem: a vehicle that cannot be made raises PHYSICS_VEHICLE_INVALID once, and its body moves as a plain body")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddGround(fixture.GetScene()));
			Entity buggy = AddBuggy(fixture.GetScene(), "Buggy");
			buggy.Patch<WheeledVehicleComponent>([](WheeledVehicleComponent& vehicle)
			{
				vehicle.MinRpm = 7000.0f;
			});
			const Test::ExpectLog invalid(LogLevel::Error, "PHYSICS_VEHICLE_INVALID");
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 60);
			CHECK(CountDiagnostics(*session, "/Buggy", PhysicsVehicleInvalidCode, "engine") == 1);
			CHECK(invalid.GetMatchCount() == 1);
			CHECK_FALSE(GetVehicle(*session, "/Buggy").Created);
			// Without its vehicle, the chassis falls onto the ground (its box's bottom at 0.55 m before).
			CHECK(Test::GetWorldPosition(*session, "/Buggy").y < 0.3f);
			// Mended, the vehicle is made and lifts the chassis on its wheels.
			PatchVehicle(*session, "/Buggy", [](WheeledVehicleComponent& vehicle)
			{
				vehicle.MinRpm = 1000.0f;
			});
			Test::RunTicks(*session, 120);
			CHECK(GetVehicle(*session, "/Buggy").Created);
			CHECK(Test::GetWorldPosition(*session, "/Buggy").y > 0.6f);
		}

		TEST_CASE("PhysicsSystem: a vehicle reads its wheels by either entity, and nothing for an entity that is neither")
		{
			Test::SceneTestFixture fixture;
			static_cast<void>(Test::AddGround(fixture.GetScene()));
			static_cast<void>(AddBuggy(fixture.GetScene(), "Buggy"));
			Scope<PlaySession> session = StartSession(fixture);
			Test::RunTicks(*session, 120);
			const PhysicsVehicleReport vehicle = GetVehicle(*session, "/Buggy");
			const PhysicsVehicleReport byWheel = GetVehicle(*session, "/Buggy/RR");
			CHECK(byWheel.Entity == vehicle.Entity);
			CHECK(vehicle.Body == vehicle.Entity);
			// At rest without input, Auto stays in neutral at the engine's idle.
			CHECK(vehicle.Gear == 0);
			CHECK(vehicle.EngineRpm >= 1000.0f);
			for (const PhysicsVehicleWheelReport& wheel : vehicle.Wheels)
			{
				CHECK(wheel.Touching);
				CHECK(wheel.Collider == Find(*session, "/Ground").GetUUID());
				CHECK(wheel.Normal.y == doctest::Approx(1.0f).epsilon(1.0e-3));
				CHECK(wheel.SuspensionForce > 1000.0f);
			}
			CHECK_FALSE(session->GetPhysics().GetVehicleInfo(Find(*session, "/Ground").GetUUID()).has_value());
			CHECK_FALSE(session->GetPhysics().GetVehicleInfo(UUID(0x1234u)).has_value());
		}

		TEST_CASE("PhysicsSystem: random vehicles and edits never reach a Jolt assert")
		{
			// Random vehicles with every field drawn within its registry range (the composition's rules span fields, which the
			// draws break), random wheel sets and references, and random edits each tick: the composition and the world refuse
			// what Jolt would assert on (PHYSICS_VEHICLE_INVALID), and nothing else happens.
			Test::SetTestCaseTimeLimit(240.0);
			Random random(0x44A11u);
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			const auto range = [&random](double minimum, double maximum)
			{
				switch (random.RangeInt(0, 5))
				{
					case 0:  return static_cast<float>(minimum);
					case 1:  return static_cast<float>(maximum);
					default: return static_cast<float>(random.RangeDouble(minimum, maximum));
				}
			};
			const auto curve = [&random, &range](bool unit)
			{
				std::vector<VehicleCurveKey> keys;
				const int64_t count = random.RangeInt(0, 10);
				float x = unit ? 0.0f : -2.0f;
				for (int64_t key = 0; key < count; ++key)
				{
					x += random.RangeInt(0, 6) == 0 ? -0.1f : range(0.0, unit ? 0.3 : 30.0);
					keys.push_back(VehicleCurveKey{ .X = std::clamp(x, -1000.0f, 1000.0f), .Y = range(0.0, 1000.0) });
				}
				return keys;
			};
			const auto wheelOf = [&range, &curve]()
			{
				return VehicleWheelComponent{ .Radius = range(0.001, 2.0),
					.Width = range(0.0, 1.0),
					.SuspensionMinLength = range(0.0, 1.0),
					.SuspensionMaxLength = range(0.0, 1.0),
					.SuspensionPreload = range(0.0, 1.0),
					.SpringFrequency = range(0.0, 1000.0),
					.SpringDamping = range(0.0, 1000.0),
					.MaxSteerAngle = range(0.0, 90.0),
					.MaxBrakeTorque = range(0.0, 1.0e6),
					.MaxHandBrakeTorque = range(0.0, 1.0e6),
					.Inertia = range(0.001, 1.0e6),
					.AngularDamping = range(0.0, 1000.0),
					.LongitudinalFriction = curve(false),
					.LateralFriction = curve(false) };
			};
			std::vector<UUID> allWheels;
			const auto reference = [&random, &allWheels]()
			{
				const int64_t pick = random.RangeInt(0, static_cast<int64_t>(allWheels.size()));
				return pick == static_cast<int64_t>(allWheels.size()) ? UUID() : allWheels[static_cast<size_t>(pick)];
			};
			const auto vehicleOf = [&random, &range, &curve, &reference]()
			{
				WheeledVehicleComponent vehicle;
				vehicle.Throttle = range(-1.0, 1.0);
				vehicle.Steering = range(-1.0, 1.0);
				vehicle.Brake = range(0.0, 1.0);
				vehicle.HandBrake = range(0.0, 1.0);
				vehicle.Gear = static_cast<int32_t>(random.RangeInt(-8, 8));
				vehicle.Clutch = range(0.0, 1.0);
				vehicle.MaxTorque = range(0.0, 1.0e6);
				vehicle.MinRpm = range(0.0, 1.0e5);
				vehicle.MaxRpm = range(1.0, 1.0e5);
				vehicle.EngineInertia = range(0.001, 1.0e6);
				vehicle.EngineDamping = range(0.0, 1000.0);
				vehicle.TorqueCurve = curve(true);
				vehicle.Transmission = static_cast<VehicleTransmissionMode>(random.RangeInt(0, 1));
				vehicle.GearRatios.clear();
				for (int64_t gear = random.RangeInt(0, 9); gear > 0; --gear)
					vehicle.GearRatios.push_back(range(-1.0, 100.0));
				vehicle.ReverseGearRatios.clear();
				for (int64_t gear = random.RangeInt(0, 9); gear > 0; --gear)
					vehicle.ReverseGearRatios.push_back(range(-100.0, 1.0));
				vehicle.SwitchTime = range(0.0, 100.0);
				vehicle.ClutchReleaseTime = range(0.0, 100.0);
				vehicle.SwitchLatency = range(0.0, 100.0);
				vehicle.ShiftUpRpm = range(0.0, 1.0e5);
				vehicle.ShiftDownRpm = range(0.0, 1.0e5);
				vehicle.ClutchStrength = range(0.0, 1000.0);
				for (int64_t count = random.RangeInt(0, 9); count > 0; --count)
				{
					vehicle.Differentials.push_back(VehicleDifferential{ .LeftWheel = reference(),
						.RightWheel = reference(),
						.Ratio = range(0.0, 100.0),
						.LeftRightSplit = range(0.0, 1.0),
						.LimitedSlipRatio = range(0.0, 1000.0),
						.EngineTorqueRatio = range(0.0, 1.0) });
				}
				vehicle.DifferentialLimitedSlipRatio = range(0.0, 1000.0);
				for (int64_t count = random.RangeInt(0, 9); count > 0; --count)
					vehicle.AntiRollBars.push_back(VehicleAntiRollBar{ .LeftWheel = reference(), .RightWheel = reference(), .Stiffness = range(0.0, 1.0e6) });
				vehicle.MaxPitchRollAngle = range(0.0, 180.0);
				vehicle.WheelCast = static_cast<VehicleWheelCast>(random.RangeInt(0, 2));
				return vehicle;
			};

			// Eight bodies, each with a random number of wheels under it (some under a child with a RigidBody of its own, some with a
			// collider); half of them vehicles from the start, made from plain draws so the reasonable ones drive.
			std::vector<Entity> bodies;
			for (int index = 0; index < 8; ++index)
			{
				Entity body = Test::AddBoxBody(scene, std::format("Body{}", index), glm::vec3(static_cast<float>(index) * 5.0f - 20.0f, 1.0f, 0.0f),
					glm::vec3(0.8f, 0.25f, 1.5f), index == 7 ? BodyType::Kinematic : BodyType::Dynamic);
				for (int64_t count = random.RangeInt(0, 6); count > 0; --count)
				{
					Entity wheel = scene.CreateEntity("Wheel", body);
					wheel.Patch<TransformComponent>([&range](TransformComponent& transform)
					{
						transform.Translation = glm::vec3(range(-1.0, 1.0), range(-1.0, 0.0), range(-2.0, 2.0));
					});
					wheel.AddComponent<VehicleWheelComponent>(random.RangeInt(0, 2) == 0 ? wheelOf() : VehicleWheelComponent{});
					if (random.RangeInt(0, 9) == 0)
						wheel.AddComponent<BoxColliderComponent>();
					allWheels.push_back(wheel.GetUUID());
				}
				bodies.push_back(body);
			}
			for (size_t index = 0; index < bodies.size(); index += 2)
				bodies[index].AddComponent<WheeledVehicleComponent>(random.RangeInt(0, 1) == 0 ? vehicleOf() : WheeledVehicleComponent{});

			const Test::ExpectLog invalid(LogLevel::Error, "PHYSICS_");
			Scope<PlaySession> session = StartSession(fixture);
			Scene& runtime = session->GetScene();
			for (int tick = 0; tick < 240; ++tick)
			{
				const std::vector<UUID> order(runtime.GetCanonicalOrder().begin(), runtime.GetCanonicalOrder().end());
				Entity entity = runtime.FindEntityByID(order[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(order.size()) - 1))]);
				if (entity.IsValid() && entity.GetName() != "Ground")
				{
					switch (random.RangeInt(0, 7))
					{
						case 0:
							if (entity.HasComponent<RigidBodyComponent>())
							{
								if (entity.HasComponent<WheeledVehicleComponent>())
									entity.RemoveComponent<WheeledVehicleComponent>();
								entity.AddComponent<WheeledVehicleComponent>(vehicleOf());
							}
							break;
						case 1:
							if (entity.HasComponent<WheeledVehicleComponent>())
							{
								entity.Patch<WheeledVehicleComponent>([&range](WheeledVehicleComponent& vehicle)
								{
									vehicle.Throttle = range(-1.0, 1.0);
									vehicle.Steering = range(-1.0, 1.0);
									vehicle.Gear = static_cast<int32_t>(range(-8.0, 8.0));
								});
							}
							break;
						case 2:
							if (entity.HasComponent<VehicleWheelComponent>())
								entity.Patch<VehicleWheelComponent>([&wheelOf](VehicleWheelComponent& wheel)
								{
									wheel = wheelOf();
								});
							break;
						case 3:
							entity.SetActive(!entity.IsActiveSelf());
							break;
						case 4:
							if (entity.HasComponent<RigidBodyComponent>())
							{
								Test::PatchRigidBody(entity, [&random](RigidBodyComponent& body)
								{
									body.Type = static_cast<BodyType>(random.RangeInt(0, 2));
									body.Mass = static_cast<float>(random.RangeDouble(1.0, 3000.0));
								});
							}
							break;
						case 5:
							if (entity.HasComponent<VehicleWheelComponent>() && random.RangeInt(0, 1) == 0)
								entity.RemoveComponent<VehicleWheelComponent>();
							else if (!entity.HasComponent<VehicleWheelComponent>() && !entity.HasComponent<RigidBodyComponent>())
								entity.AddComponent<VehicleWheelComponent>(wheelOf());
							break;
						case 6:
							// A reparent: a wheel under another body.
							if (entity.HasComponent<VehicleWheelComponent>())
								static_cast<void>(runtime.SetParent(entity, runtime.FindEntityByID(bodies[static_cast<size_t>(random.RangeInt(0, 7))].GetUUID())));
							break;
						default:
							if (tick % 60 == 59)
								runtime.DestroyEntity(entity);
							break;
					}
				}
				Test::RunTicks(*session, 1);
			}
			CHECK(session->GetTick() == 240);
		}
	}

}
