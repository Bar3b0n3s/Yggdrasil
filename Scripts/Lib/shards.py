"""Sharded Tests runs (Docs/Architecture.md §2.3, §15.8): Scripts/Test.py --parallel <n> splits the unit suite's run
into shards, each run by its own Tests process, n of them at a time, which the hardening stage's sanitized unit run uses
(Scripts/Hardening.py asan-unit; Docs/Decisions/0060-skerries-engine-program.md decision 3). Test.py starts and
watches the processes; this module plans the shards, orders them, reads and keeps the measured costs that size them, and
merges their reports.

The selection: the test cases the run's filters select that doctest::skip does not leave out, in the order the Tests
binary runs them (doctest's default order, by file, line and template), as one listing with --no-skip
--list-test-cases --reporters=xml gives them, skipped ones marked. plan_shards cuts it into contiguous ranges; each
shard runs its range with doctest's --first=<a> --last=<b>, which count only the cases that pass the filters and are
not skipped, in that same order (Vendor/doctest), so every case of the selection runs in exactly one shard and none
twice. check_shard_cases compares each shard's JUnit report with its range, so a shard that ran other cases than
planned fails.

The costs: each case's seconds as the last passing sharded run of the same report stem that ran it measured them,
kept in <git common directory>/test-costs/<report stem>.xml (machine_costs_path, shared by every checkout of the
repository). A run whose shards all passed keeps its merged report there with the earlier entries of the cases it did
not select (keep_costs), so a run of part of the suite (a filter, more excluded test suites, a contract run's skipped
cases) replaces only the costs of the cases it ran, and a failed run keeps nothing. A case that report lacks costs the
measured cases' average; without any measurement, every case costs the same, so the shards hold equal case counts
and have no expected time. The cuts fall where the summed costs reach each k/n of the total, so the shards' expected
times are as even as the order allows, and each shard's timeout follows its expected time (shard_timeout).

The shards are small (shard_count): about SHARD_SECONDS of measured work each, so many more than run at a time, and
Test.py starts the next one, dearest first (start_order), whenever one ends. A process's memory follows the work it
ran, not only its dearest case: a sanitized Tests process never gives back what its allocator and its stack depot took,
so one that ran 1,075 s of the unit suite held 5.9 GB where one that ran 269 s held 0.7 GB (Lib/hardening.py
UNIT_PEAK_PROCESS_BYTES), while a plain Release process over the same cases stayed below 160 MB. Small shards bound each
process's memory, and so what n of them hold together, whatever the suite's size; the dearest-first order keeps the
run's end close to the work over n.
"""

from __future__ import annotations

import bisect
import copy
import dataclasses
import math
import os
import statistics
import xml.etree.ElementTree as ElementTree
from collections.abc import Iterable, Mapping, Sequence
from pathlib import Path

from Lib import paths, vendor_cache

# A shard's timeout: its expected time times SHARD_TIMEOUT_FACTOR, at least SHARD_MINIMUM_TIMEOUT_SECONDS, and at most
# what is left of the run's own bound when it starts (shard_timeout). The costs come from an earlier run whose machine
# load may differ: a case took 1.15 s under phase 6's load against 0.59 s on phase 7's quiet machine (the sanitized unit
# run, Docs/Backlog.md), and in the setup's second sharded run, which shared the machine with another checkout's 12-job
# build, shards of about 120 s measured took up to 5.1 times that (2.4 times at the median), and two of them met the
# timeout of three times their measured time, at least 600 s, that the setup first gave them. A hung test case is
# mostly ended by the Tests binary's own per-case limit (120 s, 600 s under the sanitized run's --test-timeout-scale 5),
# which names it, so the shard's timeout mostly bounds a process that hangs outside a case and can be generous: five
# times the expected time, and at least ten times a shard's measured work (SHARD_SECONDS), which also covers the process
# start and the fixed setup that the per-case costs leave out. A case that raises its own limit (SetTestCaseTimeLimit:
# six cases to 600 s, 3,000 s under that scale) can meet its shard's timeout first, and the shard's detail names it
# from the log.
SHARD_TIMEOUT_FACTOR = 5.0
SHARD_MINIMUM_TIMEOUT_SECONDS = 1200.0
# The measured work of a shard (shard_count): with the costs, a run has at least its work over SHARD_SECONDS shards;
# without them, one per SHARD_CASES cases (in phase 7's sanitized unit run, 100 cases of its dearest region took about
# 185 s at eight processes at a time). Either way a run has at least as many shards as it runs at a time.
SHARD_SECONDS = 120.0
SHARD_CASES = 100
# Where the last full, passing sharded run of each report stem keeps its merged report for the next run, in every
# checkout of the repository (machine_costs_path).
COSTS_DIRECTORY_NAME = "test-costs"
# The name of the property that names a shard in the merged report: one <testsuite> per shard, carrying
# <properties><property name="shard" value="<k>/<n>"/></properties>.
SHARD_PROPERTY = "shard"
# The test suite of the kept costs (keep_costs) that holds the earlier entries of the cases the last run did not select.
EARLIER_SUITE = "kept from earlier runs"
# The separator doctest's JUnit reporter puts between a test case's name and the subcases of one pass through it: each
# pass is one <testcase>, named "<case>/<subcase>/<nested subcase>" when it entered subcases.
SUBCASE_SEPARATOR = "/"


@dataclasses.dataclass(frozen=True)
class ShardPlan:
    """One shard of a run: the k-th (from 0) of `count` contiguous ranges of the selection."""

    index: int
    count: int
    first: int  # the 1-based position of its first case in the selection (doctest's --first)
    last: int  # the 1-based position of its last case, inclusive (doctest's --last)
    cases: tuple[str, ...]  # the names of its cases, in run order
    expected_seconds: float | None  # the summed costs of its cases; None without a measurement

    @property
    def label(self) -> str:
        return f"shard {self.index}/{self.count}"

    def options(self) -> tuple[str, ...]:
        """The doctest options that select the shard's range."""
        return (f"--first={self.first}", f"--last={self.last}")


def case_weights(cases: Sequence[str], costs: Mapping[str, float]) -> list[float] | None:
    """Each of `cases`'s expected seconds: its cost in `costs` (at least 0), or for a case `costs` lacks the measured
    cases' average; None when `costs` measures none of `cases`."""
    measured = [max(0.0, costs[name]) for name in cases if name in costs]
    if not measured:
        return None
    fallback = statistics.fmean(measured)
    return [max(0.0, costs[name]) if name in costs else fallback for name in cases]


def plan_shards(cases: Sequence[str], count: int, costs: Mapping[str, float]) -> list[ShardPlan]:
    """Splits `cases` (the selection's names in run order) into min(count, len(cases)) contiguous, non-empty ranges that
    cover it exactly once. Each case costs costs[name] (seconds), a case `costs` lacks the measured cases' average; when
    `costs` measures none of `cases`, every case costs 1 and no shard has an expected time, so the shards hold equal
    case counts, as they do when every measured cost is 0. The k-th cut falls at the case boundary whose summed cost is
    nearest k/n of the total, within the bounds that leave every shard at least one case. Raises ValueError for a count
    below 1."""
    if count < 1:
        raise ValueError(f"a run needs at least 1 shard, got {count}")
    if not cases:
        return []
    weights = case_weights(cases, costs)
    expected = [0.0]
    for weight in weights if weights is not None else [1.0] * len(cases):
        expected.append(expected[-1] + weight)
    # The cuts follow the costs; when they sum to nothing (every case measured at 0 s), the case counts.
    prefix = expected if expected[-1] > 0.0 else [float(position) for position in range(len(cases) + 1)]
    shard_count = min(count, len(cases))
    total = prefix[-1]
    cuts = [0]
    for k in range(1, shard_count):
        low, high = cuts[-1] + 1, len(cases) - (shard_count - k)
        target = total * k / shard_count
        # The summed costs never decrease, so the nearest boundary is the first at or past the target or the one before.
        above = bisect.bisect_left(prefix, target, low, high + 1)
        candidates = [cut for cut in (above - 1, above) if low <= cut <= high]
        cuts.append(min(candidates, key=lambda cut: (abs(prefix[cut] - target), cut)))
    cuts.append(len(cases))
    return [ShardPlan(index, shard_count, cuts[index] + 1, cuts[index + 1], tuple(cases[cuts[index]:cuts[index + 1]]),
                      expected[cuts[index + 1]] - expected[cuts[index]] if weights is not None else None)
            for index in range(shard_count)]


def shard_count(cases: Sequence[str], parallel: int, costs: Mapping[str, float]) -> int:
    """How many shards a run of `cases` (the selection) that runs `parallel` at a time has: the measured work of
    `cases` (each case at its cost in `costs`, a case it lacks at the measured average) over SHARD_SECONDS, or without
    a measurement the cases over SHARD_CASES, rounded up; at least `parallel` and at most one per case. Raises
    ValueError for a `parallel` below 1."""
    if parallel < 1:
        raise ValueError(f"a run needs at least 1 process at a time, got {parallel}")
    if not cases:
        return 0
    weights = case_weights(cases, costs)
    needed = math.ceil(sum(weights) / SHARD_SECONDS) if weights is not None else math.ceil(len(cases) / SHARD_CASES)
    return min(len(cases), max(parallel, needed))


def start_order(plans: Sequence[ShardPlan]) -> list[ShardPlan]:
    """The order in which a run starts `plans`: the dearest expected first (ties, and every shard of a run without a
    measurement, in shard order), so the last shards to start are the cheapest and the run ends close to its work over
    the processes at a time."""
    return sorted(plans, key=lambda plan: (-(plan.expected_seconds or 0.0), plan.index))


def shard_timeout(expected_seconds: float | None, bound_seconds: float) -> float:
    """The timeout of a shard whose cases are expected to take `expected_seconds` (None: no measurement) and that
    starts with `bound_seconds` left of its run's bound: the expected time times SHARD_TIMEOUT_FACTOR, at least
    SHARD_MINIMUM_TIMEOUT_SECONDS, at most `bound_seconds`; without an expected time, `bound_seconds`."""
    if expected_seconds is None:
        return bound_seconds
    return min(bound_seconds, max(SHARD_MINIMUM_TIMEOUT_SECONDS, expected_seconds * SHARD_TIMEOUT_FACTOR))


def case_of_entry(entry: str, names: set[str] | frozenset[str]) -> str | None:
    """The test case among `names` that the JUnit <testcase> named `entry` is a pass through: `entry` itself, or the
    longest of its prefixes that ends before a SUBCASE_SEPARATOR (doctest appends the subcases of the pass); None when
    none is among `names`. A case name may hold the separator itself, so the longest name that matches wins."""
    candidate = entry
    while True:
        if candidate in names:
            return candidate
        cut = candidate.rfind(SUBCASE_SEPARATOR)
        if cut < 0:
            return None
        candidate = candidate[:cut]


def report_entries(report: Path) -> list[tuple[str, float]]:
    """(name, seconds) of every <testcase> of the JUnit report `report`, in order; a time that is not a finite number of
    at least 0 counts as 0. Raises OSError or ElementTree.ParseError when it cannot be read."""
    entries: list[tuple[str, float]] = []
    for case in ElementTree.parse(report).getroot().iter("testcase"):
        try:
            seconds = float(case.get("time", "0") or 0)
        except ValueError:
            seconds = 0.0
        entries.append((case.get("name", ""), seconds if math.isfinite(seconds) and seconds >= 0.0 else 0.0))
    return entries


def case_seconds(entries: Iterable[tuple[str, float]], cases: Iterable[str]) -> dict[str, float]:
    """{case: seconds} for the cases of `cases` that `entries` (report_entries) ran: the times of their passes summed.
    An entry of no case among `cases` is left out."""
    names = frozenset(cases)
    seconds: dict[str, float] = {}
    for entry, entry_seconds in entries:
        case = case_of_entry(entry, names)
        if case is not None:
            seconds[case] = seconds.get(case, 0.0) + entry_seconds
    return seconds


def read_case_costs(report: Path, cases: Iterable[str]) -> dict[str, float]:
    """The costs that the kept report `report` (machine_costs_path) measured for `cases`: {case: seconds}
    (case_seconds); doctest writes a time below 0.1 ms as 0, so a case measured at 0 s costs 0. Empty when the report
    does not exist. Raises ValueError naming the file when it cannot be read."""
    try:
        entries = report_entries(report)
    except FileNotFoundError:
        return {}
    except (OSError, ElementTree.ParseError) as error:
        raise ValueError(f"{paths.display_path(report)}: cannot be read: {error}") from None
    return case_seconds(entries, cases)


def check_shard_cases(plan: ShardPlan, entries: Sequence[str]) -> list[str]:
    """The problems of a shard whose Tests process ran to its end and reported the <testcase> names `entries`: each case
    of `plan` without an entry ("did not run: <case>"), and each entry of no case of `plan` ("ran outside its range:
    <entry>"), in order. Empty when the shard ran exactly its range."""
    planned = frozenset(plan.cases)
    ran: set[str] = set()
    problems: list[str] = []
    for entry in entries:
        case = case_of_entry(entry, planned)
        if case is None:
            problems.append(f"ran outside its range: {entry}")
        else:
            ran.add(case)
    problems += [f"did not run: {case}" for case in plan.cases if case not in ran]
    return problems


def merge_reports(reports: Sequence[tuple[ShardPlan, Path]], suite_name: str) -> ElementTree.Element:
    """One JUnit document of the shards' reports (each a shard's plan and its JUnit report, which Test.py has read and
    renamed, or written for a process that left none): <testsuites> with each shard's test suites in shard order, each
    named `suite_name` and naming its shard in a "shard" property (SHARD_PROPERTY: "<k>/<n>"). Raises OSError or
    ElementTree.ParseError when a report cannot be read."""
    root = ElementTree.Element("testsuites", {"name": suite_name})
    for plan, report in reports:
        for suite in list(ElementTree.parse(report).getroot().iter("testsuite")):
            suite.set("name", suite_name)
            for earlier in suite.findall("properties"):
                suite.remove(earlier)
            properties = ElementTree.Element("properties")
            ElementTree.SubElement(properties, "property",
                                   {"name": SHARD_PROPERTY, "value": f"{plan.index}/{plan.count}"})
            suite.insert(0, properties)
            root.append(suite)
    ElementTree.indent(root, space="\t")
    return root


def write_report(root: ElementTree.Element, destination: Path) -> None:
    """Writes the JUnit document `root` to `destination` through a temporary file and a rename, so a reader (another
    checkout's run reading the kept costs) sees the previous file or this one, never a partial one. Raises OSError."""
    destination.parent.mkdir(parents=True, exist_ok=True)
    temporary = destination.with_name(f"{destination.name}.{os.getpid()}.tmp")
    try:
        ElementTree.ElementTree(root).write(temporary, encoding="utf-8", xml_declaration=True)
        os.replace(temporary, destination)
    finally:
        temporary.unlink(missing_ok=True)


def keep_costs(report: ElementTree.Element, destination: Path, selection: Iterable[str]) -> None:
    """Keeps the costs of a run whose shards all passed in `destination` (machine_costs_path): its merged report
    `report` (merge_reports) and, in a test suite named EARLIER_SUITE, every entry of the report kept there before that
    is a pass through none of `selection` (the run's selection), so the cases the run did not select keep their earlier
    costs and the cases it ran take its own. A kept report that does not exist or cannot be read adds nothing; `report`
    is not changed. Raises OSError when `destination` cannot be written."""
    names = frozenset(selection)
    try:
        earlier = [case for case in ElementTree.parse(destination).getroot().iter("testcase")
                   if case_of_entry(case.get("name", ""), names) is None]
    except (OSError, ElementTree.ParseError):
        earlier = []
    document = copy.deepcopy(report)
    if earlier:
        suite = ElementTree.SubElement(document, "testsuite", {"name": EARLIER_SUITE, "tests": str(len(earlier))})
        suite.extend(earlier)
    ElementTree.indent(document, space="\t")
    write_report(document, destination)


def machine_costs_path(report_stem: str) -> Path:
    """Where the last full, passing sharded run whose report stem is `report_stem` (Unit-Release-Asan) keeps its merged
    report: <git common directory>/test-costs/<report stem>.xml, beside the vendor cache, the fuzz costs and the
    machine-wide locks, so every checkout's next sharded run of that stem is sized from it; bin-int/test-costs/ of this
    checkout when git cannot name the common directory."""
    try:
        directory = vendor_cache.git_common_directory() / COSTS_DIRECTORY_NAME
    except vendor_cache.VendorCacheError:
        directory = paths.BIN_INT_ROOT / COSTS_DIRECTORY_NAME
    return directory / f"{report_stem}.xml"
