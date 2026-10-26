#include "TestsPCH.h"
#include "Engine/Session/ReplayRecorder.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Session/PlaySession.h"
#include "Engine/Session/ReplayFingerprint.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

#include <array>
#include <string>
#include <vector>

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
			CHECK(document.error().GetCode() == ErrorCode::InvalidState);
			CHECK(document.error().GetMessageText().find("external edit") != std::string::npos);
			CHECK(document.error().GetMessageText().find("hot reload") == std::string::npos);
			// An invalidated recording can never finish, so the failed Finish ends it: the owner can begin again.
			CHECK_FALSE(recorder.IsRecording());
			CHECK_FALSE(recorder.Finish(*session));
			auto restarted = f.Create(92);
			REQUIRE(recorder.Begin(f.Header, *restarted));
			f.Step(recorder, *restarted);
			CHECK(recorder.Finish(*restarted));
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

		// Docs/Decisions/0030-m19-contract.md decision 2 (A-FPRINT): Begin takes the session's start fingerprint at tick 0 and
		// Finish writes it, whatever the play changed in the scene since.
		TEST_CASE("ReplayRecorder: a recording keeps the start fingerprint its session held at tick zero")
		{
			RecordingFixture f;
			static_cast<void>(f.Scene.GetScene().CreateEntityWithID(UUID(0x31), "Board"));
			auto session = f.Create();
			const Result<ReplayFingerprint> atStart = ComputeReplayFingerprint(session->GetScene(), session->GetScriptSchemas(), session->GetAssets());
			REQUIRE_MESSAGE(atStart.has_value(), atStart.error().ToString());
			ReplayRecorder recorder;
			REQUIRE(recorder.Begin(f.Header, *session));
			f.Step(recorder, *session);
			REQUIRE(session->CreateEntity("Spawned").has_value());
			f.Step(recorder, *session);
			const auto document = recorder.Finish(*session);
			REQUIRE_MESSAGE(document.has_value(), document.error().ToString());
			REQUIRE(document->Fingerprint.has_value());
			CHECK(*document->Fingerprint == *atStart);
			const Result<ReplayFingerprint> atEnd = ComputeReplayFingerprint(session->GetScene(), session->GetScriptSchemas(), session->GetAssets());
			REQUIRE(atEnd.has_value());
			CHECK(atEnd->Entities.size() == atStart->Entities.size() + 1);
			// The written replay carries it.
			const auto text = ReplayToText(*document);
			REQUIRE(text.has_value());
			ReplayLoadReport report;
			const auto read = ReplayFromText(*text, report, true);
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Fingerprint == document->Fingerprint);
		}

		// Decision 2 (the review of W19.1): a recording that cannot be fingerprinted is refused rather than written without the
		// asset: Begin fails with the error of the referenced asset whose cooked hash cannot be taken (an import that failed),
		// naming it, and records nothing.
		TEST_CASE("ReplayRecorder: a session whose start fingerprint cannot be taken is refused, naming the asset")
		{
			RecordingFixture f;
			const AssetHandle material(0x515);
			f.Assets.Publish(material, AssetRef<Asset>(CreateRef<MaterialData>()), "Assets/Materials/Broken.material");
			Entity box = f.Scene.GetScene().CreateEntityWithID(UUID(0x66), "Box");
			box.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = {}, .Materials = { TypedAssetHandle<AssetType::Material>(material) } });
			f.Assets.FailCookedHash(material, ErrorCode::ImportFailed);
			auto session = f.Create();
			ReplayRecorder recorder;
			const Status refused = recorder.Begin(f.Header, *session);
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::ImportFailed);
			CHECK(refused.error().ToString().find("Assets/Materials/Broken.material") != std::string::npos);
			CHECK_FALSE(recorder.IsRecording());
		}

		// M16 friction S2-01 (FX-4): the session feeds its own recorder the state hash of the boundary every fixed step starts
		// from, and the recording keeps every positive multiple of ReplayCheckpointInterval before its final tick, whose hash
		// stays FinalStateHash.
		TEST_CASE("ReplayRecorder: a recording keeps the state hash of every 60th completed tick before its final tick")
		{
			RecordingFixture f;
			for (const uint64_t finalTick : { uint64_t{ 150 }, 2 * ReplayCheckpointInterval })
			{
				CAPTURE(finalTick);
				auto session = f.Create();
				ReplayRecorder* recorder = session->GetRecorder();
				REQUIRE(recorder != nullptr);
				REQUIRE(recorder->Begin(f.Header, *session));
				std::vector<ReplayCheckpoint> expected;
				for (uint64_t tick = 0; tick < finalTick; ++tick)
				{
					session->Tick();
					if (session->GetTick() % ReplayCheckpointInterval == 0 && session->GetTick() < finalTick)
						expected.push_back({ .Tick = session->GetTick(), .StateHash = UUID(session->ComputeStateHash()).ToString() });
				}
				const auto document = recorder->Finish(*session);
				REQUIRE_MESSAGE(document.has_value(), document.error().ToString());
				CHECK(document->FinalTick == finalTick);
				CHECK(document->FinalStateHash == UUID(session->ComputeStateHash()).ToString());
				REQUIRE(document->Checkpoints.size() == expected.size());
				CHECK(document->Checkpoints.size() == (finalTick - 1) / ReplayCheckpointInterval);
				for (size_t index = 0; index < expected.size(); ++index)
				{
					CHECK(document->Checkpoints[index].Tick == expected[index].Tick);
					CHECK(document->Checkpoints[index].StateHash == expected[index].StateHash);
				}
			}
			// A recording without a 60th tick keeps none.
			auto session = f.Create();
			ReplayRecorder* recorder = session->GetRecorder();
			REQUIRE(recorder != nullptr);
			REQUIRE(recorder->Begin(f.Header, *session));
			for (uint64_t tick = 0; tick < ReplayCheckpointInterval - 1; ++tick)
				session->Tick();
			const auto shortRecording = recorder->Finish(*session);
			REQUIRE(shortRecording.has_value());
			CHECK(shortRecording->Checkpoints.empty());
		}

		// M16 review R22: a recorder that is not recording keeps no checkpoint, even when it is handed checkpoint boundaries
		// (ticks 60 and 120, where the tick-0 exemption hides nothing) of a session whose state differs (another seed): it
		// has nothing to finish, and the recording it makes next holds exactly its own session's checkpoint. An idle recorder
		// is stopped twice over (no recording is active, and its next tick is 0), and Begin starts an empty document, so the
		// check fails only when an idle capture can reach a document.
		TEST_CASE("ReplayRecorder: a recorder that is not recording keeps no checkpoint for a later recording")
		{
			RecordingFixture f;
			ReplayRecorder recorder;
			f.Spec.Seed = f.Header.Seed + 1;
			auto other = f.Create(93);
			f.Spec.Seed = f.Header.Seed;
			std::vector<std::string> otherHashes;
			for (uint64_t tick = 0; tick < 2 * ReplayCheckpointInterval; ++tick)
			{
				other->Tick();
				if (other->GetTick() % ReplayCheckpointInterval == 0)
				{
					otherHashes.push_back(UUID(other->ComputeStateHash()).ToString());
					recorder.CaptureCheckpoint(*other);
				}
			}
			REQUIRE(otherHashes.size() == 2);
			CHECK_FALSE(recorder.IsRecording());
			const auto nothing = recorder.Finish(*other);
			REQUIRE_FALSE(nothing.has_value());
			CHECK(nothing.error().GetCode() == ErrorCode::InvalidState);

			// The recording feeds the standalone recorder as a session feeds its own: the checkpoint before each step, the
			// applied input after it.
			auto session = f.Create();
			REQUIRE(recorder.Begin(f.Header, *session));
			std::string ownHash;
			for (uint64_t tick = 0; tick <= ReplayCheckpointInterval; ++tick)
			{
				if (session->GetTick() == ReplayCheckpointInterval)
					ownHash = UUID(session->ComputeStateHash()).ToString();
				recorder.CaptureCheckpoint(*session);
				f.Step(recorder, *session);
			}
			const auto document = recorder.Finish(*session);
			REQUIRE_MESSAGE(document.has_value(), document.error().ToString());
			REQUIRE(document->Checkpoints.size() == 1);
			CHECK(document->Checkpoints[0].Tick == ReplayCheckpointInterval);
			CHECK(document->Checkpoints[0].StateHash == ownHash);
			CHECK(ownHash != otherHashes[0]);
		}
	}

}
