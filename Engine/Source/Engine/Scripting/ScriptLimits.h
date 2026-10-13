#pragma once

#include <cstddef>
#include <cstdint>

namespace Engine {

	// Bounds on script-supplied data that the host copies or retains outside the VM allocator (Architecture §11.1). A
	// short VM string can otherwise be copied into any number of host objects without counting against
	// Scripting.MemoryLimitMB. Retained values over a limit raise a located script error before any host mutation;
	// diagnostic text (log entries, print lines, test report messages) is truncated instead, so diagnostics never fail.

	// Bytes of one Log.Trace/Info/Warn/Error or print entry after tostring and tab joining, and of one Test.Fail, Test.Skip
	// or Test.Expect* message; the same cap applies to captured evaluation and load-time print lines and to the message of
	// every script error the host keeps (ScriptErrorStream; the value a script raises is captured this way). Private chunk
	// locations are rewritten to authored ones first, then longer text is cut at a UTF-8 sequence boundary and ends with
	// " [truncated: <n> bytes]" (n is the full byte count of the script's text); the marker counts toward the cap. At most
	// this many bytes of script text are ever assembled, whatever the argument count or the aliasing of one VM string.
	inline constexpr size_t MaxScriptLogMessageBytes = 8 * 1024;
	// Log entries one VM writes per host frame window, that is one (simulation tick, frame index) pair. Later entries in
	// the window are dropped and counted: one warning when dropping starts, a summary with the count when the window ends.
	// Evaluation print capture is not rate-limited; it remains charged to the VM allocator.
	inline constexpr uint32_t MaxScriptLogEntriesPerFrame = 1024;
	// Debug.DrawText text bytes. The draw list keeps at most DebugDrawList::MaxCommands commands of any duration.
	inline constexpr size_t MaxScriptDebugTextBytes = 1024;
	// Entity names written by Entity.Name and Scene.CreateEntity.
	inline constexpr size_t MaxScriptEntityNameBytes = 256;
	// One tag added by Entity:AddTag.
	inline constexpr size_t MaxScriptTagBytes = 256;
	// Tags an entity may carry for Entity:AddTag to add another one; tags authored in the scene count too.
	inline constexpr size_t MaxScriptEntityTags = 256;
	// Text in one component field value a script writes, through a proxy field assignment (e.Text.Text = s) or as one
	// Entity:AddComponent value: every string, map key and Variant string or object key of the value together. Text.Text
	// and Script.Fields are the common cases.
	inline constexpr size_t MaxScriptComponentStringBytes = 4 * 1024;
	// A Test.Suite or Test.Case name, which the runner keeps for the run's results.
	inline constexpr size_t MaxScriptTestNameBytes = 256;
	// A Test.ExpectScriptError pattern, which its waiting thread keeps until the wait ends.
	inline constexpr size_t MaxScriptErrorPatternBytes = 1024;
	// Traceback frames one script error keeps, innermost first. A deeper stack keeps the innermost
	// MaxScriptErrorTracebackFrames - 1 frames and ends with a frame without a script or line whose function reads
	// "[truncated: <n> more frames]" (n frames left out).
	inline constexpr size_t MaxScriptErrorTracebackFrames = 32;
	// Distinct errors one script error stream retains (ScriptErrorStream: a VM's, a play session's, and the editor's or
	// Runtime's history). Adding another evicts the error whose latest occurrence is the oldest; script.errors tells a
	// reader whose cursor precedes an evicted error (`evicted`), and an evicted error that occurs again starts a new count.
	inline constexpr size_t MaxScriptErrorEntries = 256;
	// Unclaimed script errors one running test case keeps, both for Test.ExpectScriptError and for its report: a claimed
	// error is released at once, and a later error past this cap can no longer be claimed and fails the case.
	inline constexpr size_t MaxScriptTestUnclaimedErrors = MaxScriptErrorEntries;
	// Failures one test report row keeps for Test.Expect* failures and unexpected script errors, a final
	// "[truncated: <n> more failures]" failure counting any further ones included (so the case fails all the same), and
	// such failures one suite keeps across its rows, truncation failures aside. The runner's own failures (a timeout, a
	// stopped VM, an unexpected quit) are always kept; a row has at most a few of them.
	inline constexpr size_t MaxScriptTestFailuresPerCase = 32;
	inline constexpr size_t MaxScriptTestFailuresPerSuite = 1024;
	// Test cases one Test.Suite registers.
	inline constexpr size_t MaxScriptTestCasesPerSuite = 4096;
	// Input events Test.InjectAction/Key/Mouse/Gamepad queue for one input tick.
	inline constexpr size_t MaxScriptInjectedInputsPerTick = 1024;
	// Test.Screenshot calls one suite makes, counted by the runner whatever the host's outcome; each may write a new image
	// file (the editor writes every capture to a new file in its automation output directory). A later call in the suite
	// raises a located error naming this limit and reaches no host; the case fails unless the script handles the error,
	// and the run's report is published as usual.
	inline constexpr size_t MaxScriptTestScreenshotsPerSuite = 256;
	// Audio.PlayOneShot requests are bounded by the audio system for every caller, scripts included: at most
	// MaxAudioOneShotsPerFrame per frame (Scene/AudioSystem.h), each later one a located error.
	// Bytes of one require request, and distinct require edges (importer, request, resolved path) one VM records for
	// dependency tracking; an edge is recorded before its module loads, so a failed require counts too.
	inline constexpr size_t MaxScriptRequireBytes = 1024;
	inline constexpr size_t MaxScriptRequireEdges = 16384;

}
