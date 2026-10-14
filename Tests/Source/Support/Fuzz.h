#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Random.h"
#include "Support/TestCaseTracker.h"

#include <doctest/doctest.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// Seeded mutation fuzzing for the unit and hardening stages (Architecture §15.1 T6, §15.2; Docs/Decisions/
// 0024-m15-contract.md decisions 13 and 14). Every fuzz case of a reader of external input carries
// doctest::test_suite(Test::FuzzSuite), draws iteration i's input from GetFuzzIterationSeed(caseSeed, i) alone, and runs
// the iterations GetFuzzRange returns, so:
//   - the unit stage runs each case's acceptance count in Release and the first tenth of it in Debug (Test.py passes
//     --fuzz-scale=0.1 to the Debug unit run, approved by the product owner in ADR 0024 decision 14), and every
//     iteration's input and outcome are independent of the configuration, the scale, the range and the shard;
//   - the hardening stage runs 1,000,000 iterations per reader (--fuzz-iterations=1000000) in shards across processes
//     (--fuzz-shard=<k>/<n>);
//   - a failure names the iteration (CAPTURE the index and its seed), which `Tests -tc=<case> --fuzz-iterations=<total>
//     --fuzz-index=<i>` replays alone (the commas of <case> escaped as "\,", GetFuzzReplayCommand).
// A case whose reader keeps state across calls (the play-session binding fuzz) resets, before every iteration, all the
// state its checks observe, so an iteration's outcome still depends only on its index; what else accumulates in the
// session (the tick count, the error history) is not an input of any check. The binding fuzz maps its indices as follows
// (decision 13): iteration i is a corpus call when i % 10 < 7 and a generated call otherwise; the corpus calls, numbered
// in index order, form rounds of 2 x the corpus size, each a permutation of every case over both transports seeded from
// (caseSeed, round); every call's arguments come from GetFuzzIterationSeed(caseSeed, i); its thresholds are ratios of the
// calls the process ran (accepted > 10 % and rejected > 50 % in each phase); and its minimum is the smallest count whose
// corpus calls complete the first round.
// ReportFuzzRun's log line, which the console sink writes to the process's standard error, is what Scripts/Hardening.py
// counts against Tests/Data/Fuzz/Targets.json. Fuzz case names contain no apostrophe (the line quotes them).
//
// The helpers below RunFuzzCase, IsFuzzSample, MutateBytes and MutateText (an extension of the frozen interface, recorded
// with stream C's result) give every case the same loop and mutation vocabulary, so a case states only its seed corpus,
// its reader and its oracle.

namespace Engine {

	namespace Test {

		// The doctest suite of the fuzz cases (--test-suite=Fuzz selects them; the unit stage runs them with the rest).
		inline constexpr const char* FuzzSuite = "Fuzz";

		// The iteration indices one process runs: [Begin, End) of [0, Total).
		struct FuzzRange
		{
			uint64_t Begin = 0;
			uint64_t End = 0;
			uint64_t Total = 0;
		};

		// The iterations of a case whose acceptance count is `acceptance` (the count its name states, run by the unit
		// stage): Total is --fuzz-iterations when given, else max(minimum, round(acceptance * --fuzz-scale)); with
		// --fuzz-shard=<k>/<n>, [Begin, End) is the k-th of n contiguous slices of [0, Total) (the first Total % n slices one
		// longer); with --fuzz-index=<i>, [i, i + 1) (a failed REQUIRE when i >= Total); otherwise all of it. `minimum` keeps
		// a scaled run meaningful (for example a corpus that must be visited whole). Reads GetTestOptions().
		[[nodiscard]] FuzzRange GetFuzzRange(uint64_t acceptance, uint64_t minimum = 1);

		// The seed of iteration `index` of a case seeded with `caseSeed`: SplitMix64's output for the state
		// caseSeed + (index + 1) * 0x9E3779B97F4A7C15, so it depends on nothing else (not the range, shard, count or
		// configuration). Pure.
		[[nodiscard]] uint64_t GetFuzzIterationSeed(uint64_t caseSeed, uint64_t index);

		// What a case's iterations did: accepted inputs (the reader returned a value) and rejected ones (an error Result).
		struct FuzzCounts
		{
			uint64_t Accepted = 0;
			uint64_t Rejected = 0;
		};

		// Logs, at Info through the Engine logger, exactly one line per run of a case:
		// "Fuzz '<case>': iterations <Begin>..<End> of <Total> (seed 0x<16 hex>): <accepted> accepted, <rejected> rejected".
		// A case name holding an apostrophe fails the running test case instead.
		void ReportFuzzRun(std::string_view caseName, uint64_t caseSeed, const FuzzRange& range, const FuzzCounts& counts);

		// The iterations a run needs before a case's aggregate checks over its counts apply ("some inputs were rejected",
		// "some were accepted"): a property of a sample, which a --fuzz-index replay of one iteration is not.
		inline constexpr uint64_t FuzzSampleIterations = 100;

		// Whether `range` is a sample whose counts a case may check: not a --fuzz-index replay, and at least `iterations`
		// long. Reads GetTestOptions().
		[[nodiscard]] bool IsFuzzSample(const FuzzRange& range, uint64_t iterations = FuzzSampleIterations);

		// The name of the running doctest case (TestCaseTracker), which ReportFuzzRun quotes; empty outside a test case.
		[[nodiscard]] std::string GetRunningFuzzCaseName();

		// The command that replays iteration `index` of `total` of the running case alone:
		// Tests "--test-case=<name>" --fuzz-iterations=<total> --fuzz-index=<index>, with the commas of the name escaped as
		// doctest's filters need ("10\,000").
		[[nodiscard]] std::string GetFuzzReplayCommand(uint64_t total, uint64_t index);

		// Runs a fuzz case: for every index of GetFuzzRange(acceptance, minimum), `body(random, index)` with `random` seeded
		// with GetFuzzIterationSeed(caseSeed, index), CAPTUREd with its index, seed and the replay command; the body builds
		// the iteration's input from `random` alone, runs the reader and its oracle, and returns true when the reader
		// accepted the input (false when it rejected it with an error Result). Then ReportFuzzRun, and the range and counts
		// are returned for the case's aggregate checks (guarded by IsFuzzSample).
		struct FuzzRun
		{
			FuzzRange Range{};
			FuzzCounts Counts{};
		};

		template<typename Body>
		FuzzRun RunFuzzCase(uint64_t caseSeed, uint64_t acceptance, Body&& body, uint64_t minimum = 1)
		{
			FuzzRun run{ .Range = GetFuzzRange(acceptance, minimum) };
			for (uint64_t index = run.Range.Begin; index < run.Range.End; ++index)
			{
				const uint64_t seed = GetFuzzIterationSeed(caseSeed, index);
				CAPTURE(index);
				CAPTURE(seed);
				INFO("replay this iteration alone: ", GetFuzzReplayCommand(run.Range.Total, index));
				Random random(seed);
				if (body(random, index))
					++run.Counts.Accepted;
				else
					++run.Counts.Rejected;
			}
			ReportFuzzRun(GetRunningFuzzCaseName(), caseSeed, run.Range, run.Counts);
			return run;
		}

		// A case's seed corpus (decision 13: Tests/Data/Assets/**, Tests/Data/Scenes/**, Tests/Data/Project/AllSettings.eproj and
		// FeatureTest's metas, replays and .sfx files): the bytes of every regular file below `root` whose extension is
		// `extension` (".meta", compared exactly), in the order of their sorted paths, read once before the iterations.
		// REQUIREs a readable directory and at least one file.
		[[nodiscard]] std::vector<std::vector<std::byte>> ReadFuzzSeeds(const std::filesystem::path& root, std::string_view extension);
		// ReadFuzzSeeds as text.
		[[nodiscard]] std::vector<std::string> ReadFuzzSeedTexts(const std::filesystem::path& root, std::string_view extension);

		// One seeded mutation of binary input, drawn from `random` alone: flipped bits, a truncation, an extreme 8-, 16-,
		// 32- or 64-bit value written over a field, inserted random bytes, a removed or duplicated chunk, or bytes copied
		// from elsewhere in the input; one to three of them in a row. An empty input only grows.
		void MutateBytes(std::vector<std::byte>& bytes, Random& random);

		// One seeded mutation of text input (JSON, Luau, .meta), drawn from `random` alone: the binary mutations of
		// MutateBytes on its bytes (so invalid UTF-8 occurs), or a token-level edit: an extreme number written over a
		// number, a structural character ({}[]:,") inserted or removed, a duplicated or removed line, or a quoted string
		// replaced by another value; one to three of them in a row.
		void MutateText(std::string& text, Random& random);

	}

}
