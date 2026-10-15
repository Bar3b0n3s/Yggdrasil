#include "EditorPCH.h"
#include "EditorCore/Automation/ExportMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/Export/Private/ExportPaths.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <format>
#include <utility>

namespace Engine {

	ProjectExportResult MakeProjectExportResult(const ExportReport& report)
	{
		ProjectExportResult result;
		result.OutputDirectory = FileSystem::PathToUtf8(report.OutputDirectory);
		result.Executable = FileSystem::PathToUtf8(report.Executable);
		result.Config = report.Configuration;
		result.Files.reserve(report.Files.size());
		for (const ExportedFile& file : report.Files)
		{
			result.Files.push_back({
				.Path = file.Path,
				.Size = ToAutomationCounter(file.Size),
				.Hash = std::format("{:016x}", file.Hash),
			});
		}
		result.GameAssetCount = report.GameAssetCount;
		result.EngineEntryCount = report.EngineEntryCount;
		result.Warnings = report.Warnings;
		result.SmokeTestRan = report.SmokeTestRan;
		if (report.SmokeTestRan)
			result.SmokeTestExitCode = VariantValue(Json(report.SmokeTestExitCode));
		result.Seconds = static_cast<float>(report.Seconds);
		result.Testing = report.Testing;
		result.TestSuiteCount = report.TestSuiteCount;
		result.ReplayCount = report.ReplayCount;
		result.ResourcesPatched = report.ResourcesPatched;
		result.PhaseTimings.reserve(report.PhaseTimings.size());
		for (const ExportPhaseTiming& timing : report.PhaseTimings)
			result.PhaseTimings.push_back({ .Phase = timing.Phase, .Seconds = static_cast<float>(timing.Seconds) });
		return result;
	}

	namespace {

		// project.export's operation: the exporter, advanced once per frame (§13.2 "Pending operations"); a disconnect
		// cancels it, which removes the staging directory and leaves an earlier export untouched.
		class ProjectExportOperation final : public PendingOperation
		{
		public:
			explicit ProjectExportOperation(Scope<Exporter> exporter)
				: m_Exporter(std::move(exporter))
			{
			}

			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext& context) override
			{
				std::optional<Result<ExportReport>> outcome = m_Exporter->Poll();
				if (!outcome.has_value())
					return std::nullopt;
				if (!outcome->has_value())
					return Result<Json>(std::unexpected(std::move(*outcome).error()));
				return context.SerializeResult(MakeProjectExportResult(**outcome));
			}

			void Cancel(MethodContext& /*context*/) override
			{
				m_Exporter->Cancel();
			}

			[[nodiscard]] std::string GetPhase() const override
			{
				return std::format("Export:{}", ExportPhaseToString(m_Exporter->GetPhase()));
			}
		private:
			Scope<Exporter> m_Exporter;
		};

	}

	namespace Automation {

		Result<Scope<PendingOperation>> ProjectExport(EditorMethodContext& context, const ProjectExportParams& params)
		{
			if (Status directory = Utils::CheckExportOutputDirectory(params.OutDir); !directory.has_value())
			{
				return std::unexpected(
					Utils::MakeParamError(ErrorCode::InvalidArgument, "/outDir", directory.error().GetMessageText(), directory.error().GetHint()));
			}
			const std::filesystem::path& binaryRoot = context.GetServer().GetSpecification().ExportBinaryRoot;
			if (binaryRoot.empty())
				return MakeError(ErrorCode::Unsupported, "this editor exports nothing: it was started without the directory of the build outputs");

			// The smoke test runs ExportSpecification's default of 300 frames (§14.2 step 7).
			ExportSpecification specification;
			specification.Configuration = params.Config;
			specification.OutputDirectory = params.OutDir;
			specification.BinaryRoot = binaryRoot;
			// Editor --export-root (§13.4; M15, ADR 0024 decision 3; wired by the contract): empty unless the editor was started
			// with it.
			specification.ExportRoot = context.GetServer().GetSpecification().ExportRoot;
			specification.SmokeTest = params.SmokeTest;
			specification.Testing = params.Testing;
			ENGINE_TRY_ASSIGN(Scope<Exporter> exporter, Exporter::Start(context.GetEditor(), specification));
			return Scope<PendingOperation>(CreateScope<ProjectExportOperation>(std::move(exporter)));
		}

	}

	void RegisterExportMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<ExportConfiguration>("ExportConfiguration", "The build configuration an export targets (§2.2).")
			.Entry(ExportConfiguration::Debug, "Debug", "Engine asserts on, unoptimized, with the automation server.")
			.Entry(ExportConfiguration::Release, "Release", "Engine asserts on, optimized, with the automation server.")
			.Entry(ExportConfiguration::Dist, "Dist", "The shipping configuration: asserts off, no automation server.");

		registry.Enum<ExportPhase>("ExportPhase", "A step of an export (§14.2), in the order the steps run.")
			.Entry(ExportPhase::Validate, "Validate", "The project, its scenes, scripts and settings and the build outputs are checked; nothing is written.")
			.Entry(ExportPhase::Cook, "Cook", "Every included asset is cooked through the asset manager.")
			.Entry(ExportPhase::WritePaks, "WritePaks", "Data/Engine.pak and Data/Game.pak are written and verified.")
			.Entry(ExportPhase::CopyRuntime, "CopyRuntime", "The configuration's Runtime is copied, renamed after the project, with what it needs.")
			.Entry(ExportPhase::PatchResources, "PatchResources", "The executable's icon and version (Windows) or Info.plist (macOS) are written.")
			.Entry(ExportPhase::WriteManifest, "WriteManifest", "Game.json and THIRD_PARTY_NOTICES.txt are written.")
			.Entry(ExportPhase::Sign, "Sign", "The macOS bundle is signed; nothing happens elsewhere.")
			.Entry(ExportPhase::SmokeTest, "SmokeTest", "The staged game runs headless when smokeTest asks for it.")
			.Entry(ExportPhase::MoveToOutput, "MoveToOutput", "The staged package replaces the output directory.")
			.Entry(ExportPhase::Done, "Done", "The export finished.");

		registry.Struct<ProjectExportParams>("ProjectExportParams", "The params of project.export.")
			.Field("config", &ProjectExportParams::Config, "The build configuration whose Runtime and shaders the export packages.")
			.Field("outDir", &ProjectExportParams::OutDir,
				"The project-relative output directory below Build/; empty or absent: Build/<Platform>-<Config>/<Name>. An earlier export there "
				"is replaced once the new one is complete.")
			.Field("smokeTest", &ProjectExportParams::SmokeTest,
				"Run the exported game headless for 300 frames with --expect-no-errors before it replaces the output directory, and require exit "
				"code 0.")
			.Field("testing", &ProjectExportParams::Testing,
				"A testing export: Game.json says Testing true, so the exported game runs --feature-test; Assets/Tests/** and every "
				"Testing.Suites script and scene and Testing.Replays match are packaged whatever Export.Exclude says; invalid suites "
				"(TEST_SUITE_INVALID) and replays that do not import fail validation. Its default output directory is "
				"Build/<Platform>-<Config>-Testing/<Name>.");

		registry.Struct<ExportPhaseTimingSummary>("ExportPhaseTimingSummary", "How long one phase of an export took.")
			.Field("phase", &ExportPhaseTimingSummary::Phase, "The phase.")
			.Field("seconds", &ExportPhaseTimingSummary::Seconds, "Its wall-clock time, for information.", { .Unit = "s" });

		registry.Struct<ExportedFileSummary>("ExportedFileSummary", "One file of an exported game.")
			.Field("path", &ExportedFileSummary::Path, "The path relative to the output directory, with forward slashes.")
			.Field("size", &ExportedFileSummary::Size, "The size in bytes.", { .Unit = "bytes" })
			.Field("hash", &ExportedFileSummary::Hash, "The XXH64 of the bytes, as 16 lowercase hex digits.");

		registry.Struct<ProjectExportResult>("ProjectExportResult", "What project.export produced (§14.2).")
			.Field("outputDirectory", &ProjectExportResult::OutputDirectory, "The absolute path of the exported game's folder.")
			.Field("executable", &ProjectExportResult::Executable, "The absolute path of its executable, named after the project.")
			.Field("config", &ProjectExportResult::Config, "The configuration exported.")
			.Field("files", &ProjectExportResult::Files, "Every file of the package, sorted by path.")
			.Field("gameAssetCount", &ProjectExportResult::GameAssetCount, "The cooked assets in Data/Game.pak.")
			.Field("engineEntryCount", &ProjectExportResult::EngineEntryCount, "The entries of Data/Engine.pak: shader files and built-ins.")
			.Field("warnings", &ProjectExportResult::Warnings, "What the export left out or could not check.")
			.Field("smokeTestRan", &ProjectExportResult::SmokeTestRan,
				"Whether the smoke test ran: false without smokeTest, and for a Dist export from an editor without a graphics device.")
			.Field("smokeTestExitCode", &ProjectExportResult::SmokeTestExitCode, "The smoke test's exit code, an integer; null when it did not run (smokeTestRan false).")
			.Field("seconds", &ProjectExportResult::Seconds, "How long the export took (wall-clock time, for information).", { .Unit = "s" })
			.Field("testing", &ProjectExportResult::Testing, "Whether this was a testing export (Game.json Testing true).")
			.Field("testSuiteCount", &ProjectExportResult::TestSuiteCount,
				"A testing export's Testing.Suites entries, whatever their modes; 0 for a plain export.")
			.Field("replayCount", &ProjectExportResult::ReplayCount, "The replays a testing export packaged for Testing.Replays; 0 for a plain export.")
			.Field("resourcesPatched", &ProjectExportResult::ResourcesPatched,
				"Whether the executable's resources were written: its icon and version on Windows, Info.plist on macOS.")
			.Field("phaseTimings", &ProjectExportResult::PhaseTimings,
				"The time of every phase the export entered, in phase order; SmokeTest only when the smoke test ran (its time otherwise counts toward MoveToOutput).");
	}

	void RegisterExportMethods(MethodRegistry& methods)
	{
		Json example = Json::object();
		example["config"] = "Release";
		example["smokeTest"] = true;
		Json testingExample = Json::object();
		testingExample["config"] = "Dist";
		testingExample["testing"] = true;
		methods.AddPending<EditorMethodContext, ProjectExportParams, ProjectExportResult>(
			{
				.Name = "project.export",
				.Description = "Exports the open project as a standalone game below Build/: the configuration's Runtime renamed after the project, "
							   "the app-local CRT, Game.json, Data/Engine.pak and Data/Game.pak, optionally smoke-tested, or with testing a "
							   "testing export whose game runs its suites and replays with --feature-test; resolves with the report.",
				.RequiredParams = { "config" },
				.ExposeAsTool = true,
				.Mutates = true,
				.TimeoutSeconds = 900,
				.Examples = { { .Description = "Export a Release build and smoke-test it.", .Params = example },
					{ .Description = "Export a Dist build that runs the project's test suites and replays.", .Params = testingExample } },
			},
			&Automation::ProjectExport);
	}

}
