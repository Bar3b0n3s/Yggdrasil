#include "TestsPCH.h"
#include "Engine/Session/ReplayPlayer.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptCompiler.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Session/PlaySession.h"
#include "Support/ExpectLog.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <nlohmann/json.hpp>

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
	}

}
