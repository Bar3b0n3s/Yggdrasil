#include "TestsPCH.h"
#include "Engine/Session/PlaySession.h"

#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>

#include <string>
#include <vector>

// M28 (Docs/Decisions/0043-m28-game-services.md decision 6): frame-time signals. A frame-phase decision (Input.PostSignal)
// is input for the next tick, which latches it for Input.WasSignalled; live play and recordings take live signals, while
// test runs and lockstep sessions take only injected ones.

namespace Engine {

	namespace {

		// Signal.luau: OnUpdate posts "Slow" in the session's second frame and records whether the session took it;
		// OnFixedUpdate records the tick that latched it, and that a fixed-step post raises.
		constexpr std::string_view SignalSource = R"(
local Signal = {}
local function trace(what: string)
	Scene.CreateEntity("trace:" .. what)
end
function Signal:OnFixedUpdate()
	if not self.Tried then
		self.Tried = true
		local ok = pcall(Input.PostSignal, "Fixed")
		trace("fixed-post:" .. tostring(ok))
	end
	if Input.WasSignalled("Slow") then
		trace("latched:" .. Time.GetTick())
	end
end
function Signal:OnUpdate()
	self.Frames = (self.Frames or 0) + 1
	if self.Frames == 2 then
		trace("posted:" .. tostring(Input.PostSignal("Slow")))
	end
	if Input.WasSignalled("Slow") then
		trace("frame-saw")
	end
end
return Script.Define("Signal", Signal)
)";

		std::vector<std::string> Traces(Scene& scene)
		{
			std::vector<std::string> names;
			for (const UUID id : scene.GetCanonicalOrder())
			{
				const Entity entity = scene.FindEntityByID(id);
				if (entity && entity.GetName().starts_with("trace:"))
					names.push_back(entity.GetName().substr(6));
			}
			return names;
		}

		Scope<PlaySession> MakeSignalSession(Test::ScriptTestFixture& fixture, bool testMode)
		{
			const AssetHandle script(0x7a2810);
			REQUIRE(fixture.AddScript(script, "Assets/Scripts/Signal.luau", std::string(SignalSource)));
			Entity probe = fixture.GetScene().CreateEntity("Probe");
			ScriptSlot slot{};
			slot.Script.SetHandle(script);
			probe.AddComponent<ScriptComponent>(ScriptComponent{ { slot } });
			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.Seed = 5;
			specification.Serial = 1;
			specification.TestMode = testMode;
			auto created = PlaySession::CreateFromScene(specification, fixture.GetScene());
			REQUIRE_MESSAGE(created.has_value(), (created ? "" : created.error().ToString()));
			(*created)->SetExtractionEnabled(false);
			return std::move(*created);
		}

		void RunFrame(PlaySession& session)
		{
			session.FixedStep();
			session.FrameUpdate(FrameTime{ .DeltaTime = session.GetFixedDelta(), .UnscaledDeltaTime = session.GetFixedDelta(), .Alpha = 1.0, .FrameIndex = 0 });
		}

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("PlaySession: a live signal posted in the frame phase is latched by the next tick, and the fixed step cannot post one")
		{
			Test::ScriptTestFixture fixture;
			const Scope<PlaySession> session = MakeSignalSession(fixture, false);
			for (int frame = 0; frame < 4; ++frame)
				RunFrame(*session);
			// Frame 2 (tick 1's frame) posts; tick 2 latches it, and frame 3's view saw it.
			CHECK(Traces(session->GetScene()) == std::vector<std::string>{ "fixed-post:false", "posted:true", "latched:2", "frame-saw" });
		}

		TEST_CASE("PlaySession: a test session and a lockstep session ignore live signals and take injected ones")
		{
			Test::ScriptTestFixture fixture;
			const Scope<PlaySession> session = MakeSignalSession(fixture, true);
			RunFrame(*session);
			RunFrame(*session);
			RunFrame(*session);
			CHECK(Traces(session->GetScene()) == std::vector<std::string>{ "fixed-post:false", "posted:false" });
			// An injected signal (a replay's, a test's, input.inject's) is latched by its tick.
			REQUIRE(session->GetInput().Queue(session->GetInput().GetNextTick(), PlayInputEvent{ .Type = PlayInputEventType::Signal, .Name = "Slow" }).has_value());
			RunFrame(*session);
			CHECK(Traces(session->GetScene()) == std::vector<std::string>{ "fixed-post:false", "posted:false", "latched:3", "frame-saw" });

			Test::ScriptTestFixture lockstepFixture;
			const Scope<PlaySession> lockstep = MakeSignalSession(lockstepFixture, false);
			lockstep->SetLockstep(true, 1);
			for (int frame = 0; frame < 4; ++frame)
				RunFrame(*lockstep);
			CHECK(Traces(lockstep->GetScene()) == std::vector<std::string>{ "fixed-post:false", "posted:false" });
		}
	}

}
