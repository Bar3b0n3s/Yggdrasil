#include "TestsPCH.h"

#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Scene/ComponentAccess.h"
#include "Engine/Scene/Components/BoxCollider2DComponent.h"
#include "Engine/Scene/Components/RigidBody2DComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Physics2DSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Support/ScriptTestFixture.h"

#include <algorithm>
#include <format>
#include <string>
#include <string_view>
#include <vector>

// 2D contact callbacks (Docs/Decisions/0032-m21-contract.md decision 13, stream P2; Roadmap M21 "2D contact events reach
// scripts in canonical order"): the 2D world's events reach OnCollisionEnter2D, OnCollisionExit2D, OnTriggerEnter2D and
// OnTriggerExit2D on every slot of the receiving entity, in slot order, as the 3D callbacks do (M20's script slots), and
// never the 3D callbacks; and the RigidBody2D proxy and the Physics2D module take and return planar values, angles in
// degrees. Skipped until stream P2 implements the dispatch and the bindings.

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

		// A script fixture over a 2D world without gravity: a static wall of 1 x 4 m at x = 3 and a floating Dynamic crate at
		// x = -5. The fixture's host borrows the system, and the VM stops before the system is destroyed.
		class Physics2DBindingFixture
		{
		public:
			Physics2DBindingFixture()
				: Script({ .TestMode = true })
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
				crate.AddComponent<RigidBody2DComponent>(RigidBody2DComponent{ .GravityFactor = 0.0f });
				crate.AddComponent<BoxCollider2DComponent>();
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

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptPhysics2D: 2D contact callbacks reach every slot of the receiving entity in dispatch order" * doctest::skip(true))
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

		TEST_CASE("ScriptPhysics2D: RigidBody2D and Physics2D take and return planar values in degrees" * doctest::skip(true))
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
	}

}
