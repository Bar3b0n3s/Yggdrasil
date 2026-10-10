#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Project/ProjectSettings.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	class TypeRegistry;

	struct TestFailureResult
	{
		std::string Message{};
		std::string File{};
		uint32_t Line = 0;
		std::string JsonPointer{}; // Embedded source, e.g. /Expect/0/Luau; empty for an ordinary script file.
	};

	// Architecture §11.10, identical in RPC, editor CLI and exported FeatureTest. Status is deliberately a constrained
	// string: the specified wire spellings are lowercase, unlike ordinary reflected enum names. Exactly one of
	// passed, failed, error, timeout, skipped, quit. Failures retains every Expect* issue; the flat fields summarize the
	// first, including its JsonPointer. Embedded locations retain the real replay File and decoded Luau Line, never
	// a generated wrapper location. A quit row carries QuitCode and QuitExpected, decided against the suite's ExpectQuit.
	struct TestCaseResult
	{
		std::string Suite{};
		std::string Case{};
		std::string Status = "error";
		std::string Message{};
		std::string File{};
		uint32_t Line = 0;
		std::string JsonPointer{};
		uint32_t Ticks = 0;
		std::vector<TestFailureResult> Failures{};
		int32_t QuitCode = 0;
		bool QuitExpected = false;
	};

	struct TestSuiteResult
	{
		std::string Suite{};
		std::string Scene{};   // Readable project path; empty for the documented empty-scene suite.
		std::string SceneId{}; // Canonical handle; empty for an empty scene.
		std::string Script{};
		TestSuiteSettings::Mode Mode = TestSuiteSettings::Mode::Editor;
		uint32_t Ticks = 0; // Total work across isolated cases, not just the final incarnation's tick.
		uint32_t Breaks = 0;
		uint32_t ScriptErrors = 0;
		std::string FinalStateHash{};
		bool Passed = false;
	};

	// Stable registry identity, never a VM pointer or registration index. Kind distinguishes function, method,
	// property read/write, operator, constructor, enum value, callback and component field read/write counters.
	// Other coverage producers use the same envelope in M14. Counts are the run's delta, not lifetime totals.
	struct TestCoverageCounter
	{
		std::string Id{};
		std::string Kind{};
		std::vector<TestSuiteSettings::Mode> Modes{};
		uint32_t Count = 0;
	};

	struct TestCoverageGateResult
	{
		std::string Gate{};
		bool Evaluated = false;
		bool Passed = false;
		std::vector<std::string> Missing{};
	};

	struct TestCoverageReport
	{
		std::string RegistryFingerprint{};
		std::vector<TestCoverageCounter> Counters{};
		std::vector<TestCoverageGateResult> Gates{};
	};

	struct TestRunResult
	{
		TestSuiteSettings::Mode Mode = TestSuiteSettings::Mode::Editor;
		bool Passed = false;
		bool Cancelled = false;
		std::vector<TestCaseResult> Cases{};
		std::vector<TestSuiteResult> Suites{};
		TestCoverageReport Coverage{};
		std::string JsonPath{};
		std::string JunitPath{};
		std::string RecordingPath{};
	};

	// Registers the result structs with camelCase keys and mandatory descriptions before TypeRegistry::Freeze.
	// No global registration: host owners explicitly add this call. Counts/ticks use saturated uint32 for the existing
	// registry; runner deadlines and replay files keep uint64. Canonical hashes are 16 lowercase hexadecimal digits.
	void RegisterTestResultTypes(TypeRegistry& registry);
	// Preserves jsonPointer on each case and each failures entry, including the empty string for ordinary sources.
	// Entries with the same file/line/message but different embedded pointers remain separate and in source order.
	[[nodiscard]] Result<Json> TestRunResultToJson(const TestRunResult& result, const TypeRegistry& registry);
	// Pure UTF-8 XML generation, escaping all user names/messages. failed -> failure; error/timeout -> error;
	// skipped -> skipped; quit uses QuitExpected and its accumulated Failures. Includes all diagnostic locations.
	// Diagnostic text uses File#JsonPointer:Line for embedded source, otherwise File:Line, retaining every failure.
	// It never reads wall time; reported duration is simulated time supplied by the result producer if added later.
	[[nodiscard]] Result<std::string> TestRunResultToJUnit(const TestRunResult& result);

}
