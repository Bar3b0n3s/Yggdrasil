#include "EnginePCH.h"
#include "Engine/Testing/TestResults.h"

#include "Engine/Core/Utf8.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/ScriptLimits.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <format>

namespace Engine {

	namespace {

		Status Validate(const TestRunResult& result)
		{
			constexpr std::array<std::string_view, 6> States = { "passed", "failed", "error", "timeout", "skipped", "quit" };
			for (const auto& row : result.Cases)
				if (std::find(States.begin(), States.end(), row.Status) == States.end())
					return MakeError(ErrorCode::Validation, "invalid test case status '{}'", row.Status);
			for (const auto& gate : result.Coverage.Gates)
				if (!gate.Evaluated && gate.Passed)
					return MakeError(ErrorCode::Validation, "unevaluated coverage gate '{}' cannot pass", gate.Gate);
			return {};
		}

		Result<std::string> Escape(std::string_view value)
		{
			if (!IsValidUtf8(value))
				return MakeError(ErrorCode::Validation, "JUnit text must be valid UTF-8");
			if (value.find("\xef\xbf\xbe") != std::string_view::npos || value.find("\xef\xbf\xbf") != std::string_view::npos)
				return MakeError(ErrorCode::Validation, "JUnit text contains a forbidden XML code point");
			std::string result;
			for (char c : value)
			{
				switch (c)
				{
					case '&':  result += "&amp;"; break;
					case '<':  result += "&lt;"; break;
					case '>':  result += "&gt;"; break;
					case '"':  result += "&quot;"; break;
					case '\'': result += "&apos;"; break;
					default:
						if (static_cast<unsigned char>(c) < 32 && c != '\t' && c != '\r' && c != '\n')
							return MakeError(ErrorCode::Validation, "JUnit text contains an XML control character");
						result += c;
				}
			}
			return result;
		}

		bool IsFailure(const TestCaseResult& row)
		{
			return row.Status == "failed" || (row.Status == "quit" && (!row.QuitExpected || !row.Failures.empty()));
		}

		std::string Location(const TestFailureResult& failure)
		{
			std::string result = failure.File;
			if (!failure.JsonPointer.empty())
				result += "#" + failure.JsonPointer;
			if (failure.Line != 0)
				result += std::format(":{}", failure.Line);
			if (!result.empty())
				result += ": ";
			return result + failure.Message;
		}

	}

	std::string SanitizeTestReportText(std::string_view text)
	{
		constexpr std::string_view Replacement = "\xEF\xBF\xBD"; // U+FFFD
		std::string result;
		result.reserve(text.size());
		while (!text.empty())
		{
			const size_t valid = FindInvalidUtf8(text);
			const std::string_view prefix = text.substr(0, valid);
			for (size_t index = 0; index < prefix.size(); ++index)
			{
				const auto byte = static_cast<unsigned char>(prefix[index]);
				if (byte < 0x20 && byte != '\t' && byte != '\n' && byte != '\r')
					result += std::format("\\u{:04X}", static_cast<unsigned int>(byte));
				else if (byte == 0xEF && (prefix.substr(index, 3) == "\xEF\xBF\xBE" || prefix.substr(index, 3) == "\xEF\xBF\xBF"))
				{
					result += Replacement;
					index += 2;
				}
				else
					result += prefix[index];
			}
			if (valid == text.size())
				break;
			// One replacement per invalid byte; the next byte starts a new attempt.
			result += Replacement;
			text.remove_prefix(valid + 1);
		}
		return result;
	}

	TestRunResult MakeFailedTestRunResult(TestSuiteSettings::Mode mode, std::string_view message)
	{
		TestRunResult result;
		result.Mode = mode;
		result.Passed = false;
		result.Cases.push_back(TestCaseResult{ .Suite = "FeatureTest", .Case = "run", .Status = "error", .Message = SanitizeTestReportText(message) });
		SummarizeTestRunCases(result);
		return result;
	}

	void SummarizeTestRunCases(TestRunResult& result)
	{
		const auto saturate = [](uint32_t& count)
		{
			if (count != UINT32_MAX)
				++count;
		};
		result.Counts = TestRunCounts{};
		result.FailedCases.clear();
		for (const TestCaseResult& row : result.Cases)
		{
			saturate(result.Counts.Total);
			if (row.Status == "passed")
				saturate(result.Counts.Passed);
			else if (row.Status == "failed")
				saturate(result.Counts.Failed);
			else if (row.Status == "error")
				saturate(result.Counts.Error);
			else if (row.Status == "timeout")
				saturate(result.Counts.Timeout);
			else if (row.Status == "skipped")
				saturate(result.Counts.Skipped);
			else if (row.Status == "quit")
				saturate(result.Counts.Quit);
			const bool fails = IsFailure(row) || row.Status == "error" || row.Status == "timeout";
			if (fails && result.FailedCases.size() < MaxReportedFailedCases)
				result.FailedCases.push_back(std::format("{}/{}", row.Suite, row.Case));
		}
	}

	void RegisterTestResultTypes(TypeRegistry& registry)
	{
		registry.Struct<TestRunCounts>("TestRunCounts", "The rows of a test run by status.")
			.Field("total", &TestRunCounts::Total, "Every row of cases.")
			.Field("passed", &TestRunCounts::Passed, "Rows with status passed.")
			.Field("failed", &TestRunCounts::Failed, "Rows with status failed.")
			.Field("error", &TestRunCounts::Error, "Rows with status error.")
			.Field("timeout", &TestRunCounts::Timeout, "Rows with status timeout.")
			.Field("skipped", &TestRunCounts::Skipped, "Rows with status skipped.")
			.Field("quit", &TestRunCounts::Quit, "Rows with status quit.");
		registry.Struct<TestFailureResult>("TestFailureResult", "TestFailureResult shared test report.")
			.Field("message", &TestFailureResult::Message, "Message of the test report.")
			.Field("file", &TestFailureResult::File, "File of the test report.")
			.Field("line", &TestFailureResult::Line, "Line of the test report.")
			.Field("jsonPointer", &TestFailureResult::JsonPointer, "JsonPointer of the test report.");
		registry.Struct<TestCaseResult>("TestCaseResult", "TestCaseResult shared test report.")
			.Field("suite", &TestCaseResult::Suite, "Suite of the test report.")
			.Field("case", &TestCaseResult::Case, "Case of the test report.")
			.Field("status", &TestCaseResult::Status, "Status of the test report.")
			.Field("message", &TestCaseResult::Message, "Message of the test report.")
			.Field("file", &TestCaseResult::File, "File of the test report.")
			.Field("line", &TestCaseResult::Line, "Line of the test report.")
			.Field("jsonPointer", &TestCaseResult::JsonPointer, "JsonPointer of the test report.")
			.Field("ticks", &TestCaseResult::Ticks, "Ticks of the test report.")
			.Field("failures", &TestCaseResult::Failures, std::format("Failures of the test report: at most {} for expectations and unexpected script errors, the last then counting any further ones as [truncated: <n> more failures], and at most {} such failures per suite.", MaxScriptTestFailuresPerCase, MaxScriptTestFailuresPerSuite))
			.Field("quitCode", &TestCaseResult::QuitCode, "QuitCode of the test report.")
			.Field("quitExpected", &TestCaseResult::QuitExpected, "QuitExpected of the test report.");
		registry.Struct<TestSuiteResult>("TestSuiteResult", "TestSuiteResult shared test report.")
			.Field("suite", &TestSuiteResult::Suite, "Suite of the test report.")
			.Field("scene", &TestSuiteResult::Scene, "Scene of the test report.")
			.Field("sceneId", &TestSuiteResult::SceneId, "SceneId of the test report.")
			.Field("script", &TestSuiteResult::Script, "Script of the test report.")
			.Field("mode", &TestSuiteResult::Mode, "Mode of the test report.")
			.Field("ticks", &TestSuiteResult::Ticks, "Ticks of the test report.")
			.Field("breaks", &TestSuiteResult::Breaks, "Breaks of the test report.")
			.Field("scriptErrors", &TestSuiteResult::ScriptErrors, "ScriptErrors of the test report.")
			.Field("finalStateHash", &TestSuiteResult::FinalStateHash, "FinalStateHash of the test report.")
			.Field("passed", &TestSuiteResult::Passed, "Passed of the test report.")
			.Field("checkpointsChecked", &TestSuiteResult::CheckpointsChecked, "A replay suite: the state-hash checkpoints its playback compared, a checkpoint that differed included (all of them when it passed); 0 for a script suite.")
			.Field("failedCheckpointTick", &TestSuiteResult::FailedCheckpointTick, "A replay suite: the tick of the checkpoint whose state hash differed, where playback stopped (its Checkpoint@<tick> row); 0 when none differed.");
		registry.Struct<TestCoverageCounter>("TestCoverageCounter", "TestCoverageCounter shared test report.")
			.Field("id", &TestCoverageCounter::Id, "Id of the test report.")
			.Field("kind", &TestCoverageCounter::Kind, "Kind of the test report.")
			.Field("modes", &TestCoverageCounter::Modes, "Modes of the test report.")
			.Field("count", &TestCoverageCounter::Count, "Count of the test report.");
		registry.Struct<TestCoverageGateResult>("TestCoverageGateResult", "TestCoverageGateResult shared test report.")
			.Field("gate", &TestCoverageGateResult::Gate, "Gate of the test report.")
			.Field("evaluated", &TestCoverageGateResult::Evaluated, "Evaluated of the test report.")
			.Field("passed", &TestCoverageGateResult::Passed, "Passed of the test report.")
			.Field("missing", &TestCoverageGateResult::Missing, "Missing of the test report.");
		registry.Struct<TestCoverageReport>("TestCoverageReport", "TestCoverageReport shared test report.")
			.Field("registryFingerprint", &TestCoverageReport::RegistryFingerprint, "RegistryFingerprint of the test report.")
			.Field("counters", &TestCoverageReport::Counters, "Counters of the test report.")
			.Field("gates", &TestCoverageReport::Gates, "Gates of the test report.");
		registry.Struct<TestRecordingInputCheck>("TestRecordingInputCheck", "The input check of a written recording: the recording replayed without its input events.")
			.Field("ran", &TestRecordingInputCheck::Ran, "The check ran; false for a recording without input events, which has nothing to check.")
			.Field("divergedAtTick", &TestRecordingInputCheck::DivergedAtTick, "Where the run without input first differed from the recording: the first checkpoint whose state hash differed, else where that run ended (the final tick); 0 when the check did not run.");
		registry.Struct<TestRecordingResult>("TestRecordingResult", "The replay test.run {record} wrote.")
			.Field("path", &TestRecordingResult::Path, "The written replay, project-relative (recordingPath).")
			.Field("formatVersion", &TestRecordingResult::FormatVersion, "The Replay format version written.")
			.Field("ticks", &TestRecordingResult::Ticks, "The replay's final tick (FinalTick).")
			.Field("events", &TestRecordingResult::Events, "The input events written.")
			.Field("checkpoints", &TestRecordingResult::Checkpoints, "The state-hash checkpoints written, one every 60 ticks before the final tick (none at a tick inside a frame where the suite's scripted clock ran several steps).")
			.Field("finalStateHash", &TestRecordingResult::FinalStateHash, "The state hash at the final tick (FinalStateHash).")
			.Field("inputCheck", &TestRecordingResult::InputCheck, "The input check that let the recording through.")
			.Field("expectations", &TestRecordingResult::Expectations, "The expectations written (test.run's expect), each verified with the recording's input before it was written.")
			.Field("warnings", &TestRecordingResult::Warnings, "Notes on the written replay: the input check warns when its run without input differed only at checkpoints but reached the recorded final state hash, which then proves only the end state.");
		registry.Struct<TestRunResult>("TestRunResult", "TestRunResult shared test report.")
			.Field("mode", &TestRunResult::Mode, "Mode of the test report.")
			.Field("passed", &TestRunResult::Passed, "Passed of the test report.")
			.Field("cancelled", &TestRunResult::Cancelled, "Cancelled of the test report.")
			.Field("counts", &TestRunResult::Counts, "The rows of cases by status.")
			.Field("failedCases", &TestRunResult::FailedCases, std::format("\"<suite>/<case>\" of each row that fails the run (failed, error, timeout, or a quit that was not expected or has failures), in the order of cases, at most {}.", MaxReportedFailedCases))
			.Field("cases", &TestRunResult::Cases, "Cases of the test report.")
			.Field("suites", &TestRunResult::Suites, "Suites of the test report.")
			.Field("coverage", &TestRunResult::Coverage, "The script API coverage: registryFingerprint and gates always; the counters in the JSON report always, and in test.run's result only with coverage: true or Testing.CoverageGates (left out, an empty list, otherwise).")
			.Field("jsonPath", &TestRunResult::JsonPath, "JsonPath of the test report.")
			.Field("junitPath", &TestRunResult::JunitPath, "JunitPath of the test report.")
			.Field("recordingPath", &TestRunResult::RecordingPath, "RecordingPath of the test report.")
			.Field("recording", &TestRunResult::Recording, "The replay test.run {record} wrote: {path, formatVersion, ticks, events, checkpoints, finalStateHash, inputCheck: {ran, divergedAtTick}, expectations, warnings} (TestRecordingResult); null for every other run.");
	}

	Result<Json> TestRunResultToJson(const TestRunResult& result, const TypeRegistry& registry)
	{
		ENGINE_TRY(Validate(result));
		const auto* type = registry.FindStruct<TestRunResult>();
		if (!type)
			return MakeError(ErrorCode::InvalidState, "TestRunResult is not registered");
		return type->ToJson(&result);
	}

	Result<std::string> TestRunResultToJUnit(const TestRunResult& result)
	{
		ENGINE_TRY(Validate(result));
		size_t failures = 0, errors = 0, skipped = 0;
		for (const auto& row : result.Cases)
		{
			failures += IsFailure(row) ? 1 : 0;
			errors += row.Status == "error" || row.Status == "timeout" ? 1 : 0;
			skipped += row.Status == "skipped" ? 1 : 0;
		}
		// The coverage gates (ADR 0022 decision 3) form a suite of their own, counted in the document's totals.
		size_t gateFailures = 0, gatesSkipped = 0;
		for (const auto& gate : result.Coverage.Gates)
		{
			gateFailures += gate.Evaluated && !gate.Passed ? 1 : 0;
			gatesSkipped += gate.Evaluated ? 0 : 1;
		}
		const size_t gates = result.Coverage.Gates.size();
		std::string xml = std::format("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuites tests=\"{}\" failures=\"{}\" errors=\"{}\" skipped=\"{}\">\n<testsuite name=\"FeatureTest\" tests=\"{}\" failures=\"{}\" errors=\"{}\" skipped=\"{}\">\n",
			result.Cases.size() + gates, failures + gateFailures, errors, skipped + gatesSkipped, result.Cases.size(), failures, errors, skipped);
		for (const auto& row : result.Cases)
		{
			ENGINE_TRY_ASSIGN(auto suite, Escape(row.Suite));
			ENGINE_TRY_ASSIGN(auto name, Escape(row.Case));
			xml += std::format("<testcase classname=\"{}\" name=\"{}\">", suite, name);
			const std::string_view tag = row.Status == "skipped" ? "skipped" : row.Status == "error" || row.Status == "timeout" ? "error"
				: IsFailure(row)                                                                                                ? "failure"
																																: "";
			if (!tag.empty())
			{
				const auto append = [&xml, tag](const TestFailureResult& failure) -> Status
				{
					ENGINE_TRY_ASSIGN(auto message, Escape(failure.Message));
					ENGINE_TRY_ASSIGN(auto diagnostic, Escape(Location(failure)));
					xml += std::format("<{} message=\"{}\">{}</{}>", tag, message, diagnostic, tag);
					return {};
				};
				if (row.Failures.empty())
					ENGINE_TRY(append({ row.Message, row.File, row.Line, row.JsonPointer }));
				else
					for (const auto& failure : row.Failures)
						ENGINE_TRY(append(failure));
			}
			xml += "</testcase>\n";
		}
		xml += "</testsuite>\n";
		if (gates != 0)
		{
			xml += std::format("<testsuite name=\"FeatureTest.coverage\" tests=\"{}\" failures=\"{}\" errors=\"0\" skipped=\"{}\">\n", gates, gateFailures, gatesSkipped);
			for (const auto& gate : result.Coverage.Gates)
			{
				ENGINE_TRY_ASSIGN(auto name, Escape(gate.Gate));
				xml += std::format("<testcase name=\"gate {}\" classname=\"FeatureTest.coverage\">", name);
				if (!gate.Evaluated)
					xml += "<skipped message=\"only an unfiltered, non-recording run that completes evaluates the coverage gates\"/>";
				else if (!gate.Passed)
				{
					std::string missing;
					for (const auto& id : gate.Missing)
					{
						ENGINE_TRY_ASSIGN(auto escaped, Escape(id));
						if (!missing.empty())
							missing += '\n';
						missing += escaped;
					}
					xml += std::format("<failure message=\"{} uncovered {}\">{}</failure>", gate.Missing.size(), gate.Missing.size() == 1 ? "entry" : "entries", missing);
				}
				xml += "</testcase>\n";
			}
			xml += "</testsuite>\n";
		}
		xml += "</testsuites>\n";
		return xml;
	}

}
