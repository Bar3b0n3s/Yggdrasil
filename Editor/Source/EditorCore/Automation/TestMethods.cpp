#include "EditorPCH.h"
#include "EditorCore/Automation/TestMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Play/EditorFeatureTestHost.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace {

		// The host owns the editor lease. Keep it alive until the runner has unpublished and destroyed its sessions,
		// including failure and disconnect paths that will not be polled again.
		class TestRunOperation final : public PendingOperation
		{
		public:
			TestRunOperation(Scope<IFeatureTestHost> host, std::string record)
				: m_Host(std::move(host)), m_Record(std::move(record))
			{
			}
			~TestRunOperation() override
			{
				if (!m_Released)
					m_Runner.Cancel();
			}

			[[nodiscard]] Status Begin(const TestRunParams& params)
			{
				return m_Runner.Begin(*m_Host, { .Mode = TestSuiteSettings::Mode::Editor, .Filter = params.Filter, .TimeoutTicks = params.TimeoutTicks, .Record = !m_Record.empty() });
			}

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
						// files, while returning the verification error and never transferring the candidate recording.
						const Error failure = advanced.error();
						if (const auto result = m_Runner.GetResult(); result)
						{
							Cancel(context);
							auto report = PublishReports(host, *result);
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
				// The simulation is terminal. Restore the editor before publishing through its guarded write path;
				// this remains one invocation, with the original request's attribution and no interleaved frame.
				Cancel(context);
				if (recording)
				{
					ENGINE_TRY_ASSIGN(result.RecordingPath, context.WriteReplay(m_Record, *recording));
				}
				return PublishReports(context, std::move(result));
			}

			[[nodiscard]] static Result<Json> PublishReports(EditorMethodContext& context, TestRunResult result)
			{
				ENGINE_TRY_ASSIGN(const std::string junit, TestRunResultToJUnit(result));
				ENGINE_TRY_ASSIGN(result.JunitPath, context.WriteOutputFile("xml", AsBytes(junit)));
				ENGINE_TRY_ASSIGN(const Json report, context.SerializeResult(result));
				ENGINE_TRY_ASSIGN(const std::string text, JsonWriter::Write(report));
				ENGINE_TRY_ASSIGN(result.JsonPath, context.WriteOutputFile("json", AsBytes(text)));
				return context.SerializeResult(result);
			}
		private:
			Scope<IFeatureTestHost> m_Host{};
			FeatureTestRunner m_Runner{};
			std::string m_Record{};
			bool m_Released = false;
		};

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
			auto host = CreateScope<EditorFeatureTestHost>(context);
			ENGINE_TRY(host->Preflight());
			EditorFeatureTestHost* adapter = host.get();
			auto operation = CreateScope<TestRunOperation>(std::move(host), params.Record);
			const Status begun = operation->Begin(params);
			if (!begun)
			{
				if (begun.error().GetCode() == ErrorCode::InvalidArgument)
					return std::unexpected(Error(ErrorCode::Validation, begun.error().GetMessageText()).WithLocation(begun.error().GetLocation()));
				return std::unexpected(begun.error());
			}
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
			.Field("record", &TestRunParams::Record, "Optional project Assets .replay output; requires one selected suite and strict reproduction.");
	}

	void RegisterTestMethods(MethodRegistry& methods)
	{
		methods.Add({ .Name = "test.list", .Description = "Collects test suite and case names and source locations without running case bodies.", .Examples = { { .Description = "List configured tests.", .Params = Json::object() } } }, &Automation::TestList);
		methods.AddPending<EditorMethodContext, TestRunParams, TestRunMethodResult>(
			{ .Name = "test.run", .Description = "Runs configured tests and returns every outcome plus JSON/JUnit reports. Optional recording is published only after strict replay verification.", .ExposeAsTool = true, .TimeoutSeconds = 900, .Examples = { { .Description = "Run every configured test.", .Params = Json::object() } } }, &Automation::TestRun);
	}

}
