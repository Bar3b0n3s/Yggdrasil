#include "TestsPCH.h"

#include "Engine/Testing/TestResults.h"

#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <cstddef>
#include <string>

namespace Engine {

	TEST_SUITE("Testing")
	{
		TEST_CASE("TestResults: all entry points use the same case suite and coverage schema")
		{
			TypeRegistry registry;
			RegisterProjectSettingsTypes(registry);
			RegisterTestResultTypes(registry);
			registry.Freeze();
			TestRunResult result;
			result.Passed = true;
			result.Cases.push_back({ .Suite = "S", .Case = "C", .Status = "passed" });
			result.Suites.push_back({ .Suite = "S", .Passed = true });
			auto json = TestRunResultToJson(result, registry);
			REQUIRE(json);
			JsonReader root(*json);
			for (auto key : { "mode", "passed", "cancelled", "cases", "suites", "coverage", "jsonPath", "junitPath", "recordingPath" })
				CHECK(root.HasMember(key));
			auto cases = root.GetMember("cases");
			REQUIRE(cases);
			auto row = cases->GetElement(0);
			REQUIRE(row);
			CHECK(row->ReadMember<std::string>("status").value_or("") == "passed");
		}

		TEST_CASE("TestResults: every expectation failure retains its source location")
		{
			TestRunResult result;
			TestCaseResult row;
			row.Status = "failed";
			row.Failures = { { "first", "Assets/A.luau", 3, {} }, { "second", "Assets/B.luau", 9, {} } };
			result.Cases.push_back(row);
			auto xml = TestRunResultToJUnit(result);
			REQUIRE(xml);
			CHECK(xml->find("Assets/A.luau:3: first") != std::string::npos);
			CHECK(xml->find("Assets/B.luau:9: second") != std::string::npos);
		}

		TEST_CASE("TestResults: JUnit escapes names and distinguishes failures errors skips and expected quit")
		{
			TestRunResult result;
			for (const auto* status : { "passed", "failed", "error", "timeout", "skipped", "quit" })
			{
				TestCaseResult row;
				row.Status = status;
				row.Suite = "<suite&";
				row.Case = "\"case\"";
				row.Message = "<why&";
				row.QuitExpected = true;
				result.Cases.push_back(row);
			}
			auto xml = TestRunResultToJUnit(result);
			REQUIRE(xml);
			CHECK(xml->find("failures=\"1\" errors=\"2\" skipped=\"1\"") != std::string::npos);
			CHECK(xml->find("&lt;suite&amp;") != std::string::npos);
			CHECK(xml->find("&quot;case&quot;") != std::string::npos);
			result.Cases.back().Failures.push_back({ "prior", {}, 0, {} });
			xml = TestRunResultToJUnit(result);
			REQUIRE(xml);
			CHECK(xml->find("failures=\"2\"") != std::string::npos);
		}

		TEST_CASE("TestResults: JSON and JUnit preserve distinct embedded pointers at the same source line")
		{
			TypeRegistry registry;
			RegisterProjectSettingsTypes(registry);
			RegisterTestResultTypes(registry);
			registry.Freeze();

			TestFailureResult first;
			first.Message = "expected true";
			first.File = "Assets/Tests/Embedded.replay";
			first.Line = 1;
			first.JsonPointer = "/Expect/0/Luau";
			TestFailureResult second = first;
			second.JsonPointer = "/Expect/1/Luau";
			TestCaseResult testCase;
			testCase.Suite = "replay:Assets/Tests/Embedded.replay";
			testCase.Case = "Expect@1";
			testCase.Status = "failed";
			testCase.Message = first.Message;
			testCase.File = first.File;
			testCase.Line = first.Line;
			testCase.JsonPointer = first.JsonPointer;
			testCase.Failures = { first, second };
			TestRunResult result;
			result.Cases = { testCase };

			const auto json = TestRunResultToJson(result, registry);
			REQUIRE(json.has_value());
			const auto cases = JsonReader(*json).GetMember("cases");
			REQUIRE(cases.has_value());
			const auto row = cases->GetElement(0);
			REQUIRE(row.has_value());
			const auto flatPointer = row->ReadMember<std::string>("jsonPointer");
			REQUIRE(flatPointer.has_value());
			CHECK(*flatPointer == first.JsonPointer);
			const auto failures = row->GetMember("failures");
			REQUIRE(failures.has_value());
			const auto failureCount = failures->GetArraySize();
			REQUIRE(failureCount.has_value());
			REQUIRE(*failureCount == 2);
			for (size_t index = 0; index < testCase.Failures.size(); ++index)
			{
				const auto failure = failures->GetElement(index);
				REQUIRE(failure.has_value());
				const auto pointer = failure->ReadMember<std::string>("jsonPointer");
				REQUIRE(pointer.has_value());
				CHECK(*pointer == testCase.Failures[index].JsonPointer);
			}

			const auto junit = TestRunResultToJUnit(result);
			REQUIRE(junit.has_value());
			CHECK(junit->find("Assets/Tests/Embedded.replay#/Expect/0/Luau:1") != std::string::npos);
			CHECK(junit->find("Assets/Tests/Embedded.replay#/Expect/1/Luau:1") != std::string::npos);
		}

		TEST_CASE("TestResults: coverage retains stable identities and per-mode availability")
		{
			TypeRegistry registry;
			RegisterProjectSettingsTypes(registry);
			RegisterTestResultTypes(registry);
			registry.Freeze();
			TestRunResult result;
			result.Coverage.RegistryFingerprint = "stable";
			result.Coverage.Counters.push_back({ "Test.ReloadScript", "function", { TestSuiteSettings::Mode::Editor }, 7 });
			auto json = TestRunResultToJson(result, registry);
			REQUIRE(json);
			auto coverage = JsonReader(*json).GetMember("coverage");
			REQUIRE(coverage);
			CHECK(coverage->ReadMember<std::string>("registryFingerprint").value_or("") == "stable");
			auto counters = coverage->GetMember("counters");
			REQUIRE(counters);
			auto row = counters->GetElement(0);
			REQUIRE(row);
			CHECK(row->ReadMember<std::string>("id").value_or("") == "Test.ReloadScript");
			CHECK(row->ReadMember<uint32_t>("count").value_or(0) == 7);
		}

		TEST_CASE("TestResults: unevaluated coverage gates never report success")
		{
			TypeRegistry registry;
			RegisterProjectSettingsTypes(registry);
			RegisterTestResultTypes(registry);
			registry.Freeze();
			TestRunResult result;
			result.Coverage.Gates.push_back({ "M14", false, false, {} });
			CHECK(TestRunResultToJson(result, registry));
			result.Coverage.Gates[0].Passed = true;
			CHECK_FALSE(TestRunResultToJson(result, registry));
			CHECK_FALSE(TestRunResultToJUnit(result));
		}

		TEST_CASE("TestResults: JUnit rejects invalid XML characters and preserves Unicode")
		{
			TestRunResult result;
			result.Cases.push_back({ .Suite = "Unicode ☃", .Case = "case", .Status = "failed", .Message = "diagnostic ☃" });
			auto xml = TestRunResultToJUnit(result);
			REQUIRE(xml);
			CHECK(xml->find("diagnostic ☃") != std::string::npos);
			for (const auto& text : { std::string(1, '\0'), std::string("\xef\xbf\xbe"), std::string("\xef\xbf\xbf"), std::string(1, static_cast<char>(0xff)) })
			{
				result.Cases[0].Message = text;
				CHECK_FALSE(TestRunResultToJUnit(result));
			}
		}
	}

}
