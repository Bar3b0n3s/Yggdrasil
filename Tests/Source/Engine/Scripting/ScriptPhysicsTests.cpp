#include "TestsPCH.h"
#include "Engine/Scripting/ScriptEngine.h"

#include "Engine/Scene/Components/RigidBodyComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Session/PlaySession.h"
#include "Support/PhysicsTestScene.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The physics callbacks scripts receive (Architecture §9.4, §11.3) for scripted colliders gathered into another owner's
// compound (Docs/Decisions/0031-m20-contract.md decision 2, ADR 0026 P01): each piece's own behaviour runs its collision
// callbacks, after the body owner's, in a real PlaySession.

namespace Engine {

	namespace {

		// Records each callback as a "Trace:<entity>:<callback>:<other>" entity, so the trace keeps the dispatch order in the
		// scene's canonical order. A piece asserts that the contact names it.
		constexpr std::string_view CollisionTraceSource = R"(
local T = {}
local function record(self, phase, other)
	Scene.CreateEntity("Trace:" .. self.Entity.Name .. ":" .. phase .. ":" .. other.Name)
end
function T:OnCollisionEnter(other, contact)
	if self.Entity.Name == "Brick" then
		assert(contact.Collider == self.Entity, "the contact names the brick itself")
		assert(contact.OtherCollider == other and other.Name == "Ball")
		assert(contact.Normal.y > 0.9, "the normal points from the brick's body towards the ball")
	end
	record(self, "enter", other)
end
function T:OnCollisionExit(other)
	record(self, "exit", other)
end
return Script.Define("CollisionTrace", T)
)";

		std::vector<std::string> CollisionTrace(Scene& scene)
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

		std::string DescribeErrors(const ScriptErrorStream& errors)
		{
			std::string result;
			for (const ScriptError& error : errors.GetErrors())
				result += std::format("{}:{} {}\n", error.Script, error.Line, error.Message);
			return result;
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptEngine: a scripted collider gathered into a compound runs its own collision callbacks after its body owner's")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle handle(0x2b0001);
			REQUIRE(fixture.AddScript(handle, "Assets/Scripts/CollisionTrace.luau", std::string(CollisionTraceSource)));
			ScriptComponent script{};
			script.Script.SetHandle(handle);

			// A level root with a Static RigidBody gathers its three pieces into one compound; only the brick at x = 2 and the
			// level have a script.
			Scene& editScene = fixture.GetScene();
			Entity level = editScene.CreateEntity("Level");
			level.AddComponent<RigidBodyComponent>(RigidBodyComponent{ .Type = BodyType::Static });
			level.AddComponent<ScriptComponent>(script);
			for (const auto& [name, x] : { std::pair{ "Piece", 0.0f }, std::pair{ "Piece", 1.0f }, std::pair{ "Brick", 2.0f } })
				static_cast<void>(Test::AddBoxBody(editScene, name, glm::vec3(x, 0.0f, 0.0f), glm::vec3(0.5f), std::nullopt, level));
			editScene.FindEntityByPath("/Level/Brick").AddComponent<ScriptComponent>(script);
			Entity ball = Test::AddSphereBody(editScene, "Ball", glm::vec3(2.0f, 3.0f, 0.0f), 0.25f, BodyType::Dynamic);
			Test::PatchRigidBody(ball, [](RigidBodyComponent& body)
			{
				body.AllowSleeping = false;
			});

			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.Seed = 7;
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.ScriptRunMode = RunModes::Editor;
			Result<Scope<PlaySession>> started = PlaySession::CreateFromScene(specification, editScene);
			REQUIRE_MESSAGE(started.has_value(), (started ? "" : started.error().ToString()));
			Scope<PlaySession> session = std::move(*started);
			session->SetExtractionEnabled(false);
			Scene& scene = session->GetScene();

			Test::RunTicks(*session, 90);
			REQUIRE_MESSAGE(session->GetScriptErrors().GetErrors().empty(), DescribeErrors(session->GetScriptErrors()));
			CHECK(CollisionTrace(scene) == std::vector<std::string>{ "Level:enter:Ball", "Brick:enter:Ball" });

			REQUIRE(session->GetPhysics().Teleport(ball.GetUUID(), glm::vec3(2.0f, 10.0f, 0.0f), std::nullopt).has_value());
			Test::RunTicks(*session, 2);
			REQUIRE_MESSAGE(session->GetScriptErrors().GetErrors().empty(), DescribeErrors(session->GetScriptErrors()));
			CHECK(CollisionTrace(scene) == std::vector<std::string>{ "Level:enter:Ball", "Brick:enter:Ball", "Level:exit:Ball", "Brick:exit:Ball" });
		}
	}

}
