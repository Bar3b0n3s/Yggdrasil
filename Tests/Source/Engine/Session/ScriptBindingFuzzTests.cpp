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
#include "Support/Fuzz.h"
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
#include <utility>
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
			AddFuzzFamily(cases, "RigidBody:AddForce", "dynamic:AddForce(@)", invalidVectors, finiteVectors);
			AddFuzzFamily(cases, "RigidBody:AddForceAtPosition", "dynamic:AddForceAtPosition(vector.create(1,0,0), @)", invalidVectors, finiteVectors);
			AddFuzzFamily(cases, "RigidBody:AddTorque", "dynamic:AddTorque(@)", invalidVectors, finiteVectors);
			AddFuzzFamily(cases, "RigidBody:AddImpulse", "dynamic:AddImpulse(@)", invalidVectors, finiteVectors);
			AddFuzzFamily(cases, "RigidBody:AddAngularImpulse", "dynamic:AddAngularImpulse(@)", invalidVectors, finiteVectors);
			AddFuzzFamily(cases, "RigidBody:SetLinearVelocity", "dynamic:SetLinearVelocity(@); assert(dynamic:GetLinearVelocity() == @)", invalidVectors, finiteVectors);
			AddFuzzFamily(cases, "RigidBody:SetAngularVelocity", "dynamic:SetAngularVelocity(@); assert(dynamic:GetAngularVelocity() == @)", invalidVectors, finiteVectors);
			AddFuzzFamily(cases, "RigidBody:Teleport", "dynamic:Teleport(@)", invalidVectors, std::array<std::string_view, 1>{ "vector.create(10,2,0)" });
			AddFuzzFamily(cases, "RigidBody:MoveKinematic", "kinematic:MoveKinematic(@, Quat.Identity())", invalidVectors, std::array<std::string_view, 1>{ "vector.create(20,2,0)" });
			AddFuzzFamily(cases, "CharacterController:Move", "character:Move(@)", invalidVectors, finiteVectors);
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
			cases.push_back({ "static impulse", "fixed:AddImpulse(vector.create(1,0,0))", "RigidBody:AddImpulse" });
			cases.push_back({ "kinematic velocity", "kinematic:SetLinearVelocity(vector.create(1,0,0))", "RigidBody:SetLinearVelocity" });
			cases.push_back({ "dynamic kinematic movement", "dynamic:MoveKinematic(vector.zero, Quat.Identity())", "RigidBody:MoveKinematic" });
			cases.push_back({ "stale entity bounds", "Physics.GetBodyBounds(saved.Stale)", "Physics.GetBodyBounds" });
			cases.push_back({ "stale collider bounds", "Physics.GetColliderBounds(saved.Stale)", "Physics.GetColliderBounds" });
			cases.push_back({ "stale body proxy", "saved.StaleBody:AddForce(vector.create(1,0,0))", "RigidBody:AddForce" });
			cases.push_back({ "stale transform proxy", "saved.StaleTransform:Teleport(vector.zero)", "Transform:Teleport" });
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

		// Seeded argument generation for the surfaces through which script values reach transforms, colliders, bodies and
		// prefab instantiation: every value class (NaN, infinities, zero, negative, huge, subnormal, wrong type, stale).
		static std::string FuzzNumber(Random& random)
		{
			static constexpr std::array<std::string_view, 15> Classes = { "0/0", "math.huge", "-math.huge", "0", "-0", "-1", "1e30", "-1e30", "3e38", "-3e38", "1e-40", "1e-45", "'1'", "true", "nil" };
			if (random.RangeInt(0, 2) == 0)
				return std::format("{:.9g}", random.RangeDouble(-1000.0, 1000.0));
			return std::string(Classes[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(Classes.size()) - 1))]);
		}

		static std::string FuzzVector(Random& random)
		{
			static constexpr std::array<std::string_view, 5> WrongTypes = { "'v'", "{}", "Color.New(1,1,1)", "Quat.Identity()", "nil" };
			if (random.RangeInt(0, 6) == 0)
				return std::string(WrongTypes[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(WrongTypes.size()) - 1))]);
			return std::format("vector.create({}, {}, {})", FuzzNumber(random), FuzzNumber(random), FuzzNumber(random));
		}

		static std::string FuzzQuat(Random& random)
		{
			if (random.RangeInt(0, 3) == 0)
				return "Quat.Identity()";
			return std::format("Quat.New({}, {}, {}, {})", FuzzNumber(random), FuzzNumber(random), FuzzNumber(random), FuzzNumber(random));
		}

		static std::string FuzzParent(Random& random)
		{
			static constexpr std::array<std::string_view, 8> Parents = { "nil", "holder", "tiny", "far", "child", "dynamic", "stale", "{}" };
			return std::string(Parents[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(Parents.size()) - 1))]);
		}

		static std::string FuzzBool(Random& random)
		{
			static constexpr std::array<std::string_view, 4> Values = { "true", "false", "nil", "1" };
			return std::string(Values[static_cast<size_t>(random.RangeInt(0, static_cast<int64_t>(Values.size()) - 1))]);
		}

		static std::string GeneratedFuzzStatement(Random& random)
		{
			switch (random.RangeInt(0, 21))
			{
				case 0:  return "dynamic.Transform.Translation = " + FuzzVector(random);
				case 1:  return "dynamic.Transform.Scale = " + FuzzVector(random);
				case 2:  return "dynamic.Transform.Rotation = " + FuzzQuat(random);
				case 3:  return "dynamic.Transform.EulerAngles = " + FuzzVector(random);
				case 4:  return "child.Transform.WorldPosition = " + FuzzVector(random);
				case 5:  return "holder.Transform.Scale = " + FuzzVector(random);
				case 6:  return "holder.Transform.Translation = " + FuzzVector(random);
				case 7:  return std::format("dynamic.Transform:Teleport({}, {})", FuzzVector(random), FuzzQuat(random));
				case 8:  return std::format("dynamic.Transform:LookAt({})", FuzzVector(random));
				case 9:  return std::format("dynamic.Transform:Translate({})", FuzzVector(random));
				case 10: return std::format("dynamic.Transform:Rotate({})", FuzzVector(random));
				case 11: return "dynamic.BoxCollider.HalfExtents = " + FuzzVector(random);
				case 12: return "ball.SphereCollider.Radius = " + FuzzNumber(random);
				case 13: return "dynamic.RigidBody.Mass = " + FuzzNumber(random);
				case 14: return std::format("Scene.CreateEntity('Gen'):AddComponent('BoxCollider', {{HalfExtents = {}}})", FuzzVector(random));
				case 15: return std::format("local e = Scene.CreateEntity('Gen'); e:AddComponent('BoxCollider'); e:AddComponent('RigidBody', {{Mass = {}}})", FuzzNumber(random));
				case 16: return std::format("Scene.Instantiate(prefab, {}, {}, {})", FuzzVector(random), FuzzQuat(random), FuzzParent(random));
				case 17: return std::format("Scene.Instantiate(prefab, {}, nil, {})", FuzzVector(random), FuzzParent(random));
				case 18: return std::format("child:SetParent({}, {})", FuzzParent(random), FuzzBool(random));
				case 19: return std::format("dynamic:SetParent({}, {})", FuzzParent(random), FuzzBool(random));
				case 20: return "tiny.Transform.Scale = " + FuzzVector(random);
				default: return std::format("ball.Transform.WorldPosition = {}", FuzzVector(random));
			}
		}

		// The statement is on line 6. Line 7 always raises, so every generated call publishes exactly one error: line 6 when
		// the bindings rejected the statement, line 7 (fuzz-accepted) when the statement ran and its effects remain.
		static std::string GeneratedFuzzSource(std::string_view statement)
		{
			return std::format("local dynamic, holder, child = Scene.FindByName('Dynamic'), Scene.FindByName('Holder'), Scene.FindByName('Child')\n"
							   "local tiny, far, ball = Scene.FindByName('Tiny'), Scene.FindByName('Far'), Scene.FindByName('Ball')\n"
							   "local prefab = Assets.Load('Assets/Prefabs/FuzzBody.prefab')\n"
							   "local saved = Scene.FindByName('Keeper'):GetScript()\n"
							   "local stale = saved.Stale\n"
							   "{}\nerror('fuzz-accepted')",
				statement);
		}

		static constexpr std::string_view GeneratedFuzzReset = R"(
for _, name in {'Gen', 'FuzzBody'} do
	for _, e in Scene.FindAllByName(name) do e:Destroy() end
end
local dynamic, holder, child = Scene.FindByName('Dynamic'), Scene.FindByName('Holder'), Scene.FindByName('Child')
local tiny, ball = Scene.FindByName('Tiny'), Scene.FindByName('Ball')
dynamic:SetParent(nil, false)
child:SetParent(holder, false)
ball:SetParent(nil, false)
holder:SetParent(nil, false)
for _, entity in {dynamic, holder, child, ball, tiny} do
	entity.Transform.Rotation = Quat.Identity()
	entity.Transform.Scale = vector.one
end
dynamic.Transform.Translation = vector.create(10, 0, 0)
holder.Transform.Translation = vector.create(80, 0, 0)
child.Transform.Translation = vector.zero
ball.Transform.Translation = vector.create(100, 0, 0)
tiny.Transform.Scale = vector.create(0.00011, 0.00011, 0.00011)
dynamic.BoxCollider.HalfExtents = vector.create(0.5, 0.5, 0.5)
dynamic.RigidBody.Mass = 1
ball.SphereCollider.Radius = 0.5
return true
)";

		// No script value may leave a non-finite or degenerate transform anywhere in the scene, nor a non-finite body.
		static void CheckGeneratedFuzzScene(PlaySession& session)
		{
			auto& physics = session.GetPhysics();
			for (const UUID id : session.GetScene().GetCanonicalOrder())
			{
				const Entity entity = session.GetScene().FindEntityByID(id);
				if (!entity)
					continue;
				const auto& transform = entity.GetComponent<TransformComponent>();
				for (int axis = 0; axis < 3; ++axis)
				{
					CHECK(std::isfinite(transform.Translation[axis]));
					CHECK(std::isfinite(transform.Scale[axis]));
				}
				for (int axis = 0; axis < 4; ++axis)
					CHECK(std::isfinite(transform.Rotation[axis]));
				if (physics.GetBodyInfo(id))
					CheckFuzzBody(physics, id);
			}
		}

	}

	TEST_SUITE("Session")
	{
		// The watchdog's default limit applies, scaled by --test-timeout-scale, which the sanitized unit run and the hardening
		// stage's 1,000,000-iteration shards rely on: the Debug unit run executes the first tenth of the count (Test.py's
		// --fuzz-scale=0.1, ADR 0024 decision 14), and a manual Debug run of the whole count passes --test-timeout-scale=2.
		TEST_CASE("Bindings: 10,000 seeded play-session calls reject invalid inputs before physics" * doctest::test_suite(Test::FuzzSuite))
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
			// Targets of the generated phase, far from the corpus's bodies at the origin: a body under a plain parent, a parent
			// with the smallest allowed scale, one with a huge translation, and a sphere body.
			const Entity holder = edit.CreateEntity("Holder");
			holder.Patch<TransformComponent>([](auto& transform)
			{
				transform.Translation = glm::vec3(80, 0, 0);
			});
			Test::AddBoxBody(edit, "Child", glm::vec3(0), glm::vec3(0.5f), BodyType::Dynamic, holder);
			edit.CreateEntity("Tiny").Patch<TransformComponent>([](auto& transform)
			{
				transform.Translation = glm::vec3(70, 0, 0);
				transform.Scale = glm::vec3(0.00011f);
			});
			edit.CreateEntity("Far").Patch<TransformComponent>([](auto& transform)
			{
				transform.Translation = glm::vec3(3e38f, 0, 0);
			});
			Test::AddSphereBody(edit, "Ball", glm::vec3(100, 0, 0), 0.5f, BodyType::Dynamic);
			Test::AddBoxBody(edit, "FuzzBody", glm::vec3(120, 0, 0), glm::vec3(0.5f), BodyType::Dynamic);
			REQUIRE(fixture.Call("prefab.create", Json{ { "entity", "/FuzzBody" }, { "path", "Assets/Prefabs/FuzzBody.prefab" }, { "replaceWithInstance", false } }));
			REQUIRE(fixture.Call("entity.destroy", Json{ { "entities", Json::array({ "/FuzzBody" }) } }));
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
			// Fixed, Dynamic, Kinematic, Character, Child and Ball; Stale is destroyed by its keeper's OnCreate.
			constexpr uint32_t ExpectedBodies = 6;
			session->SetExtractionEnabled(false);
			REQUIRE(session->GetPhysics().SetGravity(glm::vec3(0)));
			session->Tick();
			CHECK_FALSE(session->GetScene().FindEntityByID(staleId));
			CHECK_FALSE(session->GetPhysics().GetBodyInfo(staleId));
			CHECK(session->GetPhysics().GetStats().BodyCount == ExpectedBodies);
			REQUIRE(session->GetScriptErrors().GetErrors().empty());
			const auto path = VfsPath::Parse("project://Assets/Tests/BindingFuzz.luau");
			REQUIRE(path);
			auto corpus = Test::BindingFuzzCorpus();
			for (const std::string_view method : { "AddForce", "AddImpulse", "AddTorque", "AddAngularImpulse", "SetLinearVelocity", "SetAngularVelocity" })
				corpus.push_back({ std::format("seeded finite {}", method), std::format("dynamic:{}(vector.create(k,-k,k/2))", method), {}, true });
			// The index mapping of decision 13 (Support/Fuzz.h): iteration i is a corpus call when i % 10 < 7 and a generated
			// call otherwise. The corpus calls, numbered in index order, form rounds of 2 x the corpus size, each a seeded
			// permutation of every case over both transports, so every boundary is visited in each complete round.
			constexpr uint64_t CaseSeed = 0x139af022;
			constexpr uint64_t RoundSeed = CaseSeed ^ 0x526F756E64ull; // the permutation of corpus round r: Random(GetFuzzIterationSeed(RoundSeed, r))
			const uint64_t roundSize = corpus.size() * 2;
			const auto corpusCallsBelow = [](uint64_t index)
			{
				return index / 10 * 7 + std::min<uint64_t>(index % 10, 7);
			};
			const auto generatedCallsBelow = [](uint64_t index)
			{
				return index / 10 * 3 + (index % 10 > 7 ? index % 10 - 7 : 0);
			};
			// The smallest count whose corpus calls complete the first round.
			const uint64_t minimum = (roundSize - 1) / 7 * 10 + (roundSize - 1) % 7 + 1;
			std::vector<size_t> order(roundSize);
			uint64_t orderRound = UINT64_MAX;
			const auto permutationOf = [&order, &orderRound](uint64_t round) -> const std::vector<size_t>&
			{
				if (round != orderRound)
				{
					for (size_t slot = 0; slot < order.size(); ++slot)
						order[slot] = slot;
					Random shuffle(Test::GetFuzzIterationSeed(RoundSeed, round));
					for (size_t remaining = order.size(); remaining > 1; --remaining)
						std::swap(order[remaining - 1], order[static_cast<size_t>(shuffle.RangeInt(0, static_cast<int64_t>(remaining - 1)))]);
					orderRound = round;
				}
				return order;
			};

			// Every call starts from the same physics state: the corpus's bodies at rest where they began, no gravity and the
			// normal time scale. The scene's structure is restored after every generated call (reset below), and corpus calls
			// change none of it, so an iteration's outcome depends on its index alone. The mass a valid "Mass" case sets is
			// observed by no check.
			const auto resetPhysics = [session, dynamicId, kinematicId, characterId]()
			{
				auto& physics = session->GetPhysics();
				const glm::quat identity(1, 0, 0, 0);
				REQUIRE(session->SetTimeScale(1.0));
				REQUIRE(physics.SetGravity(glm::vec3(0)));
				REQUIRE(physics.Teleport(dynamicId, glm::vec3(10, 0, 0), identity));
				REQUIRE(physics.SetLinearVelocity(dynamicId, glm::vec3(0)));
				REQUIRE(physics.SetAngularVelocity(dynamicId, glm::vec3(0)));
				REQUIRE(physics.Teleport(kinematicId, glm::vec3(20, 0, 0), identity));
				REQUIRE(physics.Teleport(characterId, glm::vec3(30, 0, 0), identity));
				REQUIRE(physics.MoveCharacter(characterId, glm::vec3(0)));
			};
			const auto reset = [session, &path]()
			{
				const auto restored = session->GetScripts()->Evaluate(Test::GeneratedFuzzReset, *path);
				REQUIRE_MESSAGE(restored, (restored ? "" : restored.error().ToString()));
				session->Tick();
				auto& physics = session->GetPhysics();
				for (const std::string_view name : { "/Dynamic", "/Holder/Child", "/Ball" })
				{
					const UUID id = Test::GetEntityId(session->GetScene(), name);
					REQUIRE(id.IsValid());
					if (physics.GetBodyInfo(id))
					{
						REQUIRE(physics.SetLinearVelocity(id, glm::vec3(0)));
						REQUIRE(physics.SetAngularVelocity(id, glm::vec3(0)));
					}
				}
			};
			// A generated call either fails at line 6 (the bindings rejected it) or reaches line 7, which always raises. The
			// guard in Scene.Instantiate's host path rejects an unrepresentable position before anything is created. Callers
			// declare the published error (and any physics diagnostic an accepted extreme value causes) with ExpectLog.
			const auto runGenerated = [&fixture, session, &path](const std::string& source, bool automation) -> uint32_t
			{
				const uint64_t cursor = session->GetScriptErrors().GetCursor();
				if (automation)
				{
					const Json response = fixture.Request("script.eval", Json{ { "context", "play" }, { "code", source } });
					REQUIRE_MESSAGE(response.contains("error"), response.dump());
				}
				else
					CHECK_FALSE(session->GetScripts()->Evaluate(source, *path));
				const auto errors = session->GetScriptErrors().Read(cursor);
				REQUIRE(errors.size() == 1);
				CHECK(errors[0].Kind == ScriptErrorKind::Runtime);
				CHECK((errors[0].Line == 6 || errors[0].Line == 7));
				CHECK((errors[0].Line == 7) == (errors[0].Message.find("fuzz-accepted") != std::string::npos));
				return errors[0].Line;
			};

			// Once per process, before any call: the scene's canonical state and the three fixed generated statements.
			reset();
			for (const auto& [statement, expected] : { std::pair<std::string_view, std::string_view>{ "Scene.Instantiate(prefab, vector.create(1e36, 0, 0), nil, tiny)", "cannot be represented relative to parent" },
					 std::pair<std::string_view, std::string_view>{ "Scene.Instantiate(prefab, vector.create(-3e38, 0, 0), Quat.Identity(), far)", "cannot be represented relative to parent" },
					 std::pair<std::string_view, std::string_view>{ "child.Transform.WorldPosition = vector.create(1e36, 0, 0); tiny.Transform.Scale = vector.one", "" } })
			{
				CAPTURE(statement);
				Test::ExpectLog published(LogLevel::Error, "");
				const size_t before = session->GetScene().GetEntityCount();
				const uint64_t cursor = session->GetScriptErrors().GetCursor();
				CHECK(runGenerated(Test::GeneratedFuzzSource(statement), false) == (expected.empty() ? 7u : 6u));
				const auto errors = session->GetScriptErrors().Read(cursor);
				REQUIRE(errors.size() == 1);
				CHECK(errors[0].Message.find(expected) != std::string::npos);
				CHECK(session->GetScene().GetEntityCount() == before);
				reset();
			}

			std::vector<uint32_t> visits(roundSize);
			uint64_t accepted = 0;
			uint64_t rejected = 0;
			uint64_t generatedAccepted = 0;
			uint64_t generatedRejected = 0;
			const auto corpusCall = [&](Random& random, uint64_t index) -> bool
			{
				const uint64_t number = corpusCallsBelow(index);
				const size_t selected = permutationOf(number / roundSize)[static_cast<size_t>(number % roundSize)];
				++visits[selected];
				const auto& item = corpus[selected / 2];
				const bool automation = selected % 2 != 0;
				const std::string source = Test::BindingFuzzSource(item, random.RangeInt(1, 28));
				CAPTURE(item.Name);
				CAPTURE(automation);
				CAPTURE(source);
				auto& physics = session->GetPhysics();
				// The session keeps every physics diagnostic it raised (a generated call's extreme values may raise some): a
				// corpus call raises none of its own.
				const size_t diagnostics = physics.GetDiagnostics().size();
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
				CHECK(physics.GetDiagnostics().size() == diagnostics);
				CHECK(physics.GetStats().BodyCount == ExpectedBodies);
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
				return item.Valid;
			};
			const auto generatedCall = [&](Random& random, uint64_t index) -> bool
			{
				const std::string statement = Test::GeneratedFuzzStatement(random);
				const bool automation = generatedCallsBelow(index) % 2 != 0;
				CAPTURE(statement);
				CAPTURE(automation);
				Test::ExpectLog published(LogLevel::Error, "");
				const uint32_t line = runGenerated(Test::GeneratedFuzzSource(statement), automation);
				++(line == 7 ? generatedAccepted : generatedRejected);
				const uint64_t tick = session->GetTick();
				session->Tick();
				REQUIRE(session->GetScripts() != nullptr);
				CHECK_FALSE(session->GetScripts()->IsStopped());
				CHECK_FALSE(session->GetQuitRequest());
				CHECK(session->GetTick() == tick + 1);
				Test::CheckGeneratedFuzzScene(*session);
				reset();
				Test::CheckGeneratedFuzzScene(*session);
				return line == 7;
			};
			const Test::FuzzRun run = Test::RunFuzzCase(CaseSeed, 10000, [&](Random& random, uint64_t index)
			{
				resetPhysics();
				return index % 10 < 7 ? corpusCall(random, index) : generatedCall(random, index);
			}, minimum);

			// The thresholds are ratios of the calls this process ran, checked when they hold a complete corpus round.
			const uint64_t firstCorpus = corpusCallsBelow(run.Range.Begin);
			const uint64_t corpusCalls = corpusCallsBelow(run.Range.End) - firstCorpus;
			const uint64_t generatedCalls = generatedCallsBelow(run.Range.End) - generatedCallsBelow(run.Range.Begin);
			CHECK(accepted + rejected == corpusCalls);
			CHECK(generatedAccepted + generatedRejected == generatedCalls);
			const uint64_t firstCompleteRound = (firstCorpus + roundSize - 1) / roundSize * roundSize;
			if (Test::IsFuzzSample(run.Range) && firstCompleteRound + roundSize <= firstCorpus + corpusCalls)
			{
				CHECK(accepted * 10 > corpusCalls);
				CHECK(rejected * 2 > corpusCalls);
				for (const uint32_t count : visits)
					CHECK(count > 0);
				CHECK(generatedAccepted * 10 > generatedCalls);
				CHECK(generatedRejected * 2 > generatedCalls);
			}
			const auto healthy = fixture.Call("script.eval", Json{ { "context", "play" }, { "code", "local e = Scene.CreateEntity('StillUsable'); return e:IsValid()" } });
			REQUIRE_MESSAGE(healthy, (healthy ? "" : healthy.error().ToString()));
			CHECK((*healthy)["value"] == Json(true));
			CHECK(Test::GetEntityId(session->GetScene(), "/StillUsable").IsValid());
		}
	}

}
