# 0004 — Contract stubs and skipped tests are a mechanical gate

- **Status:** accepted
- **Date:** 2026-10-06
- **Context:** Roadmap rule 3 starts every milestone with a contract task. It commits the frozen public headers with stub implementations that return `Unsupported`, and the milestone's tests marked `doctest::skip`. The implementation streams then replace the stubs and un-skip the tests. Until now only the review stopped a stub or a skip from surviving into a milestone commit. A forgotten skip hides an unimplemented acceptance test while the run stays green, which defeats rule 1 ("green or not done") without anyone noticing.

## Decision

### 1. Stubs carry a marker

`Engine/Source/Engine/Core/Base.h` defines `ENGINE_CONTRACT_STUB()`, a statement that does nothing (`static_cast<void>(0)`). A contract task writes `ENGINE_CONTRACT_STUB();` as the first statement of every stub body it commits, in Engine, Editor, Runtime and Tests code. The implementation replaces the whole body, marker included.

Searching for `Unsupported` instead was rejected: it is also a legitimate error code of finished code (an unsupported format version or feature), so only an explicit marker separates a stub from an implementation.

### 2. Lint rejects stubs and skips

`Scripts/Lint.py` has a `contract` step with two rules. `Scripts/ModuleRules.json` (`Contract`) holds their path scopes.

- **`contract-stub`:** any use of `ENGINE_CONTRACT_STUB` in first-party C++ (the `CxxRoots`), except the name in its `#define` in `Contract.StubDefinitionFiles` (`Core/Base.h`). A second definition, or a use inside `Base.h`, is a finding.
- **`test-skip`:** a `doctest::skip` in `Contract.TestFiles` (`Tests/Source/**`) is a finding unless the decorator expression of the same doctest test macro also carries `doctest::test_suite(Test::ChildTargetSuite)`. The macros are `TEST_CASE`, `TEST_CASE_FIXTURE`, `SUBCASE` and doctest's other decorated macros, with or without the `DOCTEST_` prefix. The decorator expression runs up to the macro's closing parenthesis, across lines. A skip outside every test macro, such as one stored in a shared decorator variable, is a finding too.
- **Parsing:** comments and string literals never count, in either direction: a suite named only in a comment does not make a child target, and a string that spells a skip is not one. doctest named through a namespace alias, or unqualified after `using namespace doctest;`, is recognized.
- **Self-test:** the seeded fixtures `Tests/Data/Lint/ContractStub` and `Tests/Data/Lint/UnexpectedTestSkip` must keep failing with exactly their findings (`Lint.py --self-test`). The self-test always runs strict.

### 3. Test.py checks what actually ran

After a unit run, `Scripts/Test.py` asks the Tests binary for the test cases the run's filters select, skipped ones included:

```
Tests --test-suite-exclude=GPU,Golden [--test-case=<filter>] --no-skip --list-test-cases --reporters=xml --out=<file>
```

doctest's XML reporter gives each `<TestCase>` its suite and its `skipped` flag. A plain `--list-test-cases` omits skipped cases, and the console reporter's "skipped" count also includes cases that a filter excluded, so neither can name what `doctest::skip` left out. The listing goes to a file, so the log on stderr cannot interleave with it.

The run fails when a skipped case belongs to any suite other than `ChildTargets`, and the failure names the case. This is stricter than comparing the number of skipped cases with the number of child targets: it also catches an ordinary test that gained a skip while a child target lost one. It also catches what the textual lint rule cannot see, such as a skip produced by a macro expansion. The counts appear in the step detail and in the step's `skipCheck` object (`skipped`, `childTargets`, `unexpected`, `allowed`) of `--json`.

### 4. Contract mode

- `Lint.py --allow-contract-stubs` and `Test.py --allow-skips` report the stubs and skips they find without failing.
- `PreCommit.py --contract` and `CI.py --contract` pass both flags. They print "contract mode: stubs and skipped tests allowed" at the start and in the summary.
- The static-check list and the flag mapping live in `Scripts/Lib/scripts.py`, so PreCommit and CI cannot drift apart.
- Only the commit of a milestone's contract task uses contract mode. Every other commit, and therefore every milestone end, runs strict. GitHub Actions runs `CI.py` strict.

### 5. The one permanent exception: child-process targets

The test cases of `Test::ChildTargetSuite` (`Docs/Decisions/0003-m1-contract-decisions.md`, decision 7) are not unfinished work. Another test runs each of them in a child process with `--no-skip --test-case=<name>`, and they hang, fail or end the process by design: the watchdog targets, the recording-assert-handler targets, the ExpectLog meta-test targets, the death-test failure target and the command-line quoting target. Running them in the normal suite would hang or fail it, and deleting them would delete what their parent tests check. The parent tests run on every unit run, so the targets are exercised; they are only never run directly. M1 has 14 of them.

They are recognized by the suite decorator in their own decorator expression, not by a committed list of names. The exception is visible where the test is declared, both the lint rule and Test.py identify it the same way, and no list can drift from the code. A new kind of permanent skip needs an ADR that amends this one.

## Consequences

- A contract commit runs `python Scripts/PreCommit.py --contract`. Its `Reviewed:` trailer records the mode, for example `PreCommit green, contract mode (...)`.
- Every commit after the contract commit runs strict, so the implementation streams' work reaches the main branch in commits that leave no stub and no skip behind. In practice that is the milestone's integration commit, as in M1: contract commit `8479ad8`, integration commit `f6efd44`.
- Contract tasks write `ENGINE_CONTRACT_STUB();` in every stub. A stub without the marker still passes Lint, but its skipped test still fails Test.py.
- Test.py starts the Tests binary once more per unit run, to list the test cases; this takes well under a second.
