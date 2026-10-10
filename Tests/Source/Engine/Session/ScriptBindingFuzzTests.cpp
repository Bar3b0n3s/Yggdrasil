#include "TestsPCH.h"
#include "Engine/Session/PlaySession.h"

#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Core/Random.h"
#include "Engine/Scene/Components/CharacterControllerComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Support/AutomationTestClient.h"
#include "Support/ExpectLog.h"
#include "Support/PhysicsTestScene.h"

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <format>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace Test {

		struct ScriptBindingFuzzCase
		{
			std::string Name{};
			std::string Statement{};
			std::string ErrorMember{};
			bool Valid = false;
		};

		static std::string SubstituteFuzzArgument(std::string text, std::string_view argument)
		{
			size_t position = 0;
			while ((position = text.find('@', position)) != std::string::npos)
			{
				text.replace(position, 1, argument);
				position += argument.size();
			}
			return text;
		}

		static void AddFuzzFamily(std::vector<ScriptBindingFuzzCase>& cases, std::string_view member,
			std::string_view statement, std::span<const std::string_view> invalid, std::span<const std::string_view> valid)
		{
			for (const std::string_view argument : invalid)
				cases.push_back({ std::format("{} rejects {}", member, argument), SubstituteFuzzArgument(std::string(statement), argument), std::string(member), false });
			for (const std::string_view argument : valid)
				cases.push_back({ std::format("{} accepts {}", member, argument), SubstituteFuzzArgument(std::string(statement), argument), {}, true });
		}

		static std::vector<ScriptBindingFuzzCase> BindingFuzzCorpus()
		{
			// Numeric-looking strings must not coerce. Vector components vary independently so validation cannot check x only.
			const std::array<std::string_view, 10> invalidNumbers = { "0/0", "math.huge", "-math.huge", "-1", "1e30", "'1'", "{}", "true", "nil", "-1e30" };
			const std::array<std::string_view, 9> invalidVectors = {
				"vector.create(0/0,0,0)", "vector.create(0,math.huge,0)", "vector.create(0,0,-math.huge)",
				"vector.create(1e30,0,0)", "vector.create(0,-1e30,0)", "'vector'", "{}", "42", "Color.New(1,1,1)"
			};
			const std::array<std::string_view, 3> finiteVectors = { "vector.zero", "vector.create(1,-2,3)", "vector.create(-2,1,-1)" };
			const std::array<std::string_view, 3> positiveNumbers = { "0.25", "1", "7" };
			const std::array<std::string_view, 2> durations = { "0", "1" };
			const std::array<std::string_view, 5> invalidRotations = { "Quat.New(0,0,0,0)", "Quat.New(0,0/0,0,1)", "Quat.New(0,0,math.huge,1)", "vector.zero", "{}" };
			std::vector<ScriptBindingFuzzCase> cases;
			AddFuzzFamily(cases, "Physics.Raycast", "Physics.Raycast(@, vector.create(0,0,-1), 10)", invalidVectors, finiteVectors);
			AddFuzzFamily(cases, "Physics.RaycastAll", "Physics.RaycastAll(vector.create(0,0,3), vector.create(0,0,-1), @)", invalidNumbers, positiveNumbers);
			AddFuzzFamily(cases, "Physics.SphereCast", "Physics.SphereCast(vector.create(0,0,3), @, vector.create(0,0,-1), 10)", invalidNumbers, positiveNumbers);
			AddFuzzFamily(cases, "Physics.OverlapSphere", "Physics.OverlapSphere(vector.zero, @)", invalidNumbers, positiveNumbers);
			AddFuzzFamily(cases, "Physics.OverlapBox", "Physics.OverlapBox(@, vector.create(1,1,1))", invalidVectors, finiteVectors);
			AddFuzzFamily(cases, "Physics.SetGravity", "Physics.SetGravity(@)", invalidVectors, std::array<std::string_view, 1>{ "vector.zero" });
			AddFuzzFamily(cases, "RigidBody.AddForce", "dynamic:AddForce(@)", invalidVectors, finiteVectors);
			AddFuzzFamily(cases, "RigidBody.AddForceAtPosition", "dynamic:AddForceAtPosition(vector.create(1,0,0), @)", invalidVectors, finiteVectors);
			AddFuzzFamily(cases, "RigidBody.AddTorque", "dynamic:AddTorque(@)", invalidVectors, finiteVectors);
			AddFuzzFamily(cases, "RigidBody.AddImpulse", "dynamic:AddImpulse(@)", invalidVectors, finiteVectors);
			AddFuzzFamily(cases, "RigidBody.AddAngularImpulse", "dynamic:AddAngularImpulse(@)", invalidVectors, finiteVectors);
			AddFuzzFamily(cases, "RigidBody.SetLinearVelocity", "dynamic:SetLinearVelocity(@); assert(dynamic:GetLinearVelocity() == @)", invalidVectors, finiteVectors);
			AddFuzzFamily(cases, "RigidBody.SetAngularVelocity", "dynamic:SetAngularVelocity(@); assert(dynamic:GetAngularVelocity() == @)", invalidVectors, finiteVectors);
			AddFuzzFamily(cases, "RigidBody.Teleport", "dynamic:Teleport(@)", invalidVectors, std::array<std::string_view, 1>{ "vector.create(10,2,0)" });
			AddFuzzFamily(cases, "RigidBody.MoveKinematic", "kinematic:MoveKinematic(@, Quat.Identity())", invalidVectors, std::array<std::string_view, 1>{ "vector.create(20,2,0)" });
			AddFuzzFamily(cases, "CharacterController.Move", "character:Move(@)", invalidVectors, finiteVectors);
			AddFuzzFamily(cases, "Time.SetTimeScale", "Time.SetTimeScale(@); assert(Time.GetTimeScale() == @)", invalidNumbers, durations);
			AddFuzzFamily(cases, "Task.Delay", "local task = Task.Delay(@, function() end); Task.Cancel(task)", invalidNumbers, durations);
			AddFuzzFamily(cases, "Input.IsGamepadConnected", "Input.IsGamepadConnected(@)", invalidNumbers, durations);
			AddFuzzFamily(cases, "Random.Integer", "Random.Integer(0, @)", invalidNumbers, durations);
			AddFuzzFamily(cases, "Mass", "dynamic.Mass = @", invalidNumbers, std::array<std::string_view, 1>{ "2" });
			// Zero is a valid velocity and wait duration, but not a cast direction or collision volume.
			cases.push_back({ "zero ray direction", "Physics.Raycast(vector.zero, vector.zero, 10)", "Physics.Raycast" });
			cases.push_back({ "zero cast distance", "Physics.Raycast(vector.zero, vector.create(0,0,1), 0)", "Physics.Raycast" });
			cases.push_back({ "zero sphere radius", "Physics.OverlapSphere(vector.zero, 0)", "Physics.OverlapSphere" });
			cases.push_back({ "negative box extent", "Physics.OverlapBox(vector.zero, vector.create(1,-1,1))", "Physics.OverlapBox" });
			cases.push_back({ "zero box extent", "Physics.OverlapBox(vector.zero, vector.create(1,1,0))", "Physics.OverlapBox" });
			cases.push_back({ "huge box extent", "Physics.OverlapBox(vector.zero, vector.create(1,1e30,1))", "Physics.OverlapBox" });
			cases.push_back({ "NaN box extent", "Physics.OverlapBox(vector.zero, vector.create(0/0,1,1))", "Physics.OverlapBox" });
			cases.push_back({ "infinite ray direction", "Physics.Raycast(vector.zero, vector.create(0,math.huge,0), 10)", "Physics.Raycast" });
			cases.push_back({ "subnormal ray direction", "Physics.Raycast(vector.zero, vector.create(0,0,1e-38), 10)", "Physics.Raycast" });
			cases.push_back({ "negative layer mask", "Physics.Raycast(vector.zero, vector.create(0,0,1), 1, -1)", "Physics.Raycast" });
			cases.push_back({ "fractional layer mask", "Physics.OverlapSphere(vector.zero, 1, 1.5)", "Physics.OverlapSphere" });
			cases.push_back({ "overflow layer mask", "Physics.OverlapBox(vector.zero, vector.create(1,1,1), nil, 4294967296)", "Physics.OverlapBox" });
			cases.push_back({ "unknown collision layer", "Physics.LayerMask('MissingFuzzLayer')", "Physics.LayerMask" });
			cases.push_back({ "static impulse", "fixed:AddImpulse(vector.create(1,0,0))", "RigidBody.AddImpulse" });
			cases.push_back({ "kinematic velocity", "kinematic:SetLinearVelocity(vector.create(1,0,0))", "RigidBody.SetLinearVelocity" });
			cases.push_back({ "dynamic kinematic movement", "dynamic:MoveKinematic(vector.zero, Quat.Identity())", "RigidBody.MoveKinematic" });
			cases.push_back({ "stale entity bounds", "Physics.GetBodyBounds(saved.Stale)", "Physics.GetBodyBounds" });
			cases.push_back({ "stale collider bounds", "Physics.GetColliderBounds(saved.Stale)", "Physics.GetColliderBounds" });
			cases.push_back({ "stale body proxy", "saved.StaleBody:AddForce(vector.create(1,0,0))", "RigidBody.AddForce" });
			cases.push_back({ "stale transform proxy", "saved.StaleTransform:Teleport(vector.zero)", "Transform.Teleport" });
			cases.push_back({ "wrong entity type", "Physics.GetBodyBounds({})", "Physics.GetBodyBounds" });
			cases.push_back({ "wrong task function", "Task.Spawn({})", "Task.Spawn" });
			cases.push_back({ "fractional wait ticks", "Task.WaitTicks(0.5)", "Task.WaitTicks" });
			for (const std::string_view rotation : invalidRotations)
			{
				// Constructor failures are intentional: malformed rotations must fail before the consuming physics call.
				const std::string member = rotation.starts_with("Quat.") ? "Quat.New" : "Physics.OverlapBox";
				cases.push_back({ std::format("invalid rotation {}", rotation), std::format("Physics.OverlapBox(vector.zero, vector.create(1,1,1), {})", rotation), member });
			}
			cases.push_back({ "known ray hit", "local hit = Physics.Raycast(vector.create(0,0,3), vector.create(0,0,-7), 10); assert(hit and hit.Entity.Name == 'Fixed' and math.abs(hit.Distance - 2.5) < 0.001)", {}, true });
			cases.push_back({ "zero mask excludes all", "assert(Physics.Raycast(vector.create(0,0,3), vector.create(0,0,-1), 10, 0) == nil)", {}, true });
			cases.push_back({ "known overlap", "local hits = Physics.OverlapSphere(vector.zero, 0.6); assert(#hits == 1 and hits[1].Name == 'Fixed')", {}, true });
			cases.push_back({ "finite huge direction normalizes safely", "local hit = Physics.Raycast(vector.create(0,0,3), vector.create(0,0,-1e30), 10); assert(hit and hit.Entity.Name == 'Fixed')", {}, true });
			cases.push_back({ "non-unit finite rotation normalizes safely", "local hits = Physics.OverlapBox(vector.zero, vector.create(1,1,1), Quat.New(0,0,0,7)); assert(#hits == 1 and hits[1].Name == 'Fixed')", {}, true });
			return cases;
		}

		static std::string BindingFuzzSource(const ScriptBindingFuzzCase& item, int64_t magnitude)
		{
			// Keep the call on authored line six, independent of seed or transport. k adds finite nonzero force samples.
			return std::format("local dynamic = Scene.FindByName('Dynamic').RigidBody\n"
							   "local kinematic = Scene.FindByName('Kinematic').RigidBody\n"
							   "local fixed = Scene.FindByName('Fixed').RigidBody\n"
							   "local character = Scene.FindByName('Character').CharacterController\n"
							   "local saved = Scene.FindByName('Keeper'):GetScript(); local k = {} / 4\n"
							   "{}\nreturn true",
				magnitude, item.Statement);
		}

		static void CheckFuzzBody(const PhysicsSystem& physics, UUID id)
		{
			const auto body = physics.GetBodyInfo(id);
			REQUIRE(body);
			CHECK(IsPlaceablePhysicsPose(body->Pose));
			for (int axis = 0; axis < 3; ++axis)
			{
				CHECK(std::isfinite(body->LinearVelocity[axis]));
				CHECK(std::isfinite(body->AngularVelocity[axis]));
				CHECK(std::isfinite(body->Bounds.Min[axis]));
				CHECK(std::isfinite(body->Bounds.Max[axis]));
			}
		}

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("Bindings: 10,000 seeded play-session calls reject invalid inputs before physics" * doctest::timeout(600))
		{
			Test::AutomationFixture fixture("ScriptBindingFuzz");
			Scene& edit = fixture.GetEditor().GetScene();
			Test::AddBoxBody(edit, "Fixed", glm::vec3(0), glm::vec3(0.5f), BodyType::Static);
			const UUID dynamicId = Test::AddBoxBody(edit, "Dynamic", glm::vec3(10, 0, 0), glm::vec3(0.5f), BodyType::Dynamic).GetUUID();
			const UUID kinematicId = Test::AddBoxBody(edit, "Kinematic", glm::vec3(20, 0, 0), glm::vec3(0.5f), BodyType::Kinematic).GetUUID();
			const UUID staleId = Test::AddBoxBody(edit, "Stale", glm::vec3(40, 0, 0), glm::vec3(0.5f), BodyType::Dynamic).GetUUID();
			const Entity character = edit.CreateEntity("Character");
			character.Patch<TransformComponent>([](auto& transform)
			{
				transform.Translation = glm::vec3(30, 0, 0);
			});
			character.AddComponent<CharacterControllerComponent>();
			const UUID characterId = character.GetUUID();
			const auto written = fixture.Call("script.write", Json{ { "path", "Assets/Scripts/FuzzKeeper.luau" }, { "source", R"(
local T = {}
function T.OnCreate(self: any)
	self.Stale = Scene.FindByName("Stale")
	self.StaleBody = self.Stale.RigidBody
	self.StaleTransform = self.Stale.Transform
	self.Stale:Destroy()
end
return Script.Define("FuzzKeeper", T)
)" } });
			REQUIRE_MESSAGE(written, (written ? "" : written.error().ToString()));
			REQUIRE(fixture.Call("entity.create", Json{ { "name", "Keeper" }, { "components", { { "Script", { { "Script", "Assets/Scripts/FuzzKeeper.luau" } } } } } }));
			REQUIRE(fixture.Call("play.start", Json{ { "mode", "play" }, { "paused", true }, { "seed", 0x13f022 } }));
			auto* session = fixture.GetEditor().GetPlay().GetSession();
			REQUIRE(session != nullptr);
			session->SetExtractionEnabled(false);
			REQUIRE(session->GetPhysics().SetGravity(glm::vec3(0)));
			session->Tick();
			CHECK_FALSE(session->GetScene().FindEntityByID(staleId));
			CHECK_FALSE(session->GetPhysics().GetBodyInfo(staleId));
			CHECK(session->GetPhysics().GetStats().BodyCount == 4);
			REQUIRE(session->GetScriptErrors().GetErrors().empty());
			const auto path = VfsPath::Parse("project://Assets/Tests/BindingFuzz.luau");
			REQUIRE(path);
			auto corpus = Test::BindingFuzzCorpus();
			for (const std::string_view method : { "AddForce", "AddImpulse", "AddTorque", "AddAngularImpulse", "SetLinearVelocity", "SetAngularVelocity" })
				corpus.push_back({ std::format("seeded finite {}", method), std::format("dynamic:{}(vector.create(k,-k,k/2))", method), {}, true });
			// Visit every case over both transports, then shuffle seeded rounds. This prevents random luck from omitting a boundary.
			std::vector<size_t> order(corpus.size() * 2);
			for (size_t index = 0; index < order.size(); ++index)
				order[index] = index;
			Random random(0x139af022);
			std::vector<uint32_t> visits(order.size());
			uint32_t accepted = 0;
			uint32_t rejected = 0;
			constexpr uint32_t Calls = 10000;
			for (uint32_t iteration = 0; iteration < Calls; ++iteration)
			{
				if (iteration % order.size() == 0)
					for (size_t remaining = order.size(); remaining > 1; --remaining)
						std::swap(order[remaining - 1], order[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(remaining - 1)))]);
				const size_t selected = order[iteration % order.size()];
				++visits[selected];
				const auto& item = corpus[selected / 2];
				const bool automation = selected % 2 != 0;
				const std::string source = Test::BindingFuzzSource(item, random.RangeInt(1, 28));
				CAPTURE(iteration);
				CAPTURE(item.Name);
				CAPTURE(automation);
				CAPTURE(source);
				auto& physics = session->GetPhysics();
				const glm::quat identity(1, 0, 0, 0);
				REQUIRE(physics.SetGravity(glm::vec3(0)));
				REQUIRE(physics.Teleport(dynamicId, glm::vec3(10, 0, 0), identity));
				REQUIRE(physics.SetLinearVelocity(dynamicId, glm::vec3(0)));
				REQUIRE(physics.SetAngularVelocity(dynamicId, glm::vec3(0)));
				REQUIRE(physics.Teleport(kinematicId, glm::vec3(20, 0, 0), identity));
				REQUIRE(physics.Teleport(characterId, glm::vec3(30, 0, 0), identity));
				REQUIRE(physics.MoveCharacter(characterId, glm::vec3(0)));
				const uint64_t before = session->ComputeStateHash();
				const double timeScale = session->GetTimeScale();
				const uint64_t errorCursor = session->GetScriptErrors().GetCursor();
				const auto execute = [&fixture, session, automation, &source, &path, &item]()
				{
					if (automation)
					{
						const Json response = fixture.Request("script.eval", Json{ { "context", "play" }, { "code", source } });
						if (item.Valid)
						{
							REQUIRE_MESSAGE(response.contains("result"), response.dump());
							CHECK(response["result"]["value"] == Json(true));
						}
						else
						{
							REQUIRE_MESSAGE(response.contains("error"), response.dump());
							CHECK(response["error"]["data"]["errorCode"] == Json("Script"));
							const Json& error = response["error"]["data"]["scriptError"];
							CHECK(error["kind"] == Json("runtime"));
							CHECK(error["line"] == Json(6));
							CHECK_FALSE(error["traceback"].empty());
						}
					}
					else
					{
						const auto result = session->GetScripts()->Evaluate(source, *path);
						if (item.Valid)
						{
							REQUIRE_MESSAGE(result, (result ? "" : result.error().ToString()));
							CHECK(result->Value.Get() == Json(true));
						}
						else
						{
							REQUIRE_FALSE(result);
							CHECK(result.error().GetCode() == ErrorCode::Script);
							CHECK(result.error().GetLocation().File == "Assets/Tests/BindingFuzz.luau");
							CHECK(result.error().GetLocation().Line == 6);
						}
					}
				};
				if (item.Valid)
				{
					execute();
					++accepted;
					CHECK(session->GetScriptErrors().GetCursor() == errorCursor);
				}
				else
				{
					Test::ExpectLog expected(LogLevel::Error, item.ErrorMember);
					execute();
					CHECK(expected.GetMatchCount() == 1);
					++rejected;
					const auto errors = session->GetScriptErrors().Read(errorCursor);
					REQUIRE(errors.size() == 1);
					CHECK(errors[0].Kind == ScriptErrorKind::Runtime);
					CHECK(errors[0].Line == 6);
					CHECK(errors[0].Script == (automation ? "eval" : "Assets/Tests/BindingFuzz.luau"));
					CHECK(errors[0].Message.find(item.ErrorMember) != std::string::npos);
					CHECK_FALSE(errors[0].Traceback.empty());
					CHECK(session->ComputeStateHash() == before);
					CHECK(session->GetTimeScale() == timeScale);
				}
				const uint64_t tick = session->GetTick();
				session->Tick();
				REQUIRE(session->GetScripts() != nullptr);
				CHECK_FALSE(session->GetScripts()->IsStopped());
				CHECK_FALSE(session->GetQuitRequest());
				CHECK(session->GetTick() == tick + 1);
				CHECK(physics.GetDiagnostics().empty());
				CHECK(physics.GetStats().BodyCount == 4);
				Test::CheckFuzzBody(physics, dynamicId);
				Test::CheckFuzzBody(physics, kinematicId);
				Test::CheckFuzzBody(physics, characterId);
				if (item.Valid && item.Name.starts_with("seeded finite"))
				{
					const auto body = physics.GetBodyInfo(dynamicId);
					REQUIRE(body);
					const bool angular = item.Name.find("Torque") != std::string::npos || item.Name.find("Angular") != std::string::npos;
					const glm::vec3 velocity = angular ? body->AngularVelocity : body->LinearVelocity;
					CHECK(glm::dot(velocity, velocity) > 0.0f);
				}
				if (!item.Valid)
				{
					// Queued forces are not part of an immediate hash: advancing Jolt catches partial writes before rejection.
					const auto body = physics.GetBodyInfo(dynamicId);
					REQUIRE(body);
					CHECK(body->Pose.Position == glm::vec3(10, 0, 0));
					CHECK(body->LinearVelocity == glm::vec3(0));
					CHECK(body->AngularVelocity == glm::vec3(0));
					CHECK(physics.GetGravity() == glm::vec3(0));
				}
			}
			CHECK(accepted + rejected == Calls);
			CHECK(accepted > 1000);
			CHECK(rejected > 5000);
			for (const uint32_t count : visits)
				CHECK(count > 0);
			const auto healthy = fixture.Call("script.eval", Json{ { "context", "play" }, { "code", "local e = Scene.CreateEntity('StillUsable'); return e:IsValid()" } });
			REQUIRE_MESSAGE(healthy, (healthy ? "" : healthy.error().ToString()));
			CHECK((*healthy)["value"] == Json(true));
			CHECK(Test::GetEntityId(session->GetScene(), "/StillUsable").IsValid());
		}
	}

}
