#include "EditorPCH.h"
#include "EditorCore/Export/Private/ExportFeatureTestReportReader.h"

#include "Engine/Core/Json/JsonReader.h"

#include <nlohmann/json.hpp>

#include <map>
#include <string>
#include <utility>

namespace Engine {

	namespace {

		// A replay's report rows are named "replay:<path>" (FeatureTestRunner); a passing case row's status is "passed".
		constexpr std::string_view ReplaySuitePrefix = "replay:";
		constexpr std::string_view PassedStatus = "passed";

		// What a report says about one replay: the hash of its suite row, and whether that row and each of its case rows
		// passed.
		struct ReplayRows
		{
			std::string StateHash{};
			bool HasSuiteRow = false;
			bool Passed = true;
		};

		// Every replay of a report, by path.
		using ReplayRowMap = std::map<std::string, ReplayRows>;

	}

	namespace Utils {

		// Every replay of the report `root` from its "replay:<path>" suite and case rows.
		static Result<ReplayRowMap> ReadReplayRows(const JsonReader& root)
		{
			ReplayRowMap replays;
			ENGINE_TRY_ASSIGN(const JsonReader suites, root.GetMember("suites"));
			ENGINE_TRY_ASSIGN(const size_t suiteCount, suites.GetArraySize());
			for (size_t index = 0; index < suiteCount; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader row, suites.GetElement(index));
				ENGINE_TRY_ASSIGN(const std::string suite, row.ReadMember<std::string>("suite"));
				if (!suite.starts_with(ReplaySuitePrefix))
					continue;
				ENGINE_TRY_ASSIGN(std::string stateHash, row.ReadMember<std::string>("finalStateHash"));
				ENGINE_TRY_ASSIGN(const bool passed, row.ReadMember<bool>("passed"));
				ReplayRows& replay = replays[suite.substr(ReplaySuitePrefix.size())];
				replay.HasSuiteRow = true;
				replay.StateHash = std::move(stateHash);
				replay.Passed = replay.Passed && passed;
			}
			ENGINE_TRY_ASSIGN(const JsonReader cases, root.GetMember("cases"));
			ENGINE_TRY_ASSIGN(const size_t caseCount, cases.GetArraySize());
			for (size_t index = 0; index < caseCount; ++index)
			{
				ENGINE_TRY_ASSIGN(const JsonReader row, cases.GetElement(index));
				ENGINE_TRY_ASSIGN(const std::string suite, row.ReadMember<std::string>("suite"));
				if (!suite.starts_with(ReplaySuitePrefix))
					continue;
				ENGINE_TRY_ASSIGN(const std::string status, row.ReadMember<std::string>("status"));
				ReplayRows& replay = replays[suite.substr(ReplaySuitePrefix.size())];
				replay.Passed = replay.Passed && status == PassedStatus;
			}
			return replays;
		}

		// The report's "counts" (TestRunCounts, as TestRunResultToJson writes them).
		static Result<ExportFeatureTestCounts> ReadCounts(const JsonReader& root)
		{
			ENGINE_TRY_ASSIGN(const JsonReader counts, root.GetMember("counts"));
			ExportFeatureTestCounts read;
			ENGINE_TRY_ASSIGN(read.Total, counts.ReadMember<uint32_t>("total"));
			ENGINE_TRY_ASSIGN(read.Passed, counts.ReadMember<uint32_t>("passed"));
			ENGINE_TRY_ASSIGN(read.Failed, counts.ReadMember<uint32_t>("failed"));
			ENGINE_TRY_ASSIGN(read.Error, counts.ReadMember<uint32_t>("error"));
			ENGINE_TRY_ASSIGN(read.Timeout, counts.ReadMember<uint32_t>("timeout"));
			ENGINE_TRY_ASSIGN(read.Skipped, counts.ReadMember<uint32_t>("skipped"));
			ENGINE_TRY_ASSIGN(read.Quit, counts.ReadMember<uint32_t>("quit"));
			return read;
		}

		Result<ExportFeatureTestReport> ReadExportFeatureTestReport(std::string_view text, int exitCode)
		{
			ENGINE_TRY_ASSIGN(const Json document, JsonReader::Parse(text));
			const JsonReader root(document);
			ENGINE_TRY(root.ExpectType(JsonType::Object));
			ENGINE_TRY_ASSIGN(const bool passed, root.ReadMember<bool>("passed"));
			ENGINE_TRY_ASSIGN(const ExportFeatureTestCounts counts, ReadCounts(root));
			ENGINE_TRY_ASSIGN(ReplayRowMap rows, ReadReplayRows(root));
			ExportFeatureTestReport report;
			report.Ran = true;
			report.ExitCode = exitCode;
			report.Passed = exitCode == 0 && passed;
			report.Counts = counts;
			report.Replays.reserve(rows.size());
			for (auto& [path, replay] : rows)
				report.Replays.push_back({ .Path = path, .StateHash = std::move(replay.StateHash), .Passed = replay.HasSuiteRow && replay.Passed });
			return report;
		}

	}

}
