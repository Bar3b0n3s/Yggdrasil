# 0011 — Remote CI is non-blocking

- **Status:** accepted
- **Date:** 2026-10-07
- **Decided by:** product owner
- **Context:** Up to M6, every milestone waited until all four GitHub Actions jobs were green (Windows, Linux GCC 14, Linux Clang 19, macOS arm64) before the next one started. One CI round takes 35–100 minutes, and a failure costs at least another round. The waiting roughly doubled the wall-clock time of several milestones.

## What remote CI is for

This machine builds only Windows (MSVC and clang-cl). GitHub Actions is the only place where:

- the Linux and macOS code compiles (GCC 14, Clang 19 with libstdc++, Apple Clang with libc++);
- the Linux and macOS code runs;
- GPU tests run on a second Vulkan implementation (Lavapipe).

It has found real defects that the Windows gate cannot see. Examples:

- a self-move that emptied error messages on libc++ and libstdc++;
- missing `std::format_string` support in Apple libc++;
- `std::expected` being unavailable with Clang 18 and libstdc++;
- a crash-handler race that only the runner reproduced.

## Decision

Remote CI stays enabled, but no step waits for it.

- **Done means the local gate.** A milestone, or any change, is done when the full local Windows gate is green: strict `python Scripts/PreCommit.py`, then `python Scripts/CI.py` with every stage. The portability stage includes clang-cl builds in Release and in Dist (asserts compiled out). Those builds catch the most common class of Clang-only error before a push. The change is then merged into `main` and pushed straight away.
- **GitHub Actions runs in the background.** Its results are read occasionally through the public annotations API.
- **Linux and macOS failures are fixed in batches.** A dedicated catch-up task runs every few milestones, and always before the demo games (Roadmap M16–M18). The demo games must be built on a tree that is green on every platform.
- **Design-level failures are raised at once.** Examples: a Vulkan feature that Lavapipe or MoltenVK lacks, or an API that cannot work on POSIX. The same applies to anything that would force a finished milestone to be reworked.
- **"Verified" keeps its meaning.** A platform counts as verified only once its CI job is green (Architecture §1.1 G6). Between catch-up tasks, Linux and macOS may be temporarily unverified, and status reports say so.

## Consequences

- The milestone critical path no longer includes remote CI round trips.
- Linux and macOS problems can pile up between catch-up tasks. Each catch-up therefore starts with the newest failing run and fixes root causes, not symptoms. The local clang-cl Release and Dist builds keep the most frequent class of Clang-only errors from piling up.
- Roadmap rule 1 (green or not done) refers to the local `python Scripts/CI.py`, as it always has. This decision changes only when remote results are waited for.
