#include "EnginePCH.h"
#include "Engine/Testing/TestResults.h"

#include "Engine/Core/Utf8.h"
#include "Engine/Reflection/TypeRegistry.h"

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

	void RegisterTestResultTypes(TypeRegistry& registry)
	{
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
			.Field("failures", &TestCaseResult::Failures, "Failures of the test report.")
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
			.Field("passed", &TestSuiteResult::Passed, "Passed of the test report.");
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
		registry.Struct<TestRunResult>("TestRunResult", "TestRunResult shared test report.")
			.Field("mode", &TestRunResult::Mode, "Mode of the test report.")
			.Field("passed", &TestRunResult::Passed, "Passed of the test report.")
			.Field("cancelled", &TestRunResult::Cancelled, "Cancelled of the test report.")
			.Field("cases", &TestRunResult::Cases, "Cases of the test report.")
			.Field("suites", &TestRunResult::Suites, "Suites of the test report.")
			.Field("coverage", &TestRunResult::Coverage, "Coverage of the test report.")
			.Field("jsonPath", &TestRunResult::JsonPath, "JsonPath of the test report.")
			.Field("junitPath", &TestRunResult::JunitPath, "JunitPath of the test report.")
			.Field("recordingPath", &TestRunResult::RecordingPath, "RecordingPath of the test report.");
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
		std::string xml = std::format("<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<testsuites tests=\"{}\" failures=\"{}\" errors=\"{}\" skipped=\"{}\">\n<testsuite name=\"FeatureTest\" tests=\"{}\" failures=\"{}\" errors=\"{}\" skipped=\"{}\">\n",
			result.Cases.size(), failures, errors, skipped, result.Cases.size(), failures, errors, skipped);
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
		xml += "</testsuite>\n</testsuites>\n";
		return xml;
	}

}
