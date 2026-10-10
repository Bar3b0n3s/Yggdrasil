#include "TestsPCH.h"
#include "Engine/Testing/FeatureTestRunner.h"

#include "Engine/Asset/AudioClipData.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/AudioSourceComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptCompiler.h"
#include "Engine/Session/PlayInputEventCodec.h"
#include "Support/AudioTestData.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <map>

namespace Engine {

	namespace {

		class RunnerHost final : public IFeatureTestHost, public IPlaySessionObserver
		{
		public:
			Test::ScriptTestFixture Compiler{};
			ProjectSettings Settings{};
			VirtualFileSystem Vfs{};
			Scope<AudioEngine> Audio{};
			std::map<AssetHandle, AssetRef<ScriptData>> Scripts{};
			std::map<std::string, AssetRef<ReplayData>> ReplayAssets{};
			std::vector<std::string> ReplayGlobs{};
			std::vector<PlaySessionSpecification> Created{};
			std::vector<uint64_t> Frames{};
			std::vector<uint64_t> Generations{};
			std::vector<std::string> PlayedReplays{};
			std::vector<StampedPlayInputEvent> VerifiedInputs{};
			PlaySession* Active = nullptr;
			uint64_t Serial = 1;
			uint32_t CoverageReads = 0;
			uint32_t ReplaySessions = 0;
			bool DivergentReplay = false;
			ReplayDocument Candidate{};
			Json SceneDocument{};
			Ref<const ScriptFieldSchemaSource> Schemas{};

			RunnerHost()
			{
				Settings.Scripting.PauseOnError = false;
				Settings.Input.Actions["Jump"] = {};
				Settings.Input.Actions["Move"].Type = InputActionSettings::ActionType::Axis;
				auto document = SceneSerializer::ToJson(Compiler.GetScene());
				REQUIRE(document);
				SceneDocument = *document;
				PublishScene("Assets/Scenes/Start.scene", UUID(401), SceneDocument);
				PublishScene("Assets/Scenes/Next.scene", UUID(402), SceneDocument);
			}

			void PublishScene(std::string path, AssetHandle handle, const Json& document)
			{
				auto scene = CreateRef<SceneData>();
				scene->Document = CreateRef<const Json>(document);
				Compiler.GetAssetManager().Publish(handle, scene, std::move(path));
			}

			void RefreshScene()
			{
				auto document = SceneSerializer::ToJson(Compiler.GetScene());
				REQUIRE(document);
				SceneDocument = *document;
				PublishScene("Assets/Scenes/Start.scene", UUID(401), SceneDocument);
			}

			AssetHandle Script(std::string path, std::string source)
			{
				const AssetHandle handle(500 + Scripts.size());
				auto script = Compiler.AddScript(handle, std::move(path), std::move(source));
				REQUIRE(script);
				Scripts.emplace(handle, *script);
				auto schemas = ScriptFieldSchemaSource::Create(Scripts);
				REQUIRE(schemas);
				Schemas = *schemas;
				return handle;
			}

			void Behaviour(std::string source)
			{
				const auto handle = Script("Assets/Scripts/Driver.luau", std::move(source));
				ScriptComponent component;
				component.Script.SetHandle(handle);
				Compiler.GetScene().CreateEntity("Driver").AddComponent<ScriptComponent>(component);
				RefreshScene();
			}

			TestSuiteSettings& Suite(std::string source)
			{
				const auto path = std::format("Assets/Tests/Suite{}.luau", Settings.Testing.Suites.size());
				static_cast<void>(Script(path, std::move(source)));
				TestSuiteSettings suite;
				suite.Script = path;
				suite.Scene = "Assets/Scenes/Start.scene";
				Settings.Testing.Suites.push_back(std::move(suite));
				return Settings.Testing.Suites.back();
			}

			void EnableAudio(bool stream = false)
			{
				auto created = AudioEngine::Create({ .Device = AudioDeviceKind::None, .Decoding = AudioDecoding::Deterministic }, Vfs);
				REQUIRE(created);
				Audio = std::move(*created);
				auto clip = CreateRef<AudioClipData>();
				Test::TestToneSpecification tone;
				tone.FrameCount = 48000;
				tone.Frequency = 110;
				tone.Amplitude = 0.5;
				clip->Encoding = stream ? AudioClipEncoding::Wav : AudioClipEncoding::Pcm16;
				clip->Stream = stream;
				clip->FrameCount = tone.FrameCount;
				clip->Bytes = stream ? Test::MakeToneWav(tone) : Test::MakeTonePcm16Bytes(tone);
				Compiler.GetAssetManager().Publish(UUID(450), clip, "Assets/Audio/Tone.wav");
				AudioSourceComponent source;
				source.Clip.SetHandle(UUID(450));
				source.Spatial = false;
				source.PlayOnStart = true;
				source.Loop = true;
				Compiler.GetScene().CreateEntity("Tone").AddComponent<AudioSourceComponent>(source);
				RefreshScene();
			}

			const ProjectSettings& GetProjectSettings() const override { return Settings; }
			Result<AssetHandle> ResolveTestScript(std::string_view path) override
			{
				const auto handle = Compiler.GetAssetManager().Resolve(path);
				if (!handle)
					return MakeError(ErrorCode::NotFound, "missing test script");
				return *handle;
			}
			Result<std::vector<std::string>> ExpandReplayPaths(std::span<const std::string>) override { return ReplayGlobs; }
			Result<AssetRef<ReplayData>> LoadReplay(std::string_view path) override
			{
				PlayedReplays.emplace_back(path);
				const auto found = ReplayAssets.find(std::string(path));
				if (found == ReplayAssets.end())
					return MakeError(ErrorCode::NotFound, "missing replay");
				return found->second;
			}
			Result<ReplayHeader> DescribeReplayHeader(const PlaySession& session) const override
			{
				ReplayHeader header;
				header.Scene = { UUID(401), "Assets/Scenes/Start.scene" };
				header.Seed = session.GetSeed();
				header.FixedHz = session.GetProjectSettings().Simulation.FixedHz;
				header.Parameters.Set(session.GetLoadParameters());
				header.EngineVersion = "1";
				header.Config = "Debug";
				return header;
			}
			Result<Scope<PlaySession>> CreateSuiteSession(const TestSuiteSettings& suite, TestSuiteSettings::Mode mode, IPlaySessionTestHook& hook, IScriptTestHost& host) override
			{
				PlaySessionSpecification spec;
				spec.Registry = &Compiler.GetTypes();
				spec.Assets = &Compiler.GetAssetManager();
				spec.ScriptSchemas = Schemas;
				spec.Project = Settings;
				spec.Serial = Serial++;
				spec.Seed = Settings.Simulation.Seed;
				spec.Parameters = suite.Parameters;
				spec.TestMode = true;
				spec.TestHook = &hook;
				spec.TestHost = &host;
				spec.Audio = Audio.get();
				spec.OwnsAudioTime = true;
				spec.Observer = this;
				spec.Environment.IsEditor = mode == TestSuiteSettings::Mode::Editor;
				spec.Environment.IsHeadless = false;
				spec.ScriptRunMode = mode == TestSuiteSettings::Mode::Editor ? RunModes::Editor : mode == TestSuiteSettings::Mode::Release ? RunModes::Release
																																		   : RunModes::Dist;
				if (suite.Overrides.CallbackBudgetMs)
					spec.Project.Scripting.CallbackBudgetMs = suite.Overrides.CallbackBudgetMs;
				if (suite.Overrides.MemoryLimitMB)
					spec.Project.Scripting.MemoryLimitMB = suite.Overrides.MemoryLimitMB;
				if (suite.Overrides.PauseOnError != TestSuiteOverrides::PauseOnErrorOverride::Inherit)
					spec.Project.Scripting.PauseOnError = suite.Overrides.PauseOnError == TestSuiteOverrides::PauseOnErrorOverride::Pause;
				Created.push_back(spec);
				return PlaySession::Create(spec, SceneDocument);
			}
			Result<Scope<PlaySession>> CreateReplaySession(const ReplayHeader& header, TestSuiteSettings::Mode mode) override
			{
				++ReplaySessions;
				PlaySessionSpecification spec;
				spec.Registry = &Compiler.GetTypes();
				spec.Assets = &Compiler.GetAssetManager();
				spec.ScriptSchemas = Schemas;
				spec.Project = Settings;
				spec.Serial = Serial++;
				spec.Seed = header.Seed;
				spec.Project.Simulation.FixedHz = header.FixedHz;
				spec.Parameters = header.Parameters;
				spec.Audio = Audio.get();
				spec.OwnsAudioTime = true;
				spec.ScriptRunMode = mode == TestSuiteSettings::Mode::Editor ? RunModes::Editor : mode == TestSuiteSettings::Mode::Release ? RunModes::Release
																																		   : RunModes::Dist;
				spec.Observer = this;
				Created.push_back(spec);
				Json document = SceneDocument;
				if (DivergentReplay)
					document["Name"] = "Divergent";
				ENGINE_TRY_ASSIGN(auto session, PlaySession::Create(spec, document));
				session->SetLockstep(true);
				return session;
			}
			void SetActiveTestSession(PlaySession* session) override { Active = session; }
			AudioEngine* GetAudioEngine() const override { return Audio.get(); }
			Status CaptureScreenshot(PlaySession&, std::string_view) override { return {}; }
			Status ReloadScript(PlaySession& session, AssetHandle asset) override
			{
				auto result = session.GetScripts()->Reload(asset, true);
				if (!result)
					return std::unexpected(result.error());
				return {};
			}
			Result<TestCoverageReport> GetCoverage() const override
			{
				TestCoverageReport report;
				report.RegistryFingerprint = "fixture";
				return report;
			}
			Result<Json> Evaluate(PlaySession& session, const ReplayBytecodeExpectation& expectation) override
			{
				ENGINE_TRY_ASSIGN(auto evaluation, session.GetScripts()->ExecuteBytecode(expectation.Script));
				return evaluation.Value.Get();
			}
			void OnPhase(PlaySession& session, PlaySessionPhase phase, uint64_t tick) override
			{
				if (phase == PlaySessionPhase::RenderExtraction)
				{
					if (session.GetScripts() && !session.GetScripts()->IsTestMode())
						for (const auto& event : session.GetInput().GetLastAppliedEvents())
							VerifiedInputs.push_back({ session.GetTick() - 1, event });
					Frames.push_back(tick);
					Generations.push_back(session.GetSceneGeneration());
				}
			}

			TestRunResult Run(FeatureTestRunOptions options = {})
			{
				FeatureTestRunner runner;
				REQUIRE(runner.Begin(*this, options));
				Finish(runner);
				auto result = runner.GetResult();
				REQUIRE(result);
				if (options.Record && result->Passed)
				{
					auto recording = runner.TakeRecording();
					REQUIRE(recording);
					Candidate = std::move(*recording);
				}
				return *result;
			}
			void Finish(FeatureTestRunner& runner)
			{
				for (uint32_t i = 0; i < 1000 && runner.IsRunning(); ++i)
				{
					auto advanced = runner.Advance();
					if (!advanced)
						break;
				}
				CHECK_FALSE(runner.IsRunning());
				CHECK(Active == nullptr);
			}
		};

		const std::string InputSuite = R"(return Test.Suite("Pilot", function()
			Test.Case("drive", function()
				Test.InjectKey("Space", "Tap")
				Test.InjectAction("Move", nil, 0.25)
				Test.WaitTicks(3)
				Test.Expect(not Input.IsKeyDown("Space"))
			end)
		end))";

	}

	TEST_SUITE("Testing")
	{
		TEST_CASE("TestRunner: a case body fault produces one failure and cannot be claimed")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Body",function()
				Test.Case("fault",function() error("body fault") end)
				Test.Case("later",function() Test.ExpectScriptError("body fault",1) end)
			end))");
			Test::ExpectLog expected(LogLevel::Error, "body fault");
			auto result = host.Run();
			REQUIRE(result.Cases.size() == 2);
			CHECK(result.Cases[0].Status == "error");
			CHECK(result.Cases[0].Failures.size() == 1);
			CHECK(result.Cases[1].Status == "failed");
			CHECK(result.Suites[0].ScriptErrors == 1);
		}

		TEST_CASE("TestRunner: a claim consumes one occurrence and cannot cross a case boundary")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Claims",function()
				local function fault() error("repeated fault") end
				Test.Case("one claim",function()
					Task.Spawn(fault); Task.Spawn(fault)
					Test.ExpectScriptError("repeated fault",4)
				end)
				Test.Case("later",function() Test.ExpectScriptError("repeated fault",1) end)
			end))");
			Test::ExpectLog expected(LogLevel::Error, "repeated fault");
			auto result = host.Run();
			REQUIRE(result.Cases.size() == 2);
			CHECK(result.Cases[0].Status == "error");
			CHECK(result.Cases[0].Failures.size() == 1);
			CHECK(result.Cases[1].Status == "failed");
			CHECK(result.Suites[0].ScriptErrors == 2);
		}

		TEST_CASE("TestRunner: a scripted clock applies the session time scale to steps and frame callbacks")
		{
			RunnerHost host;
			host.Behaviour(R"(local Driver={}
				function Driver.OnCreate(self) Time.SetTimeScale(0.5) end
				function Driver.OnUpdate(self,dt)
					if math.abs(dt-1/120)>0.000001 then error("unscaled frame") end
				end
				return Script.Define("Driver",Driver))");
			host.Suite(R"(return Test.Suite("Scaled",function()
				Test.Case("wait",function() Test.WaitTicks(1) end)
			end))")
				.Clock = { 1.0f / 60.0f };
			auto result = host.Run();
			CHECK(result.Passed);
			CHECK(host.Frames == std::vector<uint64_t>{ 0, 1, 1, 2 });
			CHECK(result.Suites[0].Ticks == 2);
		}

		TEST_CASE("TestRunner: normal replay reports persistent script faults including teardown")
		{
			RunnerHost host;
			host.Behaviour(R"(local Driver={}; function Driver.OnDestroy(self) error("replay teardown") end; return Script.Define("Driver",Driver))");
			ReplayHeader header;
			header.Scene = { UUID(401), "Assets/Scenes/Start.scene" };
			header.Seed = host.Settings.Simulation.Seed;
			header.EngineVersion = "1";
			header.Config = "Debug";
			auto session = host.CreateReplaySession(header, TestSuiteSettings::Mode::Editor);
			REQUIRE(session);
			auto data = CreateRef<ReplayData>();
			data->Header = header;
			data->FinalStateHash = UUID((*session)->ComputeStateHash()).ToString();
			Test::ExpectLog expected(LogLevel::Error, "replay teardown");
			session->reset();
			host.ReplayAssets["Assets/Fault.replay"] = data;
			host.ReplayGlobs = { "Assets/Fault.replay" };
			auto result = host.Run();
			REQUIRE(result.Suites.size() == 1);
			CHECK_FALSE(result.Passed);
			CHECK_FALSE(result.Suites[0].Passed);
			CHECK(result.Suites[0].ScriptErrors == 1);
			REQUIRE_FALSE(result.Cases.empty());
			CHECK(result.Cases[0].Status == "error");
			CHECK(result.Cases[0].Message.find("replay teardown") != std::string::npos);
		}

		TEST_CASE("TestRunner: an expected script error stays observable without failing its case")
		{
			RunnerHost host;
			host.Behaviour(R"(local Driver = {}; function Driver.OnFixedUpdate(self, dt) error("expected boom") end; return Script.Define("Driver", Driver))");
			host.Suite(R"(return Test.Suite("Claims", function()
				Test.Case("claims", function() Test.ExpectScriptError("expected b.om", 4); local errors=Test.GetScriptErrors(); Test.ExpectEqual(#errors,1); Test.Expect(errors[1].count>=1) end)
			end))");
			Test::ExpectLog expected(LogLevel::Error, "expected boom");
			auto result = host.Run();
			REQUIRE(result.Cases.size() == 1);
			CHECK(result.Passed);
			CHECK(result.Suites[0].ScriptErrors == 1);
			CHECK(result.Cases[0].Failures.empty());
		}

		TEST_CASE("TestRunner: cases run sequentially as threads with per-case timeouts")
		{
			RunnerHost host;
			host.Settings.Testing.CaseTimeoutTicks = 2;
			host.Suite(R"(return Test.Suite("Sequential", function()
				local done=false
				Test.Case("first", function() Test.WaitTicks(1); done=true end,{TimeoutTicks=6})
				Test.Case("second", function() Test.Expect(done); Test.WaitTicks(20) end)
				Test.Case("third", function() Test.Expect(done) end)
			end,{CaseTimeoutTicks=2}))");
			auto result = host.Run();
			REQUIRE(result.Cases.size() == 3);
			CHECK(result.Cases[0].Status == "passed");
			CHECK(result.Cases[1].Status == "timeout");
			CHECK(result.Cases[1].Line > 0);
			CHECK(result.Cases[2].Status == "passed");
			CHECK_FALSE(result.Passed);
		}

		TEST_CASE("TestRunner: scene restarts between suites")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("A",function() Test.Case("creates",function() Scene.CreateEntity("Transient") end) end))");
			host.Suite(R"(return Test.Suite("B",function() Test.Case("fresh",function() Test.Expect(Scene.FindByName("Transient")==nil) end) end))");
			auto result = host.Run();
			CHECK(result.Passed);
			REQUIRE(result.Suites.size() == 2);
			CHECK(result.Suites[0].Suite == "A");
			CHECK(result.Suites[1].Suite == "B");
			CHECK(host.Active == nullptr);
		}

		TEST_CASE("TestRunner: quit ends the suite and honours ExpectQuit")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Quit",function() Test.Case("quits",function() Application.Quit(7) end); Test.Case("never",function() Test.Fail("must not run") end) end))").ExpectQuit = 7;
			auto result = host.Run();
			CHECK(result.Passed);
			REQUIRE(result.Cases.size() == 1);
			CHECK(result.Cases[0].Status == "quit");
			CHECK(result.Cases[0].QuitExpected);
			host.Settings.Testing.Suites[0].ExpectQuit = 8;
			result = host.Run();
			CHECK_FALSE(result.Passed);
			CHECK_FALSE(result.Cases[0].QuitExpected);
		}

		TEST_CASE("TestRunner: final teardown quit is reported once and honours ExpectQuit")
		{
			TypeRegistry registry;
			RegisterProjectSettingsTypes(registry);
			RegisterTestResultTypes(registry);
			registry.Freeze();
			for (const auto isolation : { TestSuiteSettings::IsolationMode::Suite, TestSuiteSettings::IsolationMode::Case })
			{
				for (const int32_t expected : { -1, 7, 8 })
				{
					CAPTURE(isolation);
					CAPTURE(expected);
					RunnerHost host;
					host.Behaviour(R"(local Driver={}
function Driver.OnDestroy(self) Application.Quit(7) end
return Script.Define("Driver",Driver))");
					auto& suite = host.Suite(R"(return Test.Suite("TeardownQuit",function()
	Test.Case("passes",function() Test.Expect(true) end)
end))");
					suite.Isolation = isolation;
					suite.ExpectQuit = expected;
					const auto result = host.Run();
					CHECK(result.Passed == (expected == 7));
					REQUIRE(result.Suites.size() == 1);
					CHECK(result.Suites[0].Passed == (expected == 7));
					REQUIRE(result.Cases.size() == 2);
					CHECK(result.Cases[0].Status == "passed");
					CHECK(result.Cases[1].Case == "<teardown>");
					CHECK(result.Cases[1].Status == "quit");
					CHECK(result.Cases[1].QuitCode == 7);
					CHECK(result.Cases[1].QuitExpected == (expected == 7));
					const auto json = TestRunResultToJson(result, registry);
					REQUIRE(json);
					CHECK((*json)["cases"][1]["status"] == "quit");
					CHECK((*json)["cases"][1]["quitCode"] == 7);
					CHECK((*json)["cases"][1]["quitExpected"] == (expected == 7));
					const auto xml = TestRunResultToJUnit(result);
					REQUIRE(xml);
					CHECK(xml->find("&lt;teardown&gt;") != std::string::npos);
					CHECK((xml->find("<failure") != std::string::npos) == (expected != 7));
				}
			}
		}

		TEST_CASE("TestRunner: teardown does not duplicate a quit already reported by the case")
		{
			RunnerHost host;
			host.Behaviour(R"(local Driver={}; function Driver.OnDestroy(self) Application.Quit(8) end; return Script.Define("Driver",Driver))");
			host.Suite(R"(return Test.Suite("Quit",function() Test.Case("quits",function() Application.Quit(7) end) end))").ExpectQuit = 7;
			const auto result = host.Run();
			CHECK(result.Passed);
			REQUIRE(result.Cases.size() == 1);
			CHECK(result.Cases[0].QuitCode == 7);
		}

		TEST_CASE("TestRunner: filter matches suite/case")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("MixedCase",function() Test.Case("alpha",function() Test.Fail("unselected") end); Test.Case("Beta",function() end) end))");
			auto result = host.Run({ .Filter = "mixedcase/beTA" });
			REQUIRE(result.Cases.size() == 1);
			CHECK(result.Cases[0].Case == "Beta");
			CHECK(result.Passed);
		}

		TEST_CASE("TestRunner: case isolation recollects references in a fresh VM")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Isolated",function()
				Test.Case("first",function() Scene.CreateEntity("Transient") end)
				Test.Case("second",function() Test.Expect(Scene.FindByName("Transient")==nil) end)
			end))")
				.Isolation = TestSuiteSettings::IsolationMode::Case;
			auto result = host.Run();
			CHECK(result.Passed);
			REQUIRE(result.Cases.size() == 2);
			CHECK(host.Created.size() == 3);
			CHECK(host.Created[1].Serial != host.Created[2].Serial);
		}

		TEST_CASE("TestRunner: suite collection is bounded and never executes case bodies")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Inventory",function() Test.Case("unexecuted",function() error("body executed") end) end))");
			auto inventory = FeatureTestRunner::Discover(host, TestSuiteSettings::Mode::Editor);
			REQUIRE(inventory);
			REQUIRE(inventory->Suites.size() == 1);
			CHECK(inventory->Suites[0].Cases.size() == 1);
			CHECK(host.Frames.empty());
			CHECK(host.Active == nullptr);
			RunnerHost broken;
			broken.Suite(R"(return Test.Suite("Broken",function() error("collection boom") end))");
			Test::ExpectLog expected(LogLevel::Error, "collection boom");
			CHECK_FALSE(FeatureTestRunner::Discover(broken, TestSuiteSettings::Mode::Editor));
		}

		TEST_CASE("TestRunner: empty scene parameters modes and exact overrides reach the session")
		{
			RunnerHost host;
			auto& suite = host.Suite(R"(return Test.Suite("Params",function() Test.Case("reads",function() Test.ExpectEqual(Scene.GetEntityCount(),0); Test.ExpectEqual(Scene.GetLoadParameters().Level,9) end) end))");
			suite.Scene.clear();
			suite.Parameters.Set(Json{ { "Level", 9 } });
			suite.Modes = { TestSuiteSettings::Mode::Dist };
			suite.Overrides.CallbackBudgetMs = 50;
			suite.Overrides.MemoryLimitMB = 16;
			suite.Overrides.PauseOnError = TestSuiteOverrides::PauseOnErrorOverride::Continue;
			auto result = host.Run({ .Mode = TestSuiteSettings::Mode::Dist });
			CHECK(result.Passed);
			REQUIRE_FALSE(host.Created.empty());
			const auto& spec = host.Created.back();
			CHECK(spec.Project.Scripting.CallbackBudgetMs == 50);
			CHECK(spec.Project.Scripting.MemoryLimitMB == 16);
			CHECK_FALSE(spec.Project.Scripting.PauseOnError);
			CHECK(spec.ScriptRunMode == RunModes::Dist);
			auto empty = FeatureTestRunner::Discover(host, TestSuiteSettings::Mode::Editor);
			REQUIRE(empty);
			CHECK(empty->Suites.empty());
		}

		TEST_CASE("TestRunner: scripted clocks preserve zero one and multiple step frames")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Clock",function() Test.Case("waits",function() Test.WaitTicks(10) end) end))").Clock = { 0.0f, 1.0f / 60.0f, 2.0f / 60.0f, 5.0f / 60.0f };
			auto result = host.Run();
			CHECK(result.Passed);
			REQUIRE(host.Frames.size() >= 4);
			CHECK(host.Frames[0] == 0);
			CHECK(host.Frames[1] == 1);
			CHECK(host.Frames[2] == 3);
			CHECK(host.Frames[3] == 8);
		}

		TEST_CASE("TestRunner: scripted clocks cycle and an all zero clock reports nonprogress")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Clock",function() Test.Case("waits",function() Test.WaitTicks(3) end) end))").Clock = { 0.0f, 1.0f / 60.0f };
			auto result = host.Run();
			CHECK(result.Passed);
			REQUIRE(host.Frames.size() >= 4);
			CHECK(host.Frames[0] == 0);
			CHECK(host.Frames[1] == 1);
			CHECK(host.Frames[2] == 1);
			CHECK(host.Frames[3] == 2);
			host.Settings.Testing.Suites[0].Clock = { 0.0f };
			result = host.Run();
			CHECK_FALSE(result.Passed);
			REQUIRE_FALSE(result.Cases.empty());
			CHECK(result.Cases[0].Message.find("progress") != std::string::npos);
		}

		TEST_CASE("TestRunner: zero time scale times out without simulation progress and releases audio")
		{
			for (const bool pauseOnCreate : { false, true })
			{
				CAPTURE(pauseOnCreate);
				RunnerHost host;
				host.EnableAudio();
				host.Settings.Testing.CaseTimeoutTicks = 4;
				if (pauseOnCreate)
					host.Behaviour(R"(local Driver={}; function Driver.OnCreate(self) Time.SetTimeScale(0) end; return Script.Define("Driver",Driver))");
				host.Suite(R"(return Test.Suite("Paused",function()
	Test.Case("waits",function() Time.SetTimeScale(0); Test.WaitTicks(1) end)
end))")
					.Clock = { 1.0f / 60.0f };
				const auto result = host.Run();
				CHECK_FALSE(result.Passed);
				REQUIRE(result.Cases.size() == 1);
				CHECK(result.Cases[0].Status == "timeout");
				CHECK(result.Cases[0].Message.find("no fixed-step progress") != std::string::npos);
				CHECK_FALSE(result.Cases[0].File.empty());
				CHECK(result.Cases[0].Line > 0);
				CHECK(result.Cases[0].Ticks == (pauseOnCreate ? 0 : 1));
				CHECK(host.Frames.size() == (pauseOnCreate ? 4 : 5));
				CHECK(host.Active == nullptr);
				CHECK_FALSE(host.Audio->IsSimulationTimeOwned());
			}
		}

		TEST_CASE("TestRunner: finite frame pauses recover and reset the nonprogress budget")
		{
			RunnerHost host;
			host.Settings.Testing.CaseTimeoutTicks = 6;
			host.Behaviour(R"(local Driver={}
function Driver.OnCreate(self) Time.SetTimeScale(0); self.Frames=0 end
function Driver.OnUpdate(self)
	self.Frames += 1
	if self.Frames % 3 == 2 then Time.SetTimeScale(1) else Time.SetTimeScale(0) end
end
return Script.Define("Driver",Driver))");
			host.Suite(R"(return Test.Suite("Recovers",function()
	Test.Case("waits",function() Test.WaitTicks(3) end)
end))")
				.Clock = { 1.0f / 60.0f };
			const auto result = host.Run();
			CHECK(result.Passed);
			REQUIRE(result.Cases.size() == 1);
			CHECK(result.Cases[0].Ticks == 4);
			CHECK(host.Frames.size() == 12);
		}

		TEST_CASE("TestRunner: every suite owns audio time including scripted clock suites")
		{
			RunnerHost host;
			host.EnableAudio();
			host.Suite(R"(return Test.Suite("Audio",function() Test.Case("capture",function() local levels=Test.CaptureAudio(2); Test.Expect(levels.Peak>0) end) end))").Clock = { 5.0f / 60.0f };
			auto result = host.Run();
			CHECK(result.Passed);
			for (const auto& spec : host.Created)
				CHECK(spec.OwnsAudioTime);
			CHECK(host.Audio->GetStats().PulledFrames > 0);
			CHECK(host.Audio->GetVoices().empty());
		}

		TEST_CASE("TestRunner: startup audio mix is isolated across suites and restores the host")
		{
			RunnerHost host;
			host.EnableAudio();
			REQUIRE(host.Audio->SetGroupVolume(AudioGroup::Music, 0.75f));
			host.Behaviour(R"(local Driver={}
function Driver.OnCreate(self) assert(Audio.GetGroupVolume("Music")==1); Audio.SetGroupVolume("Music",0.25) end
function Driver.OnStart(self) assert(Audio.GetGroupVolume("Music")==0.25); Audio.SetGroupVolume("Music",0.5) end
function Driver.OnDestroy(self) Audio.SetGroupVolume("Music",0.125) end
return Script.Define("Driver",Driver))");
			host.Suite(R"(return Test.Suite("First",function()
	Test.Case("mix",function() Test.ExpectEqual(Audio.GetGroupVolume("Music"),0.5); Audio.SetGroupVolume("Music",0.375) end)
end))");
			host.Suite(R"(return Test.Suite("Second",function()
	Test.Case("mix",function() Test.ExpectEqual(Audio.GetGroupVolume("Music"),0.5) end)
end))");
			const auto result = host.Run();
			CHECK(result.Passed);
			REQUIRE(result.Suites.size() == 2);
			CHECK(result.Suites[0].Passed);
			CHECK(result.Suites[1].Passed);
			CHECK(host.Audio->GetGroupVolume(AudioGroup::Music) == 0.75f);
			CHECK_FALSE(host.Audio->IsSimulationTimeOwned());
			CHECK(host.Audio->GetVoices().empty());
		}

		TEST_CASE("TestRunner: windowed streamed audio is decoded deterministically")
		{
			RunnerHost host;
			host.EnableAudio(true);
			host.Suite(R"(return Test.Suite("Stream",function() Test.Case("samples",function() local levels=Test.CaptureAudio(3); Test.Expect(levels.RmsLeft>0.1); Test.ExpectNear(levels.RmsLeft,levels.RmsRight,0.000001) end) end))");
			auto first = host.Run();
			auto second = host.Run();
			CHECK(first.Passed);
			CHECK(second.Passed);
			CHECK(host.Audio->GetSpecification().Decoding == AudioDecoding::Deterministic);
			CHECK_FALSE(host.Created.back().Environment.IsHeadless);
			CHECK(first.Suites[0].FinalStateHash == second.Suites[0].FinalStateHash);
		}

		TEST_CASE("TestRunner: capture cancellation releases audio and case tasks")
		{
			RunnerHost host;
			host.EnableAudio();
			host.Suite(R"(return Test.Suite("Cancel",function() Test.Case("capture",function() Task.Spawn(function() Test.WaitTicks(20); Test.Fail("late task") end); Test.CaptureAudio(100) end) end))");
			FeatureTestRunner runner;
			REQUIRE(runner.Begin(host));
			for (uint32_t i = 0; i < 20 && host.Audio->GetStats().PulledFrames == 0; ++i)
			{
				REQUIRE(runner.Advance());
			}
			REQUIRE(host.Audio->GetStats().PulledFrames > 0);
			runner.Cancel();
			CHECK_FALSE(runner.IsRunning());
			CHECK(host.Active == nullptr);
			CHECK(host.Audio->GetVoices().empty());
			auto result = runner.GetResult();
			REQUIRE(result);
			CHECK(result->Cancelled);
			CHECK_FALSE(result->Passed);
		}

		TEST_CASE("TestRunner: audio capture measures the requested ticks across multi step frames")
		{
			RunnerHost host;
			host.EnableAudio();
			host.Settings.Simulation.FixedHz = 144;
			ReplayHeader header;
			header.Scene.Handle = UUID(401);
			header.Seed = host.Settings.Simulation.Seed;
			header.FixedHz = 144;
			auto reference = host.CreateReplaySession(header, TestSuiteSettings::Mode::Editor);
			REQUIRE(reference);
			host.Audio->StartCapture();
			for (uint32_t i = 0; i < 5; ++i)
				(*reference)->FixedStep();
			(*reference)->FrameUpdate({ 5.0 / 144.0, 5.0 / 144.0, 1.0f, 0 });
			const auto capture = host.Audio->StopCapture();
			REQUIRE(capture.GetFrameCount() >= 1000);
			// K=1, n=2 at 144 Hz: [round(48000/144), round(3*48000/144)) = [333,1000).
			const auto expected = MeasureAudioLevels(std::span<const float>(capture.Samples).subspan(333 * 2, 667 * 2));
			reference->reset();
			const auto referenceFrames = host.Audio->GetStats().PulledFrames;
			host.Suite(std::format(R"(return Test.Suite("ExactAudio",function() Test.Case("capture",function()
				Test.WaitTicks(1)
				local levels=Test.CaptureAudio(2)
				Test.ExpectNear(levels.Peak,{:.9g},0.000001)
				Test.ExpectNear(levels.RmsLeft,{:.9g},0.000001)
				Test.ExpectNear(levels.RmsRight,{:.9g},0.000001)
				Test.Expect(Time.GetTick()>=5)
			end) end))",
						   expected.Peak, expected.RmsLeft, expected.RmsRight))
				.Clock = { 5.0f / 144.0f };
			auto first = host.Run();
			const auto baseline = host.Audio->GetStats().PulledFrames;
			auto second = host.Run();
			CHECK(first.Passed);
			CHECK(second.Passed);
			CHECK(baseline > 0);
			CHECK(host.Audio->GetStats().PulledFrames - baseline == baseline - referenceFrames);
			CHECK(first.Suites[0].Ticks == second.Suites[0].Ticks);
		}

		TEST_CASE("TestRunner: expectations accumulate and skip cannot erase a failure")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Assertions",function() Test.Case("retains",function() Test.Expect(false,"one"); Test.ExpectEqual(1,2,"two"); Test.Skip("later") end) end))");
			auto result = host.Run();
			REQUIRE(result.Cases.size() == 1);
			CHECK(result.Cases[0].Status == "failed");
			CHECK(result.Cases[0].Failures.size() == 2);
			CHECK(result.Cases[0].Failures[0].Line > 0);
			CHECK_FALSE(result.Passed);
		}

		TEST_CASE("TestRunner: setup quit and debug breaks are reported without exiting the host")
		{
			RunnerHost host;
			host.Behaviour(R"(local Driver={}; function Driver.OnCreate(self) Debug.Break(); Application.Quit(4) end; return Script.Define("Driver",Driver))");
			host.Suite(R"(return Test.Suite("Setup",function() Test.Case("never",function() Test.Fail("never") end) end))").ExpectQuit = 4;
			auto result = host.Run();
			CHECK(result.Passed);
			REQUIRE(result.Suites.size() == 1);
			CHECK(result.Suites[0].Breaks == 1);
			REQUIRE(result.Cases.size() == 1);
			CHECK(result.Cases[0].Status == "quit");
		}

		TEST_CASE("TestRunner: suite and run tick limits survive isolated session restarts")
		{
			RunnerHost host;
			host.Settings.Testing.SuiteTickLimit = 3;
			host.Suite(R"(return Test.Suite("Limits",function() Test.Case("a",function() Test.WaitTicks(1) end); Test.Case("b",function() Test.WaitTicks(3) end) end))").Isolation = TestSuiteSettings::IsolationMode::Case;
			auto result = host.Run();
			CHECK_FALSE(result.Passed);
			CHECK(result.Suites[0].Ticks == 3);
			CHECK(result.Cases.back().Status == "timeout");
			host.Settings.Testing.SuiteTickLimit = 100;
			result = host.Run({ .TimeoutTicks = 3 });
			CHECK_FALSE(result.Passed);
			CHECK(result.Suites[0].Ticks == 3);
		}

		TEST_CASE("TestRunner: recording selects one suite and captures Test injections")
		{
			RunnerHost host;
			host.Suite(InputSuite);
			auto result = host.Run({ .Record = true });
			CHECK(result.Passed);
			REQUIRE(host.Candidate.Events.size() >= 3);
			CHECK(host.Candidate.Events[0].Tick == 1);
			CHECK(host.Candidate.Events[0].KeyName == "Space");
			CHECK(host.Candidate.Events[0].State == "Down");
			CHECK(host.Candidate.Header.Scene.Handle == UUID(401));
			host.Suite(R"(return Test.Suite("Another",function() Test.Case("x",function() end) end))");
			FeatureTestRunner runner;
			CHECK_FALSE(runner.Begin(host, { .Record = true }));
		}

		TEST_CASE("TestRunner: recording rejects invalidation without publishing a partial replay")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Mutation",function() Test.Case("writes",function() Scene.CreateEntity("External") end) end))");
			FeatureTestRunner runner;
			REQUIRE(runner.Begin(host, { .Record = true }));
			host.Finish(runner);
			CHECK_FALSE(runner.TakeRecording());
			CHECK(host.ReplaySessions == 0);
			auto result = runner.GetResult();
			REQUIRE(result);
			CHECK_FALSE(result->Passed);
		}

		TEST_CASE("TestRunner: recording verifies fresh ordinary gameplay before publication")
		{
			RunnerHost host;
			host.Suite(InputSuite);
			auto result = host.Run({ .Record = true });
			CHECK(result.Passed);
			CHECK(host.ReplaySessions == 1);
			const auto& spec = host.Created.back();
			CHECK_FALSE(spec.TestMode);
			CHECK(spec.TestHost == nullptr);
			CHECK(spec.TestHook == nullptr);
			CHECK(spec.OwnsAudioTime);
		}

		TEST_CASE("TestRunner: recording mismatch retains the observed hash and publishes no candidate")
		{
			RunnerHost host;
			host.Suite(InputSuite);
			const auto ordinary = host.Run();
			host.DivergentReplay = true;
			FeatureTestRunner runner;
			REQUIRE(runner.Begin(host, { .Record = true }));
			host.Finish(runner);
			CHECK_FALSE(runner.TakeRecording());
			auto result = runner.GetResult();
			REQUIRE(result);
			CHECK_FALSE(result->Passed);
			CHECK(result->Suites[0].FinalStateHash == ordinary.Suites[0].FinalStateHash);
			CHECK(result->Cases[0].Status == "passed");
		}

		TEST_CASE("TestRunner: a verified recording transfers once without changing suite results")
		{
			RunnerHost host;
			host.Suite(InputSuite);
			FeatureTestRunner runner;
			REQUIRE(runner.Begin(host, { .Record = true }));
			host.Finish(runner);
			auto before = runner.GetResult();
			REQUIRE(before);
			auto candidate = runner.TakeRecording();
			REQUIRE(candidate);
			CHECK_FALSE(runner.TakeRecording());
			auto after = runner.GetResult();
			REQUIRE(after);
			CHECK(before->Suites[0].FinalStateHash == after->Suites[0].FinalStateHash);
			CHECK(before->Suites[0].Ticks == after->Suites[0].Ticks);
			CHECK(candidate->FinalStateHash == before->Suites[0].FinalStateHash);
		}

		TEST_CASE("TestRunner: recording accepts a scripted clock when normal playback reproduces it")
		{
			RunnerHost host;
			host.Suite(InputSuite).Clock = { 0.0f, 2.0f / 60.0f, 5.0f / 60.0f };
			auto result = host.Run({ .Record = true });
			CHECK(result.Passed);
			CHECK(host.ReplaySessions == 1);
			CHECK(host.Candidate.FinalTick == result.Suites[0].Ticks);
		}

		TEST_CASE("TestRunner: case isolated recording preserves all inputs and reports reproduction failure")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Segments",function()
				Test.Case("a",function() Test.InjectKey("A","Tap"); Test.WaitTicks(2) end)
				Test.Case("b",function() Test.InjectKey("B","Tap"); Test.WaitTicks(2) end)
			end))")
				.Isolation = TestSuiteSettings::IsolationMode::Case;
			FeatureTestRunner runner;
			REQUIRE(runner.Begin(host, { .Record = true }));
			host.Finish(runner);
			auto result = runner.GetResult();
			REQUIRE(result);
			CHECK_FALSE(result->Passed);
			CHECK(result->Cases[0].Status == "passed");
			CHECK(result->Cases[1].Status == "passed");
			CHECK(result->Suites[0].Ticks == 6);
			CHECK(host.ReplaySessions == 1);
			REQUIRE(host.VerifiedInputs.size() == 4);
			CHECK(host.VerifiedInputs[0].Tick == 1);
			CHECK(host.VerifiedInputs[0].Event.KeyCode == Key::A);
			CHECK(host.VerifiedInputs[1].Tick == 2);
			CHECK(host.VerifiedInputs[2].Tick == 4);
			CHECK(host.VerifiedInputs[2].Event.KeyCode == Key::B);
			CHECK(host.VerifiedInputs[3].Tick == 5);
			CHECK_FALSE(runner.TakeRecording());
		}

		TEST_CASE("TestRunner: a single selected isolated case records when normal playback reproduces it")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Segments",function()
				Test.Case("a",function() Test.Fail("not selected") end)
				Test.Case("b",function() Test.InjectKey("B","Tap"); Test.WaitTicks(2) end)
			end))")
				.Isolation = TestSuiteSettings::IsolationMode::Case;
			auto result = host.Run({ .Filter = "Segments/b", .Record = true });
			CHECK(result.Passed);
			REQUIRE(result.Cases.size() == 1);
			CHECK(host.Candidate.Events[0].KeyName == "B");
		}

		TEST_CASE("TestRunner: recording verification cancels without publishing or inflating coverage")
		{
			RunnerHost host;
			host.Suite(InputSuite);
			FeatureTestRunner runner;
			REQUIRE(runner.Begin(host, { .Record = true }));
			for (uint32_t i = 0; i < 100 && runner.GetPhase() != "verify recording"; ++i)
				REQUIRE(runner.Advance());
			REQUIRE(runner.GetPhase() == "verify recording");
			REQUIRE(runner.Advance());
			runner.Cancel();
			CHECK_FALSE(runner.TakeRecording());
			auto result = runner.GetResult();
			REQUIRE(result);
			CHECK(result->Cancelled);
			CHECK(result->Suites[0].Ticks == 4);
			CHECK(result->Cases[0].Ticks == 4);
			CHECK(result->Coverage.RegistryFingerprint == "fixture");
		}

		TEST_CASE("TestRunner: setup late frame and teardown errors survive VM replacement")
		{
			RunnerHost host;
			host.Behaviour(R"(local Driver={}
			function Driver.OnCreate(self) error("setup fault") end
			return Script.Define("Driver",Driver))");
			host.Suite(R"(return Test.Suite("Faults",function() Test.Case("body",function() end) end))");
			Test::ExpectLog setup(LogLevel::Error, "setup fault");
			auto result = host.Run();
			CHECK_FALSE(result.Passed);
			REQUIRE_FALSE(result.Cases.empty());
			CHECK(result.Cases[0].Case == "<setup>");
			CHECK(result.Suites[0].ScriptErrors >= 1);
			RunnerHost late;
			late.Behaviour(R"(local Driver={}; function Driver.OnLateUpdate(self,dt) error("late fault") end; return Script.Define("Driver",Driver))");
			late.Suite(R"(return Test.Suite("Late",function() Test.Case("body",function() end) end))");
			Test::ExpectLog lateLog(LogLevel::Error, "late fault");
			auto lateResult = late.Run();
			CHECK_FALSE(lateResult.Passed);
			CHECK(lateResult.Cases[0].Status == "error");
			RunnerHost teardown;
			teardown.Behaviour(R"(local Driver={}; function Driver.OnDestroy(self) error("teardown fault") end; return Script.Define("Driver",Driver))");
			teardown.Suite(R"(return Test.Suite("Teardown",function()
				Test.Case("replace",function() Scene.Load(Assets.Load("Assets/Scenes/Next.scene")) end)
				Test.Case("after",function() Test.ExpectEqual(#Test.GetScriptErrors(),0) end)
			end))");
			Test::ExpectLog teardownLog(LogLevel::Error, "teardown fault");
			auto teardownResult = teardown.Run();
			CHECK_FALSE(teardownResult.Passed);
			CHECK(teardownResult.Cases[0].Status == "error");
			CHECK(teardownResult.Cases[0].Failures[0].Message.find("teardown fault") != std::string::npos);
			CHECK(teardownResult.Cases[1].Status == "passed");
			CHECK(teardownResult.Suites[0].ScriptErrors == 1);
		}

		TEST_CASE("TestRunner: cancellation releases sessions references and captures without another advance")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Cancel",function() Test.Case("wait",function() Test.WaitTicks(100) end) end))");
			FeatureTestRunner runner;
			REQUIRE(runner.Begin(host));
			REQUIRE(runner.Advance());
			REQUIRE(runner.Advance());
			REQUIRE(runner.Advance());
			REQUIRE(host.Active != nullptr);
			runner.Cancel();
			runner.Cancel();
			CHECK(host.Active == nullptr);
			CHECK_FALSE(runner.IsRunning());
			auto result = runner.GetResult();
			REQUIRE(result);
			CHECK(result->Cancelled);
		}

		TEST_CASE("TestRunner: completed cases recollect after scene load and continue at the next index")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Load",function()
				Test.Case("loads",function() Test.Expect(Scene.GetLoadParameters().Level==nil); Scene.Load(Assets.Load("Assets/Scenes/Next.scene"),{Level=9}) end)
				Test.Case("verifies",function() Test.ExpectEqual(Scene.GetLoadParameters().Level,9) end)
			end))");
			auto result = host.Run();
			CHECK(result.Passed);
			REQUIRE(result.Cases.size() == 2);
			CHECK(result.Cases[0].Case == "loads");
			CHECK(result.Cases[1].Case == "verifies");
			CHECK(result.Suites[0].Ticks == 2);
		}

		TEST_CASE("TestRunner: a case yielded across scene load reports a located error")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Load",function()
				Test.Case("yields",function() Scene.Load(Assets.Load("Assets/Scenes/Next.scene")); Test.WaitTicks(3) end)
			end))");
			auto result = host.Run();
			CHECK_FALSE(result.Passed);
			REQUIRE(result.Cases.size() == 1);
			CHECK(result.Cases[0].Status == "error");
			CHECK(result.Cases[0].Line > 0);
			CHECK(result.Cases[0].Message.find("Scene.Load") != std::string::npos);
		}

		TEST_CASE("TestRunner: scene load recollection validates case identities and preserves case isolation")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Changed",function()
				Test.Case("load",function() Scene.Load(Assets.Load("Assets/Scenes/Next.scene"),{New=true}) end)
				if Scene.GetLoadParameters().New then Test.Case("added",function() end) end
				Test.Case("last",function() end)
			end))");
			auto result = host.Run();
			CHECK_FALSE(result.Passed);
			CHECK(result.Cases.back().Message.find("recollection") != std::string::npos);
			host.Settings.Testing.Suites[0].Isolation = TestSuiteSettings::IsolationMode::Case;
			result = host.Run();
			CHECK(result.Passed);
			CHECK(result.Cases.size() == 2);
		}

		TEST_CASE("TestRunner: replay paths are deduplicated and run after suites in canonical order")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("First",function() Test.Case("body",function() end) end))");
			auto header = ReplayHeader{};
			header.Scene = { UUID(401), "Assets/Scenes/Start.scene" };
			header.Seed = host.Settings.Simulation.Seed;
			header.EngineVersion = "1";
			header.Config = "Debug";
			auto session = host.CreateReplaySession(header, TestSuiteSettings::Mode::Editor);
			REQUIRE(session);
			auto data = CreateRef<ReplayData>();
			data->Header = header;
			data->FinalStateHash = UUID((*session)->ComputeStateHash()).ToString();
			session->reset();
			host.ReplayAssets["Assets/A.replay"] = data;
			host.ReplayAssets["Assets/B.replay"] = data;
			host.ReplayGlobs = { "Assets/B.replay", "Assets/A.replay", "Assets/B.replay" };
			auto result = host.Run();
			CHECK(result.Passed);
			REQUIRE(result.Suites.size() == 3);
			CHECK(result.Suites[0].Suite == "First");
			CHECK(result.Suites[1].Suite == "replay:Assets/A.replay");
			CHECK(host.PlayedReplays == std::vector<std::string>{ "Assets/A.replay", "Assets/B.replay" });
		}
	}

}
