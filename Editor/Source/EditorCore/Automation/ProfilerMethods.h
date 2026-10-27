#pragma once

#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>

// profiler.capture (M18, R-PROF; Docs/Decisions/0029-m18-contract.md decision 11): one Chrome trace of the editor's next
// frames, with the CPU zones of every thread (ENGINE_PROFILE_SCOPE, the frame loop's and the job system's, and the scene
// renderer's stages) and the GPU zones of every rendered view (its stages and passes, merged into the profiler's timeline by
// RenderStatsHistory::PublishGpu). Conventions as in MethodRegistry.h.
//
// A pending method, polled once per frame: it records the profiler's clock (Profiler::GetTimeNs) when it starts, lets
// `frames` frames pass and then the host's frames in flight more, so the GPU timings of the captured frames have been
// collected, and writes Profiler::ExportChromeTrace of every zone since the start ({"traceEvents": [...]}, loadable in
// chrome://tracing and Perfetto) to the server's output directory (AutomationServer::WriteOutputFile, a ".json" file, as a
// screenshot writes its PNG). Without a renderer (--renderer none) the trace holds CPU zones only and gpuZones is 0; that is
// not an error. Timings are diagnostic: nothing reads them back into simulated state, and no outcome of a test depends on
// their values.
//
// A capture is bounded by the profiler's rings, not only by `frames`: each thread keeps its last
// ProfilerSpecification::ZonesPerThread zones (65,536 by default) and overwrites the oldest, and a busy editor frame records
// about a hundred zones on the main thread (the frame loop's, twelve or more "Stage:" zones per rendered view, the systems'
// and scripts' scopes). A zone of the capture that a ring overwrote before the trace was written is missing from it: the
// capture counts those zones per thread from the zones each recorded since its start, reports the sum as droppedZones, also
// writes it into the trace (its top-level "otherData": {"droppedZones": n}, Chrome's metadata object) and logs one warning;
// the trace is still written, and a caller that needs every zone asks for fewer frames. droppedZones is 0 for a complete
// trace.
//
// Editor only (not in the Runtime's subset, decision 11), not a tool (engine_call reaches it), not in the launcher state, no
// dry run, not a batch op, not flagged Mutates (the trace goes to the output directory, as screenshots do). Disconnecting
// cancels it and writes nothing.

namespace Engine {

	class EditorMethodContext;
	class MethodRegistry;
	class TypeRegistry;

	// The largest `frames` a capture may ask for.
	inline constexpr uint32_t MaxProfilerCaptureFrames = 600;

	// profiler.capture {frames?}.
	struct ProfilerCaptureParams
	{
		uint32_t Frames = 60; // 1 to MaxProfilerCaptureFrames
	};

	struct ProfilerCaptureResult
	{
		std::string Path{};  // the trace's absolute native path (a .json file in the server's output directory)
		uint32_t Frames = 0; // the frames captured (the request's frames)
		// The zones the trace holds: CPU zones of every thread, and GPU zones (Profiler::GpuThreadIndex).
		uint32_t CpuZones = 0;
		uint32_t GpuZones = 0;
		// The zones of the capture its threads' rings overwrote before the trace was written, which the trace lacks (see the
		// file comment); 0 for a complete trace.
		uint32_t DroppedZones = 0;
	};

	namespace Automation {

		// The pending operation (see the file comment). Errors: InvalidArgument at /frames for 0 or more than
		// MaxProfilerCaptureFrames; Unsupported when the process profiler is not initialized; those of
		// AutomationServer::WriteOutputFile when the trace cannot be written.
		[[nodiscard]] Result<Scope<PendingOperation>> BeginProfilerCapture(EditorMethodContext& context, const ProfilerCaptureParams& params);

	}

	void RegisterProfilerMethodTypes(TypeRegistry& registry);
	// Registers profiler.capture (see the file comment for its flags).
	void RegisterProfilerMethods(MethodRegistry& methods);

}
