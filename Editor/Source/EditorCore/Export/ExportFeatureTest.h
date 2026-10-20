#pragma once

#include "EditorCore/Export/ExportConfiguration.h"
#include "EditorCore/Export/Exporter.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

// Step 6a of the export (Exporter.h; Docs/Decisions/0030-m19-contract.md decision 10, A-DISTSELF): the staged testing
// package's --feature-test run, whose report gives the builder the exported game's replay hashes. Exporter.cpp starts the run
// at step 6a and polls it once per frame, which the contract wired, so the exporter's state machine stays one stream's file in
// W19.1 (stream D's, decision 15) while stream F implements the run here.

namespace Engine {

	// What step 6a runs; Exporter.cpp fills it from its state.
	struct ExportFeatureTestRun
	{
		// The staged package's executable (the renamed Runtime), run by this fixed path with the fixed argument --feature-test,
		// never an argument a request supplies.
		std::filesystem::path Executable{};
		// The staged package's Game.json: the run's report is read below its testing user data
		// (GetTestingUserDataRoot(StagedManifest) / Name / "TestResults" / "FeatureTest-<Mode>.json", Project/GameManifest.h).
		std::filesystem::path StagedManifest{};
		// The directory step 7 moves the staged package to: ExportFeatureTestReport::ReportPath names the report there.
		std::filesystem::path OutputDirectory{};
		std::string Name{}; // the game's Name (ProjectSettings::Name), the testing user data's folder
		ExportConfiguration Configuration = ExportConfiguration::Release;
		// The exporting editor has a graphics device. Without one Debug and Release run with --renderer none, as the smoke test
		// does, and Dist, which has no --renderer none (§13.9), does not run: the outcome then has Ran false and a warning.
		bool HasGraphicsDevice = false;
	};

	// Step 6a's result.
	struct ExportFeatureTestOutcome
	{
		ExportFeatureTestReport Report{};    // ExportReport::FeatureTest
		std::vector<std::string> Warnings{}; // appended to ExportReport::Warnings ("the feature test did not run: ...")
	};

	// One running step 6a. Main thread; Poll once per frame. Destroying a job whose process still runs kills it (the export's
	// cancellation), so no run outlives its export.
	class IExportFeatureTestJob
	{
	public:
		virtual ~IExportFeatureTestJob() = default;

		// nullopt while the run lasts; then its outcome: a run that exited, whatever its exit code (a failed run is Passed
		// false with its exit code and rows, not an error, so the builder sees which rows failed), or one that could not run
		// (Ran false and a warning). Errors fail the export, naming why: a report that is missing or cannot be read after the
		// run. Called again after it resolved: the same result.
		[[nodiscard]] virtual std::optional<Result<ExportFeatureTestOutcome>> Poll() = 0;
	};

	// Starts step 6a's run of `run` (decision 10). Errors fail the export: those of Process::Spawn (a run that cannot start)
	// and of creating the testing user data's directory.
	[[nodiscard]] Result<Scope<IExportFeatureTestJob>> StartExportFeatureTest(const ExportFeatureTestRun& run);

}
