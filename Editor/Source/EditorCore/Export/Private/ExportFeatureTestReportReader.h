#pragma once

#include "EditorCore/Export/Exporter.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <string_view>

// How step 6a (Export/ExportFeatureTest.h; Docs/Decisions/0030-m19-contract.md decision 10) reads the JSON report of an
// exported game's --feature-test run, apart from the run, so its rules are tested on reports of every shape.

namespace Engine {

	namespace Utils {

		// Step 6a's report from `text`, the run's FeatureTest report (a TestRunResult as TestRunResultToJson writes it,
		// Testing/TestResults.h), and `exitCode`, the run's: Ran true; Passed when the exit code is 0 and the report's
		// "passed" is true; Counts the report's "counts"; every replay, from the "replay:<path>" rows, sorted by path, with
		// its suite row's finalStateHash and Passed when its suite row and each of its case rows passed. A replay with case
		// rows but no suite row has no hash and has not passed. ReportPath stays empty: the caller names the report's place.
		// Errors: Parse for text that is not JSON; Validation, located at the offending value, for a document that is not
		// such a report (one without counts included).
		[[nodiscard]] Result<ExportFeatureTestReport> ReadExportFeatureTestReport(std::string_view text, int exitCode);

	}

}
