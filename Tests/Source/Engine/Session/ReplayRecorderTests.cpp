#include "TestsPCH.h"
#include "Engine/Session/ReplayRecorder.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Session/PlaySession.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

#include <array>

namespace Engine {

	namespace {

		struct RecordingFixture
		{
			Test::SceneTestFixture Scene{};
			Test::InMemoryAssetManager Assets{};
			PlaySessionSpecification Spec{};
			ReplayHeader Header{};

			RecordingFixture()
			{
				Spec.Registry = &Scene.GetRegistry();
				Spec.Assets = &Assets;
				Spec.Serial = 91;
				Spec.Seed = 73;
				Spec.Project.Simulation.FixedHz = 144;
				Spec.Parameters.Set(Json{ { "Level", 2 } });
				Header.Scene = { UUID(200), "Assets/Scenes/Test.scene" };
				Header.Parameters = Spec.Parameters;
				Header.Seed = Spec.Seed;
				Header.FixedHz = Spec.Project.Simulation.FixedHz;
				Header.EngineVersion = "1";
				Header.Config = "Debug";
			}

			Scope<PlaySession> Create(uint64_t serial = 91)
			{
				Spec.Serial = serial;
				auto session = PlaySession::CreateFromScene(Spec, Scene.GetScene());
				REQUIRE(session);
				return std::move(*session);
			}

			void Step(ReplayRecorder& recorder, PlaySession& session)
			{
				const auto tick = session.GetTick();
				session.Tick();
				REQUIRE(recorder.CaptureAppliedInput(tick, session.GetInput().GetLastAppliedEvents()));
			}
		};

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("ReplayRecorder: recording begins only at tick zero of one session")
		{
			RecordingFixture f;
			auto session = f.Create();
			ReplayRecorder recorder;
			REQUIRE(recorder.Begin(f.Header, *session));
			CHECK_FALSE(recorder.Begin(f.Header, *session));
			auto other = f.Create(92);
			CHECK_FALSE(recorder.Finish(*other));
			recorder.Cancel();
			session->Tick();
			CHECK_FALSE(recorder.Begin(f.Header, *session));
		}

		TEST_CASE("ReplayRecorder: every applied input source is captured in order")
		{
			RecordingFixture f;
			auto session = f.Create();
			ReplayRecorder recorder;
			REQUIRE(recorder.Begin(f.Header, *session));
			PlayInputEvent key;
			key.KeyCode = Key::A;
			REQUIRE(session->GetInput().Queue(0, key));
			session->GetInput().QueueDeviceEvent(KeyEvent{ .KeyCode = Key::B });
			key.KeyCode = Key::C;
			REQUIRE(session->GetInput().Queue(0, key));
			f.Step(recorder, *session);
			auto document = recorder.Finish(*session);
			REQUIRE(document);
			REQUIRE(document->Events.size() == 3);
			CHECK(document->Events[0].KeyName == "A");
			CHECK(document->Events[1].KeyName == "B");
			CHECK(document->Events[2].KeyName == "C");
		}

		TEST_CASE("ReplayRecorder: coalesced device input and releaseAll record their applied events")
		{
			RecordingFixture f;
			auto session = f.Create();
			ReplayRecorder recorder;
			REQUIRE(recorder.Begin(f.Header, *session));
			session->GetInput().QueueDeviceEvent(MouseMoveEvent{ .Position = { 1, 2 } });
			session->GetInput().QueueDeviceEvent(MouseMoveEvent{ .Position = { 3, 4 } });
			session->GetInput().QueueDeviceEvent(KeyEvent{ .KeyCode = Key::Space });
			f.Step(recorder, *session);
			session->GetInput().QueueReleaseAll(1);
			f.Step(recorder, *session);
			auto document = recorder.Finish(*session);
			REQUIRE(document);
			REQUIRE(document->Events.size() == 3);
			CHECK(document->Events[0].Position == glm::vec2(3, 4));
			CHECK(document->Events[2].State == "Up");
			CHECK(document->Events[2].Tick == 1);
		}

		TEST_CASE("ReplayRecorder: header retains scene parameters seed and fixed rate")
		{
			RecordingFixture f;
			auto session = f.Create();
			ReplayRecorder recorder;
			REQUIRE(recorder.Begin(f.Header, *session));
			f.Step(recorder, *session);
			auto document = recorder.Finish(*session);
			REQUIRE(document);
			CHECK(document->Header.Scene.Handle == f.Header.Scene.Handle);
			CHECK(document->Header.Parameters == f.Header.Parameters);
			CHECK(document->Header.Seed == 73);
			CHECK(document->Header.FixedHz == 144);
		}

		TEST_CASE("ReplayRecorder: external edits and hot reload preserve the first invalidation reason")
		{
			RecordingFixture f;
			auto session = f.Create();
			ReplayRecorder recorder;
			REQUIRE(recorder.Begin(f.Header, *session));
			recorder.Invalidate("external edit");
			recorder.Invalidate("hot reload");
			auto document = recorder.Finish(*session);
			REQUIRE_FALSE(document);
			CHECK(document.error().GetMessageText().find("external edit") != std::string::npos);
			CHECK(document.error().GetMessageText().find("hot reload") == std::string::npos);
			CHECK(recorder.IsRecording());
			recorder.Cancel();
		}

		TEST_CASE("ReplayRecorder: cancellation discards unwritten data without a later poll")
		{
			RecordingFixture f;
			auto session = f.Create();
			ReplayRecorder recorder;
			REQUIRE(recorder.Begin(f.Header, *session));
			recorder.Cancel();
			recorder.Cancel();
			CHECK_FALSE(recorder.IsRecording());
			CHECK_FALSE(recorder.Finish(*session));
			REQUIRE(recorder.Begin(f.Header, *session));
			auto document = recorder.Finish(*session);
			REQUIRE(document);
			CHECK(document->Events.empty());
		}

		TEST_CASE("ReplayRecorder: gameplay scene loads preserve the recording timeline")
		{
			RecordingFixture f;
			auto document = SceneSerializer::ToJson(f.Scene.GetScene());
			REQUIRE(document);
			auto scene = CreateRef<SceneData>();
			scene->Document = CreateRef<const Json>(*document);
			f.Assets.Publish(UUID(201), scene, "Assets/Scenes/Next.scene");
			auto session = f.Create();
			ReplayRecorder recorder;
			REQUIRE(recorder.Begin(f.Header, *session));
			const auto generation = session->GetSceneGeneration();
			// Drive the public load request; this recorder is independent of eval provenance and observes only the
			// Session timeline. The runner acceptance suite separately proves gameplay vs test-driver invalidation.
			auto loaded = session->GetScripts()->Evaluate("Scene.Load(Assets.Load(\"Assets/Scenes/Next.scene\"), {Level = 3})", {});
			REQUIRE(loaded);
			f.Step(recorder, *session);
			REQUIRE(session->GetSceneGeneration() > generation);
			f.Step(recorder, *session);
			auto finished = recorder.Finish(*session);
			REQUIRE(finished);
			CHECK(finished->FinalTick == 2);
			CHECK(finished->Header.Parameters == f.Header.Parameters);
			CHECK(session->GetSerial() == 91);
		}

		TEST_CASE("ReplayRecorder: final tick and state hash describe the completed boundary")
		{
			RecordingFixture f;
			auto session = f.Create();
			ReplayRecorder recorder;
			REQUIRE(recorder.Begin(f.Header, *session));
			for (uint32_t i = 0; i < 3; ++i)
				f.Step(recorder, *session);
			auto invalid = recorder.Finish(*session, std::array{ ReplayExpectation{ 4, "true" } });
			CHECK_FALSE(invalid);
			CHECK(recorder.IsRecording());
			auto result = recorder.Finish(*session, std::array{ ReplayExpectation{ 3, "true" } });
			REQUIRE(result);
			CHECK(result->FinalTick == 3);
			CHECK(result->FinalStateHash == UUID(session->ComputeStateHash()).ToString());
			CHECK_FALSE(recorder.IsRecording());
		}
	}

}
