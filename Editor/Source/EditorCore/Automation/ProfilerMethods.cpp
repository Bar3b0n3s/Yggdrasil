#include "EditorPCH.h"
#include "EditorCore/Automation/ProfilerMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Automation {

		Result<Scope<PendingOperation>> BeginProfilerCapture(EditorMethodContext& /*context*/, const ProfilerCaptureParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return std::unexpected(Error(ErrorCode::Unsupported, "M18 profiler.capture contract (Docs/Decisions/0029-m18-contract.md decision 11, stream C)"));
		}

	}

	void RegisterProfilerMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<ProfilerCaptureParams>("ProfilerCaptureParams", "The params of profiler.capture.")
			.Field("frames", &ProfilerCaptureParams::Frames, "How many of the editor's next frames the trace captures.",
				{ .Min = 1.0, .Max = static_cast<double>(MaxProfilerCaptureFrames) });

		registry.Struct<ProfilerCaptureResult>("ProfilerCaptureResult", "The Chrome trace profiler.capture wrote.")
			.Field("path", &ProfilerCaptureResult::Path, "The trace's absolute path: a Chrome trace JSON file for chrome://tracing or Perfetto.")
			.Field("frames", &ProfilerCaptureResult::Frames, "The frames captured.")
			.Field("cpuZones", &ProfilerCaptureResult::CpuZones, "The CPU zones the trace holds, of every thread.")
			.Field("gpuZones", &ProfilerCaptureResult::GpuZones, "The GPU zones the trace holds (render stages and passes); 0 without a renderer.")
			.Field("droppedZones", &ProfilerCaptureResult::DroppedZones,
				"The zones of the captured frames that the profiler's per-thread rings overwrote before the trace was written, which "
				"the trace lacks (also in its otherData); 0 for a complete trace. Capture fewer frames when it is not 0.");
	}

	void RegisterProfilerMethods(MethodRegistry& methods)
	{
		Json example = Json::object();
		example["frames"] = 30;
		methods.AddPending<EditorMethodContext, ProfilerCaptureParams, ProfilerCaptureResult>(
			{
				.Name = "profiler.capture",
				.Description = "Captures the editor's next frames as one Chrome trace with the CPU zones of every thread and the GPU zones of "
							   "every rendered view (stages and passes), written to the automation output directory; without a renderer "
							   "the trace holds CPU zones only. Timings are diagnostic.",
				.TimeoutSeconds = 120,
				.Examples = { { .Description = "Profile the next 30 frames.", .Params = example } },
			},
			&Automation::BeginProfilerCapture);
	}

}
