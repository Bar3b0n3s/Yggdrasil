#pragma once

#include "EditorCore/Export/Exporter.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstdint>
#include <string>
#include <vector>

// project.export (Architecture §13.5 "project", §14.2), the editor's export method: a pending operation (§13.2) over the
// Exporter (EditorCore/Export/Exporter.h). Its own file, as ScreenshotMethods is for the screenshot theme, so the project
// domain's other methods stay in ProjectMethods (Docs/Decisions/0012-m7-decisions.md decision 14). Conventions as in
// MethodRegistry.h; frozen by the M7 contract.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class PendingOperation;
	class TypeRegistry;

	// project.export {config, outDir?, smokeTest?, testing?, featureTest?} (§13.5).
	struct ProjectExportParams
	{
		ExportConfiguration Config = ExportConfiguration::Release; // required; registry enum "ExportConfiguration"
		// The project-relative output directory under "Build/"; empty: Build/<Platform>-<Config>/<Name> (§14.1).
		std::string OutDir{};
		// Run the staged package's executable with --headless --frames 300 --expect-no-errors before it reaches the output
		// directory and require exit code 0 (§14.2 step 7; Exporter.h step 6 has the per-configuration options and the Dist
		// export from an editor without a device, which reports smokeTestRan false with a warning).
		bool SmokeTest = false;
		// A testing export (§7.6, §14.1; M15, Docs/Decisions/0024-m15-contract.md decision 6): Game.json says "Testing": true,
		// Assets/Tests/** and every Testing.Suites script and scene and Testing.Replays match are packaged whatever
		// Export.Exclude says, and validation also refuses invalid suites and replays that do not import. The default output
		// directory becomes Build/<Platform>-<Config>-Testing/<Name>, so it never replaces the plain export.
		bool Testing = false;
		// M19 (Docs/Decisions/0030-m19-contract.md decision 10, A-DISTSELF): run the staged package's --feature-test
		// (ExportSpecification::FeatureTest, Exporter.h step 6a) and report its replays' hashes in the result's featureTest, so a
		// builder sees the hashes the games suite's Dist half compares. Requires testing: InvalidParams at /featureTest
		// otherwise.
		bool FeatureTest = false;
	};

	// One replay of the --feature-test run (ExportFeatureTestReplay).
	struct ExportFeatureTestReplaySummary
	{
		std::string Path{};      // project-relative
		std::string StateHash{}; // observed at its final tick
		bool Passed = false;
	};

	// The --feature-test run of a testing export (ExportFeatureTestReport).
	struct ExportFeatureTestSummary
	{
		bool Ran = false;
		VariantValue ExitCode{}; // an integer, null when it did not run (as smokeTestExitCode)
		bool Passed = false;
		std::string ReportPath{}; // absolute native path; empty when it did not run
		// The report's rows by status (ExportFeatureTestReport::Counts, registry struct "ExportFeatureTestCounts"); zero when it
		// did not run.
		ExportFeatureTestCounts Counts{};
		std::vector<ExportFeatureTestReplaySummary> Replays{};
	};

	// One file of the package (ExportedFile).
	struct ExportedFileSummary
	{
		std::string Path{}; // relative to outputDirectory
		uint32_t Size = 0;  // bytes (ToAutomationCounter)
		std::string Hash{}; // XXH64, 16 lowercase hex digits ("xxh64")
	};

	// How long one phase of the export took (ExportPhaseTiming).
	struct ExportPhaseTimingSummary
	{
		ExportPhase Phase = ExportPhase::Validate; // registry enum "ExportPhase"
		float Seconds = 0.0f;                      // wall-clock, for information
	};

	// The export report (§14.2 step 8).
	struct ProjectExportResult
	{
		std::string OutputDirectory{}; // absolute native path
		std::string Executable{};      // absolute native path
		ExportConfiguration Config = ExportConfiguration::Release;
		std::vector<ExportedFileSummary> Files{};
		uint32_t GameAssetCount = 0;
		uint32_t EngineEntryCount = 0;
		std::vector<std::string> Warnings{};
		bool SmokeTestRan = false;
		// The smoke test's exit code, an integer; null when it did not run (M16 friction S2-06: it was 0, which read like a
		// passing run). ExportReport::SmokeTestExitCode with ExportReport::SmokeTestRan.
		VariantValue SmokeTestExitCode{};
		float Seconds = 0.0f;
		// M15 (ADR 0024 decisions 2, 4 and 6): ExportReport's testing, resource and timing members.
		bool Testing = false;
		uint32_t TestSuiteCount = 0;
		uint32_t ReplayCount = 0;
		bool ResourcesPatched = false;
		std::vector<ExportPhaseTimingSummary> PhaseTimings{};
		// M19 (decision 10): the featureTest run; Ran false without featureTest.
		ExportFeatureTestSummary FeatureTest{};
		// M19 (Docs/Decisions/0030-m19-contract.md decisions 20 and 21): ExportReport::Reachability as it is, the registry
		// structs "ExportReachability", "ExportReachedAsset" and "ExportComputedLoad".
		ExportReachability Reachability{};
		// M19 (decision 33): ExportReport::NativeModule.
		bool NativeModule = false;
	};

	// project.export's result for `report` (§14.2 step 8): the same document the method returns, which `Editor --export
	// --export-report` writes too (Docs/Decisions/0024-m15-contract.md decision 3), so the two never differ.
	[[nodiscard]] ProjectExportResult MakeProjectExportResult(const ExportReport& report);

	namespace Automation {

		// project.export (pending): Exporter::Start with the params (testing included, M15), AutomationServerSpecification::
		// ExportBinaryRoot and (M15, `Editor --export-root`) AutomationServerSpecification::ExportRoot, polled once per frame;
		// a disconnect cancels it (Exporter::Cancel). Errors: InvalidParams for an outDir outside "Build/"; those of
		// Exporter::Start and Exporter::Poll (a testing export's TEST_SUITE_INVALID and replay issues among the Validation
		// issues).
		[[nodiscard]] Result<Scope<PendingOperation>> ProjectExport(EditorMethodContext& context, const ProjectExportParams& params);

	}

	// Registers ExportConfiguration, ExportPhase, ProjectExportParams, ExportedFileSummary, ExportPhaseTimingSummary,
	// ExportFeatureTestReplaySummary, ExportFeatureTestCounts, ExportFeatureTestSummary, ExportReachReason, ExportReachedAsset,
	// ExportComputedLoad, ExportReachability and ProjectExportResult.
	void RegisterExportMethodTypes(TypeRegistry& registry);

	// Registers project.export: a tool (§13.8 project_export), Mutates (it writes under the project's Build/, which read-only
	// editors refuse), pending with a timeout of 900 s, no dry run (§13.4), not a batch op, not available in the launcher
	// state, not AvailableInRuntime.
	void RegisterExportMethods(MethodRegistry& methods);

}
