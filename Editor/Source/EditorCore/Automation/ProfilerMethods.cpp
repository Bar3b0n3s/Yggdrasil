#include "EditorPCH.h"
#include "EditorCore/Automation/ProfilerMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/EditorContext.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Profiler.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <limits>
#include <optional>
#include <span>
#include <utility>

namespace Engine {

	namespace {

		// A count as the result's uint32 field, saturated.
		[[nodiscard]] uint32_t SaturateCount(uint64_t count)
		{
			return static_cast<uint32_t>(std::min<uint64_t>(count, std::numeric_limits<uint32_t>::max()));
		}

		// profiler.capture's operation (ProfilerMethods.h): polled once per frame, the first time in the frame it started.
		class ProfilerCaptureOperation final : public PendingOperation
		{
		public:
			ProfilerCaptureOperation(uint32_t frames, uint32_t framesInFlight, ProfilerCaptureMark mark)
				: m_Frames(frames), m_FramesInFlight(framesInFlight), m_Mark(std::move(mark))
			{
			}

			[[nodiscard]] std::optional<Result<Json>> Poll(MethodContext& context) override
			{
				// The first poll comes in the frame the capture started, so the n-th poll sees n - 1 frames passed: the
				// captured frames, then the frames in flight, whose GPU timings retire after them.
				++m_Polls;
				if (m_Polls - 1 < static_cast<uint64_t>(m_Frames) + m_FramesInFlight)
					return std::nullopt;

				const ProfilerCapture capture = Profiler::ExportCapture(m_Mark);
				if (capture.DroppedZones > 0)
				{
					ENGINE_WARN("profiler.capture: the profiler's rings overwrote {} zone(s) of the {} captured frame(s) before the trace "
								"was written, so the trace lacks them; capture fewer frames for a complete trace",
						capture.DroppedZones, m_Frames);
				}
				// Every EditorMethodContext of this server is one (AutomationServer::CreateContext).
				EditorMethodContext& editorContext = static_cast<EditorMethodContext&>(context);
				const auto bytes = std::as_bytes(std::span(capture.ChromeTrace.data(), capture.ChromeTrace.size()));
				Result<std::string> path = editorContext.WriteOutputFile("json", bytes);
				if (!path)
					return std::unexpected(std::move(path).error().WithContext("while writing the profiler.capture trace"));
				ProfilerCaptureResult result;
				result.Path = std::move(*path);
				result.Frames = m_Frames;
				result.CpuZones = SaturateCount(capture.CpuZones);
				result.GpuZones = SaturateCount(capture.GpuZones);
				result.DroppedZones = SaturateCount(capture.DroppedZones);
				return context.SerializeResult(result);
			}

			[[nodiscard]] std::string GetPhase() const override
			{
				const uint64_t passed = m_Polls > 0 ? m_Polls - 1 : 0;
				return std::format("Profiler:capture {}/{}", std::min<uint64_t>(passed, m_Frames), m_Frames);
			}
		private:
			uint32_t m_Frames = 0;
			uint32_t m_FramesInFlight = 0;
			uint64_t m_Polls = 0;
			ProfilerCaptureMark m_Mark{};
		};

	}

	namespace Automation {

		Result<Scope<PendingOperation>> BeginProfilerCapture(EditorMethodContext& context, const ProfilerCaptureParams& params)
		{
			if (params.Frames == 0 || params.Frames > MaxProfilerCaptureFrames)
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/frames",
					std::format("frames must be from 1 to {}, got {}", MaxProfilerCaptureFrames, params.Frames)));
			}
			if (!Profiler::IsInitialized())
				return MakeError(ErrorCode::Unsupported, "profiler.capture needs the process profiler, which is not initialized");
			// The renderer's GPU timings of a frame retire the frames in flight after it (SceneRenderer's timing slots).
			const GraphicsDevice* device = context.GetEditor().GetEngine().GetGraphicsDevice();
			const uint32_t framesInFlight = device != nullptr ? device->GetFramesInFlight() : 0;
			return CreateScope<ProfilerCaptureOperation>(params.Frames, framesInFlight, Profiler::MarkCapture());
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
