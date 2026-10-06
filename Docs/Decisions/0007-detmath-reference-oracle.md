# 0007 — DetMath accuracy is measured against correctly rounded references

- **Status:** accepted
- **Date:** 2026-10-06
- **Context:** The M1 acceptance test "DetMath: within 2 ULP of std over seeded tables" compared DetMath with the C++ standard library (`std::sin` to `std::pow`) over seeded inputs. On the macOS arm64 CI runner its `double` subcase failed with "worst 3 ULP", in Debug and in Release. On the same runner "DetMath: output hash over 1,000,000 seeded inputs matches the committed value" passed with `0x1f03563cdf5ec2cf`, so DetMath's outputs there are bit-identical to those on Windows and Linux. The 3 ULP therefore came from the oracle, Apple's libm, and not from DetMath.

## Why the platform's libm cannot be the oracle

- **It is a different implementation on every platform.** The MSVC CRT, glibc and Apple's libm are separate code bases. None of them promises correct rounding, and each has its own error distribution per function and domain. On the sampled domains glibc and the MSVC CRT happened to stay within 2 ULP of DetMath, and Apple's libm did not.
- **The verdict measured the libm, not DetMath.** DetMath's results are the same on every platform by design (§4.12), so a check that passes on two platforms and fails on the third with identical DetMath results is a statement about that platform's library. Any bound against it either fails on whichever C runtime is least accurate, today or on a future platform, or must grow until it no longer detects a real DetMath regression.
- **It cannot be tightened.** DetMath is far more accurate than the bound (ADR 0003, decision 21: within 0.5005 ULP of exact results, 0.72 ULP for subnormal `Exp` results). A libm oracle hides any regression smaller than its own error.

The failure was also hard to diagnose: the message printed the function name as a pointer ("double 0x10121a529"). The name was a `const char*` field, and doctest stringifies character pointers as pointers unless `DOCTEST_CONFIG_TREAT_CHAR_STAR_AS_STRING` is defined. The message only appeared on macOS because only macOS failed.

## Decision

### 1. The oracle is the correctly rounded result

The reference for an input is its exact mathematical result rounded to nearest, ties to even, in the input's format. A float reference is rounded from the exact value, never through double. `Scripts/GenerateDetMathReference.py` (standard library only) computes the references:

- Inputs are IEEE values, so each is an exact dyadic rational, converted to a `Decimal` with integer arithmetic.
- Each function is evaluated with the `decimal` module at a working precision of 40 digits or more, with a rigorous bound on the absolute error. The bound is accumulated with upward rounding: one unit per correctly rounded operation (`exp`, `ln`, `log10` and `sqrt` are correctly rounded in `decimal`), plus explicit truncation bounds for the Taylor series of sine, cosine, arc tangent and small hyperbolic sines.
- Trigonometric arguments are reduced exactly, by k pi/2 with pi (from Machin's formula in integers, 1,700 places) known to more places than the argument has integer digits plus the working precision. This does for the references what the exact integer Payne-Hanek reduction does in DetMath, up to 1.8e308.
- Both ends of the error interval are rounded with an exact rational round-to-nearest-even routine, including gradual underflow and overflow. When the ends disagree, the precision doubles (Ziv's strategy) up to 1,280 digits, and a case that is still undecided stops the generator. None of the 29,658 committed cases needed more than 40 digits.
- An exact result on a rounding boundary would never be decided by an interval, so the generator recognizes them first and computes them with rational arithmetic. These are the zeros of `log 1`, `log10 1` and `acos 1`, `log10` of the exact powers of ten, and every rational power: integer exponents, and dyadic exponents of perfect powers, such as 2^-1075, which ties to 0.
- Every rounding must happen in a context that the bounds account for. Python's `abs()`, unary minus and unary plus on a `Decimal` round to the thread's context instead, so evaluation runs under a thread context that traps any inexact result.
- **Self-test before every run:**
  - the rounding routine against round trips, ties, the subnormal range and overflow, and against the host's own correctly rounded integer division and double-to-float conversion;
  - pi's leading digits;
  - the recognized exact cases;
  - the correctly rounded values that `DetMathTests.cpp` already commits for huge trigonometric arguments and notable arguments. DetMath's authors computed those independently (60-digit arithmetic with a 780-digit pi), and the generator reproduces all of them.
- **Reproducibility:**
  - Inputs come from SplitMix64, seeded per region, and every value is formed and rounded with integer arithmetic. The output is byte-for-byte identical on every platform with Python 3.10 or later.
  - A full run takes about 2 seconds, and `--check` compares a fresh run with the committed file.

### 2. The references are a generated header

The cases are committed as `Tests/Source/Engine/Core/DetMathReferenceData.h`: one `constexpr` table of bit patterns per function and format, 800 to 1,300 cases each, 29,658 in all (1.2 MB). A comment above each table lists its regions.

- **Not a fixture under `Tests/Data/`:** the Tests binary has no way to locate `Tests/Data` before M3 (ADR 0003, decision 27).
- **A `.h` file:** CodeStyle §4.1 allows only `.h` and `.cpp`. The data sits inside `// clang-format off` (tabular data, `Docs/ReviewChecklist.md` §2).
- **Effect on the test:** it reads no file, draws no random numbers and runs in milliseconds. The C++ code never reproduces the generator's input sequence.

### 3. Coverage

Each table covers the domains of the old test plus edge regions:

- **All functions:** subnormal and tiny arguments, and arguments around 2^-27, where DetMath returns x or 1 directly.
- **Sin, Cos and Tan:** arguments within 2 ULP of k pi/2 for small and large k, the Cody-Waite limit 2^19, huge arguments up to the largest finite value (the Payne-Hanek path), and the double closest to a multiple of pi/2 with its neighbours.
- **ASin and ACos:** arguments near +-1 and around DetMath's switch points 1/2 and 1/sqrt(2).
- **ATan and ATan2:** arguments around 1 and 2^60, extreme ratios, and both arguments subnormal.
- **Sinh, Cosh and Tanh:** the switch points 1/8, 1/4 and 22, and arguments near overflow.
- **Exp:** results near overflow, in the subnormal range and near underflow to zero.
- **Log and Log10:** arguments near 1, subnormal arguments, the whole range of the format, the exact powers of ten and their neighbours.
- **Pow:** integer exponents, negative bases, bases near 1 with exponents up to 2^62, results near overflow and in the subnormal range, perfect squares with half-integer exponents, and listed edge cases such as 2^-1075.

`SinCos` is checked on the `Sin` and `Cos` tables.

### 4. The acceptance test

The test is now "DetMath: within 1 ULP of correctly rounded references over seeded tables". Every committed case must be within 1 ULP of its reference.

- **Why 1 ULP holds:** within 0.5005 ULP, a double result is the correctly rounded value or, near a halfway point, its neighbour. A float result rounds such a double result once more, which also stays within 1 ULP of the correctly rounded float.
- **Measured:** DetMath matches the reference bit for bit in all but 2 of the 29,658 cases. Both are subnormal results, 1 ULP away: `Exp(-708.6238995803252)` and `Pow(0.14545582981164754, 367.8022794605353)`.
- **Failure message:** for each failing table it names the function as a `std::string` (for example `Cos(double)`), gives the failure count and the worst distance, and lists up to 8 cases with the bit patterns and values of the arguments, the actual result and the reference, plus their ULP distance. A trial build that shifted `Cos(double)` by 2 ULP failed 1,158 of the 1,266 `Cos(double)` cases with such a message. The other 108 have arguments below 2^-27, where `Cos` returns 1 before the shifted code.
- **Notable-argument test:** it now prints the function name instead of its enumerator value.
- **`DetMath.h`:** the accuracy line now promises 1 ULP of the correctly rounded result over the domains the reference tables sample. This is a stronger promise that the implementation already met, with no signature change. The contract owner reviews the wording (Roadmap rule 3).

### 5. No comparison with `std::` remains

A sanity test against the libm with a loose bound, such as 4 ULP, was considered and dropped:

- It would still test the libm. A loose enough bound must cover the worst library in the CI matrix and on any future platform, and at that point it detects nothing that the references miss.
- The references are a strictly stronger oracle over a superset of the old domains.
- The cross-check stays independent:
  - The references come from different algorithms and arithmetic: `decimal` series, exact reduction and rational rounding, where DetMath uses double-double tables and short polynomials.
  - The independently computed values in "huge arguments of Sin, Cos and Tan are reduced exactly" and "results are correctly rounded at notable arguments" remain in the test file. The generator's self-test reproduces them.
  - The cross-check already worked during development. DetMath disagreed with an early generator on `Pow` with bases near 1 and exponents of magnitude 2^45 to 2^58. An independent rational computation sided with DetMath, which exposed the context rounding in the generator that section 1 now traps.

## Consequences

- A difference between C runtimes can no longer fail the accuracy test. A DetMath change that loses accuracy anywhere in the tables fails identically on every platform, and the message gives the bits needed to reproduce it.
- Changing DetMath's domains, or adding a function, means editing the regions in `Scripts/GenerateDetMathReference.py`, regenerating and committing both files. A hand edit of the header is visible to `--check`, which is not yet part of the static checks. Adding it is a separate change to `Scripts/Lib/scripts.py` and the documents that list the checks.
- **Renamed acceptance test:** Roadmap M1 acceptance and Architecture §4.12 and §15.2 use the new name and oracle.
- **Script table:** Architecture §2.3 lists the generator.
