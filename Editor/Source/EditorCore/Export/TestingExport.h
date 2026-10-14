#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// What a testing export packages beyond a plain one, and what blocks it (Architecture §7.6, §14.1, §14.2 step 1, §15.1 T3;
// Docs/Decisions/0024-m15-contract.md decision 6). The exporter calls these when ExportSpecification::Testing is set:
// CollectTestingExportContent in Validate, IsForcedByTestingExport in CollectSources (Exporter.cpp; the call sites are
// wired by the contract, stream B implements this file).

namespace Engine {

	class EditorContext;
	struct ProjectSettings;

	// The directory a testing export always packages, whatever Export.Exclude says (§7.6).
	inline constexpr std::string_view TestingExportForcedDirectory = "Assets/Tests/";

	// The project-relative paths (FormatAssetReference spelling, no "#key") a testing export packages whatever
	// Export.Exclude says, besides every registered main asset below TestingExportForcedDirectory.
	struct TestingExportContent
	{
		std::vector<std::string> SuiteScripts{}; // every Testing.Suites Script, sorted, unique
		std::vector<std::string> SuiteScenes{};  // every non-empty Testing.Suites Scene, sorted, unique
		std::vector<std::string> Replays{};      // every registered .replay a Testing.Replays glob matches, sorted, unique
		uint32_t SuiteCount = 0;                 // Testing.Suites entries, whatever their Modes (ExportReport::TestSuiteCount)
	};

	// Collects the content of a testing export of the project open in `editor`, whose settings are `settings`, appending
	// one issue to `issues` per problem (the exporter aggregates them with its other Validate issues):
	//   - each ProjectValidator::CheckTestSuites diagnostic (TEST_SUITE_INVALID) as an issue at
	//     "/Testing/Suites/<i>/Script" or "/Testing/Suites/<i>/Scene", its message starting with "TEST_SUITE_INVALID: ";
	//   - a Testing.Replays entry without wildcards that names no registered replay, at "/Testing/Replays/<i>";
	//   - every matched replay is loaded through the editor's asset manager, which imports it with ReplayImporter and so
	//     compiles each expectation (§14.2 step 1 "every replay compiles"); an import failure is an issue at
	//     "/Testing/Replays/<i>" naming the replay and carrying the import error's located message.
	// Replays are matched with MatchesPathGlob over the registered .replay sources below Assets/, exactly as the editor's
	// and the Runtime's --feature-test expand them. A glob that matches nothing is not an issue (as at run time). Errors
	// (not issues): InvalidState without an open project or script service; the asset manager's non-import errors.
	[[nodiscard]] Result<TestingExportContent> CollectTestingExportContent(EditorContext& editor, const ProjectSettings& settings,
		std::vector<ErrorIssue>& issues);

	// Whether a testing export packages the registered asset at `projectPath` whatever Export.Exclude says: a path below
	// TestingExportForcedDirectory, or one listed in `content`. Pure.
	[[nodiscard]] bool IsForcedByTestingExport(std::string_view projectPath, const TestingExportContent& content);

}
