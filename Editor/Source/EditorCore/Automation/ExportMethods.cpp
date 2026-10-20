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
		result.FeatureTest.Ran = report.FeatureTest.Ran;
		if (report.FeatureTest.Ran)
		{
			result.FeatureTest.ExitCode = VariantValue(Json(report.FeatureTest.ExitCode));
			result.FeatureTest.ReportPath = FileSystem::PathToUtf8(report.FeatureTest.ReportPath);
		}
		result.FeatureTest.Passed = report.FeatureTest.Passed;
		result.FeatureTest.Replays.reserve(report.FeatureTest.Replays.size());
		for (const ExportFeatureTestReplay& replay : report.FeatureTest.Replays)
			result.FeatureTest.Replays.push_back({ .Path = replay.Path, .StateHash = replay.StateHash, .Passed = replay.Passed });
		result.Reachability = report.Reachability;
		result.NativeModule = report.NativeModule;
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

			if (params.FeatureTest && !params.Testing)
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/featureTest", "featureTest requires testing:true", "a feature test runs a testing export's suites and replays"));
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
			specification.FeatureTest = params.FeatureTest;
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
			.Entry(ExportPhase::NativeBuild, "NativeBuild", "A game with a Native/ folder gets a Runtime built with its native module linked in; other games skip it.")
			.Entry(ExportPhase::CopyRuntime, "CopyRuntime", "The configuration's Runtime is copied, renamed after the project, with what it needs.")
			.Entry(ExportPhase::PatchResources, "PatchResources", "The executable's icon and version (Windows) or Info.plist (macOS) are written.")
			.Entry(ExportPhase::WriteManifest, "WriteManifest", "Game.json and THIRD_PARTY_NOTICES.txt are written.")
			.Entry(ExportPhase::Sign, "Sign", "The macOS bundle is signed; nothing happens elsewhere.")
			.Entry(ExportPhase::SmokeTest, "SmokeTest", "The staged game runs headless when smokeTest asks for it.")
			.Entry(ExportPhase::FeatureTest, "FeatureTest", "The staged testing export runs --feature-test when featureTest asks for it, and its replays' hashes are read.")
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
				"Build/<Platform>-<Config>-Testing/<Name>.")
			.Field("featureTest", &ProjectExportParams::FeatureTest,
				"With testing: run the staged game's --feature-test before it replaces the output directory and report its replays' state "
				"hashes in featureTest, the hashes the games suite's Dist half compares with the editors'. A failing run does not fail the "
				"export; the report says so.");

		registry.Struct<ExportFeatureTestReplaySummary>("ExportFeatureTestReplaySummary", "One replay of an exported game's --feature-test run.")
			.Field("path", &ExportFeatureTestReplaySummary::Path, "The replay's project-relative path.")
			.Field("stateHash", &ExportFeatureTestReplaySummary::StateHash, "The state hash the exported game observed at the replay's final tick.")
			.Field("passed", &ExportFeatureTestReplaySummary::Passed, "Every row of the replay passed.");

		registry.Struct<ExportFeatureTestSummary>("ExportFeatureTestSummary", "The --feature-test run of a testing export (featureTest).")
			.Field("ran", &ExportFeatureTestSummary::Ran, "Whether it ran: false without featureTest, and for a Dist export from an editor without a graphics device.")
			.Field("exitCode", &ExportFeatureTestSummary::ExitCode, "The run's exit code, an integer; null when it did not run.")
			.Field("passed", &ExportFeatureTestSummary::Passed, "Exit code 0 and every row passed.")
			.Field("reportPath", &ExportFeatureTestSummary::ReportPath, "The run's JSON report, inside the exported game's folder; empty when it did not run.")
			.Field("replays", &ExportFeatureTestSummary::Replays, "Every replay's observed final state hash, sorted by path.");

		registry.Enum<ExportReachReason>("ExportReachReason", "Why an export put an asset into its paks (the first reason its walk found).")
			.Entry(ExportReachReason::StartScene, "StartScene", "Game.pak: the project's StartScene.")
			.Entry(ExportReachReason::BuildScene, "BuildScene", "Game.pak: an Export.BuildScenes entry.")
			.Entry(ExportReachReason::Testing, "Testing", "Game.pak: content a testing export packages whatever Export.Exclude says.")
			.Entry(ExportReachReason::Dependency, "Dependency",
				"Another packaged asset (from) references it or one of its sub-assets: an import dependency or a Script field value.")
			.Entry(ExportReachReason::LiteralLoad, "LiteralLoad", "A packaged script (from) loads it with Assets.Load and a string literal (at line).")
			.Entry(ExportReachReason::Required, "Required", "Engine.pak: the exported game asks for this built-in by handle, so every export holds it.")
			.Entry(ExportReachReason::ComputedLoad, "ComputedLoad",
				"Engine.pak: kept with every built-in, because a packaged script (from) computes an Assets.Load reference (at line).");

		registry.Struct<ExportReachedAsset>("ExportReachedAsset", "One asset of an exported game's paks, and why the export packaged it.")
			.Field("path", &ExportReachedAsset::Path, "The asset's project path, or the built-in's engine:// path.")
			.Field("reason", &ExportReachedAsset::Reason, "The first reason the export's walk found for it.")
			.Field("from", &ExportReachedAsset::From, "The path of the asset that reached it (Dependency, LiteralLoad, ComputedLoad); empty otherwise.")
			.Field("line", &ExportReachedAsset::Line, "The 1-based line of from's Assets.Load (LiteralLoad, ComputedLoad); 0 otherwise.");

		registry.Struct<ExportComputedLoad>("ExportComputedLoad", "An Assets.Load whose reference a packaged script computes, which the export cannot follow.")
			.Field("script", &ExportComputedLoad::Script, "The script's project path.")
			.Field("line", &ExportComputedLoad::Line, "The 1-based line.")
			.Field("column", &ExportComputedLoad::Column, "The 1-based column.");

		registry.Struct<ExportReachability>("ExportReachability", "What an export put into its paks and why.")
			.Field("pruned", &ExportReachability::Pruned,
				"Export.PruneUnreferenced was on: Data/Game.pak holds only what StartScene and Export.BuildScenes reach (and a testing export's "
				"testing content). Off, gameAssets and prunedAssets are empty and Data/Game.pak holds every asset below Assets/ that "
				"Export.Exclude leaves in.")
			.Field("gameAssets", &ExportReachability::GameAssets, "Every asset of a pruned Data/Game.pak with its reason, sorted by path.")
			.Field("prunedAssets", &ExportReachability::PrunedAssets, "The assets an unpruned export would have packaged but nothing reaches, sorted.")
			.Field("engineBuiltins", &ExportReachability::EngineBuiltins,
				"Every built-in of Data/Engine.pak with its reason, sorted by path: the ones the game asks for by handle and the ones its assets "
				"reach (procedural built-ins are generated by the game and never packaged).")
			.Field("prunedBuiltins", &ExportReachability::PrunedBuiltins, "The file and generated built-ins Data/Engine.pak leaves out, sorted.")
			.Field("computedLoads", &ExportReachability::ComputedLoads,
				"Every Assets.Load of a packaged script whose reference is computed, sorted by script, line and column; such a load makes "
				"Data/Engine.pak keep every built-in.");

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
				"The time of every phase the export entered, in phase order; SmokeTest only when the smoke test ran, FeatureTest only when the feature test ran and NativeBuild only for a game with a Native/ folder (their time otherwise counts toward the next phase).")
			.Field("featureTest", &ProjectExportResult::FeatureTest, "The featureTest run of a testing export; ran false without featureTest.")
			.Field("reachability", &ProjectExportResult::Reachability,
				"What the paks hold and why: the built-ins Data/Engine.pak kept and left out, the scripts' computed Assets.Load calls, and with "
				"Export.PruneUnreferenced the assets Data/Game.pak kept and left out.")
			.Field("nativeModule", &ProjectExportResult::NativeModule,
				"Whether the executable links the project's native module: true for a game with a Native/ folder, whose Runtime the export built.");
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
				// The caller's bound, which the MCP bridge waits for: cooking, the native build and the paks, then the smoke
				// test (at most 300 s, Exporter.cpp) and a testing export's feature test (at most 1800 s, ExportFeatureTest.cpp;
				// Exporter.h steps 6 and 6a), which run one after the other.
				.TimeoutSeconds = 3600,
				.Examples = { { .Description = "Export a Release build and smoke-test it.", .Params = example },
					{ .Description = "Export a Dist build that runs the project's test suites and replays.", .Params = testingExample } },
			},
			&Automation::ProjectExport);
	}

}
