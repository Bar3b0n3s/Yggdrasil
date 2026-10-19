#include "TestsPCH.h"
#include "Engine/Testing/FeatureTestRunner.h"

#include "Engine/Asset/AudioClipData.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Asset/ReplayData.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Jobs/MainThreadQueue.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/AudioSourceComponent.h"
#include "Engine/Scene/Components/PointLightComponent.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/RegisterBindings.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptCompiler.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Engine/Session/PlayInputEventCodec.h"
#include "Engine/Testing/CoverageGates.h"
#include "Support/AudioTestData.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <map>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

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
			std::vector<std::string> Screenshots{}; // the names CaptureScreenshot received, in order
			PlaySession* Active = nullptr;
			uint64_t Serial = 1;
			uint32_t CoverageReads = 0;
			uint32_t ReplaySessions = 0;
			bool DivergentReplay = false;
			bool OwnAudioTime = true;                          // false: a host that forgets PlaySessionSpecification::OwnsAudioTime
			ScriptApiRegistry* ScriptApi = nullptr;            // a frozen registry the suite sessions share (null: each session owns one)
			AssetManager* ObservedAssets = nullptr;            // GetAssetManager: the manager a coverage run observes (null: none)
			std::vector<TestCoverageCounter> ScriptCounters{}; // GetCoverage's counters
			ReplayDocument Candidate{};
			std::optional<uint64_t> InputCheckTick{}; // FeatureTestRunner::GetInputCheckTick after the last Run
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
				REQUIRE_MESSAGE(script, (script ? std::string() : script.error().ToString()));
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

			// A Driver that moves one unit along X per fixed step while Space, A or B is held, plus the Move axis, and jumps
			// back to the origin on an R press (a restart in place): a recording that presses those keys has an outcome that
			// depends on its input, which the recording's input check requires (M16 FX-4).
			void InputDriven()
			{
				Behaviour(R"(local Driver={}
function Driver.OnFixedUpdate(self)
	if Input.IsKeyPressed("R") then self.Entity.Transform.Translation = vector.zero return end
	local x = Input.GetAxis("Move")
	if Input.IsKeyDown("Space") or Input.IsKeyDown("A") or Input.IsKeyDown("B") then x += 1 end
	if x ~= 0 then self.Entity.Transform:Translate(vector.create(x, 0, 0)) end
end
return Script.Define("Driver",Driver))");
			}

			// Runs `runner` to its end and returns the error Advance ended it with, if any.
			std::optional<Error> RunToEnd(FeatureTestRunner& runner)
			{
				for (uint32_t i = 0; i < 2000 && runner.IsRunning(); ++i)
				{
					const auto advanced = runner.Advance();
					if (!advanced)
						return advanced.error();
				}
				CHECK_FALSE(runner.IsRunning());
				return std::nullopt;
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
				spec.ScriptApi = ScriptApi;
				spec.Audio = Audio.get();
				spec.OwnsAudioTime = OwnAudioTime;
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
				spec.OwnsAudioTime = OwnAudioTime;
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
			AssetManager* GetAssetManager() const override { return ObservedAssets; }
			Status CaptureScreenshot(PlaySession&, std::string_view name) override
			{
				Screenshots.emplace_back(name);
				return {};
			}
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
				report.Counters = ScriptCounters;
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
				InputCheckTick = runner.GetInputCheckTick();
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

		// An asset manager that tells whether a load observer is installed (HasLoadObserver) and whether it records a load
		// made now: NotifyLoaded reaches the runner's observer, whose FormatCoverageRecorder asks GetReferencePath about every
		// handle it has not seen.
		class ObserverProbe final : public AssetManager
		{
		public:
			ObserverProbe()
				: m_Jobs(0, m_MainThreadQueue)
			{
			}

			[[nodiscard]] bool IsObserved() const
			{
				return HasLoadObserver();
			}

			// Whether a load made now, outside the runner's Advance, is recorded.
			[[nodiscard]] bool RecordsLoadNow()
			{
				const uint64_t before = m_Queries;
				NotifyLoaded(AssetHandle(++m_Next));
				return m_Queries != before;
			}

			Result<AssetRef<Asset>> Load(AssetHandle handle) override { return MakeError(ErrorCode::NotFound, "the probe holds no asset {}", handle); }
			JobHandle<AssetRef<Asset>> LoadAsync(AssetHandle handle) override
			{
				return m_Jobs.Submit([result = Load(handle)]()
				{
					return result;
				});
			}
			AssetState GetState(AssetHandle) const override { return AssetState::Unloaded; }
			const AssetMetadata* GetMetadata(AssetHandle) const override { return nullptr; }
			AssetType GetAssetType(AssetHandle) const override { return AssetType::None; }
			std::optional<AssetHandle> Resolve(std::string_view) const override { return std::nullopt; }
			std::string GetReferencePath(AssetHandle) const override
			{
				++m_Queries;
				return "Assets/Probe.png";
			}
			void WaitIdle() override {}
		private:
			MainThreadQueue m_MainThreadQueue{};
			JobSystem m_Jobs; // inline
			uint64_t m_Next = 0x9000;
			mutable uint64_t m_Queries = 0;
		};

		const TestCoverageCounter* FindCounter(const TestRunResult& result, std::string_view id)
		{
			const auto found = std::ranges::find(result.Coverage.Counters, id, &TestCoverageCounter::Id);
			return found == result.Coverage.Counters.end() ? nullptr : &*found;
		}

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
		// ADR 0022 decision 3: Testing.CoverageGates opts a project in; only an unfiltered, non-recording run evaluates.
		TEST_CASE("TestRunner: a coverage project evaluates every per-mode gate and a gap fails the run")
		{
			RunnerHost host;
			host.Settings.Testing.CoverageGates = true;
			host.Suite(R"(return Test.Suite("Covered",function()
				Test.Case("passes",function() Test.Expect(true) end)
			end))");

			const TestRunResult result = host.Run();
			REQUIRE(result.Cases.size() == 1);
			CHECK(result.Cases[0].Status == "passed");
			REQUIRE(result.Coverage.Gates.size() == PerModeCoverageGates.size());
			for (size_t index = 0; index < PerModeCoverageGates.size(); ++index)
			{
				CHECK(result.Coverage.Gates[index].Gate == CoverageGateName(PerModeCoverageGates[index]));
				CHECK(result.Coverage.Gates[index].Evaluated);
			}
			CHECK_FALSE(result.Passed);
			const auto& counters = result.Coverage.Counters;
			CHECK(std::ranges::is_sorted(counters, {}, &TestCoverageCounter::Id));
			CHECK(std::ranges::find(counters, "component:Camera", &TestCoverageCounter::Id) != counters.end());
			CHECK(std::ranges::find(counters, "format:Texture/.png", &TestCoverageCounter::Id) != counters.end());

			FeatureTestRunOptions filteredOptions;
			filteredOptions.Filter = "Covered";
			const TestRunResult filtered = host.Run(filteredOptions);
			CHECK(filtered.Passed);
			REQUIRE(filtered.Coverage.Gates.size() == PerModeCoverageGates.size());
			CHECK(std::ranges::none_of(filtered.Coverage.Gates, [](const TestCoverageGateResult& gate)
			{
				return gate.Evaluated || gate.Passed;
			}));

			host.Settings.Testing.CoverageGates = false;
			const TestRunResult plain = host.Run();
			CHECK(plain.Passed);
			CHECK(plain.Coverage.Gates.empty());
		}

		// ADR 0022 decision 5: the observer exists exactly for the life of a coverage run and is removed at every end of it,
		// so the stored function never outlives the recorder it captures.
		TEST_CASE("TestRunner: a coverage run observes loads only while it runs and removes the observer at every end")
		{
			ObserverProbe probe;
			RunnerHost host;
			host.ObservedAssets = &probe;
			host.Settings.Testing.CoverageGates = true;
			host.Suite(R"(return Test.Suite("Observed",function()
				Test.Case("waits",function() Test.WaitTicks(2) Test.Expect(true) end)
			end))");
			{
				FeatureTestRunner runner;
				REQUIRE(runner.Begin(host));
				CHECK(probe.IsObserved());
				// Installed, but a load between two Advance calls is never recorded (review finding M03).
				CHECK_FALSE(probe.RecordsLoadNow());
				host.Finish(runner);
				CHECK_FALSE(probe.IsObserved());
			}
			{
				FeatureTestRunner runner;
				REQUIRE(runner.Begin(host));
				REQUIRE(runner.Advance());
				CHECK(probe.IsObserved());
				runner.Cancel();
				CHECK_FALSE(probe.IsObserved());
				const auto result = runner.GetResult();
				REQUIRE(result);
				CHECK(result->Cancelled);
				REQUIRE(result->Coverage.Gates.size() == PerModeCoverageGates.size());
				CHECK(std::ranges::none_of(result->Coverage.Gates, &TestCoverageGateResult::Evaluated));
			}
			{
				{
					FeatureTestRunner runner;
					REQUIRE(runner.Begin(host));
					CHECK(probe.IsObserved());
				}
				CHECK_FALSE(probe.IsObserved());
			}
			// A filtered or recording run of a coverage project, and any run of another project, records no loads.
			for (const FeatureTestRunOptions& options : { FeatureTestRunOptions{ .Filter = "Observed" }, FeatureTestRunOptions{ .Record = true } })
			{
				FeatureTestRunner runner;
				REQUIRE(runner.Begin(host, options));
				CHECK_FALSE(probe.IsObserved());
				runner.Cancel();
			}
			host.Settings.Testing.CoverageGates = false;
			{
				FeatureTestRunner runner;
				REQUIRE(runner.Begin(host));
				CHECK_FALSE(probe.IsObserved());
				runner.Cancel();
			}
			// A Begin that fails leaves nothing installed.
			host.Settings.Testing.CoverageGates = true;
			host.Settings.Testing.CaseTimeoutTicks = 0;
			FeatureTestRunner refused;
			CHECK_FALSE(refused.Begin(host));
			CHECK_FALSE(probe.IsObserved());
		}

		// ADR 0022 decisions 4 and 5: the scene is recorded after setup and again after a replacement, so a component only
		// the loaded scene holds counts; the suite's own script and the loaded scene count for gate 9.
		TEST_CASE("TestRunner: a coverage run counts the scenes it simulates and the formats its sessions load")
		{
			RunnerHost host;
			host.ObservedAssets = &host.Compiler.GetAssetManager();
			host.Settings.Testing.CoverageGates = true;
			Entity lamp = host.Compiler.GetScene().CreateEntity("Lamp");
			lamp.AddComponent<PointLightComponent>();
			const auto next = SceneSerializer::ToJson(host.Compiler.GetScene());
			REQUIRE(next);
			host.PublishScene("Assets/Scenes/Next.scene", UUID(402), *next);
			host.Compiler.GetScene().DestroyEntity(lamp);
			host.RefreshScene();
			host.Suite(R"(return Test.Suite("Covered",function()
				Test.Case("replace",function() Scene.Load(Assets.Load("Assets/Scenes/Next.scene")) end)
				Test.Case("after",function() Test.Expect(true) end)
			end))");

			const TestRunResult result = host.Run();
			REQUIRE(result.Cases.size() == 2);
			CHECK(result.Cases[0].Status == "passed");
			CHECK(result.Cases[1].Status == "passed");
			const TestCoverageCounter* light = FindCounter(result, "component:PointLight");
			REQUIRE(light != nullptr);
			CHECK(light->Count >= 1);
			const TestCoverageCounter* script = FindCounter(result, "format:Script/.luau");
			REQUIRE(script != nullptr);
			CHECK(script->Count >= 1);
			const TestCoverageCounter* scene = FindCounter(result, "format:Scene/.scene");
			REQUIRE(scene != nullptr);
			CHECK(scene->Count == 1);
			CHECK(std::ranges::is_sorted(result.Coverage.Counters, {}, &TestCoverageCounter::Id));
			REQUIRE(result.Coverage.Gates.size() == PerModeCoverageGates.size());
			CHECK(std::ranges::all_of(result.Coverage.Gates, &TestCoverageGateResult::Evaluated));
			// Most of the registry is uncovered here, so the run fails on its gates alone.
			CHECK_FALSE(result.Passed);
			const auto gates = EvaluateCoverageGates(result.Coverage.Counters, TestSuiteSettings::Mode::Editor);
			REQUIRE(gates);
			CHECK(gates->size() == result.Coverage.Gates.size());
		}

		// Review finding M03 (ADR 0022 decision 5): the observer sits on the host's shared asset manager, and the editor renders
		// the published test session in its viewports in host frames between two Advance calls, whose number depends on the
		// wall-clock budget that partitions the run. Only the loads made inside Advance count, so a load between two calls
		// (the "viewport" below) never counts, however many host frames ran.
		TEST_CASE("TestRunner: gate 9 counts the loads of the run's own work and never a load between two Advance calls")
		{
			RunnerHost host;
			Test::InMemoryAssetManager& assets = host.Compiler.GetAssetManager();
			host.ObservedAssets = &assets;
			host.Settings.Testing.CoverageGates = true;
			const auto document = SceneSerializer::ToJson(host.Compiler.GetScene());
			REQUIRE(document);
			host.PublishScene("Assets/Scenes/Loaded.scene", UUID(411), *document);
			host.PublishScene("Assets/Scenes/Viewport.scene", UUID(412), *document);
			host.RefreshScene();
			host.Suite(R"(return Test.Suite("Frames",function()
				Test.Case("loads",function() Test.Expect(Assets.Load("Assets/Scenes/Loaded.scene") ~= nil) Test.WaitTicks(3) end)
			end))");

			FeatureTestRunner runner;
			REQUIRE(runner.Begin(host));
			uint32_t hostFrames = 0;
			for (; hostFrames < 1000 && runner.IsRunning(); ++hostFrames)
			{
				// A host frame: the editor's viewport renders the published session's scene through the same manager.
				REQUIRE(assets.Load(UUID(412)));
				const Result<bool> advanced = runner.Advance();
				REQUIRE_MESSAGE(advanced.has_value(), advanced.error().ToString());
			}
			CHECK(hostFrames > 1);
			const auto result = runner.GetResult();
			REQUIRE(result);
			REQUIRE(result->Cases.size() == 1);
			CHECK(result->Cases[0].Status == "passed");
			const TestCoverageCounter* scene = FindCounter(*result, "format:Scene/.scene");
			REQUIRE(scene != nullptr);
			// Loaded.scene only: Viewport.scene was loaded before every Advance call and never inside one.
			CHECK(scene->Count == 1);
		}

		// ADR 0022 decision 3: an evaluation error fails the run with unevaluated gates and an Error naming the mode.
		TEST_CASE("TestRunner: a coverage counter the gates refuse fails the run with unevaluated gates")
		{
			RunnerHost host;
			host.Settings.Testing.CoverageGates = true;
			host.ScriptCounters.push_back(TestCoverageCounter{ .Id = "api:Legacy.Call", .Kind = "call", .Modes = { TestSuiteSettings::Mode::Editor }, .Count = 1 });
			host.Suite(R"(return Test.Suite("Refused",function()
				Test.Case("passes",function() Test.Expect(true) end)
			end))");
			Test::ExpectLog expected(LogLevel::Error, "coverage gates of the Editor test run could not be evaluated");
			const TestRunResult result = host.Run();
			REQUIRE(result.Cases.size() == 1);
			CHECK(result.Cases[0].Status == "passed");
			CHECK_FALSE(result.Passed);
			REQUIRE(result.Coverage.Gates.size() == PerModeCoverageGates.size());
			CHECK(std::ranges::none_of(result.Coverage.Gates, [](const TestCoverageGateResult& gate)
			{
				return gate.Evaluated || gate.Passed;
			}));
		}

		// ADR 0022 decision 4 (gate 2): FeatureTest covers Test.Fail and Test.Skip without a failed or skipped case row by
		// calling them through pcall where no case is active (here the suite body, which collects the cases). Each raises a
		// catchable script error and still counts as called, because dispatch counts calls on entry
		// (ScriptApiRegistry::GetCoverage); the runner executes a suite body more than once (collection, then the run), so the
		// count only has to grow. A change to either rule would silently break gate 2, so it is pinned here.
		TEST_CASE("TestRunner: Test.Fail and Test.Skip outside an active case raise a catchable error and still count as called")
		{
			RunnerHost host;
			ScriptApiRegistry api;
			REQUIRE(RegisterBindings(api, host.Compiler.GetTypes()));
			host.ScriptApi = &api;
			const auto calls = [&api](std::string_view member) -> uint64_t
			{
				const Result<ScriptApiCoverage> coverage = api.GetCoverage(RunModes::Editor);
				REQUIRE_MESSAGE(coverage.has_value(), coverage.error().ToString());
				const auto found = std::ranges::find_if(coverage->Members, [member](const ScriptApiCounter& counter)
				{
					return counter.Owner == "Test" && counter.Member == member;
				});
				REQUIRE(found != coverage->Members.end());
				return found->Calls;
			};
			const uint64_t fails = calls("Fail");
			const uint64_t skips = calls("Skip");
			host.Suite(R"(return Test.Suite("Terminal",function()
				local failed, failure = pcall(Test.Fail, "outside a case")
				local skipped, skip = pcall(Test.Skip, "outside a case")
				Test.Case("observes",function()
					Test.Expect(not failed and type(failure) == "string", "Test.Fail raised a catchable error")
					Test.Expect(not skipped and type(skip) == "string", "Test.Skip raised a catchable error")
				end)
			end))");

			const TestRunResult result = host.Run();
			REQUIRE(result.Cases.size() == 1);
			CHECK_MESSAGE(result.Cases[0].Status == "passed", result.Cases[0].Message);
			CHECK(result.Passed);
			CHECK(calls("Fail") > fails);
			CHECK(calls("Skip") > skips);
		}

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
			CHECK(host.Audio->GetStats().PulledFrames > 0);
			CHECK(host.Audio->GetVoices().empty());
			// The runner checks the session the host returned, not the host's word: one that does not own audio time fails
			// its setup (a ScriptedClock suite runs without lockstep, so only OwnsAudioTime keeps the time).
			host.OwnAudioTime = false;
			result = host.Run();
			CHECK_FALSE(result.Passed);
			REQUIRE_FALSE(result.Cases.empty());
			CHECK(result.Cases[0].Case == "<setup>");
			CHECK(result.Cases[0].Message.find("does not own simulation audio time") != std::string::npos);
			CHECK_FALSE(host.Audio->IsSimulationTimeOwned());
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

		TEST_CASE("TestRunner: streamed audio captures identically across runs with deterministic decoding")
		{
			// Whether every editor really decodes deterministically is checked against the editor itself (EditorAppTests:
			// "--run-tests runs in a windowed editor"); this case proves capture of a streamed clip given such an engine.
			RunnerHost host;
			host.EnableAudio(true);
			host.Suite(R"(return Test.Suite("Stream",function() Test.Case("samples",function() local levels=Test.CaptureAudio(3); Test.Expect(levels.RmsLeft>0.1); Test.ExpectNear(levels.RmsLeft,levels.RmsRight,0.000001) end) end))");
			auto first = host.Run();
			auto second = host.Run();
			CHECK(first.Passed);
			CHECK(second.Passed);
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

		TEST_CASE("TestRunner: a case keeps a bounded number of failures and counts the rest")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Flood",function() Test.Case("misses",function()
				for index = 1, 100 do Test.Expect(false, "miss " .. index) end
			end) end))");
			auto result = host.Run();
			REQUIRE(result.Cases.size() == 1);
			const TestCaseResult& row = result.Cases[0];
			CHECK(row.Status == "failed");
			REQUIRE(row.Failures.size() == MaxScriptTestFailuresPerCase);
			CHECK(row.Message == "miss 1");
			for (size_t index = 0; index + 1 < row.Failures.size(); ++index)
				CHECK(row.Failures[index].Message == std::format("miss {}", index + 1));
			CHECK(row.Failures.back().Message == std::format("[truncated: {} more failures]", 100 - (MaxScriptTestFailuresPerCase - 1)));
			CHECK(row.Failures.back().File.empty());
			CHECK_FALSE(result.Passed);
		}

		TEST_CASE("TestRunner: a suite keeps a bounded number of failures across its cases")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Floods",function()
				for case = 1, 40 do
					Test.Case("misses " .. case, function() for index = 1, 40 do Test.Expect(false, "miss") end end)
				end
			end))");
			auto result = host.Run();
			REQUIRE(result.Cases.size() == 40);
			size_t kept = 0;
			uint64_t counted = 0;
			for (const TestCaseResult& row : result.Cases)
			{
				CHECK(row.Status == "failed");
				REQUIRE_FALSE(row.Failures.empty());
				CHECK(row.Failures.size() <= MaxScriptTestFailuresPerCase);
				const std::string& last = row.Failures.back().Message;
				REQUIRE(last.starts_with("[truncated: "));
				kept += row.Failures.size() - 1;
				counted += std::stoull(last.substr(std::string_view("[truncated: ").size()));
			}
			CHECK(kept == MaxScriptTestFailuresPerSuite);
			CHECK(kept + counted == 40 * 40);
			// The suite's budget ran out during the 34th case: the later ones keep only the count.
			CHECK(result.Cases[32].Failures.size() == MaxScriptTestFailuresPerCase);
			CHECK(result.Cases[39].Failures.size() == 1);
		}

		TEST_CASE("TestRunner: claimed script errors free their place and unclaimed ones past the cap fail the case")
		{
			RunnerHost host;
			// Two batches of claims, together more than a case keeps unclaimed: each claim releases its error at once.
			host.Suite(R"(return Test.Suite("Errors",function()
				Test.Case("claims",function()
					for batch = 1, 2 do
						for index = 1, 200 do Task.Spawn(function() error("expected " .. index, 0) end) end
						Test.WaitTicks(2)
						for index = 1, 200 do Test.ExpectScriptError("^expected %d+$", 1) end
					end
				end)
				Test.Case("floods",function()
					for index = 1, 300 do Task.Spawn(function() error("unexpected " .. index, 0) end) end
					Test.WaitTicks(2)
				end)
			end))");
			const Test::ExpectLog expected(LogLevel::Error, "expected ");
			auto result = host.Run();
			REQUIRE(result.Cases.size() == 2);
			CHECK(result.Cases[0].Status == "passed");
			CHECK(result.Cases[0].Failures.empty());
			const TestCaseResult& flooded = result.Cases[1];
			CHECK(flooded.Status == "error");
			REQUIRE(flooded.Failures.size() == MaxScriptTestFailuresPerCase);
			CHECK(flooded.Failures.front().Message == "unexpected 1");
			CHECK(flooded.Failures.back().Message == std::format("[truncated: {} more failures]", 300 - (MaxScriptTestFailuresPerCase - 1)));
			CHECK(result.Suites[0].ScriptErrors == 700);
		}

		TEST_CASE("TestRunner: a script error past the unclaimed-error cap cannot be claimed and still fails the case")
		{
			RunnerHost host;
			// No error is claimed before the newest one, which arrives with the case's unclaimed places all taken.
			host.Suite(std::format(R"(return Test.Suite("Overflow",function()
				Test.Case("claims the newest",function()
					for index = 1, {0} do Task.Spawn(function() error("fault " .. index, 0) end) end
					Test.WaitTicks(2)
					Test.ExpectScriptError("^fault {0}$", 1)
				end)
			end))",
				MaxScriptTestUnclaimedErrors + 1));
			const Test::ExpectLog expected(LogLevel::Error, "fault ");
			auto result = host.Run();
			REQUIRE(result.Cases.size() == 1);
			const TestCaseResult& row = result.Cases[0];
			CHECK(row.Status == "error");
			// The claim found nothing, so the case failed at it; then its unclaimed errors joined the row, oldest first, and
			// every error is counted, the one past the cap included.
			REQUIRE(row.Failures.size() == MaxScriptTestFailuresPerCase);
			CHECK(row.Failures.front().Message == "No matching script error was observed within the tick limit");
			CHECK(row.Failures.front().File == "Assets/Tests/Suite0.luau");
			CHECK(row.Failures.front().Line == 5);
			CHECK(row.Failures[1].Message == "fault 1");
			CHECK(row.Failures.back().Message == std::format("[truncated: {} more failures]", MaxScriptTestUnclaimedErrors + 1 - (MaxScriptTestFailuresPerCase - 2)));
			CHECK(result.Suites[0].ScriptErrors == MaxScriptTestUnclaimedErrors + 1);
		}

		TEST_CASE("TestRunner: a claim frees one place under the unclaimed-error cap, and an error that arrived past it stays unclaimable")
		{
			RunnerHost host;
			host.Suite(std::format(R"(return Test.Suite("Places",function()
				Test.Case("arrived past the cap",function()
					for index = 1, {0} do Task.Spawn(function() error("fault " .. index, 0) end) end
					Test.WaitTicks(2)
					for index = 1, {1} do Test.ExpectScriptError("^fault " .. index .. "$", 1) end
					Test.ExpectScriptError("^fault {0}$", 1)
				end)
				Test.Case("arrives after a claim",function()
					for index = 1, {1} do Task.Spawn(function() error("fault " .. index, 0) end) end
					Test.WaitTicks(2)
					Test.ExpectScriptError("^fault 1$", 1)
					Task.Spawn(function() error("fault {0}", 0) end)
					Test.ExpectScriptError("^fault {0}$", 1)
					for index = 2, {1} do Test.ExpectScriptError("^fault " .. index .. "$", 1) end
				end)
			end))",
				MaxScriptTestUnclaimedErrors + 1, MaxScriptTestUnclaimedErrors));
			const Test::ExpectLog expected(LogLevel::Error, "fault ");
			auto result = host.Run();
			REQUIRE(result.Cases.size() == 2);
			// Every kept error was claimed, the last at the cap's last place, but the one that arrived past the cap was never
			// kept: the VM cannot claim it, and the row counts it without listing it.
			const TestCaseResult& overflow = result.Cases[0];
			CHECK(overflow.Status == "error");
			REQUIRE(overflow.Failures.size() == 2);
			CHECK(overflow.Failures[0].Message == "No matching script error was observed within the tick limit");
			CHECK(overflow.Failures[0].Line == 6);
			CHECK(overflow.Failures[1].Message == "[truncated: 1 more failures]");
			// With the cap full, one claim releases one place, which the next error takes; it can be claimed.
			const TestCaseResult& released = result.Cases[1];
			CAPTURE(released.Message);
			CHECK(released.Status == "passed");
			CHECK(released.Failures.empty());
			CHECK(result.Suites[0].ScriptErrors == 2 * (MaxScriptTestUnclaimedErrors + 1));
		}

		TEST_CASE("TestRunner: a suite registers a bounded number of cases")
		{
			RunnerHost host;
			host.Suite(std::format(R"(return Test.Suite("Many",function() for index = 1, {} do Test.Case("case " .. index, function() end) end end))", MaxScriptTestCasesPerSuite));
			auto inventory = FeatureTestRunner::Discover(host, TestSuiteSettings::Mode::Editor);
			REQUIRE_MESSAGE(inventory, (inventory ? std::string() : inventory.error().ToString()));
			REQUIRE(inventory->Suites.size() == 1);
			CHECK(inventory->Suites[0].Cases.size() == MaxScriptTestCasesPerSuite);
			RunnerHost tooMany;
			tooMany.Suite(std::format(R"(return Test.Suite("TooMany",function() for index = 1, {} do Test.Case("case " .. index, function() end) end end))", MaxScriptTestCasesPerSuite + 1));
			const Test::ExpectLog expected(LogLevel::Error, std::format("at most {} cases", MaxScriptTestCasesPerSuite));
			const auto refused = FeatureTestRunner::Discover(tooMany, TestSuiteSettings::Mode::Editor);
			REQUIRE_FALSE(refused);
			CHECK(refused.error().ToString().find(std::format("at most {} cases", MaxScriptTestCasesPerSuite)) != std::string::npos);
		}

		TEST_CASE("TestRunner: Test.Inject queues a bounded number of events per input tick")
		{
			RunnerHost host;
			host.Suite(std::format(R"(return Test.Suite("Inputs",function() Test.Case("floods",function()
				for index = 1, {0} do Test.InjectAction("Jump", "Down") end
				local ok, message = pcall(Test.InjectAction, "Jump", "Down")
				Test.Expect(not ok, "the event past the cap was queued")
				Test.Expect(type(message) == "string" and string.find(message, "at most {0} events per input tick", 1, true) ~= nil, tostring(message))
				Test.WaitTicks(1)
				Test.InjectAction("Jump", "Up")
				Test.WaitTicks(1)
				Test.Expect(not Input.IsActionDown("Jump"), "the next tick's event was not applied")
			end) end))",
				MaxScriptInjectedInputsPerTick));
			auto result = host.Run();
			REQUIRE(result.Cases.size() == 1);
			CAPTURE(result.Cases[0].Message);
			CHECK(result.Cases[0].Status == "passed");
			CHECK(result.Passed);
		}

		TEST_CASE("TestRunner: Test.Screenshot takes a bounded number of screenshots per suite and the report is still published")
		{
			RunnerHost host;
			// Each capture may write a new file, so a loop within the watchdog budget may not write files without bound.
			host.Suite(std::format(R"(return Test.Suite("Shots",function()
				Test.Case("floods",function()
					for index = 1, {0} do Test.Screenshot("shot" .. index) end
					for _ = 1, 10 do
						local ok, message = pcall(function() Test.Screenshot("refused") end)
						Test.Expect(not ok, "the screenshot past the cap was taken")
						Test.Expect(type(message) == "string" and string.find(message, "at most {0} screenshots per suite", 1, true) ~= nil, tostring(message))
					end
				end)
				Test.Case("unprotected",function()
					Test.Screenshot("later")
				end)
			end))",
				MaxScriptTestScreenshotsPerSuite));
			// The next suite starts with a new allowance.
			host.Suite(R"(return Test.Suite("Again",function() Test.Case("one",function() Test.Screenshot("again") end) end))");
			const Test::ExpectLog refusal(LogLevel::Error, std::format("at most {} screenshots per suite", MaxScriptTestScreenshotsPerSuite));
			auto result = host.Run();
			REQUIRE(result.Cases.size() == 3);
			CAPTURE(result.Cases[0].Message);
			CHECK(result.Cases[0].Status == "passed");
			// Unprotected, the refusal fails its case with a located error; the run goes on and its report is complete.
			CHECK(result.Cases[1].Status == "error");
			CHECK(result.Cases[1].Message.find(std::format("at most {} screenshots per suite", MaxScriptTestScreenshotsPerSuite)) != std::string::npos);
			CHECK(result.Cases[1].File == "Assets/Tests/Suite0.luau");
			CHECK(result.Cases[1].Line == 11);
			CHECK(result.Cases[2].Status == "passed");
			CHECK_FALSE(result.Passed);
			REQUIRE(result.Suites.size() == 2);
			CHECK_FALSE(result.Suites[0].Passed);
			CHECK(result.Suites[1].Passed);
			// Refused calls never reach the host.
			REQUIRE(host.Screenshots.size() == MaxScriptTestScreenshotsPerSuite + 1);
			CHECK(host.Screenshots.front() == "shot1");
			CHECK(host.Screenshots[MaxScriptTestScreenshotsPerSuite - 1] == std::format("shot{}", MaxScriptTestScreenshotsPerSuite));
			CHECK(host.Screenshots.back() == "again");
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
			host.InputDriven();
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
			host.InputDriven();
			host.Suite(InputSuite);
			auto result = host.Run({ .Record = true });
			CHECK(result.Passed);
			// The verification, then the input check (the recording has input events): both ordinary play.
			CHECK(host.ReplaySessions == 2);
			REQUIRE(host.Created.size() >= 2);
			for (size_t index = host.Created.size() - 2; index < host.Created.size(); ++index)
			{
				const auto& spec = host.Created[index];
				CHECK_FALSE(spec.TestMode);
				CHECK(spec.TestHost == nullptr);
				CHECK(spec.TestHook == nullptr);
				CHECK(spec.OwnsAudioTime);
			}
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
			host.InputDriven();
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
			host.InputDriven();
			host.Suite(InputSuite).Clock = { 0.0f, 2.0f / 60.0f, 5.0f / 60.0f };
			auto result = host.Run({ .Record = true });
			CHECK(result.Passed);
			CHECK(host.ReplaySessions == 2); // the verification and the input check
			CHECK(host.Candidate.FinalTick == result.Suites[0].Ticks);
		}

		// M16 review R17: a checkpoint describes a completed-tick boundary, the previous tick's frame phase included, which is
		// where ordinary playback compares it. A scripted clock that runs several steps in one frame leaves no such boundary
		// at the ticks inside the frame: a checkpoint taken there missed the frame phase (here a LateUpdate write that follows
		// the Driver), so normal playback refused a recording whose final state it reproduces. Those ticks now get none.
		TEST_CASE("TestRunner: a recording under a grouping scripted clock keeps checkpoints only at completed-tick boundaries")
		{
			RunnerHost host;
			host.Behaviour(R"(local Driver={}
function Driver.OnFixedUpdate(self)
	if Input.IsKeyDown("Space") then self.Entity.Transform:Translate(vector.create(1, 0, 0)) end
end
function Driver.OnLateUpdate(self)
	self.Entity.Transform.Scale = vector.create(1 + self.Entity.Transform.Translation.x, 1, 1)
end
return Script.Define("Driver",Driver))");
			host.Suite(R"(return Test.Suite("Grouped",function()
	Test.Case("drive across the checkpoints",function()
		Test.InjectKey("Space","Down"); Test.WaitTicks(250)
		Test.InjectKey("Space","Up"); Test.WaitTicks(10)
	end)
end))")
				.Clock = { 0.0f, 2.0f / 60.0f, 5.0f / 60.0f };
			const auto result = host.Run({ .Record = true });
			REQUIRE(result.Passed);
			CHECK(host.ReplaySessions == 2); // the verification reproduced every checkpoint, then the input check differed
			// The clock's frames run 0, 2 and 5 steps in turn, so at most two of the four multiples of 60 the run passes start
			// a frame; every one kept does, and ordinary playback reproduced it.
			const uint64_t multiples = (host.Candidate.FinalTick - 1) / ReplayCheckpointInterval;
			CHECK(multiples >= 4);
			CHECK(host.Candidate.Checkpoints.size() < multiples);
			for (const ReplayCheckpoint& checkpoint : host.Candidate.Checkpoints)
				CHECK(checkpoint.Tick % ReplayCheckpointInterval == 0);
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
			host.InputDriven();
			host.Suite(R"(return Test.Suite("Segments",function()
				Test.Case("a",function() Test.Fail("not selected") end)
				Test.Case("b",function() Test.InjectKey("B","Tap"); Test.WaitTicks(2) end)
			end))")
				.Isolation = TestSuiteSettings::IsolationMode::Case;
			auto result = host.Run({ .Filter = "Segments/b", .Record = true });
			CHECK(result.Passed);
			REQUIRE(result.Cases.size() == 1);
			REQUIRE_FALSE(host.Candidate.Events.empty());
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

		TEST_CASE("TestRunner: a replay that quits early or cannot load fails its own rows and the run continues")
		{
			RunnerHost host;
			host.Behaviour(R"(local Driver={}
function Driver.OnFixedUpdate(self) if Scene.GetLoadParameters().quit and Time.GetTick() == 2 then Application.Quit(5) end end
return Script.Define("Driver",Driver))");
			ReplayHeader header;
			header.Scene = { UUID(401), "Assets/Scenes/Start.scene" };
			header.Seed = host.Settings.Simulation.Seed;
			header.EngineVersion = "1";
			header.Config = "Debug";
			auto reference = host.CreateReplaySession(header, TestSuiteSettings::Mode::Editor);
			REQUIRE(reference);
			for (uint32_t tick = 0; tick < 6; ++tick)
				(*reference)->Tick();
			auto good = CreateRef<ReplayData>();
			good->Header = header;
			good->FinalTick = 6;
			good->FinalStateHash = UUID((*reference)->ComputeStateHash()).ToString();
			reference->reset();
			auto quits = CreateRef<ReplayData>();
			quits->Header = header;
			quits->Header.Parameters.Set(Json{ { "quit", true } });
			quits->FinalTick = good->FinalTick;
			quits->FinalStateHash = good->FinalStateHash;
			host.ReplayAssets["Assets/A.replay"] = quits;
			host.ReplayAssets["Assets/C.replay"] = good;
			host.ReplayGlobs = { "Assets/A.replay", "Assets/B.replay", "Assets/C.replay" }; // B is missing
			const auto result = host.Run();
			CHECK_FALSE(result.Passed);
			REQUIRE(result.Suites.size() == 3);
			CHECK(result.Suites[0].Suite == "replay:Assets/A.replay");
			CHECK_FALSE(result.Suites[0].Passed);
			CHECK(result.Suites[0].Ticks == 3);
			CHECK(result.Suites[1].Suite == "replay:Assets/B.replay");
			CHECK_FALSE(result.Suites[1].Passed);
			CHECK(result.Suites[2].Suite == "replay:Assets/C.replay");
			CHECK(result.Suites[2].Passed);
			const auto row = [&result](std::string_view suite, std::string_view name) -> const TestCaseResult*
			{
				const auto found = std::ranges::find_if(result.Cases, [suite, name](const TestCaseResult& item)
				{
					return item.Suite == suite && item.Case == name;
				});
				return found == result.Cases.end() ? nullptr : &*found;
			};
			const TestCaseResult* stopped = row("replay:Assets/A.replay", "<stopped>");
			REQUIRE(stopped != nullptr);
			CHECK(stopped->Status == "error");
			CHECK(stopped->Message.find("quit with exit code 5 at tick 3") != std::string::npos);
			CHECK(row("replay:Assets/A.replay", "FinalStateHash") == nullptr);
			const TestCaseResult* missing = row("replay:Assets/B.replay", "<replay>");
			REQUIRE(missing != nullptr);
			CHECK(missing->Status == "error");
			CHECK(missing->Message.find("missing replay") != std::string::npos);
			CHECK(row("replay:Assets/C.replay", "FinalStateHash") != nullptr);
			CHECK(host.PlayedReplays == std::vector<std::string>{ "Assets/A.replay", "Assets/B.replay", "Assets/C.replay" });
		}

		TEST_CASE("TestRunner: a script test screenshot extracts the view with the project's rendering quality")
		{
			// The Editor and Runtime test hosts both build their capture through this request (ADR 0017: every capture path
			// observes ProjectSettings.Rendering, like the game view and viewport.screenshot).
			RunnerHost host;
			host.Settings.Rendering.ShadowMapSize = 512;
			host.Settings.Rendering.SsaoHalfResolution = true;
			ReplayHeader header;
			header.Scene.Handle = UUID(401);
			header.Seed = host.Settings.Simulation.Seed;
			auto session = host.CreateReplaySession(header, TestSuiteSettings::Mode::Editor);
			REQUIRE(session);
			const RenderExtractionRequest request = MakeTestScreenshotRequest(**session, 320, 180);
			CHECK(request.Camera == RenderCameraSource::Primary);
			CHECK(request.Width == 320);
			CHECK(request.Height == 180);
			CHECK(request.Quality.ShadowMapSize == 512);
			CHECK(request.Quality.SsaoHalfResolution);
			const auto snapshot = (*session)->ExtractView(request);
			REQUIRE(snapshot);
			CHECK(snapshot->Quality.ShadowMapSize == 512);
			CHECK(snapshot->Quality.SsaoHalfResolution);
		}

		TEST_CASE("TestRunner: a suite that must not quit fails on Application.Quit(-1)")
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("NoQuit",function() Test.Case("quits",function() Application.Quit(-1) end) end))");
			const auto result = host.Run();
			CHECK_FALSE(result.Passed);
			REQUIRE(result.Cases.size() == 1);
			CHECK(result.Cases[0].Status == "quit");
			CHECK(result.Cases[0].QuitCode == -1);
			CHECK_FALSE(result.Cases[0].QuitExpected);
			REQUIRE(result.Suites.size() == 1);
			CHECK_FALSE(result.Suites[0].Passed);
		}

		TEST_CASE("TestRunner: script control characters and invalid UTF-8 keep every report format writable")
		{
			TypeRegistry registry;
			RegisterProjectSettingsTypes(registry);
			RegisterTestResultTypes(registry);
			registry.Freeze();
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Bytes",function()
				Test.Case("fails" .. string.char(7),function() Test.Fail("bad byte " .. string.char(255)) end)
				Test.Case("raises",function() error("\27[31mred") end)
			end))");
			Test::ExpectLog expected(LogLevel::Error, "red");
			const auto result = host.Run();
			CHECK_FALSE(result.Passed);
			REQUIRE(result.Cases.size() == 2);
			CHECK(result.Cases[0].Case == "fails\\u0007");
			CHECK(result.Cases[0].Message == "bad byte \xEF\xBF\xBD");
			CHECK(result.Cases[1].Message.find("\\u001B[31mred") != std::string::npos);
			const auto junit = TestRunResultToJUnit(result);
			CHECK_MESSAGE(junit, (junit ? "" : junit.error().ToString()));
			const auto json = TestRunResultToJson(result, registry);
			REQUIRE(json);
			const auto text = JsonWriter::Write(*json);
			CHECK_MESSAGE(text, (text ? "" : text.error().ToString()));
		}

		TEST_CASE("TestRunner: the tick limit after a finished case keeps its result and reports the cases left unrun")
		{
			// A ScriptedClock frame of five steps: the case returns at tick 6 and the limit of 8 stops the frame afterwards.
			RunnerHost host;
			host.Settings.Testing.SuiteTickLimit = 8;
			host.Suite(R"(return Test.Suite("Limit",function() Test.Case("waits",function() Test.WaitTicks(6) end) end))").Clock = { 5.0f / 60.0f };
			auto result = host.Run();
			CHECK(result.Passed);
			REQUIRE(result.Cases.size() == 1);
			CHECK(result.Cases[0].Status == "passed");
			CHECK(result.Cases[0].Failures.empty());
			CHECK(result.Suites[0].Ticks == 8);
			// With a case still selected, the limit is reported once as a suite row; the passed case keeps its status.
			RunnerHost remaining;
			remaining.Settings.Testing.SuiteTickLimit = 8;
			remaining.Suite(R"(return Test.Suite("Limit",function()
				Test.Case("waits",function() Test.WaitTicks(6) end)
				Test.Case("never",function() Test.Fail("must not run") end)
			end))")
				.Clock = { 5.0f / 60.0f };
			result = remaining.Run();
			CHECK_FALSE(result.Passed);
			REQUIRE(result.Cases.size() == 2);
			CHECK(result.Cases[0].Status == "passed");
			CHECK(result.Cases[1].Case == "<tick limit>");
			CHECK(result.Cases[1].Status == "timeout");
			CHECK(result.Cases[1].Message.find("1 more selected case") != std::string::npos);
		}

		TEST_CASE("TestRunner: a recorded suite starts from its parameters in the replay header form")
		{
			RunnerHost host;
			auto& suite = host.Suite(R"(return Test.Suite("Params",function()
				Test.Case("reads",function() Test.ExpectEqual(tostring(Scene.GetLoadParameters().ratio), "0.12345679"); Test.WaitTicks(1) end)
			end))");
			suite.Parameters.Set(Json{ { "ratio", 0.123456789 } });
			auto result = host.Run({ .Record = true });
			CHECK(result.Passed);
			const auto stored = CanonicalizeReplayParameters(Json{ { "ratio", 0.123456789 } });
			REQUIRE(stored);
			CHECK(host.Candidate.Header.Parameters.Get() == *stored);
			// A float the replay format cannot store fails before anything runs, located at the suite's parameter.
			host.Settings.Testing.Suites.back().Parameters.Set(Json{ { "big", 1e39 } });
			FeatureTestRunner runner;
			const auto begun = runner.Begin(host, { .Record = true });
			REQUIRE_FALSE(begun);
			CHECK(begun.error().GetLocation().JsonPointer == std::optional<std::string>("/Testing/Suites/0/Parameters/big"));
		}

		// M16 friction S2-01 (FX-4): Breakout's autopilot ended with a restart in place, so the recorded final state was the
		// idle state an input-free run reaches too, and the replay proved nothing about the gameplay it recorded. The
		// recording now gets an input check: an input-free run that reproduces every checkpoint and the final hash refuses
		// it; a checkpoint taken while the played state was still in the scene, or an ending that keeps it, lets it through.
		TEST_CASE("TestRunner: recording refuses a replay whose final state hash equals that of the same session without input")
		{
			RunnerHost host;
			host.InputDriven();
			// Space held for 10 ticks moves the Driver; the R press at tick 15 puts it back, and the run ends idle at tick 30,
			// before any checkpoint.
			host.Suite(R"(return Test.Suite("Autopilot",function()
	Test.Case("play then restart",function()
		Test.InjectKey("Space","Down"); Test.WaitTicks(10)
		Test.InjectKey("Space","Up"); Test.WaitTicks(4)
		Test.InjectKey("R","Tap"); Test.WaitTicks(15)
	end)
end))");
			FeatureTestRunner runner;
			REQUIRE(runner.Begin(host, { .Record = true }));
			const std::optional<Error> refused = host.RunToEnd(runner);
			REQUIRE(refused.has_value());
			CHECK(refused->GetCode() == ErrorCode::Validation);
			CHECK(refused->GetMessageText().find("does not depend on its input") != std::string::npos);
			// M16 friction S3-01: the hint names the input check's real rule (no checkpoint and no ending showed the input),
			// not a trailing restart as such, which a checkpoint that saw the play lets through (below).
			CHECK(refused->GetHint().find("restart") != std::string::npos);
			CHECK(refused->GetHint().find(std::format("never shows in the world at a checkpoint (one every {} ticks) or at its end", ReplayCheckpointInterval))
				!= std::string::npos);
			CHECK_FALSE(runner.TakeRecording());
			CHECK(host.ReplaySessions == 2); // the verification passed, then the input check reproduced it
			const auto result = runner.GetResult();
			REQUIRE(result);
			CHECK_FALSE(result->Passed);
			REQUIRE(result->Cases.size() == 1);
			CHECK(result->Cases[0].Status == "passed");
			CHECK(host.Active == nullptr);

			// The same play that ends while the moved Driver is still in the scene depends on its input.
			RunnerHost kept;
			kept.InputDriven();
			kept.Suite(R"(return Test.Suite("Autopilot",function()
	Test.Case("play",function()
		Test.InjectKey("Space","Down"); Test.WaitTicks(10)
		Test.InjectKey("Space","Up"); Test.WaitTicks(19)
	end)
end))");
			CHECK(kept.Run({ .Record = true }).Passed);
			CHECK(kept.Candidate.Checkpoints.empty());

			// A restart at the end no longer hides play that a checkpoint saw: Space held across tick 60, R at tick 80.
			RunnerHost checkpointed;
			checkpointed.InputDriven();
			checkpointed.Suite(R"(return Test.Suite("Autopilot",function()
	Test.Case("play across a checkpoint then restart",function()
		Test.InjectKey("Space","Down"); Test.WaitTicks(70)
		Test.InjectKey("Space","Up"); Test.WaitTicks(9)
		Test.InjectKey("R","Tap"); Test.WaitTicks(50)
	end)
end))");
			const auto passed = checkpointed.Run({ .Record = true });
			CHECK(passed.Passed);
			REQUIRE(checkpointed.Candidate.Checkpoints.size() == 2);
			CHECK(checkpointed.Candidate.Checkpoints[0].Tick == 60);
			CHECK(checkpointed.Candidate.Checkpoints[1].Tick == 120);
			CHECK(checkpointed.Candidate.FinalTick == passed.Suites[0].Ticks);
		}

		// M16 friction S3-03: a recording's result said nothing about its input check. The runner keeps where the input-free
		// run of an accepted recording first differed: at the first checkpoint, or at the final tick of a recording without
		// one; nothing for a recording without input events or a refused one.
		TEST_CASE("TestRunner: the result of a recording names where its input check diverged")
		{
			RunnerHost checkpointed;
			checkpointed.InputDriven();
			checkpointed.Suite(R"(return Test.Suite("Autopilot",function()
	Test.Case("play across a checkpoint",function()
		Test.InjectKey("Space","Down"); Test.WaitTicks(70)
		Test.InjectKey("Space","Up"); Test.WaitTicks(9)
	end)
end))");
			CHECK(checkpointed.Run({ .Record = true }).Passed);
			REQUIRE(checkpointed.Candidate.Checkpoints.size() == 1);
			CHECK(checkpointed.InputCheckTick == std::optional<uint64_t>(60));

			RunnerHost brief;
			brief.InputDriven();
			brief.Suite(R"(return Test.Suite("Autopilot",function()
	Test.Case("play",function()
		Test.InjectKey("Space","Down"); Test.WaitTicks(10)
		Test.InjectKey("Space","Up"); Test.WaitTicks(19)
	end)
end))");
			CHECK(brief.Run({ .Record = true }).Passed);
			CHECK(brief.Candidate.Checkpoints.empty());
			CHECK(brief.InputCheckTick == std::optional<uint64_t>(brief.Candidate.FinalTick));

			// No input events: nothing to check. No recording: no check either.
			RunnerHost idle;
			idle.Suite(R"(return Test.Suite("Autopilot",function()
	Test.Case("wait",function() Test.WaitTicks(5) end)
end))");
			CHECK(idle.Run({ .Record = true }).Passed);
			CHECK(idle.Candidate.Events.empty());
			CHECK_FALSE(idle.InputCheckTick.has_value());
			CHECK(checkpointed.Run().Passed);
			CHECK_FALSE(checkpointed.InputCheckTick.has_value());
		}

		// One rule decides input independence (Architecture §13.6): the input-free run passes the recording's verification,
		// whatever script errors that run raised. The Driver faults only when Space was never held, so the input check's run
		// raises a script error that the recorded play and its verification never see, while the world, which the Driver
		// never changes, reproduces the final hash.
		TEST_CASE("TestRunner: recording refuses an input-independent replay although its input-free run raised a script error")
		{
			RunnerHost host;
			host.Behaviour(R"(local Driver={}
function Driver.OnFixedUpdate(self)
	if Input.IsKeyDown("Space") then self.Pressed = true end
	if Time.GetTick() == 20 and not self.Pressed then error("idle fault") end
end
return Script.Define("Driver",Driver))");
			host.Suite(R"(return Test.Suite("Autopilot",function()
	Test.Case("press",function()
		Test.InjectKey("Space","Tap"); Test.WaitTicks(30)
	end)
end))");
			FeatureTestRunner runner;
			REQUIRE(runner.Begin(host, { .Record = true }));
			std::optional<Error> refused;
			{
				Test::ExpectLog expected(LogLevel::Error, "idle fault");
				refused = host.RunToEnd(runner);
			}
			REQUIRE(refused.has_value());
			CHECK(refused->GetCode() == ErrorCode::Validation);
			CHECK(refused->GetMessageText().find("does not depend on its input") != std::string::npos);
			CHECK_FALSE(runner.TakeRecording());
			CHECK(host.ReplaySessions == 2); // the verification passed without a script error, then the input check raised one
			const auto result = runner.GetResult();
			REQUIRE(result);
			REQUIRE(result->Cases.size() == 1);
			CHECK(result->Cases[0].Status == "passed");
		}

		TEST_CASE("TestRunner: a committed replay whose checkpoint differs fails with a located Checkpoint row")
		{
			RunnerHost host;
			ReplayHeader header;
			header.Scene = { UUID(401), "Assets/Scenes/Start.scene" };
			header.Seed = host.Settings.Simulation.Seed;
			header.EngineVersion = "1";
			header.Config = "Debug";
			auto reference = host.CreateReplaySession(header, TestSuiteSettings::Mode::Editor);
			REQUIRE(reference);
			auto replay = CreateRef<ReplayData>();
			replay->Header = header;
			replay->FinalTick = 130;
			for (uint64_t tick = 1; tick <= replay->FinalTick; ++tick)
			{
				(*reference)->Tick();
				if (tick % ReplayCheckpointInterval == 0)
					replay->Checkpoints.push_back({ .Tick = tick, .StateHash = UUID((*reference)->ComputeStateHash()).ToString() });
			}
			replay->FinalStateHash = UUID((*reference)->ComputeStateHash()).ToString();
			reference->reset();
			REQUIRE(replay->Checkpoints.size() == 2);
			const std::string recorded = replay->Checkpoints[0].StateHash;
			host.ReplayAssets["Assets/Good.replay"] = replay;
			host.ReplayGlobs = { "Assets/Good.replay" };
			const auto matched = host.Run();
			CHECK(matched.Passed);

			auto tampered = CreateRef<ReplayData>();
			tampered->Header = replay->Header;
			tampered->Checkpoints = replay->Checkpoints;
			tampered->FinalTick = replay->FinalTick;
			tampered->FinalStateHash = replay->FinalStateHash;
			tampered->Checkpoints[0].StateHash = recorded == "0000000000000000" ? "0000000000000001" : "0000000000000000";
			host.ReplayAssets["Assets/Good.replay"] = tampered;
			const auto result = host.Run();
			CHECK_FALSE(result.Passed);
			REQUIRE(result.Suites.size() == 1);
			CHECK_FALSE(result.Suites[0].Passed);
			CHECK(result.Suites[0].Ticks == 60);
			CHECK(result.Suites[0].FinalStateHash == recorded);
			REQUIRE(result.Cases.size() == 1);
			const TestCaseResult& row = result.Cases[0];
			CHECK(row.Suite == "replay:Assets/Good.replay");
			CHECK(row.Case == "Checkpoint@60");
			CHECK(row.Status == "failed");
			CHECK(row.Message.find(recorded) != std::string::npos);
			CHECK(row.Message.find(tampered->Checkpoints[0].StateHash) != std::string::npos);
			CHECK(row.File == "Assets/Good.replay");
			CHECK(row.JsonPointer == "/Checkpoints/0/StateHash");
			// M16 friction S3-04: the suite entry counts the checkpoints compared and names the one that differed.
			CHECK(matched.Suites[0].CheckpointsChecked == 2);
			CHECK(matched.Suites[0].FailedCheckpointTick == 0);
			CHECK(result.Suites[0].CheckpointsChecked == 1);
			CHECK(result.Suites[0].FailedCheckpointTick == 60);
		}

		// Docs/Decisions/0027-m17-contract.md decision 3, A3 (M17 map GTF-07): a recording refused because a selected row did
		// not pass said only "recording requires passing selected cases and a valid starting scene", so a recording filter
		// that also selected a stale replay's path left the builder guessing. The refusal names each selected row that did
		// not pass, with its status, in report order; the rows that passed are not named.
		TEST_CASE("TestRunner: a refused recording names the selected rows that did not pass" * doctest::skip(true))
		{
			RunnerHost host;
			host.InputDriven();
			host.Suite(R"(return Test.Suite("Pilot", function()
				Test.Case("drive", function()
					Test.InjectKey("Space", "Tap")
					Test.WaitTicks(3)
				end)
				Test.Case("check", function()
					Test.Expect(false, "deliberate")
				end)
			end))");
			ReplayHeader header;
			header.Scene = { UUID(401), "Assets/Scenes/Start.scene" };
			header.Seed = host.Settings.Simulation.Seed;
			header.EngineVersion = "1";
			header.Config = "Debug";
			auto stale = CreateRef<ReplayData>();
			stale->Header = header;
			stale->FinalTick = 6;
			stale->FinalStateHash = "0000000000000001";
			host.ReplayAssets["Assets/Tests/Replays/Pilot.replay"] = stale;
			host.ReplayGlobs = { "Assets/Tests/Replays/Pilot.replay" };

			FeatureTestRunner runner;
			REQUIRE(runner.Begin(host, { .Record = true }));
			const std::optional<Error> refused = host.RunToEnd(runner);
			REQUIRE(refused.has_value());
			CHECK(refused->GetCode() == ErrorCode::Validation);
			const std::string message = refused->GetMessageText();
			CAPTURE(message);
			CHECK(message.starts_with("recording requires passing selected cases and a valid starting scene; 2 selected row(s) did not pass: "));
			CHECK(message.find("Pilot/check (failed)") != std::string::npos);
			CHECK(message.find("replay:Assets/Tests/Replays/Pilot.replay/FinalStateHash (failed)") != std::string::npos);
			CHECK(message.find("Pilot/check (failed), replay:Assets/Tests/Replays/Pilot.replay/FinalStateHash (failed)") != std::string::npos);
			CHECK(message.find("Pilot/drive") == std::string::npos);
			// A replay row among them: the hint says how a recording filter leaves the replays out.
			CHECK(refused->GetHint().find("Record.") != std::string::npos);
			CHECK_FALSE(runner.TakeRecording());
			// The rows the message names are the report's rows that did not pass, in report order.
			const auto result = runner.GetResult();
			REQUIRE(result);
			CHECK_FALSE(result->Passed);
			REQUIRE(result->Cases.size() == 3);
			CHECK(result->Cases[0].Suite + "/" + result->Cases[0].Case + " (" + result->Cases[0].Status + ")" == "Pilot/drive (passed)");
			CHECK(result->Cases[1].Suite + "/" + result->Cases[1].Case + " (" + result->Cases[1].Status + ")" == "Pilot/check (failed)");
			CHECK(result->Cases[2].Suite + "/" + result->Cases[2].Case + " (" + result->Cases[2].Status + ")"
				== "replay:Assets/Tests/Replays/Pilot.replay/FinalStateHash (failed)");
		}

		// Docs/Decisions/0027-m17-contract.md decision 3, A2: test.run refuses, before it takes its lease, a dirty open scene
		// that a selected suite or replay plays, so the runner reports what Begin selected: each selected suite's index into
		// Testing.Suites (a suite of the run's mode with a case the filter matches) and each replay the filter matches. The
		// selection is kept until the next Begin.
		TEST_CASE("TestRunner: Begin's selection names each selected suite and replay" * doctest::skip(true))
		{
			RunnerHost host;
			host.Suite(R"(return Test.Suite("Pilot",function() Test.Case("drive",function() end) end))");
			host.Suite(R"(return Test.Suite("Exported",function() Test.Case("pilot",function() end) end))").Modes = { TestSuiteSettings::Mode::Release };
			host.Suite(R"(return Test.Suite("Board",function() Test.Case("clears",function() end); Test.Case("autopilot",function() end) end))");
			host.Suite(R"(return Test.Suite("Other",function() Test.Case("runs",function() end) end))");
			auto replay = CreateRef<ReplayData>();
			replay->Header.Scene = { UUID(401), "Assets/Scenes/Start.scene" };
			replay->Header.Seed = host.Settings.Simulation.Seed;
			replay->Header.EngineVersion = "1";
			replay->Header.Config = "Debug";
			host.ReplayAssets["Assets/Tests/Replays/Other.replay"] = replay;
			host.ReplayAssets["Assets/Tests/Replays/Pilot.replay"] = replay;
			host.ReplayGlobs = { "Assets/Tests/Replays/Pilot.replay", "Assets/Tests/Replays/Other.replay" };

			FeatureTestRunner runner;
			const auto before = runner.GetSelection();
			REQUIRE_FALSE(before.has_value());
			CHECK(before.error().GetCode() == ErrorCode::InvalidState);

			// "pilot" matches Pilot/drive, Board/autopilot and the Pilot replay; the Release-only suite is not of the run's mode.
			REQUIRE(runner.Begin(host, { .Filter = "pilot" }));
			const auto filtered = runner.GetSelection();
			REQUIRE_MESSAGE(filtered.has_value(), (filtered ? std::string() : filtered.error().ToString()));
			CHECK(filtered->Suites == std::vector<size_t>{ 0, 2 });
			CHECK(filtered->Replays == std::vector<std::string>{ "Assets/Tests/Replays/Pilot.replay" });
			runner.Cancel();
			const auto kept = runner.GetSelection();
			REQUIRE(kept.has_value());
			CHECK(kept->Suites == std::vector<size_t>{ 0, 2 });

			// The next Begin replaces it: an empty filter selects every suite of the mode and every replay, in sorted order.
			REQUIRE(runner.Begin(host, {}));
			const auto everything = runner.GetSelection();
			REQUIRE(everything.has_value());
			CHECK(everything->Suites == std::vector<size_t>{ 0, 2, 3 });
			CHECK(everything->Replays == std::vector<std::string>{ "Assets/Tests/Replays/Other.replay", "Assets/Tests/Replays/Pilot.replay" });
			runner.Cancel();
		}
	}

}
