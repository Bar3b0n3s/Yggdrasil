#include "TestsPCH.h"

#include "EditorCore/Export/Private/ExportFeatureTestReportReader.h"

#include <string>
#include <string_view>

// How step 6a reads an exported game's --feature-test report (Docs/Decisions/0030-m19-contract.md decision 10), on reports
// of every shape; ExportFeatureTestTests.cpp runs real exported games.

namespace Engine {

	namespace {

		// A report's "counts" member with no row.
		constexpr std::string_view ZeroCounts = R"("counts":{"total":0,"passed":0,"failed":0,"error":0,"timeout":0,"skipped":0,"quit":0})";

		// A report that passed, with one script suite and no replay.
		constexpr std::string_view PassingReport = R"({"mode":"Release","passed":true,"cases":[{"suite":"Smoke","case":"passes","status":"passed"}],)"
												   R"("counts":{"total":1,"passed":1,"failed":0,"error":0,"timeout":0,"skipped":0,"quit":0},)"
												   R"("suites":[{"suite":"Smoke","finalStateHash":"","passed":true}]})";

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("ExportFeatureTestReportReader: every replay has its suite row's hash, sorted by path, and passes only when all its rows passed")
		{
			// Z passes; A's suite row passed but one of its case rows failed; M's suite row failed; Orphan has only a case
			// row, so no hash. The script suite Smoke is no replay, whatever its rows say.
			constexpr std::string_view Text = R"({"mode":"Release","passed":false,)"
											  R"("counts":{"total":5,"passed":2,"failed":2,"error":1,"timeout":0,"skipped":0,"quit":0},)"
											  R"("suites":[{"suite":"Smoke","finalStateHash":"","passed":false},)"
											  R"({"suite":"replay:Assets/Z.replay","finalStateHash":"00000000000000aa","passed":true},)"
											  R"({"suite":"replay:Assets/A.replay","finalStateHash":"00000000000000bb","passed":true},)"
											  R"({"suite":"replay:Assets/M.replay","finalStateHash":"00000000000000cc","passed":false}],)"
											  R"("cases":[{"suite":"Smoke","case":"fails","status":"failed"},)"
											  R"({"suite":"replay:Assets/Z.replay","case":"FinalStateHash","status":"passed"},)"
											  R"({"suite":"replay:Assets/A.replay","case":"Expect@3","status":"failed"},)"
											  R"({"suite":"replay:Assets/M.replay","case":"FinalStateHash","status":"passed"},)"
											  R"({"suite":"replay:Assets/Orphan.replay","case":"<replay>","status":"error"}]})";
			const Result<ExportFeatureTestReport> report = Utils::ReadExportFeatureTestReport(Text, 1);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			REQUIRE(report->Replays.size() == 4);
			CHECK(report->Replays[0].Path == "Assets/A.replay");
			CHECK(report->Replays[0].StateHash == "00000000000000bb");
			CHECK_FALSE(report->Replays[0].Passed);
			CHECK(report->Replays[1].Path == "Assets/M.replay");
			CHECK(report->Replays[1].StateHash == "00000000000000cc");
			CHECK_FALSE(report->Replays[1].Passed);
			CHECK(report->Replays[2].Path == "Assets/Orphan.replay");
			CHECK(report->Replays[2].StateHash.empty());
			CHECK_FALSE(report->Replays[2].Passed);
			CHECK(report->Replays[3].Path == "Assets/Z.replay");
			CHECK(report->Replays[3].StateHash == "00000000000000aa");
			CHECK(report->Replays[3].Passed);
		}

		TEST_CASE("ExportFeatureTestReportReader: the run passed only with exit code 0 and a passing report")
		{
			const Result<ExportFeatureTestReport> passed = Utils::ReadExportFeatureTestReport(PassingReport, 0);
			REQUIRE_MESSAGE(passed.has_value(), passed.error().ToString());
			CHECK(passed->Ran);
			CHECK(passed->ExitCode == 0);
			CHECK(passed->Passed);
			CHECK(passed->Replays.empty());
			CHECK(passed->ReportPath.empty());

			const Result<ExportFeatureTestReport> failedExit = Utils::ReadExportFeatureTestReport(PassingReport, 3);
			REQUIRE_MESSAGE(failedExit.has_value(), failedExit.error().ToString());
			CHECK(failedExit->Ran);
			CHECK(failedExit->ExitCode == 3);
			CHECK_FALSE(failedExit->Passed);

			const Result<ExportFeatureTestReport> failedReport =
				Utils::ReadExportFeatureTestReport(R"({"mode":"Release","passed":false,"cases":[],"suites":[],"counts":{"total":0,"passed":0,"failed":0,"error":0,"timeout":0,"skipped":0,"quit":0}})", 0);
			REQUIRE_MESSAGE(failedReport.has_value(), failedReport.error().ToString());
			CHECK_FALSE(failedReport->Passed);
		}

		TEST_CASE("ExportFeatureTestReportReader: a report that is not a FeatureTest result is refused at its value")
		{
			const Result<ExportFeatureTestReport> notJson = Utils::ReadExportFeatureTestReport("{\"passed\":", 0);
			REQUIRE_FALSE(notJson.has_value());
			CHECK(notJson.error().GetCode() == ErrorCode::Parse);

			const Result<ExportFeatureTestReport> notObject = Utils::ReadExportFeatureTestReport("[]", 0);
			REQUIRE_FALSE(notObject.has_value());
			CHECK(notObject.error().GetCode() == ErrorCode::Validation);

			const Result<ExportFeatureTestReport> noSuites = Utils::ReadExportFeatureTestReport(R"({"passed":true,"cases":[],)" + std::string(ZeroCounts) + "}", 0);
			REQUIRE_FALSE(noSuites.has_value());
			CHECK(noSuites.error().GetCode() == ErrorCode::Validation);
			CHECK(noSuites.error().GetLocation().JsonPointer == std::string());

			const Result<ExportFeatureTestReport> numericHash = Utils::ReadExportFeatureTestReport(
				R"({"passed":true,"cases":[],"suites":[{"suite":"replay:Assets/A.replay","finalStateHash":7,"passed":true}],)" + std::string(ZeroCounts) + "}", 0);
			REQUIRE_FALSE(numericHash.has_value());
			CHECK(numericHash.error().GetCode() == ErrorCode::Validation);
			CHECK(numericHash.error().GetLocation().JsonPointer == std::string("/suites/0/finalStateHash"));

			const Result<ExportFeatureTestReport> caseWithoutStatus =
				Utils::ReadExportFeatureTestReport(R"({"passed":true,"suites":[],"cases":[{"suite":"replay:Assets/A.replay","case":"FinalStateHash"}],)" + std::string(ZeroCounts) + "}", 0);
			REQUIRE_FALSE(caseWithoutStatus.has_value());
			CHECK(caseWithoutStatus.error().GetCode() == ErrorCode::Validation);
			CHECK(caseWithoutStatus.error().GetLocation().JsonPointer == std::string("/cases/0"));
		}

		// The native test game's friction NT-10: project.export {featureTest} reports the report's counts, so a builder sees
		// how many rows passed without reading the report; a report without them is not a FeatureTest result.
		TEST_CASE("ExportFeatureTestReportReader: the run's counts are its report's rows by status")
		{
			constexpr std::string_view Text = R"({"passed":false,"suites":[],"cases":[],)"
											  R"("counts":{"total":28,"passed":21,"failed":2,"error":1,"timeout":3,"skipped":0,"quit":1}})";
			const Result<ExportFeatureTestReport> report = Utils::ReadExportFeatureTestReport(Text, 1);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(report->Counts.Total == 28);
			CHECK(report->Counts.Passed == 21);
			CHECK(report->Counts.Failed == 2);
			CHECK(report->Counts.Error == 1);
			CHECK(report->Counts.Timeout == 3);
			CHECK(report->Counts.Skipped == 0);
			CHECK(report->Counts.Quit == 1);

			const Result<ExportFeatureTestReport> passing = Utils::ReadExportFeatureTestReport(PassingReport, 0);
			REQUIRE_MESSAGE(passing.has_value(), passing.error().ToString());
			CHECK(passing->Counts.Total == 1);
			CHECK(passing->Counts.Passed == 1);

			const Result<ExportFeatureTestReport> noCounts = Utils::ReadExportFeatureTestReport(R"({"passed":true,"suites":[],"cases":[]})", 0);
			REQUIRE_FALSE(noCounts.has_value());
			CHECK(noCounts.error().GetCode() == ErrorCode::Validation);

			const Result<ExportFeatureTestReport> negative = Utils::ReadExportFeatureTestReport(
				R"({"passed":true,"suites":[],"cases":[],"counts":{"total":-1,"passed":0,"failed":0,"error":0,"timeout":0,"skipped":0,"quit":0}})", 0);
			REQUIRE_FALSE(negative.has_value());
			CHECK(negative.error().GetCode() == ErrorCode::Validation);
			CHECK(negative.error().GetLocation().JsonPointer == std::string("/counts/total"));
		}
	}

}
