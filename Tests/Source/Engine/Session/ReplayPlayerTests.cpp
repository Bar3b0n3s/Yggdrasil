#include "TestsPCH.h"
#include "Engine/Session/ReplayPlayer.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/TypedAssetHandle.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptCompiler.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Session/PlaySession.h"
#include "Engine/Session/ReplayFingerprint.h"
#include "Engine/Session/ReplayRecorder.h"
#include "Support/ExpectLog.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

#include <format>

namespace Engine {

	namespace {

		struct PlaybackFixture final : IReplayEvaluator
		{
			Test::SceneTestFixture Scene{};
			Test::InMemoryAssetManager Assets{};
			std::vector<uint64_t> Ticks{}, Generations{};
			ReplayHeader Header{};
			PlaybackFixture()
			{
				Header.Scene = { UUID(301), "Assets/Scenes/Play.scene" };
				Header.Seed = 19;
				Header.EngineVersion = "1";
				Header.Config = "Debug";
			}
			Scope<PlaySession> Create(uint64_t serial = 17)
			{
				PlaySessionSpecification spec;
				spec.Registry = &Scene.GetRegistry();
				spec.Assets = &Assets;
				spec.Serial = serial;
				spec.Seed = Header.Seed;
				auto session = PlaySession::CreateFromScene(spec, Scene.GetScene());
				REQUIRE(session);
				return std::move(*session);
			}
			ReplayBytecodeExpectation Predicate(uint64_t tick, std::string_view source, std::string_view pointer = "/Expect/0/Luau")
			{
				auto path = VfsPath::Create("project", "Assets/Tests/Replay.replay");
				REQUIRE(path);
				auto compiled = ScriptCompiler::Compile({ .Path = *path, .Source = source, .Mode = ScriptCompileMode::ExpressionOrChunk, .JsonPointer = pointer });
				REQUIRE(compiled);
				ReplayBytecodeExpectation e;
				e.Tick = tick;
				e.Script.Bytecode = std::move(compiled->Bytecode);
				e.Script.SourceMap = std::move(compiled->SourceMap);
				return e;
			}
			Ref<ReplayData> Data(uint64_t finalTick = 3)
			{
				auto replay = CreateRef<ReplayData>();
				replay->Header = Header;
				replay->FinalTick = finalTick;
				replay->FinalStateHash = HashAt(finalTick);
				return replay;
			}
			// The state hash of the boundary `tick` of an input-free run.
			std::string HashAt(uint64_t tick)
			{
				auto original = Create();
				for (uint64_t step = 0; step < tick; ++step)
					original->Tick();
				return UUID(original->ComputeStateHash()).ToString();
			}
			Result<Json> Evaluate(PlaySession& session, const ReplayBytecodeExpectation& expectation) override
			{
				Ticks.push_back(session.GetTick());
				Generations.push_back(session.GetSceneGeneration());
				ENGINE_TRY_ASSIGN(auto evaluation, session.GetScripts()->ExecuteBytecode(expectation.Script));
				return evaluation.Value.Get();
			}
			ReplayPlaybackResult Run(AssetRef<ReplayData> replay, ReplayPlaybackOptions options = { true, true })
			{
				auto session = Create();
				ReplayPlayer player;
				REQUIRE(player.Begin(std::move(replay), *session, options));
				bool complete = false;
				for (uint32_t i = 0; i < 1000 && !complete; ++i)
				{
					auto advanced = player.Advance(*session, *this);
					REQUIRE(advanced);
					complete = *advanced;
				}
				REQUIRE(complete);
				auto result = player.GetResult();
				REQUIRE(result);
				return *result;
			}
		};

	}

	TEST_SUITE("Session")
	{
		TEST_CASE("ReplayPlayer: a fresh session replays all input events and expectations")
		{
			PlaybackFixture f;
			auto replay = f.Data();
			replay->Events = { { .Tick = 0, .Type = "Key", .State = "Tap", .KeyName = "Space" }, { .Tick = 1, .Type = "MouseMove", .Position = glm::vec2(7, 8) } };
			replay->Expect = { f.Predicate(1, "Input.IsKeyDown(\"Space\")"), f.Predicate(2, "not Input.IsKeyDown(\"Space\")") };
			auto result = f.Run(replay);
			CHECK(result.Passed);
			CHECK(result.Expect.size() == 2);
			CHECK(result.FinalTick == 3);
		}

		TEST_CASE("ReplayPlayer: tick zero and final tick expectations run at completed boundaries")
		{
			PlaybackFixture f;
			auto replay = f.Data();
			replay->Expect = { f.Predicate(0, "true"), f.Predicate(3, "true") };
			CHECK(f.Run(replay).Passed);
			CHECK(f.Ticks == std::vector<uint64_t>{ 0, 3 });
			f.Ticks.clear();
			auto zero = f.Data(0);
			zero->Expect = { f.Predicate(0, "true") };
			CHECK(f.Run(zero).Passed);
			CHECK(f.Ticks == std::vector<uint64_t>{ 0 });
		}

		TEST_CASE("ReplayPlayer: strict hash detects divergence independently of recorded configuration")
		{
			PlaybackFixture f;
			auto replay = f.Data();
			replay->Header.Config = "Dist";
			CHECK(f.Run(replay).Passed);
			replay->FinalStateHash = "0000000000000000";
			auto strict = f.Run(replay);
			CHECK_FALSE(strict.Passed);
			CHECK(strict.HashChecked);
			CHECK_FALSE(strict.HashMatched);
			auto loose = f.Run(replay, { true, false });
			CHECK(loose.Passed);
			CHECK_FALSE(loose.HashChecked);
			auto session = f.Create();
			ReplayPlayer player;
			CHECK_FALSE(player.Begin(replay, *session, { false, true }));
		}

		TEST_CASE("ReplayPlayer: false and faulting expectations return located outcomes")
		{
			PlaybackFixture f;
			auto replay = f.Data();
			replay->Expect = { f.Predicate(1, "false", "/Expect/0/Luau"), f.Predicate(1, "error(\"predicate boom\")", "/Expect/1/Luau") };
			Test::ExpectLog expected(LogLevel::Error, "predicate boom");
			auto result = f.Run(replay);
			CHECK_FALSE(result.Passed);
			REQUIRE(result.Expect.size() == 2);
			REQUIRE(result.Expect[0].Failure);
			REQUIRE(result.Expect[1].Failure);
			CHECK(result.Expect[0].Failure->GetLocation().JsonPointer == "/Expect/0/Luau");
			CHECK(result.Expect[1].Failure->GetLocation().JsonPointer == "/Expect/1/Luau");
			CHECK(result.Expect[1].File == "Assets/Tests/Replay.replay");
		}

		TEST_CASE("ReplayPlayer: user recording labels retain authored failure locations without project paths")
		{
			PlaybackFixture fixture;
			auto replay = fixture.Data(0);
			for (const auto source : { "false", "local x = 1\nerror('user replay fault')" })
			{
				const auto pointer = replay->Expect.empty() ? "/Expect/0/Luau" : "/Expect/1/Luau";
				auto compiled = ScriptCompiler::Compile({ .Source = source, .Mode = ScriptCompileMode::ExpressionOrChunk, .ChunkName = "=user://Replays/Local.replay", .JsonPointer = pointer });
				REQUIRE(compiled);
				ReplayBytecodeExpectation expectation;
				expectation.Script.Bytecode = std::move(compiled->Bytecode);
				expectation.Script.SourceMap = std::move(compiled->SourceMap);
				CHECK(expectation.Script.SourceMap.Path.empty());
				replay->Expect.push_back(std::move(expectation));
			}
			Test::ExpectLog expected(LogLevel::Error, "user replay fault");
			const auto result = fixture.Run(replay);
			CHECK_FALSE(result.Passed);
			REQUIRE(result.Expect.size() == 2);
			for (const auto& outcome : result.Expect)
			{
				CHECK(outcome.File == "user://Replays/Local.replay");
				REQUIRE(outcome.Failure);
				CHECK(outcome.Failure->GetLocation().File == "user://Replays/Local.replay");
			}
			CHECK(result.Expect[0].Line == 1);
			CHECK(result.Expect[1].Line == 2);
			CHECK(result.Expect[0].Failure->GetLocation().JsonPointer == "/Expect/0/Luau");
			CHECK(result.Expect[1].Failure->GetLocation().JsonPointer == "/Expect/1/Luau");
		}

		TEST_CASE("ReplayPlayer: repeated runs and host budget partitions produce identical final hashes")
		{
			PlaybackFixture f;
			auto replay = f.Data(17);
			const auto expected = f.Run(replay);
			for (uint32_t budget : { 1U, 2U, 7U })
			{
				auto session = f.Create();
				ReplayPlayer player;
				REQUIRE(player.Begin(replay, *session, { true, true }));
				bool complete = false;
				for (uint32_t partition = 0; partition < 20 && !complete; ++partition)
					for (uint32_t step = 0; step < budget && !complete; ++step)
					{
						auto result = player.Advance(*session, f);
						REQUIRE(result);
						complete = *result;
					}
				REQUIRE(complete);
				auto result = player.GetResult();
				REQUIRE(result);
				CHECK(result->Passed);
				CHECK(result->StateHash == expected.StateHash);
			}
		}

		TEST_CASE("ReplayPlayer: scene transitions evaluate bytecode in the replacement VM")
		{
			PlaybackFixture f;
			auto document = SceneSerializer::ToJson(f.Scene.GetScene());
			REQUIRE(document);
			auto scene = CreateRef<SceneData>();
			scene->Document = CreateRef<const Json>(*document);
			f.Assets.Publish(UUID(302), scene, "Assets/Scenes/Next.scene");
			auto replay = f.Data();
			replay->Expect = { f.Predicate(0, "Scene.Load(Assets.Load(\"Assets/Scenes/Next.scene\"), {Level=8}); return true"), f.Predicate(1, "Scene.GetLoadParameters().Level == 8") };
			CHECK(f.Run(replay, { true, false }).Passed);
			REQUIRE(f.Generations.size() == 2);
			CHECK(f.Generations[1] > f.Generations[0]);
		}

		TEST_CASE("ReplayPlayer: a quit before the final tick completes as a failed playback with its reason")
		{
			// The session refuses to advance after the game quits; that is a failed replay with its outcome, never Cancelled.
			PlaybackFixture f;
			auto replay = f.Data(5);
			replay->Expect = { f.Predicate(1, "Application.Quit(3); return true", "/Expect/0/Luau"), f.Predicate(4, "true", "/Expect/1/Luau") };
			auto session = f.Create();
			ReplayPlayer player;
			REQUIRE(player.Begin(replay, *session, { true, true }));
			bool complete = false;
			for (uint32_t i = 0; i < 10 && !complete; ++i)
			{
				const auto advanced = player.Advance(*session, f);
				REQUIRE_MESSAGE(advanced, (advanced ? "" : advanced.error().ToString()));
				complete = *advanced;
			}
			REQUIRE(complete);
			const auto result = player.GetResult();
			REQUIRE(result);
			CHECK_FALSE(result->Passed);
			CHECK(result->FinalTick == 1);
			CHECK(result->StopReason.find("quit with exit code 3 at tick 1") != std::string::npos);
			CHECK(result->HashChecked);
			CHECK_FALSE(result->HashMatched);
			CHECK(result->StateHash == UUID(session->ComputeStateHash()).ToString());
			REQUIRE(result->Expect.size() == 2);
			CHECK(result->Expect[0].Satisfied);
			CHECK_FALSE(result->Expect[1].Evaluated);
			CHECK_FALSE(result->Expect[1].Satisfied);
			CHECK(result->Expect[1].Tick == 4);
			REQUIRE(result->Expect[1].Failure);
			CHECK(result->Expect[1].Failure->GetLocation().JsonPointer == "/Expect/1/Luau");
			CHECK(result->Expect[1].Failure->GetMessageText().find("stopped at tick 1") != std::string::npos);
			CHECK(session->GetTick() == 1);
		}

		TEST_CASE("ReplayPlayer: session replacement cancels without touching the new session")
		{
			PlaybackFixture f;
			auto replay = f.Data();
			auto first = f.Create();
			auto replacement = f.Create(18);
			ReplayPlayer player;
			REQUIRE(player.Begin(replay, *first));
			auto result = player.Advance(*replacement, f);
			REQUIRE_FALSE(result);
			CHECK(result.error().GetCode() == ErrorCode::Cancelled);
			CHECK(replacement->GetTick() == 0);
			CHECK_FALSE(player.GetResult());
		}

		TEST_CASE("ReplayPlayer: cancellation discards future authored input and preserves independent input")
		{
			PlaybackFixture fixture;
			auto replay = fixture.Data(8);
			ReplayEvent appliedTap{};
			appliedTap.Type = "Key";
			appliedTap.KeyName = "Space";
			appliedTap.State = "Tap";
			ReplayEvent futureDown{};
			futureDown.Tick = 4;
			futureDown.Type = "Key";
			futureDown.KeyName = "A";
			futureDown.State = "Down";
			ReplayEvent futureUp = futureDown;
			futureUp.Tick = 5;
			futureUp.State = "Up";
			ReplayEvent futureTap = futureDown;
			futureTap.Tick = 6;
			futureTap.KeyName = "B";
			futureTap.State = "Tap";
			replay->Events = { appliedTap, futureDown, futureUp, futureTap };
			auto session = fixture.Create();
			ReplayPlayer player;
			REQUIRE(player.Begin(replay, *session));
			const auto advanced = player.Advance(*session, fixture);
			REQUIRE(advanced);
			REQUIRE_FALSE(*advanced);
			CHECK(session->GetInput().GetDevices().IsKeyDown(InputPhase::Step, Key::Space));
			player.Cancel();
			player.Cancel();
			// Cancellation retains the Up generated by the Tap already applied, but no future authored replay input.
			CHECK(session->GetInput().GetQueuedEventCount() == 1);
			PlayInputEvent independent{};
			independent.KeyCode = Key::C;
			independent.State = PlayInputEventState::Tap;
			REQUIRE(session->GetInput().Queue(4, independent));
			for (uint64_t tick = 1; tick < 8; ++tick)
			{
				session->Tick();
				const auto& devices = session->GetInput().GetDevices();
				CHECK_FALSE(devices.IsKeyDown(InputPhase::Step, Key::Space));
				CHECK_FALSE(devices.IsKeyDown(InputPhase::Step, Key::A));
				CHECK_FALSE(devices.IsKeyDown(InputPhase::Step, Key::B));
				CHECK(devices.IsKeyDown(InputPhase::Step, Key::C) == (tick == 4));
			}
		}

		TEST_CASE("ReplayPlayer: incremental input preserves Tap release ordering at the next tick")
		{
			PlaybackFixture fixture;
			auto replay = fixture.Data(3);
			ReplayEvent tap{};
			tap.Type = "Key";
			tap.KeyName = "Space";
			tap.State = "Tap";
			ReplayEvent down = tap;
			down.Tick = 1;
			down.State = "Down";
			replay->Events = { tap, down };
			auto session = fixture.Create();
			ReplayPlayer player;
			REQUIRE(player.Begin(replay, *session));
			REQUIRE(player.Advance(*session, fixture));
			REQUIRE(player.Advance(*session, fixture));
			const auto applied = session->GetInput().GetLastAppliedEvents();
			REQUIRE(applied.size() == 2);
			CHECK(applied[0].State == PlayInputEventState::Up);
			CHECK(applied[1].State == PlayInputEventState::Down);
			player.Cancel();
			session->Tick();
			CHECK(session->GetInput().GetDevices().IsKeyDown(InputPhase::Step, Key::Space));
			session->GetInput().QueueReleaseAll(session->GetTick());
			session->Tick();
			CHECK_FALSE(session->GetInput().GetDevices().IsKeyDown(InputPhase::Step, Key::Space));
		}

		TEST_CASE("ReplayPlayer: invalid event streams queue no partial input")
		{
			PlaybackFixture f;
			auto replay = f.Data();
			replay->Events = { { .Tick = 0, .Type = "Key", .State = "Down", .KeyName = "Space" }, { .Tick = 0, .Type = "Action", .Name = "Unknown", .State = "Down" } };
			auto session = f.Create();
			ReplayPlayer player;
			CHECK_FALSE(player.Begin(replay, *session));
			session->Tick();
			CHECK(session->GetInput().GetLastAppliedEvents().empty());
		}

		// M16 friction S2-01 (FX-4): a replay is verified along its trajectory, not only at its end. Under StrictHash every
		// checkpoint is compared when playback reaches its tick, after that boundary's expectations, and the first one that
		// differs ends the playback there, naming the tick and both hashes; without StrictHash none is compared.
		TEST_CASE("ReplayPlayer: a checkpoint whose state hash differs ends the playback at its tick and names both hashes")
		{
			PlaybackFixture f;
			auto replay = f.Data(150);
			const std::string at60 = f.HashAt(60);
			const std::string at120 = f.HashAt(120);
			replay->Checkpoints = { { .Tick = 60, .StateHash = at60 }, { .Tick = 120, .StateHash = at120 } };
			replay->Expect = { f.Predicate(120, "true", "/Expect/0/Luau"), f.Predicate(130, "true", "/Expect/1/Luau") };
			const auto matched = f.Run(replay);
			CHECK(matched.Passed);
			CHECK(matched.CheckpointsChecked == 2);
			CHECK_FALSE(matched.CheckpointFailed);
			CHECK(matched.FinalTick == 150);

			replay->Checkpoints[1].StateHash = at120 == "0000000000000000" ? "0000000000000001" : "0000000000000000";
			const auto diverged = f.Run(replay);
			CHECK_FALSE(diverged.Passed);
			CHECK(diverged.CheckpointFailed);
			CHECK(diverged.CheckpointsChecked == 2);
			CHECK(diverged.FinalTick == 120);
			CHECK(diverged.StateHash == at120);
			CHECK(diverged.ExpectedStateHash == replay->Checkpoints[1].StateHash);
			CHECK(diverged.HashChecked);
			CHECK_FALSE(diverged.HashMatched);
			CHECK(diverged.StopReason.find("checkpoint tick 120") != std::string::npos);
			CHECK(diverged.StopReason.find(at120) != std::string::npos);
			CHECK(diverged.StopReason.find(replay->Checkpoints[1].StateHash) != std::string::npos);
			// M17 friction F03: the reason names what changes the world (the look's components included).
			CHECK(diverged.StopReason.find(GetReplayDivergenceCauses()) != std::string::npos);
			REQUIRE(diverged.Expect.size() == 2);
			CHECK(diverged.Expect[0].Satisfied);
			CHECK_FALSE(diverged.Expect[1].Evaluated);
			REQUIRE(diverged.Expect[1].Failure);
			CHECK(diverged.Expect[1].Failure->GetMessageText().find("stopped at tick 120") != std::string::npos);

			// The first checkpoint that differs ends it, even when a later one would match.
			replay->Checkpoints[0].StateHash = replay->Checkpoints[1].StateHash;
			replay->Checkpoints[1].StateHash = at120;
			const auto early = f.Run(replay);
			CHECK(early.CheckpointFailed);
			CHECK(early.FinalTick == 60);
			CHECK(early.CheckpointsChecked == 1);
			CHECK(early.StateHash == at60);

			const auto loose = f.Run(replay, { .Verify = true, .StrictHash = false });
			CHECK(loose.Passed);
			CHECK(loose.CheckpointsChecked == 0);
			CHECK_FALSE(loose.CheckpointFailed);
			CHECK(loose.FinalTick == 150);
		}

		TEST_CASE("ReplayPlayer: WithoutInput plays the header and ticks with none of the replay's events")
		{
			PlaybackFixture f;
			auto replay = f.Data();
			replay->Events = { { .Tick = 0, .Type = "Key", .State = "Tap", .KeyName = "Space" } };
			replay->Expect = { f.Predicate(1, "Input.IsKeyDown(\"Space\")") };
			CHECK(f.Run(replay).Passed);
			const auto withoutInput = f.Run(replay, { .Verify = true, .StrictHash = true, .WithoutInput = true });
			CHECK_FALSE(withoutInput.Passed);
			CHECK(withoutInput.FinalTick == 3);
			CHECK(withoutInput.HashMatched); // this scene's state does not depend on the key, only the expectation does
			REQUIRE(withoutInput.Expect.size() == 1);
			CHECK(withoutInput.Expect[0].Evaluated);
			CHECK_FALSE(withoutInput.Expect[0].Satisfied);
			// The event stream is still checked against the action map before anything plays.
			replay->Events.push_back({ .Tick = 1, .Type = "Action", .Name = "Unknown", .State = "Down" });
			auto session = f.Create();
			ReplayPlayer player;
			CHECK_FALSE(player.Begin(replay, *session, { .Verify = true, .StrictHash = true, .WithoutInput = true }));
		}

		// A playback WithoutInput is the input check, whose failure a replay that depends on its input is expected to give, so
		// a checkpoint that differs there names no divergence causes (M17 friction F03's, which ask for a re-recording): its
		// stop reason says instead that the replay ran without its input events.
		TEST_CASE("ReplayPlayer: a checkpoint that differs WithoutInput names no divergence causes")
		{
			PlaybackFixture f;
			auto replay = f.Data(70);
			const std::string at60 = f.HashAt(60);
			replay->Checkpoints = { { .Tick = 60, .StateHash = at60 == "0000000000000000" ? "0000000000000001" : "0000000000000000" } };
			replay->Expect = { f.Predicate(65, "true", "/Expect/0/Luau") };
			const auto withInput = f.Run(replay);
			REQUIRE(withInput.CheckpointFailed);
			CHECK(withInput.StopReason.find(GetReplayDivergenceCauses()) != std::string::npos);

			const auto withoutInput = f.Run(replay, { .Verify = true, .StrictHash = true, .WithoutInput = true });
			CHECK_FALSE(withoutInput.Passed);
			REQUIRE(withoutInput.CheckpointFailed);
			CHECK(withoutInput.FinalTick == 60);
			CHECK(withoutInput.StopReason.find("checkpoint tick 60") != std::string::npos);
			CHECK(withoutInput.StopReason.find(at60) != std::string::npos);
			CHECK(withoutInput.StopReason.find(replay->Checkpoints[0].StateHash) != std::string::npos);
			CHECK(withoutInput.StopReason.find("the replay ran without its input events") != std::string::npos);
			CHECK(withoutInput.StopReason.find(GetReplayDivergenceCauses()) == std::string::npos);
			// The expectation the stop left unevaluated repeats the reason, so it names no causes either.
			REQUIRE(withoutInput.Expect.size() == 1);
			REQUIRE(withoutInput.Expect[0].Failure);
			CHECK(withoutInput.Expect[0].Failure->GetMessageText().find(GetReplayDivergenceCauses()) == std::string::npos);
		}

		// M17 review C-01: input.record stop's verification plays a candidate recorded moments before, so a checkpoint that
		// differs there must not blame changes since the recording. NameDivergenceCauses false keeps the plain stop reason,
		// which the expectations the stop left unevaluated repeat.
		TEST_CASE("ReplayPlayer: a checkpoint that differs names no divergence causes when NameDivergenceCauses is false")
		{
			PlaybackFixture f;
			auto replay = f.Data(70);
			const std::string at60 = f.HashAt(60);
			const std::string recorded = at60 == "0000000000000000" ? "0000000000000001" : "0000000000000000";
			replay->Checkpoints = { { .Tick = 60, .StateHash = recorded } };
			replay->Expect = { f.Predicate(65, "true", "/Expect/0/Luau") };

			const auto plain = f.Run(replay, { .Verify = true, .StrictHash = true, .WithoutInput = false, .NameDivergenceCauses = false });
			CHECK_FALSE(plain.Passed);
			REQUIRE(plain.CheckpointFailed);
			CHECK(plain.FinalTick == 60);
			CHECK(plain.StopReason == std::format("the state hash at checkpoint tick 60 is {}, but the recording's is {}", at60, recorded));
			REQUIRE(plain.Expect.size() == 1);
			CHECK_FALSE(plain.Expect[0].Evaluated);
			REQUIRE(plain.Expect[0].Failure);
			CHECK(plain.Expect[0].Failure->GetMessageText() == "the replay stopped at tick 60 before this expectation's tick 65: " + plain.StopReason);
			// The default names them, and WithoutInput keeps its own note whatever NameDivergenceCauses says.
			CHECK(f.Run(replay).StopReason.find(GetReplayDivergenceCauses()) != std::string::npos);
			const auto withoutInput = f.Run(replay, { .Verify = true, .StrictHash = true, .WithoutInput = true, .NameDivergenceCauses = false });
			CHECK(withoutInput.StopReason.find("the replay ran without its input events") != std::string::npos);
		}

		TEST_CASE("ReplayPlayer: cooked playback does not require automation or a compiler")
		{
			PlaybackFixture f;
			auto replay = f.Data();
			replay->Expect = { f.Predicate(3, "true") };
			ReplayDocument source;
			source.Header = replay->Header;
			source.FinalTick = replay->FinalTick;
			source.FinalStateHash = replay->FinalStateHash;
			source.Expect = { { 3, "true" } };
			auto bytes = CookReplay(source, replay->Expect, 1);
			REQUIRE(bytes);
			auto loaded = LoadCookedReplay(*bytes);
			REQUIRE(loaded);
			replay.reset();
			CHECK(f.Run(*loaded).Passed);
		}

		// The player of exported games (the Dist Runtime's --replay --verify and --feature-test) reads the cooked replay,
		// which carries the checkpoints (ReplayData FormatVersion 2), and compares them as the editor does.
		TEST_CASE("ReplayPlayer: a cooked replay's checkpoints are compared like the source's")
		{
			PlaybackFixture f;
			ReplayDocument source;
			source.Header = f.Header;
			source.FinalTick = 130;
			source.FinalStateHash = f.HashAt(130);
			source.Checkpoints = { { .Tick = 60, .StateHash = f.HashAt(60) }, { .Tick = 120, .StateHash = f.HashAt(120) } };
			const auto cook = [](const ReplayDocument& document)
			{
				const auto bytes = CookReplay(document, {}, 1);
				REQUIRE_MESSAGE(bytes.has_value(), bytes.error().ToString());
				auto loaded = LoadCookedReplay(*bytes);
				REQUIRE_MESSAGE(loaded.has_value(), loaded.error().ToString());
				return std::move(*loaded);
			};
			const auto passed = f.Run(cook(source));
			CHECK(passed.Passed);
			CHECK(passed.CheckpointsChecked == 2);
			source.Checkpoints[0].StateHash = source.Checkpoints[1].StateHash;
			const auto failed = f.Run(cook(source));
			CHECK_FALSE(failed.Passed);
			CHECK(failed.CheckpointFailed);
			CHECK(failed.FinalTick == 60);
			CHECK(failed.ExpectedStateHash == source.Checkpoints[0].StateHash);
		}

		// Docs/Decisions/0030-m19-contract.md decision 3 (A-F29): with ContinuePastCheckpoint a checkpoint whose hash differs
		// no longer ends the playback. It plays on to FinalTick, counts every failed checkpoint, keeps the first one, and
		// compares the final hash as usual; it has failed whenever a checkpoint differed, with no stop reason. It needs
		// StrictHash.
		TEST_CASE("ReplayPlayer: ContinuePastCheckpoint plays on to the final tick and counts every failed checkpoint")
		{
			PlaybackFixture f;
			auto replay = f.Data(130);
			const std::string at60 = f.HashAt(60);
			const std::string at120 = f.HashAt(120);
			const auto other = [](const std::string& hash)
			{
				return std::string(hash == "0000000000000000" ? "0000000000000001" : "0000000000000000");
			};
			replay->Checkpoints = { { .Tick = 60, .StateHash = other(at60) }, { .Tick = 120, .StateHash = other(at120) } };
			replay->Expect = { f.Predicate(130, "true") };
			const ReplayPlaybackOptions continuing{ .Verify = true, .StrictHash = true, .ContinuePastCheckpoint = true };
			const auto result = f.Run(replay, continuing);
			CHECK_FALSE(result.Passed);
			CHECK(result.CheckpointFailed);
			CHECK(result.CheckpointsChecked == 2);
			CHECK(result.CheckpointsFailed == 2);
			REQUIRE(result.FirstCheckpointFailure.has_value());
			CHECK(result.FirstCheckpointFailure->Tick == 60);
			CHECK(result.FirstCheckpointFailure->StateHash == at60);
			CHECK(result.FirstCheckpointFailure->ExpectedStateHash == other(at60));
			CHECK(result.FinalTick == 130);
			CHECK(result.HashMatched);
			CHECK(result.StateHash == replay->FinalStateHash);
			CHECK(result.StopReason.empty());
			REQUIRE(result.Expect.size() == 1);
			CHECK(result.Expect[0].Satisfied);

			// Only the second differs: it is the first failure, and the final hash still decides HashMatched.
			replay->Checkpoints[0].StateHash = at60;
			replay->FinalStateHash = other(replay->FinalStateHash);
			const auto late = f.Run(replay, continuing);
			CHECK_FALSE(late.Passed);
			CHECK(late.CheckpointsFailed == 1);
			REQUIRE(late.FirstCheckpointFailure.has_value());
			CHECK(late.FirstCheckpointFailure->Tick == 120);
			CHECK_FALSE(late.HashMatched);
			CHECK(late.FinalTick == 130);

			// Every checkpoint and the final hash reproduced: it passes, as without the option.
			replay->Checkpoints[1].StateHash = at120;
			replay->FinalStateHash = f.HashAt(130);
			const auto matched = f.Run(replay, continuing);
			CHECK(matched.Passed);
			CHECK_FALSE(matched.CheckpointFailed);
			CHECK(matched.CheckpointsFailed == 0);
			CHECK_FALSE(matched.FirstCheckpointFailure.has_value());

			auto session = f.Create();
			ReplayPlayer player;
			const Status refused = player.Begin(replay, *session, { .Verify = true, .StrictHash = false, .ContinuePastCheckpoint = true });
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
		}

		// Decision 3: the warning of an input check whose run reproduced the final hash after differing at a checkpoint names
		// the replay, that checkpoint's tick and the final hash, in the words the suites' Python spells too
		// (Scripts/Lib/games.py FINAL_HASH_INDEPENDENT_WARNING): a frozen text.
		TEST_CASE("ReplayPlayer: the final-hash warning names the replay, the first failed checkpoint and the final hash")
		{
			CHECK(MakeFinalHashIndependentWarning("Assets/Tests/Reset.replay", 60, "3e0c91a2b4d5f607")
				== "'Assets/Tests/Reset.replay': its final state hash does not depend on its input: without its input events the run first differed "
				   "from the recording at checkpoint tick 60, then reached the recorded final state hash 3e0c91a2b4d5f607, so only its checkpoints prove "
				   "the play; end the recording while the played state is still in the scene");
		}

		// Decision 2 (A-FPRINT): a recording keeps its session's start fingerprint, so a playback whose hash differs names
		// what changed since the recording: at a failed checkpoint (whose stop reason ends with the first change) and at the
		// final hash. A playback WithoutInput, an equal playback and a replay without a fingerprint name nothing.
		TEST_CASE("ReplayPlayer: a playback whose state hash differs names what changed since the recording")
		{
			PlaybackFixture f;
			auto recordedSession = f.Create();
			ReplayRecorder recorder;
			REQUIRE(recorder.Begin(f.Header, *recordedSession));
			for (uint64_t tick = 0; tick < 70; ++tick)
			{
				REQUIRE(recordedSession->GetTick() == tick);
				recorder.CaptureCheckpoint(*recordedSession);
				recordedSession->Tick();
				REQUIRE(recorder.CaptureAppliedInput(tick, recordedSession->GetInput().GetLastAppliedEvents()));
			}
			const Result<ReplayDocument> recorded = recorder.Finish(*recordedSession);
			REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
			REQUIRE(recorded->Fingerprint.has_value());
			const Result<ReplayFingerprint> expected = ComputeReplayFingerprint(f.Scene.GetScene(), nullptr, nullptr);
			REQUIRE(expected.has_value());
			CHECK(*recorded->Fingerprint == *expected);
			REQUIRE(recorded->Checkpoints.size() == 1);
			auto replay = f.Data(70);
			replay->FinalStateHash = recorded->FinalStateHash;
			replay->Checkpoints = recorded->Checkpoints;
			replay->Fingerprint = recorded->Fingerprint;

			const auto equal = f.Run(replay);
			CHECK(equal.Passed);
			CHECK(equal.Changes.empty());

			// The scene gains an entity after the recording: the checkpoint at 60 differs and names it.
			static_cast<void>(f.Scene.GetScene().CreateEntityWithID(UUID(0x77), "Added"));
			const std::string added = "entity /Added (0000000000000077) was added";
			const auto changed = f.Run(replay);
			CHECK_FALSE(changed.Passed);
			CHECK(changed.CheckpointFailed);
			CHECK(changed.Changes == std::vector<std::string>{ added });
			CHECK(changed.StopReason.ends_with("; changed since the recording: " + added));
			// Playing on past the checkpoint, the final hash differs too; the changes are the same.
			const auto continued = f.Run(replay, { .Verify = true, .StrictHash = true, .ContinuePastCheckpoint = true });
			CHECK_FALSE(continued.HashMatched);
			CHECK(continued.Changes == std::vector<std::string>{ added });
			CHECK(continued.StopReason.empty());
			// Without checkpoints, the final hash names it.
			replay->Checkpoints.clear();
			CHECK(f.Run(replay).Changes == std::vector<std::string>{ added });
			// The input check's failure is its expected outcome: nothing is named.
			CHECK(f.Run(replay, { .Verify = true, .StrictHash = true, .WithoutInput = true }).Changes.empty());
			// A replay migrated from version 1 or 2 has no fingerprint, so it names nothing.
			replay->Fingerprint.reset();
			const auto unprinted = f.Run(replay);
			CHECK_FALSE(unprinted.Passed);
			CHECK(unprinted.Changes.empty());
		}

		// Decision 2 (the review of W19.1): the playback's own fingerprint only explains a hash that differs, so one that
		// cannot be taken (a referenced asset whose import fails, which the simulation never loads) no longer refuses the
		// playback: it plays to its verdict, and a hash that differs names that error as its one change.
		TEST_CASE("ReplayPlayer: a fingerprinted replay whose referenced asset cannot be hashed still plays to its verdict")
		{
			PlaybackFixture f;
			const AssetHandle material(0x515);
			f.Assets.Publish(material, AssetRef<Asset>(CreateRef<MaterialData>()), "Assets/Materials/Broken.material");
			Entity box = f.Scene.GetScene().CreateEntityWithID(UUID(0x66), "Box");
			box.AddComponent<MeshRendererComponent>(MeshRendererComponent{ .Mesh = {}, .Materials = { TypedAssetHandle<AssetType::Material>(material) } });
			const Result<ReplayFingerprint> recorded = ComputeReplayFingerprint(f.Scene.GetScene(), nullptr, &f.Assets);
			REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
			auto replay = f.Data(70);
			replay->Fingerprint = *recorded;
			const std::string at60 = f.HashAt(60);
			replay->Checkpoints = { { .Tick = 60, .StateHash = at60 } };
			f.Assets.FailCookedHash(material, ErrorCode::ImportFailed);
			REQUIRE_FALSE(ComputeReplayFingerprint(f.Scene.GetScene(), nullptr, &f.Assets).has_value());

			const auto played = f.Run(replay);
			CHECK(played.Passed);
			CHECK(played.HashMatched);
			CHECK(played.Changes.empty());

			replay->Checkpoints[0].StateHash = at60 == "0000000000000000" ? "0000000000000001" : "0000000000000000";
			const auto failed = f.Run(replay);
			CHECK_FALSE(failed.Passed);
			CHECK(failed.CheckpointFailed);
			REQUIRE(failed.Changes.size() == 1);
			CHECK(failed.Changes[0].starts_with("not known, because the playback's start fingerprint could not be taken: "));
			CHECK(failed.Changes[0].find("Assets/Materials/Broken.material") != std::string::npos);
			CHECK(failed.StopReason.ends_with("; changed since the recording: " + failed.Changes[0]));
		}

		// Decision 3 (A-F29): IsFinalHashIndependent holds only for an input check that continued past a checkpoint that
		// differed (FirstCheckpointFailure) to the final tick (no StopReason), satisfied every expectation and reproduced the
		// recorded final state hash; each clause alone makes it false (the suites' Python, games.input_check_warning, asks
		// the same).
		TEST_CASE("ReplayPlayer: an input check is final-hash independent only when every clause of the warning holds")
		{
			ReplayPlaybackResult independent;
			independent.FinalTick = 130;
			independent.CheckpointsChecked = 2;
			independent.CheckpointFailed = true;
			independent.CheckpointsFailed = 1;
			independent.FirstCheckpointFailure = ReplayCheckpointFailure{ .Tick = 60, .StateHash = "0000000000000001", .ExpectedStateHash = "0000000000000002" };
			independent.HashChecked = true;
			independent.HashMatched = true;
			independent.Expect = { ReplayExpectationOutcome{ .Tick = 130, .Evaluated = true, .Satisfied = true } };
			CHECK(IsFinalHashIndependent(independent));

			ReplayPlaybackResult noCheckpointFailed = independent;
			noCheckpointFailed.FirstCheckpointFailure.reset();
			CHECK_FALSE(IsFinalHashIndependent(noCheckpointFailed));
			ReplayPlaybackResult finalHashDiffered = independent;
			finalHashDiffered.HashMatched = false;
			CHECK_FALSE(IsFinalHashIndependent(finalHashDiffered));
			ReplayPlaybackResult stopped = independent;
			stopped.StopReason = "the game quit with exit code 0 at tick 100, before the replay's final tick 130";
			CHECK_FALSE(IsFinalHashIndependent(stopped));
			ReplayPlaybackResult unsatisfied = independent;
			unsatisfied.Expect.push_back(ReplayExpectationOutcome{ .Tick = 130, .Evaluated = true, .Satisfied = false });
			CHECK_FALSE(IsFinalHashIndependent(unsatisfied));
		}
	}

}
