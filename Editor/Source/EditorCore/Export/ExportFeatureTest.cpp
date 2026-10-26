#include "EditorPCH.h"
#include "EditorCore/Export/ExportFeatureTest.h"

#include "EditorCore/Export/Private/ExportFeatureTestReportReader.h"
#include "EditorCore/Export/Private/ProcessOutput.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Platform/Process.h"
#include "Engine/Project/GameManifest.h"

#include <chrono>
#include <filesystem>
#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		// The run's fixed arguments: a headless run of the exported suites and replays (Exporter.h step 6a), never an
		// argument a request supplies. From an editor without a graphics device, Debug and Release add --renderer none, as
		// the smoke test does.
		constexpr std::string_view HeadlessOption = "--headless";
		constexpr std::string_view FeatureTestOption = "--feature-test";
		constexpr std::string_view RendererOption = "--renderer";
		constexpr std::string_view NoRenderer = "none";
		// The bound of the run, a failure bound only: the exported suites and replays end at their own tick limits, so a
		// run still going after it hangs and is killed. Scripts/Test.py's suites use the same bound (SUITE_TIMEOUT_SECONDS).
		constexpr std::chrono::seconds RunTimeout{ 1800 };
		// The bound of the wait for the run's output once it exited or was killed (its readers reach the end of the pipes
		// once the process is gone), a failure bound only.
		constexpr std::chrono::seconds DrainTimeout{ 10 };
		// How much of the run's output an error quotes.
		constexpr size_t OutputTailLines = 20;
		constexpr std::string_view NotRunWarning = "the feature test did not run: a Dist Runtime has no --renderer none and needs a graphics device, which this "
												   "editor lacks (--renderer none, or no Vulkan device); export Release to run it without one";
		constexpr std::string_view RunHint = "run the exported executable with --headless --feature-test to see why";

	}

	namespace Utils {

		// The report's file name: a Dist Runtime reports in Dist mode, a Debug or Release one in Release mode (RuntimeApp.cpp,
		// RuntimeTestMode).
		static std::string_view GetFeatureTestReportName(ExportConfiguration configuration)
		{
			return configuration == ExportConfiguration::Dist ? "FeatureTest-Dist.json" : "FeatureTest-Release.json";
		}

	}

	namespace {

		// One step 6a: a run that started, or one that resolved at once (a Dist run without a graphics device).
		class ExportFeatureTestJob final : public IExportFeatureTestJob
		{
		public:
			// A run that does not start: it resolves with `outcome`.
			explicit ExportFeatureTestJob(ExportFeatureTestOutcome outcome)
				: m_Resolved(std::move(outcome))
			{
			}

			// The running `process`, whose report is `report` in the staged package and `movedReport` after step 7's move.
			ExportFeatureTestJob(Process process, std::filesystem::path report, std::filesystem::path movedReport)
				: m_Process(std::move(process)), m_Deadline(std::chrono::steady_clock::now() + RunTimeout), m_Report(std::move(report)), m_MovedReport(std::move(movedReport))
			{
			}

			[[nodiscard]] std::optional<Result<ExportFeatureTestOutcome>> Poll() override
			{
				if (!m_Resolved.has_value())
				{
					if (!m_Process->HasExited() && std::chrono::steady_clock::now() < m_Deadline)
						return std::nullopt;
					m_Resolved = Finish();
					m_Process.reset();
				}
				return m_Resolved;
			}
		private:
			// The outcome of the run, which exited or ran out of time.
			[[nodiscard]] Result<ExportFeatureTestOutcome> Finish()
			{
				if (!m_Process->HasExited())
				{
					ENGINE_TRY(m_Process->Kill());
					// The process is gone; the wait bounds only its output readers' drain, so the error quotes what the game printed.
					Result<ProcessResult> killed = m_Process->Wait(DrainTimeout);
					return std::unexpected(Error(ErrorCode::Timeout, std::format("the exported game's --feature-test run ran longer than {} s and was killed: {}", RunTimeout.count(), killed.has_value() ? Utils::TailOfProcessOutput(*killed, OutputTailLines) : killed.error().ToString()))
							.WithHint(std::string(RunHint)));
				}
				ENGINE_TRY_ASSIGN(const ProcessResult result, m_Process->Wait(DrainTimeout));
				Result<std::string> text = FileSystem::ReadText(m_Report);
				if (!text.has_value())
				{
					return std::unexpected(std::move(text).error().WithContext(std::format("while reading the report of the exported game's --feature-test run, which exited with code {}: {}", result.ExitCode,
																				   Utils::TailOfProcessOutput(result, OutputTailLines)))
							.WithHint(std::string(RunHint)));
				}
				Result<ExportFeatureTestReport> report = Utils::ReadExportFeatureTestReport(*text, result.ExitCode);
				if (!report.has_value())
				{
					ErrorLocation location;
					location.File = FileSystem::PathToUtf8(m_Report);
					return std::unexpected(std::move(report).error().WithLocation(std::move(location)).WithContext(std::format("while reading the report of the exported game's --feature-test run, which exited with code {}", result.ExitCode)));
				}
				report->ReportPath = m_MovedReport;
				return ExportFeatureTestOutcome{ .Report = std::move(*report), .Warnings = {} };
			}
		private:
			std::optional<Process> m_Process{};
			std::chrono::steady_clock::time_point m_Deadline{};
			std::filesystem::path m_Report{};
			std::filesystem::path m_MovedReport{};
			std::optional<Result<ExportFeatureTestOutcome>> m_Resolved{};
		};

	}

	Result<Scope<IExportFeatureTestJob>> StartExportFeatureTest(const ExportFeatureTestRun& run)
	{
		if (run.Configuration == ExportConfiguration::Dist && !run.HasGraphicsDevice)
		{
			ExportFeatureTestOutcome outcome;
			outcome.Warnings.emplace_back(NotRunWarning);
			return Scope<IExportFeatureTestJob>(CreateScope<ExportFeatureTestJob>(std::move(outcome)));
		}

		// The run works in the package's own testing user data (<package>/bin/TestUserData/<Name>, GetTestingUserDataRoot),
		// where a testing export's --feature-test run without --user-data-dir writes its log, and the Runtime writes its
		// report below its working directory (bin/TestResults/FeatureTest-<Mode>.json, RuntimeApp.cpp), so everything the
		// run writes lies in that folder and moves with the package in step 7.
		const std::filesystem::path testingRoot = GetTestingUserDataRoot(run.StagedManifest);
		const std::filesystem::path workingDirectory = testingRoot / FileSystem::PathFromUtf8(run.Name);
		const std::filesystem::path report = workingDirectory / "bin" / "TestResults" / std::filesystem::path(Utils::GetFeatureTestReportName(run.Configuration));
		// GetTestingUserDataRoot is "<package>/bin/TestUserData", so the report lies where it will lie below OutputDirectory.
		const std::filesystem::path package = testingRoot.parent_path().parent_path();
		const std::filesystem::path movedReport = run.OutputDirectory / report.lexically_relative(package);
		ENGINE_TRY(WithContext(FileSystem::CreateDirectories(workingDirectory), "while creating the testing user data of the exported game's --feature-test run"));
		// A report an earlier run left would pass for this run's if this one wrote none.
		if (FileSystem::Exists(report))
			ENGINE_TRY(WithContext(FileSystem::Remove(report), "while removing an earlier --feature-test report"));

		std::vector<std::string> arguments = { std::string(HeadlessOption), std::string(FeatureTestOption) };
		if (!run.HasGraphicsDevice)
		{
			arguments.emplace_back(RendererOption);
			arguments.emplace_back(NoRenderer);
		}
		ENGINE_TRY_ASSIGN(Process process, WithContext(Process::Spawn({
														   .Executable = run.Executable,
														   .Arguments = std::move(arguments),
														   .WorkingDirectory = workingDirectory,
														   .Environment = {},
													   }),
											   "while starting the exported game's --feature-test run"));
		return Scope<IExportFeatureTestJob>(CreateScope<ExportFeatureTestJob>(std::move(process), report, movedReport));
	}

}
