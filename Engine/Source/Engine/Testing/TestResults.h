#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Project/ProjectSettings.h"

#include <cstdint>
#include <string>
#include <string_view>
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
	// passed, failed, error, timeout, skipped, quit. Failures retains every Expect* issue and unexpected script error within
	// MaxScriptTestFailuresPerCase per row and MaxScriptTestFailuresPerSuite per suite (Scripting/ScriptLimits.h); a row
	// past a cap ends with a "[truncated: <n> more failures]" failure counting the rest. The flat fields summarize the
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

	// Stable registry identity, never a VM pointer or registration index. Id and Kind follow the table of
	// Testing/CoverageGates.h (Docs/Decisions/0022-m14-contract.md decision 2): every kind belongs to exactly one gate and
	// every id starts with its gate's prefix. Modes are the run modes the entry exists in. Counts are the run's delta, not
	// lifetime totals, saturated to uint32. A report lists every counter of its producers, covered or not, sorted by id.
	struct TestCoverageCounter
	{
		std::string Id{};
		std::string Kind{};
		std::vector<TestSuiteSettings::Mode> Modes{};
		uint32_t Count = 0;
	};

	// One per-mode gate of a coverage run (§15.6): Gate is CoverageGateName ("1", "1b", "2", "3", "4", "9"); Missing the
	// uncovered counter ids, sorted. Evaluated is false for a filtered, recording or cancelled run, and an unevaluated gate
	// never passes. An evaluated gate passes exactly when Missing is empty.
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

	// Report text made safe for every result format (JSON, JUnit XML, the RPC result): each invalid UTF-8 byte and the
	// XML noncharacters U+FFFE/U+FFFF become U+FFFD, and each C0 control character other than tab, line feed and carriage
	// return becomes the visible text \u00XX. Valid text without those is returned unchanged. The FeatureTestRunner applies
	// it to every row it reports, because names and messages come from scripts (Test.Fail, error(), Test.Case names).
	[[nodiscard]] std::string SanitizeTestReportText(std::string_view text);

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
	// M14 (ADR 0022 decision 3): when Coverage.Gates is not empty, a last <testsuite name="FeatureTest.coverage"> holds one
	// <testcase name="gate <Gate>"> per entry: a passed gate has no child, a failed one a <failure> whose message counts the
	// gaps and whose text lists every Missing id on its own line, an unevaluated one <skipped/>.
	[[nodiscard]] Result<std::string> TestRunResultToJUnit(const TestRunResult& result);

}
