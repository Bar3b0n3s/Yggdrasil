#pragma once

#include "Engine/Core/Base.h"

#include <cstdint>
#include <string_view>

// Seeded mutation fuzzing for the unit and hardening stages (Architecture §15.1 T6, §15.2; Docs/Decisions/
// 0024-m15-contract.md decisions 13 and 14). Every fuzz case of a reader of external input carries
// doctest::test_suite(Test::FuzzSuite), draws iteration i's input from GetFuzzIterationSeed(caseSeed, i) alone, and runs
// the iterations GetFuzzRange returns, so:
//   - the unit stage runs each case's acceptance count, and every iteration's input and outcome are independent of the
//     configuration, the range and the shard (whether the Debug unit run executes a prefix, --fuzz-scale, awaits the
//     product owner's decision; until then it runs the full count, ADR 0024 decision 14);
//   - the hardening stage runs 1,000,000 iterations per reader (--fuzz-iterations=1000000) in shards across processes
//     (--fuzz-shard=<k>/<n>);
//   - a failure names the iteration (CAPTURE the index and its seed), which `Tests -tc=<case> --fuzz-iterations=<total>
//     --fuzz-index=<i>` replays alone.
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

	}

}
