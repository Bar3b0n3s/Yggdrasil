#include "TestsPCH.h"

#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Components/BoxCollider2DComponent.h"
#include "Engine/Scene/Components/CircleCollider2DComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Physics2DSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Session/PlaySession.h"
#include "Engine/Session/ReplayRecorder.h"
#include "Support/ScriptTestFixture.h"

#include <algorithm>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// 2D contact callbacks (Docs/Decisions/0032-m21-contract.md decision 13, stream P2; Roadmap M21 "2D contact events reach
// scripts in canonical order"): the 2D world's events reach OnCollisionEnter2D, OnCollisionExit2D, OnTriggerEnter2D and
// OnTriggerExit2D on every slot of the receiving entity, in slot order, as the 3D callbacks do (M20's script slots), and
// never the 3D callbacks; and the RigidBody2D proxy and the Physics2D module take and return planar values, angles in
// degrees.

namespace Engine {

	namespace {

		int RecordPhysics2DCallback(ScriptCall& call)
		{
			const std::string message = Lua::Check<std::string>(call, 1);
			static_cast<void>(Lua::GetEngine(call)->GetHost().GetScene().CreateEntity("Trace:" + message));
			return 0;
		}

		Test::ScriptTestFixtureSpecification Physics2DCallbackSpecification()
		{
			Test::ScriptTestFixtureSpecification specification{};
			specification.TestMode = true;
			specification.ConfigureApi = [](ScriptApiRegistry& api) -> Status
			{
				api.Module("Probe", "Records 2D callbacks into the test scene.").Function("Record", &RecordPhysics2DCallback, "(message: string) -> ()", "Append one test observation.", { .Mutates = true });
				return {};
			};
			return specification;
		}

		// A behaviour named `name` that records its 2D and 3D contact callbacks as "<entity>:<name>:<callback>".
		std::string Physics2DBehaviourSource(std::string_view name)
		{
			return std::format(R"(local T = {{}}
function T:OnCreate() self.Kind = "{0}" end
function T:OnCollisionEnter2D(other, contact) Probe.Record(self.Entity.Name .. ":" .. self.Kind .. ":enter2d:" .. other.Name .. ":" .. tostring(contact.Point.x)) end
function T:OnCollisionExit2D(other) Probe.Record(self.Entity.Name .. ":" .. self.Kind .. ":exit2d:" .. other.Name) end
function T:OnTriggerEnter2D(other) Probe.Record(self.Entity.Name .. ":" .. self.Kind .. ":triggerEnter2d:" .. other.Name) end
function T:OnTriggerExit2D(other) Probe.Record(self.Entity.Name .. ":" .. self.Kind .. ":triggerExit2d:" .. other.Name) end
function T:OnCollisionEnter(other) Probe.Record(self.Entity.Name .. ":" .. self.Kind .. ":enter3d") end
function T:OnTriggerEnter(other) Probe.Record(self.Entity.Name .. ":" .. self.Kind .. ":triggerEnter3d") end
return Script.Define("{0}", T)
)",
				name);
		}

		std::vector<std::string> Physics2DTrace(Scene& scene)
		{
			std::vector<std::string> result;
			for (const UUID id : scene.GetCanonicalOrder())
			{
				const Entity entity = scene.FindEntityByID(id);
				if (entity && entity.GetName().starts_with("Trace:"))
					result.push_back(entity.GetName().substr(6));
			}
			return result;
		}

		// A script fixture over a 2D world without gravity: a static wall of 1 x 4 m at x = 3, a floating Dynamic crate at
		// x = -5 and a Kinematic lift at y = 5. The fixture's host borrows the system, and the VM stops before the system is
		// destroyed.
		class Physics2DBindingFixture
		{
		public:
			explicit Physics2DBindingFixture(RunModes mode = RunModes::Editor)
				: Script({ .Mode = mode, .TestMode = true })
			{
				Scene& scene = Script.GetScene();
				Entity wall = scene.CreateEntity("Wall");
				wall.Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(3.0f, 0.0f, 0.0f);
				});
				wall.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(0.5f, 2.0f) });
				Entity crate = scene.CreateEntity("Crate");
				crate.Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(-5.0f, 0.0f, 0.0f);
				});
				crate.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .GravityFactor = 0.0f, .AllowSleeping = false });
				crate.AddComponent<BoxCollider2DComponent>();
				Entity lift = scene.CreateEntity("Lift");
				lift.Patch<TransformComponent>([](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(0.0f, 5.0f, 0.0f);
				});
				lift.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = BodyType::Kinematic });
				lift.AddComponent<BoxCollider2DComponent>();
				TransformSystem::Update(scene);
			}
			~Physics2DBindingFixture()
			{
				Script.Stop();
				Script.Physics2D = nullptr;
			}
			Physics2DBindingFixture(const Physics2DBindingFixture&) = delete;
			Physics2DBindingFixture& operator=(const Physics2DBindingFixture&) = delete;

			Status Start()
			{
				ENGINE_TRY_ASSIGN(System, Physics2DSystem::Create({ .RuntimeScene = &Script.GetScene(), .Gravity = glm::vec2(0.0f) }));
				Script.Physics2D = System.get();
				return Script.Start();
			}

			Test::ScriptTestFixture Script;
			Scope<Physics2DSystem> System{};
		};

		void AddPhysics2DSlots(Test::ScriptTestFixture& fixture, Entity entity, std::string_view scripts)
		{
			const Result<Json> slots = JsonReader::Parse(std::string(R"({ "Scripts": )") + std::string(scripts) + "}");
			REQUIRE(slots.has_value());
			const Status added = ComponentAccess::AddComponent(entity, "Script", &*slots, fixture.GetSchemaSnapshot().get());
			REQUIRE_MESSAGE(added.has_value(), (added ? std::string() : added.error().ToString()));
		}

	}

	namespace Test {

		// The 2D callbacks through real contacts (ScriptApi's coverage of Behaviour, RunBehaviourCallbacksCoverage): in a play
		// session without gravity, a ball overlapping a floor and a ball inside a sensor enter at the first tick, and leave when
		// they are teleported away.
		void RunBehaviour2DCallbacksCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			ScriptTestFixture fixture(ScriptTestFixtureSpecification{ .Mode = mode, .TestMode = true });
			const AssetHandle handle(0x320003);
			REQUIRE(fixture.AddScript(handle, "Assets/Scripts/Contacts2D.luau", R"(local T = {}
function T:OnCollisionEnter2D(other, contact) assert(contact.Point.z == 0 and contact.Normal.z == 0); Scene.CreateEntity("Trace:" .. self.Entity.Name .. ":enter") end
function T:OnCollisionExit2D(other) Scene.CreateEntity("Trace:" .. self.Entity.Name .. ":exit") end
function T:OnTriggerEnter2D(other) Scene.CreateEntity("Trace:" .. self.Entity.Name .. ":triggerEnter") end
function T:OnTriggerExit2D(other) Scene.CreateEntity("Trace:" .. self.Entity.Name .. ":triggerExit") end
return Script.Define("Contacts2D", T)
)"));
			Scene& editScene = fixture.GetScene();
			const auto place = [&editScene](std::string_view name, const glm::vec2& position)
			{
				Entity entity = editScene.CreateEntity(name);
				entity.Patch<TransformComponent>([position](TransformComponent& transform)
				{
					transform.Translation = glm::vec3(position, 0.0f);
				});
				return entity;
			};
			Entity floor = place("Floor", glm::vec2(0.0f, -0.5f));
			floor.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = BodyType::Static });
			floor.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(2.0f, 0.5f) });
			Entity solid = place("Solid", glm::vec2(0.0f, 0.4f));
			solid.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .AllowSleeping = false });
			solid.AddComponent<CircleCollider2DComponent>();
			Entity sensor = place("Sensor", glm::vec2(10.0f, 0.0f));
			sensor.AddComponent<BoxCollider2DComponent>(BoxCollider2DComponent{ .HalfExtents = glm::vec2(1.0f), .IsTrigger = true });
			Entity actor = place("Actor", glm::vec2(10.0f, 0.0f));
			actor.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .AllowSleeping = false });
			actor.AddComponent<CircleCollider2DComponent>();
			for (const Entity entity : { solid, actor })
				AddPhysics2DSlots(fixture, entity, std::format(R"([ {{ "Script": "{}" }} ])", handle.ToString()));

			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.Seed = 721;
			specification.Project.Physics2D.Gravity = glm::vec2(0.0f);
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.ScriptRunMode = mode;
			specification.TestMode = true;
			auto started = PlaySession::CreateFromScene(specification, editScene);
			REQUIRE_MESSAGE(started, (started ? "" : started.error().ToString()));
			const Scope<PlaySession> session = std::move(*started);
			session->SetExtractionEnabled(false);
			Scene& scene = session->GetScene();
			REQUIRE(session->GetPhysics2D() != nullptr);

			// Jolt produces both pairs; no callback is invoked directly by the test. The two pairs' order is their UUIDs'.
			const auto sortedTrace = [&scene]()
			{
				std::vector<std::string> trace = Physics2DTrace(scene);
				std::ranges::sort(trace);
				return trace;
			};
			session->Tick();
			REQUIRE(session->GetScriptErrors().GetErrors().empty());
			CHECK(sortedTrace() == std::vector<std::string>{ "Actor:triggerEnter", "Solid:enter" });
			REQUIRE(session->GetPhysics2D()->Teleport(solid.GetUUID(), glm::vec2(100.0f, 0.0f), std::nullopt));
			REQUIRE(session->GetPhysics2D()->Teleport(actor.GetUUID(), glm::vec2(110.0f, 0.0f), std::nullopt));
			session->Tick();
			REQUIRE(session->GetScriptErrors().GetErrors().empty());
			CHECK(sortedTrace() == std::vector<std::string>{ "Actor:triggerEnter", "Actor:triggerExit", "Solid:enter", "Solid:exit" });
			CHECK(session->GetPhysics2D()->GetDiagnostics().empty());
			const auto coverage = session->GetScripts()->GetApi().GetCoverage(mode);
			REQUIRE(coverage);
			snapshots.push_back(*coverage);
		}

		// Every RigidBody2D method and Physics2D function, dispatched to a real 2D system (ScriptApi's coverage).
		void RunPhysics2DBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Physics2DBindingFixture fixture(mode);
			REQUIRE(fixture.Start());
			const Result<ScriptEvaluation> result = fixture.Script.Evaluate(R"(
local crateEntity, liftEntity, wall = Scene.FindByName("Crate"), Scene.FindByName("Lift"), Scene.FindByName("Wall")
local crate = crateEntity.RigidBody2D
crate:AddForce(vector.create(1, 0, 0))
crate:AddForceAtPosition(vector.create(0, 1, 0), vector.create(-5, 0.5, 0))
crate:AddTorque(1)
crate:AddImpulse(vector.create(1, 0, 0))
crate:AddAngularImpulse(0.5)
crate:SetLinearVelocity(vector.create(1, 2, 0))
crate:SetAngularVelocity(1)
assert(crate:GetLinearVelocity() == vector.create(1, 2, 0))
assert(crate:GetAngularVelocity() == 1)
crate:WakeUp()
assert(not crate:IsSleeping())
crate:Teleport(vector.create(-6, 0, 0))
assert(crateEntity.Transform.WorldPosition == vector.create(-6, 0, 0))
liftEntity.RigidBody2D:MoveKinematic(vector.create(0, 6, 0), 45)
assert(liftEntity.Transform.WorldPosition == vector.create(0, 6, 0))
assert(Physics2D.Raycast(vector.create(0, 0, 0), vector.create(1, 0, 0), 10).Entity == wall)
assert(#Physics2D.RaycastAll(vector.create(-10, 0, 0), vector.create(1, 0, 0), 20) == 2)
assert(#Physics2D.OverlapCircle(vector.create(3, 0, 0), 0.25) == 1)
assert(#Physics2D.OverlapBox(vector.create(3, 0, 0), vector.create(0.25, 0.25, 0)) == 1)
Physics2D.SetGravity(vector.create(0, -1, 0))
assert(Physics2D.GetGravity() == vector.create(0, -1, 0))
return true
)");
			REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error().ToString()));
			const SimStep step = SimStep::FromTick(0, 1.0 / 60.0);
			TransformSystem::Update(fixture.Script.GetScene());
			fixture.System->PreStep(step);
			fixture.System->Step(step);
			fixture.System->PostStep(step);
			CHECK(fixture.System->GetDiagnostics().empty());
			CHECK(fixture.Script.Errors.empty());
			const auto coverage = fixture.Script.GetApi().GetCoverage(mode);
			REQUIRE(coverage);
			for (const auto& member : coverage->Members)
			{
				if (member.Owner == "Physics2D" || (member.Owner == "RigidBody2D" && member.Kind == ScriptApiMemberKind::Method))
				{
					CAPTURE(member.Owner);
					CAPTURE(member.Member);
					CHECK(member.Calls > 0);
				}
			}
			snapshots.push_back(*coverage);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptPhysics2D: a play session's 2D contacts reach the 2D callbacks, entering and leaving")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunBehaviour2DCallbacksCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("ScriptPhysics2D: every RigidBody2D method and Physics2D function dispatches to the 2D system")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunPhysics2DBindingsCoverage(RunModes::Editor, snapshots);
		}

		// The RigidBody2D proxy validates its arguments and the body's motion type before it marks the session changed, as the
		// 3D proxy does ("PhysicsBindings: malformed ranges and wrong motion types fail before mutation"; W21.1's review), so an
		// evaluation whose call is refused records no external mutation.
		TEST_CASE("ScriptPhysics2D: malformed values and the wrong body raise errors and change nothing")
		{
			Physics2DBindingFixture fixture;
			REQUIRE(fixture.Start());
			const Result<ScriptEvaluation> result = fixture.Script.Evaluate(R"(
local zero, right = vector.zero, vector.create(1, 0, 0)
local crate, lift = Scene.FindByName("Crate").RigidBody2D, Scene.FindByName("Lift").RigidBody2D
for _, fn in {
	function() Physics2D.Raycast(zero, zero, 10) end,
	function() Physics2D.Raycast(zero, right, -1) end,
	function() Physics2D.OverlapCircle(zero, 0) end,
	function() Physics2D.OverlapBox(zero, vector.create(-1, 1, 0)) end,
	function() Physics2D.SetGravity(vector.create(1e30, 0, 0)) end,
	function() crate:AddForce(vector.create(1e30, 0, 0)) end,
	function() crate:AddTorque(1e13) end,
	function() crate:AddAngularImpulse(-1e13) end,
	function() crate:SetAngularVelocity(1e13) end,
	function() crate:MoveKinematic(zero, 0) end,
	function() lift:AddForce(right) end,
	function() lift:AddForceAtPosition(right, zero) end,
	function() lift:AddTorque(1) end,
	function() lift:AddImpulse(right) end,
	function() lift:AddAngularImpulse(1) end,
	function() lift:SetLinearVelocity(right) end,
	function() lift:SetAngularVelocity(1) end,
	function() crate:Teleport(vector.create(1e30, 0, 0)) end,
} do
	local ok, message = pcall(fn)
	assert(not ok and type(message) == "string")
end
assert(crate:GetLinearVelocity() == zero and Physics2D.GetGravity() == zero)
return true
)");
			REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.System->GetDiagnostics().empty());
			CHECK(fixture.Script.ExternalMutations.empty());
		}

		// W21.1's review: an evaluation whose RigidBody2D call is refused before it acts leaves an active input recording
		// valid; one that acts invalidates it (an external write, §13.6).
		TEST_CASE("ScriptPhysics2D: a RigidBody2D call refused before it acts leaves an active input recording valid")
		{
			Test::ScriptTestFixture fixture;
			Scene& editScene = fixture.GetScene();
			Entity crate = editScene.CreateEntity("Crate");
			crate.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .AllowSleeping = false });
			crate.AddComponent<BoxCollider2DComponent>();
			Entity lift = editScene.CreateEntity("Lift");
			lift.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 5.0f, 0.0f);
			});
			lift.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .Type = BodyType::Kinematic });
			lift.AddComponent<BoxCollider2DComponent>();
			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.Seed = 47;
			specification.Project.Physics2D.Gravity = glm::vec2(0.0f);
			auto started = PlaySession::CreateFromScene(specification, editScene);
			REQUIRE_MESSAGE(started, (started ? "" : started.error().ToString()));
			const Scope<PlaySession> session = std::move(*started);
			session->SetExtractionEnabled(false);
			REQUIRE(session->GetScripts() != nullptr);
			ReplayHeader header{};
			header.Scene = { AssetHandle(0x320010), "Assets/Scenes/Physics2D.scene" };
			header.Seed = specification.Seed;
			header.EngineVersion = "1";
			header.Config = "Debug";

			REQUIRE(session->GetRecorder()->Begin(header, *session));
			const Result<ScriptEvaluation> refused = session->GetScripts()->Evaluate(R"(
local crate, lift = Scene.FindByName("Crate").RigidBody2D, Scene.FindByName("Lift").RigidBody2D
local right = vector.create(1, 0, 0)
for _, fn in {
	function() lift:AddForce(right) end,
	function() lift:SetLinearVelocity(right) end,
	function() crate:MoveKinematic(right, 0) end,
	function() crate:AddTorque(1e13) end,
} do assert(not pcall(fn)) end
return true
)",
				{});
			REQUIRE_MESSAGE(refused.has_value(), (refused ? std::string() : refused.error().ToString()));
			CHECK(session->GetRecorder()->Finish(*session).has_value());

			REQUIRE(session->GetRecorder()->Begin(header, *session));
			REQUIRE(session->GetScripts()->Evaluate(R"(Scene.FindByName("Crate").RigidBody2D:AddForce(vector.create(1, 0, 0)))", {}).has_value());
			CHECK_FALSE(session->GetRecorder()->Finish(*session).has_value());
		}

		TEST_CASE("ScriptPhysics2D: 2D contact callbacks reach every slot of the receiving entity in dispatch order")
		{
			Test::ScriptTestFixture fixture(Physics2DCallbackSpecification());
			const AssetHandle paddle(0x320001);
			const AssetHandle sound(0x320002);
			REQUIRE(fixture.AddScript(paddle, "Assets/Scripts/Paddle.luau", Physics2DBehaviourSource("Paddle")));
			REQUIRE(fixture.AddScript(sound, "Assets/Scripts/Sound.luau", Physics2DBehaviourSource("Sound")));
			Scene& scene = fixture.GetScene();
			const Entity ball = scene.CreateEntityWithID(UUID(90), "Ball");
			const Entity bat = scene.CreateEntityWithID(UUID(91), "Bat");
			AddPhysics2DSlots(fixture, ball, std::format(R"([ {{ "Script": "{}" }} ])", sound.ToString()));
			AddPhysics2DSlots(fixture, bat, std::format(R"([ {{ "Script": "{}" }}, {{ "Script": "{}" }} ])", paddle.ToString(), sound.ToString()));
			REQUIRE(fixture.Start());
			const size_t from = Physics2DTrace(scene).size();

			// One pair's enter, delivered to the lower UUID first (PhysicsEventOrderKey), then a trigger exit.
			PhysicsContact contact{ .Point = glm::vec3(1.5f, 0.0f, 0.0f), .Normal = glm::vec3(0.0f, 1.0f, 0.0f), .Collider = UUID(90), .OtherCollider = UUID(91) };
			fixture.GetEngine()->OnPhysics2DEvent(PhysicsEvent{ .Type = PhysicsEventType::CollisionEnter, .Self = UUID(90), .Other = UUID(91), .Contact = contact });
			std::swap(contact.Collider, contact.OtherCollider);
			fixture.GetEngine()->OnPhysics2DEvent(PhysicsEvent{ .Type = PhysicsEventType::CollisionEnter, .Self = UUID(91), .Other = UUID(90), .Contact = contact });
			fixture.GetEngine()->OnPhysics2DEvent(PhysicsEvent{ .Type = PhysicsEventType::TriggerExit, .Self = UUID(91), .Other = UUID(90) });

			const std::vector<std::string> trace = Physics2DTrace(scene);
			const std::vector<std::string> since(trace.begin() + static_cast<ptrdiff_t>(std::min(from, trace.size())), trace.end());
			CHECK(since == std::vector<std::string>{ "Ball:Sound:enter2d:Bat:1.5", "Bat:Paddle:enter2d:Ball:1.5", "Bat:Sound:enter2d:Ball:1.5", "Bat:Paddle:triggerExit2d:Ball", "Bat:Sound:triggerExit2d:Ball" });
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("ScriptPhysics2D: RigidBody2D and Physics2D take and return planar values in degrees")
		{
			// Decision 13: a vector's X and Y are used and its Z ignored, a returned vector has Z 0, angular velocities are
			// numbers in radians per second about +Z, and angles are degrees about +Z (90 degrees and 90 radians give different
			// poses and overlaps below).
			Physics2DBindingFixture fixture;
			REQUIRE(fixture.Start());
			const Result<ScriptEvaluation> result = fixture.Script.Evaluate(R"(
local wall, crateEntity = Scene.FindByName("Wall"), Scene.FindByName("Crate")
local crate = crateEntity.RigidBody2D
crate:SetLinearVelocity(vector.create(1, 2, 7))
assert(crate:GetLinearVelocity() == vector.create(1, 2, 0))
crate:SetAngularVelocity(1.5)
assert(crate:GetAngularVelocity() == 1.5)
crate:Teleport(vector.create(-4, 1, 9), 90)
assert(vector.magnitude(crateEntity.Transform:Right() - vector.create(0, 1, 0)) < 0.0001)
local hit = Physics2D.Raycast(vector.create(0, 0, 5), vector.create(1, 0, 0), 10)
assert(hit and hit.Entity == wall and hit.Body == wall and math.abs(hit.Distance - 2.5) < 0.001)
assert(hit.Point.z == 0 and hit.Normal.z == 0 and math.abs(hit.Normal.x + 1) < 0.001)
assert(#Physics2D.OverlapBox(vector.create(3, 3, 0), vector.create(0.1, 1.5, 0), 0) == 1)
assert(#Physics2D.OverlapBox(vector.create(3, 3, 0), vector.create(0.1, 1.5, 0), 90) == 0)
local touching = Physics2D.OverlapCircle(vector.create(3, 2.2, 0), 0.3)
assert(#touching == 1 and touching[1] == wall)
assert(Physics2D.GetGravity() == vector.zero)
Physics2D.SetGravity(vector.create(0, -3, 4))
assert(Physics2D.GetGravity() == vector.create(0, -3, 0))
return true
)");
			REQUIRE_MESSAGE(result.has_value(), (result ? std::string() : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.System->GetGravity() == glm::vec2(0.0f, -3.0f));
			CHECK(fixture.Script.Errors.empty());
		}

		// Held events (ADR 0031 decision 17): an instance that has not started holds both worlds' events in arrival order, and
		// the start flush delivers each to its own world's callback.
		TEST_CASE("ScriptPhysics2D: held 2D and 3D events reach their own callbacks at the start flush, in arrival order")
		{
			Test::ScriptTestFixture fixture(Physics2DCallbackSpecification());
			const AssetHandle sound(0x320002);
			REQUIRE(fixture.AddScript(sound, "Assets/Scripts/Sound.luau", Physics2DBehaviourSource("Sound")));
			REQUIRE(fixture.Start());
			Scene& scene = fixture.GetScene();
			static_cast<void>(scene.CreateEntityWithID(UUID(93), "Wall"));
			const Entity late = scene.CreateEntityWithID(UUID(92), "Late");
			AddPhysics2DSlots(fixture, late, std::format(R"([ {{ "Script": "{}" }} ])", sound.ToString()));
			REQUIRE(fixture.GetEngine()->SynchronizeInstances());
			const size_t from = Physics2DTrace(scene).size();

			fixture.GetEngine()->OnPhysics2DEvent(PhysicsEvent{ .Type = PhysicsEventType::TriggerEnter, .Self = UUID(92), .Other = UUID(93) });
			fixture.GetEngine()->OnPhysicsEvent(PhysicsEvent{ .Type = PhysicsEventType::TriggerEnter, .Self = UUID(92), .Other = UUID(93) });
			CHECK(Physics2DTrace(scene).size() == from);
			fixture.Frame.Phase = InputPhase::Step;
			fixture.GetEngine()->StartPending();

			const std::vector<std::string> trace = Physics2DTrace(scene);
			const std::vector<std::string> since(trace.begin() + static_cast<ptrdiff_t>(std::min(from, trace.size())), trace.end());
			CHECK(since == std::vector<std::string>{ "Late:Sound:triggerEnter2d:Wall", "Late:Sound:triggerEnter3d" });
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("ScriptPhysics2D: a 2D diagnostic error becomes a script error of the entity's Physics2D callback")
		{
			Test::ScriptTestFixtureSpecification specification = Physics2DCallbackSpecification();
			specification.TestMode = false;
			Test::ScriptTestFixture fixture(specification);
			const AssetHandle sound(0x320002);
			REQUIRE(fixture.AddScript(sound, "Assets/Scripts/Sound.luau", Physics2DBehaviourSource("Sound")));
			Scene& scene = fixture.GetScene();
			const Entity mixed = scene.CreateEntityWithID(UUID(94), "Mixed");
			AddPhysics2DSlots(fixture, mixed, std::format(R"([ {{ "Script": "{}" }} ])", sound.ToString()));
			REQUIRE(fixture.Start());

			// A warning is no script error; an error names the entity's first slot that has an instance.
			fixture.GetEngine()->OnPhysics2DDiagnostic(PhysicsDiagnostic{ .Code = "PHYSICS2D_OFF_PLANE", .Severity = DiagnosticSeverity::Warning, .Entity = UUID(94), .Message = "tilted" });
			CHECK(fixture.Errors.empty());
			fixture.GetEngine()->OnPhysics2DDiagnostic(PhysicsDiagnostic{ .Code = "PHYSICS2D_MIXED_WORLDS", .Severity = DiagnosticSeverity::Error, .Entity = UUID(94), .Message = "both worlds" });
			REQUIRE(fixture.Errors.size() == 1);
			CHECK(fixture.Errors[0].Entity == UUID(94));
			CHECK(fixture.Errors[0].Callback == "Physics2D");
			CHECK(fixture.Errors[0].Message.contains("PHYSICS2D_MIXED_WORLDS"));
		}
	}

}
