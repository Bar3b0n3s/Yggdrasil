#include "EditorPCH.h"
#include "EditorCore/Automation/TestMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Play/EditorFeatureTestHost.h"
#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/Asset/ReplayData.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/ScriptCompiler.h"
#include "Engine/Session/ReplayPlayer.h"

#include <nlohmann/json.hpp>

#include <format>
#include <optional>
#include <string>
#include <vector>

namespace Engine {

	namespace {

		// test.run {record, expect}'s expectations (Docs/Decisions/0030-m19-contract.md decision 6, A-RECEXPECT): the sources the
		// recording carries and their compiled forms, which the runner evaluates (it never compiles: Dist has no compiler).
		struct RecordExpectations
		{
			std::vector<ReplayExpectation> Sources{};
			std::vector<ReplayBytecodeExpectation> Bytecode{};
		};

		// Checks and compiles test.run's expect, as input.record's Stop does its own: record is required (InvalidArgument at
		// /expect), the ticks are ordered (at /expect/<index>/tick), and each source compiles as an expression or chunk named
		// "=test.run", located at /expect/<index>/luau. A tick after the recording's final tick is known only once the suite
		// ran, so the runner refuses it (FeatureTestRunOptions::RecordExpect).
		Result<RecordExpectations> PrepareRecordExpectations(const TestRunParams& params, bool recording)
		{
			if (!recording)
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/expect", "expect requires record", "pass record with the replay destination the expectations are written into"));
			RecordExpectations prepared;
			for (size_t index = 0; index < params.Expect.size(); ++index)
			{
				const InputRecordExpectation& expectation = params.Expect[index];
				if (index != 0 && expectation.Tick < params.Expect[index - 1].Tick)
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, std::format("/expect/{}/tick", index), "expectation ticks must be ordered", {}));
				ENGINE_TRY_ASSIGN(ScriptCompilation compiled, ScriptCompiler::Compile({ .Source = expectation.Luau, .Mode = ScriptCompileMode::ExpressionOrChunk, .ChunkName = "=test.run", .JsonPointer = std::format("/expect/{}/luau", index) }));
				prepared.Sources.push_back({ .Tick = expectation.Tick, .Luau = expectation.Luau });
				ReplayBytecodeExpectation bytecode;
				bytecode.Tick = expectation.Tick;
				bytecode.Script.Bytecode = std::move(compiled.Bytecode);
				bytecode.Script.SourceMap = std::move(compiled.SourceMap);
				prepared.Bytecode.push_back(std::move(bytecode));
			}
			return prepared;
		}

		// The host owns the editor lease. Keep it alive until the runner has unpublished and destroyed its sessions,
		// including failure and disconnect paths that will not be polled again.
		class TestRunOperation final : public PendingOperation
		{
		public:
			// `reportCoverage`: the result keeps Coverage.Counters (TestRunParams::Coverage or Testing.CoverageGates).
			TestRunOperation(Scope<IFeatureTestHost> host, std::string record, bool reportCoverage)
				: m_Host(std::move(host)), m_Record(std::move(record)), m_ReportCoverage(reportCoverage)
			{
			}
			~TestRunOperation() override
			{
				if (!m_Released)
					m_Runner.Cancel();
			}

			[[nodiscard]] Status Begin(const TestRunParams& params, RecordExpectations expectations)
			{
				return m_Runner.Begin(*m_Host, { .Mode = TestSuiteSettings::Mode::Editor, .Filter = params.Filter, .TimeoutTicks = params.TimeoutTicks, .Record = !m_Record.empty(), .RecordExpect = std::move(expectations.Sources), .RecordExpectBytecode = std::move(expectations.Bytecode) });
			}

			// What the successful Begin selected (FeatureTestRunner::GetSelection).
			[[nodiscard]] Result<FeatureTestSelection> GetSelection() const { return m_Runner.GetSelection(); }

			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext& context) override
			{
				if (m_Released)
					return MakeError(ErrorCode::Cancelled, "the test run was cancelled");
				auto& host = static_cast<EditorMethodContext&>(context);
				const auto start = host.GetWallClockTime();
				do
				{
					const Result<bool> advanced = m_Runner.Advance();
					if (!advanced)
					{
						// Recording verification can fail after the cases finished. Preserve those cases and their report
						// files, while returning the verification error and never transferring the candidate recording. A
						// recording's failure (verification, the input check) names the replay it did not write.
						const Error failure = m_Record.empty() ? advanced.error() : Error(advanced.error()).WithContext(std::format("while recording '{}'; nothing was written", m_Record));
						if (const auto result = m_Runner.GetResult(); result)
						{
							Cancel(context);
							auto report = PublishReports(host, *result, m_ReportCoverage);
							if (report)
								context.SetErrorData("testRun", std::move(*report));
							else
							{
								Cancel(context);
								return std::unexpected(std::move(report).error().WithContext(failure.ToString()));
							}
						}
						Cancel(context);
						return std::unexpected(failure);
					}
					if (*advanced)
					{
						auto result = Finish(host);
						Cancel(context);
						return result;
					}
				} while (host.GetWallClockTime() - start < TestRunFrameBudget);
				return std::nullopt;
			}

			void Cancel(MethodContext& /*context*/) override
			{
				if (m_Released)
					return;
				m_Released = true;
				m_Runner.Cancel();
				m_Host.reset();
			}

			[[nodiscard]] std::string GetPhase() const override
			{
				return m_Released ? "Test:complete" : m_Runner.GetPhase();
			}
		private:
			[[nodiscard]] Result<Json> Finish(EditorMethodContext& context)
			{
				ENGINE_TRY_ASSIGN(TestRunResult result, m_Runner.GetResult());
				std::optional<ReplayDocument> recording;
				if (!m_Record.empty())
				{
					ENGINE_TRY_ASSIGN(recording, m_Runner.TakeRecording());
				}
				const std::optional<uint64_t> inputCheckTick = m_Runner.GetInputCheckTick();
				const std::optional<uint64_t> finalHashIndependentTick = m_Runner.GetFinalHashIndependentTick();
				// The simulation is terminal. Restore the editor before publishing through its guarded write path;
				// this remains one invocation, with the original request's attribution and no interleaved frame.
				Cancel(context);
				if (recording)
				{
					ENGINE_TRY_ASSIGN(result.RecordingPath, context.WriteReplay(m_Record, *recording));
					ENGINE_TRY_ASSIGN(result.Recording, MakeRecordingBlock(context, result.RecordingPath, *recording, inputCheckTick, finalHashIndependentTick));
				}
				return PublishReports(context, std::move(result), m_ReportCoverage);
			}

			// The result's recording block (TestRunResult::Recording, M16 friction S3-03): what the written replay holds, its
			// expectations included (Docs/Decisions/0030-m19-contract.md decision 6), where its input check diverged, and the
			// warning of an input check that reproduced the final hash only (decision 3, `finalHashIndependentTick`).
			[[nodiscard]] static Result<VariantValue> MakeRecordingBlock(EditorMethodContext& context, const std::string& path, const ReplayDocument& recording,
				std::optional<uint64_t> inputCheckTick, std::optional<uint64_t> finalHashIndependentTick)
			{
				std::vector<std::string> warnings;
				if (finalHashIndependentTick.has_value())
					warnings.push_back(MakeFinalHashIndependentWarning(path, *finalHashIndependentTick, recording.FinalStateHash));
				const TestRecordingResult block{
					.Path = path,
					.FormatVersion = recording.Version,
					.Ticks = ToAutomationCounter(recording.FinalTick),
					.Events = ToAutomationCounter(recording.Events.size()),
					.Checkpoints = ToAutomationCounter(recording.Checkpoints.size()),
					.FinalStateHash = recording.FinalStateHash,
					.InputCheck = { .Ran = inputCheckTick.has_value(), .DivergedAtTick = ToAutomationCounter(inputCheckTick.value_or(0)) },
					.Expectations = ToAutomationCounter(recording.Expect.size()),
					.Warnings = std::move(warnings),
				};
				ENGINE_TRY_ASSIGN(Json serialized, context.SerializeResult(block));
				return VariantValue(std::move(serialized));
			}

			// Writes the JUnit and JSON reports (the JSON report always holds the coverage counters) and returns the RPC
			// result, which leaves Coverage.Counters out unless `reportCoverage`: without the counters (several hundred
			// script API entries) an ordinary run's result stays below the offload threshold.
			[[nodiscard]] static Result<Json> PublishReports(EditorMethodContext& context, TestRunResult result, bool reportCoverage)
			{
				ENGINE_TRY_ASSIGN(const std::string junit, TestRunResultToJUnit(result));
				ENGINE_TRY_ASSIGN(result.JunitPath, context.WriteOutputFile("xml", AsBytes(junit)));
				ENGINE_TRY_ASSIGN(const Json report, context.SerializeResult(result));
				ENGINE_TRY_ASSIGN(const std::string text, JsonWriter::Write(report));
				ENGINE_TRY_ASSIGN(result.JsonPath, context.WriteOutputFile("json", AsBytes(text)));
				if (!reportCoverage)
					result.Coverage.Counters.clear();
				return context.SerializeResult(result);
			}
		private:
			Scope<IFeatureTestHost> m_Host{};
			FeatureTestRunner m_Runner{};
			std::string m_Record{};
			bool m_ReportCoverage = false;
			bool m_Released = false;
		};

		// The declared name of the selected suite `settings` (its script's Test.Suite name), for messages; its script path
		// when the script does not load as a suite.
		[[nodiscard]] std::string DescribeSuite(EditorContext& editor, EditorFeatureTestHost& host, const TestSuiteSettings& settings)
		{
			const Result<AssetHandle> handle = host.ResolveTestScript(settings.Script);
			if (!handle)
				return settings.Script;
			const Result<AssetRef<Asset>> loaded = editor.GetAssets().Load(*handle);
			const AssetRef<ScriptData> script = loaded ? AssetCast<ScriptData>(*loaded) : AssetRef<ScriptData>();
			return script ? script->Name : settings.Script;
		}

		// Docs/Decisions/0027-m17-contract.md "Frozen interfaces / A2": a test run plays the saved file of every scene it uses,
		// so it refuses an open scene with unsaved changes that a selected suite is bound to (its Scene resolved as play.start
		// resolves it) or that a selected replay plays (the registry's source path of its scene asset, as the replay session
		// loads it). The first such selection is named: suites in Testing.Suites order, then replays in canonical path order.
		// A suite Scene that does not resolve, or a replay whose scene asset is missing, matches nothing: the run reports it.
		[[nodiscard]] Status CheckDirtyScene(EditorMethodContext& context, EditorFeatureTestHost& host, const FeatureTestSelection& selection)
		{
			EditorContext& editor = context.GetEditor();
			if (!editor.HasScene() || !editor.IsSceneDirty() || !editor.GetScenePath().has_value())
				return {};
			const VfsPath& open = *editor.GetScenePath();
			const auto refuse = [&open](std::string selected)
			{
				return std::unexpected(Error(ErrorCode::InvalidState, std::format("'{}' is open with unsaved changes, and the selected {} plays its saved file", open.GetPath(), selected))
						.WithHint("scene.save first; test runs play the saved scene file"));
			};
			const std::vector<TestSuiteSettings>& suites = host.GetProjectSettings().Testing.Suites;
			for (const size_t index : selection.Suites)
			{
				if (index >= suites.size() || suites[index].Scene.empty())
					continue;
				const Result<VfsPath> scene = context.ResolveProjectPath(suites[index].Scene, "/scene", ".scene");
				if (scene && *scene == open)
					return refuse(std::format("suite '{}'", DescribeSuite(editor, host, suites[index])));
			}
			for (const std::string& path : selection.Replays)
			{
				const Result<AssetRef<ReplayData>> replay = host.LoadReplay(path);
				if (!replay)
					continue;
				const AssetRecord* scene = editor.GetAssets().GetRegistry().Find((*replay)->Header.Scene.Handle);
				if (scene != nullptr && scene->Metadata.Type == AssetType::Scene && scene->SourcePath == open)
					return refuse(std::format("replay '{}'", path));
			}
			return {};
		}

	}

	namespace Automation {

		Result<TestListResult> TestList(EditorMethodContext& context, const NoParams& /*params*/)
		{
			EditorFeatureTestHost host(context);
			ENGINE_TRY(host.Preflight());
			return FeatureTestRunner::Discover(host, TestSuiteSettings::Mode::Editor);
		}

		Result<Scope<PendingOperation>> TestRun(EditorMethodContext& context, const TestRunParams& params)
		{
			if (context.HasParam("timeoutTicks") && params.TimeoutTicks == 0)
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/timeoutTicks", "timeoutTicks must be positive", {}));
			if (context.HasParam("record"))
			{
				if (params.Record.empty())
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/record", "record must name a replay destination", {}));
				const auto checked = context.ValidateReplayOutput(params.Record);
				if (!checked)
					return std::unexpected(Error(checked.error().GetCode(), checked.error().GetMessageText()).WithLocation({ .File = {}, .JsonPointer = "/record", .Entity = {} }));
			}
			RecordExpectations expectations;
			if (context.HasParam("expect"))
			{
				ENGINE_TRY_ASSIGN(expectations, PrepareRecordExpectations(params, !params.Record.empty()));
			}
			auto host = CreateScope<EditorFeatureTestHost>(context);
			ENGINE_TRY(host->Preflight());
			EditorFeatureTestHost* adapter = host.get();
			const bool reportCoverage = params.Coverage || context.GetEditor().GetProject().GetSettings().Testing.CoverageGates;
			auto operation = CreateScope<TestRunOperation>(std::move(host), params.Record, reportCoverage);
			// The script API baseline immediately before Begin, whose suite collection counts (ADR 0022 decision 4).
			ENGINE_TRY(adapter->CaptureCoverageBaseline());
			const Status begun = operation->Begin(params, std::move(expectations));
			if (!begun)
			{
				if (begun.error().GetCode() == ErrorCode::InvalidArgument)
					return std::unexpected(Error(ErrorCode::Validation, begun.error().GetMessageText()).WithLocation(begun.error().GetLocation()));
				return std::unexpected(begun.error());
			}
			// After every error of Preflight and Begin, before the lease and before anything plays (A2).
			ENGINE_TRY_ASSIGN(const FeatureTestSelection selection, operation->GetSelection());
			ENGINE_TRY(CheckDirtyScene(context, *adapter, selection));
			ENGINE_TRY(adapter->Acquire());
			return Scope<PendingOperation>(std::move(operation));
		}

	}

	void RegisterTestMethodTypes(TypeRegistry& registry)
	{
		RegisterTestResultTypes(registry);
		registry.Struct<TestCaseDescription>("TestCaseDescription", "One declared test case; discovery never executes its body.")
			.Field("name", &TestCaseDescription::Name, "Case name in declaration order.")
			.Field("file", &TestCaseDescription::File, "Authored source path.")
			.Field("line", &TestCaseDescription::Line, "One-based declaration line.")
			.Field("timeoutTicks", &TestCaseDescription::TimeoutTicks, "Effective simulated-tick deadline.");
		registry.Struct<TestSuiteDescription>("TestSuiteDescription", "A configured suite and its declared cases.")
			.Field("name", &TestSuiteDescription::Name, "Declared suite name.")
			.Field("script", &TestSuiteDescription::Script, "Suite source path.")
			.Field("scene", &TestSuiteDescription::Scene, "Configured scene path, or empty for an empty scene.")
			.Field("modes", &TestSuiteDescription::Modes, "Run modes in which this suite is available.")
			.Field("cases", &TestSuiteDescription::Cases, "Cases in declaration order.");
		registry.Struct<TestListResult>("TestListResult", "Suite inventory in settings order and unique sorted replay paths.")
			.Field("suites", &TestListResult::Suites, "Configured suites and their declarations.")
			.Field("replays", &TestListResult::Replays, "Replay paths expanded from the project globs.");
		registry.Struct<TestRunParams>("TestRunParams", "Run selected suites, cases and replays without altering authored scene state.")
			.Field("filter", &TestRunParams::Filter, "Case-insensitive substring of suite/case or replay path.")
			.Field("timeoutTicks", &TestRunParams::TimeoutTicks, "Optional positive run-wide tick cap; omitted means no additional limit.")
			.Field("record", &TestRunParams::Record, "Optional project Assets .replay output; requires one selected suite and strict reproduction in ordinary play (every state-hash checkpoint, one every 60 ticks, and the final hash). A recording with input events is also replayed without them and refused when that run reproduces every checkpoint and the final hash too, whatever script errors it raises: its outcome does not depend on its input. The result's recording block describes the replay written.")
			.Field("coverage", &TestRunParams::Coverage, "Return coverage.counters in the result; without it they are left out unless the project sets Testing.CoverageGates. The JSON report at jsonPath always holds them.")
			.Field("expect", &TestRunParams::Expect, "With record only: expectations ({tick, luau}, as input.record's stop takes them) written into the recorded replay; each must hold when the recording is verified with its input, before anything is written.");
	}

	void RegisterTestMethods(MethodRegistry& methods)
	{
		methods.Add({ .Name = "test.list", .Description = "Collects test suite and case names and source locations without running case bodies.", .Examples = { { .Description = "List configured tests.", .Params = Json::object() } } }, &Automation::TestList);
		methods.AddPending<EditorMethodContext, TestRunParams, TestRunMethodResult>(
			{ .Name = "test.run", .Description = "Runs configured tests and returns every outcome (counts, failedCases and the case rows; a replay's suite entry counts the checkpoints it compared) plus JSON/JUnit reports. The coverage object always keeps its registryFingerprint and gates; its counters are returned only with coverage: true or Testing.CoverageGates. Optional recording is published only after strict replay verification and, with input events, the input check (§13.6: refused when a run without them still passes that verification, whatever script errors it raises), and reported in the recording block (format version, ticks, events, checkpoints, final hash, where the input check diverged); a refused recording names the selected rows that did not pass. Runs play saved scene files, so test.run refuses (InvalidState, nothing run) while the open scene has unsaved changes and a selected suite or replay plays it: scene.save first.", .ExposeAsTool = true, .TimeoutSeconds = 900, .Examples = { { .Description = "Run every configured test.", .Params = Json::object() }, { .Description = "Run every test and return the script API coverage counters.", .Params = Json{ { "coverage", true } } } } }, &Automation::TestRun);
	}

}
