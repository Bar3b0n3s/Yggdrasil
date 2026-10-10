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
		TEST_CASE("TestResults: all entry points use the same case suite and coverage schema" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("TestResults: every expectation failure retains its source location" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("TestResults: JUnit escapes names and distinguishes failures errors skips and expected quit" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("TestResults: JSON and JUnit preserve distinct embedded pointers at the same source line" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
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

		TEST_CASE("TestResults: coverage retains stable identities and per-mode availability" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}

		TEST_CASE("TestResults: unevaluated coverage gates never report success" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("M13 contract acceptance is not implemented");
		}
	}

}
