#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/VariantValue.h"

#include <cstddef>
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
		// A replay suite (M16 friction S3-04, additive): the state-hash checkpoints its playback compared, a checkpoint that
		// differed included (ReplayPlaybackResult::CheckpointsChecked), and that checkpoint's tick; 0 when none differed
		// (checkpoint ticks are positive). Both 0 for a script suite.
		uint32_t CheckpointsChecked = 0;
		uint32_t FailedCheckpointTick = 0;
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

	// The rows of a run's Cases by Status (SummarizeTestRunCases), so a reader sees the outcome without the rows: an
	// offloaded test.run result's summary repeats these small members (ResultOffload.h, MakeOffloadSummary).
	struct TestRunCounts
	{
		uint32_t Total = 0;
		uint32_t Passed = 0;
		uint32_t Failed = 0;
		uint32_t Error = 0;
		uint32_t Timeout = 0;
		uint32_t Skipped = 0;
		uint32_t Quit = 0;
	};

	// The most names TestRunResult::FailedCases lists; Counts still counts every row.
	inline constexpr size_t MaxReportedFailedCases = 20;

	// The input check of a written recording (Architecture §13.6): Ran is false for a recording without input events, which
	// has nothing to check. DivergedAtTick is where the recording, replayed without its input, first differed from it: the
	// tick of the first checkpoint whose state hash differed, else where that playback ended (the final tick, or where
	// gameplay stopped); 0 when the check did not run.
	struct TestRecordingInputCheck
	{
		bool Ran = false;
		uint32_t DivergedAtTick = 0;
	};

	// The replay a test.run {record} wrote (M16 friction S3-03), as TestRunResult::Recording reports it: the written path
	// (TestRunResult::RecordingPath's), the replay's format version, its final tick, input events, state-hash checkpoints and
	// final state hash, and its input check. Counters saturate to uint32 like every result counter.
	struct TestRecordingResult
	{
		std::string Path{};
		uint32_t FormatVersion = 0;
		uint32_t Ticks = 0;
		uint32_t Events = 0;
		uint32_t Checkpoints = 0;
		std::string FinalStateHash{};
		TestRecordingInputCheck InputCheck{};
	};

	struct TestRunResult
	{
		TestSuiteSettings::Mode Mode = TestSuiteSettings::Mode::Editor;
		bool Passed = false;
		bool Cancelled = false;
		TestRunCounts Counts{};
		// "<Suite>/<Case>" of each row that fails the run (failed, error, timeout, or a quit that was not expected or has
		// failures), in Cases order, at most MaxReportedFailedCases.
		std::vector<std::string> FailedCases{};
		std::vector<TestCaseResult> Cases{};
		std::vector<TestSuiteResult> Suites{};
		TestCoverageReport Coverage{};
		std::string JsonPath{};
		std::string JunitPath{};
		std::string RecordingPath{};
		// The replay test.run {record} wrote, a serialized TestRecordingResult (additive, M16 friction S3-03); null for
		// every other run, a refused recording included. The FeatureTestRunner leaves it null: the host that writes the
		// recording fills it in (TestMethods).
		VariantValue Recording{};
	};

	// Report text made safe for every result format (JSON, JUnit XML, the RPC result): each invalid UTF-8 byte and the
	// XML noncharacters U+FFFE/U+FFFF become U+FFFD, and each C0 control character other than tab, line feed and carriage
	// return becomes the visible text \u00XX. Valid text without those is returned unchanged. The FeatureTestRunner applies
	// it to every row it reports, because names and messages come from scripts (Test.Fail, error(), Test.Case names).
	[[nodiscard]] std::string SanitizeTestReportText(std::string_view text);

	// The result a report is written from when a run ends before the runner produced one (a preflight error such as script
	// type errors blocking discovery, a refused admission or a failed Begin), so the report files always describe the run
	// that was asked for and never stay absent or stale (--run-tests and --feature-test, ADR 0022 decision 7 and
	// "Integration"): mode `mode`, Passed false, one error row (Suite "FeatureTest", Case "run", Message the sanitized
	// `message`), summarized (SummarizeTestRunCases), no suites and no coverage. Every reader sees a failed run: JUnit an
	// <error>, the feature suite and the merge's `passed` rule a report that did not pass.
	[[nodiscard]] TestRunResult MakeFailedTestRunResult(TestSuiteSettings::Mode mode, std::string_view message);

	// Sets result.Counts and result.FailedCases from result.Cases (replacing what they held). The FeatureTestRunner's
	// GetResult and MakeFailedTestRunResult call it, so every report of every mode carries them.
	void SummarizeTestRunCases(TestRunResult& result);

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
