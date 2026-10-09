#include "EnginePCH.h"
#include "Engine/Automation/Methods/StatsMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Renderer/RenderStats.h"

#include <limits>

namespace Engine {

	float ToStatsTelemetry(double value)
	{
		if (!(value > 0.0))
			return 0.0f;
		constexpr float Maximum = std::numeric_limits<float>::max();
		return value >= static_cast<double>(Maximum) ? Maximum : static_cast<float>(value);
	}

	Result<StatsGetResult> Automation::StatsGet(AutomationMethodContext& context, const StatsGetParams&)
	{
		ENGINE_TRY_ASSIGN(StatsGetResult result, context.GetHostStatistics());
		result.Fps = ToStatsTelemetry(result.Fps);
		result.CpuMilliseconds = ToStatsTelemetry(result.CpuMilliseconds);
		result.DroppedSeconds = ToStatsTelemetry(result.DroppedSeconds);
		result.ScriptHeapBytes = ToStatsTelemetry(result.ScriptHeapBytes);
		result.ScriptSoftLimitBytes = ToStatsTelemetry(result.ScriptSoftLimitBytes);
		result.ScriptHardLimitBytes = ToStatsTelemetry(result.ScriptHardLimitBytes);
		for (auto& view : result.Views)
		{
			view.CpuMilliseconds = ToStatsTelemetry(view.CpuMilliseconds);
			for (auto& pass : view.Passes)
			{
				pass.CpuMilliseconds = ToStatsTelemetry(pass.CpuMilliseconds);
				pass.GpuMilliseconds = ToStatsTelemetry(pass.GpuMilliseconds);
			}
		}
		return result;
	}

	StatsViewSummary MakeStatsViewSummary(std::string name, const RenderStats& stats)
	{
		StatsViewSummary result;
		result.Name = std::move(name);
		if (!stats.Available)
			return result;
		result.Available = true;
		result.Frame = std::to_string(stats.FrameIndex);
		result.GpuAvailable = stats.GpuAvailable;
		result.GpuFrame = stats.GpuAvailable ? std::to_string(stats.GpuFrameIndex) : std::string();
		result.Width = stats.Width;
		result.Height = stats.Height;
		result.CpuMilliseconds = ToStatsTelemetry(stats.CpuMilliseconds);
		result.ShadowDraws = stats.ShadowDraws;
		result.ShadowedSpotLights = stats.ShadowedSpotLights;
		result.DroppedSpotShadows = stats.DroppedSpotShadows;
		for (const RenderPassStats& pass : stats.Passes)
		{
			const bool available = stats.GpuAvailable && pass.GpuAvailable;
			result.Passes.push_back({
				.Name = pass.Name,
				.CpuMilliseconds = ToStatsTelemetry(pass.CpuMilliseconds),
				.GpuMilliseconds = available ? ToStatsTelemetry(pass.GpuMilliseconds) : 0.0f,
				.GpuAvailable = available,
				.DrawCalls = pass.DrawCalls,
				.Dispatches = pass.Dispatches,
				.Triangles = pass.Triangles,
			});
		}
		return result;
	}

	void RegisterStatsMethodTypes(TypeRegistry& registry)
	{
		static_cast<void>(registry.Struct<StatsGetParams>("StatsGetParams", "No parameters."));
		registry.Struct<StatsPassSummary>("StatsPassSummary", "One pass of a displayed view; GPU time may belong to its separately identified delayed GPU frame.")
			.Field("name", &StatsPassSummary::Name, "Stable pass name.")
			.Field("cpuMilliseconds", &StatsPassSummary::CpuMilliseconds, "Approximate CPU recording time in milliseconds.")
			.Field("gpuMilliseconds", &StatsPassSummary::GpuMilliseconds, "Approximate completed GPU duration in milliseconds when available.")
			.Field("gpuAvailable", &StatsPassSummary::GpuAvailable, "Whether this pass has a completed GPU sample.")
			.Field("drawCalls", &StatsPassSummary::DrawCalls, "Recorded draw calls in the CPU frame.")
			.Field("dispatches", &StatsPassSummary::Dispatches, "Recorded compute dispatches in the CPU frame.")
			.Field("triangles", &StatsPassSummary::Triangles, "Submitted triangles in the CPU frame.");
		registry.Struct<StatsViewSummary>("StatsViewSummary", "Latest observations for one displayed view; transient screenshots never replace it.")
			.Field("name", &StatsViewSummary::Name, "View name: scene or game.")
			.Field("available", &StatsViewSummary::Available, "Whether a displayed frame has been recorded.")
			.Field("frame", &StatsViewSummary::Frame, "Exact decimal CPU frame index, empty when unavailable.")
			.Field("gpuFrame", &StatsViewSummary::GpuFrame, "Exact decimal source frame of delayed GPU samples, empty when unavailable.")
			.Field("gpuAvailable", &StatsViewSummary::GpuAvailable, "Whether any completed GPU frame is available.")
			.Field("width", &StatsViewSummary::Width, "Rendered framebuffer width.")
			.Field("height", &StatsViewSummary::Height, "Rendered framebuffer height.")
			.Field("cpuMilliseconds", &StatsViewSummary::CpuMilliseconds, "Approximate total CPU recording time in milliseconds.")
			.Field("passes", &StatsViewSummary::Passes, "Pass observations in render order.")
			.Field("shadowDraws", &StatsViewSummary::ShadowDraws, "Depth caster draw calls.")
			.Field("shadowedSpotLights", &StatsViewSummary::ShadowedSpotLights, "Allocated spot shadow tiles.")
			.Field("droppedSpotShadows", &StatsViewSummary::DroppedSpotShadows, "Contributing spot shadows beyond atlas capacity.");
		registry.Struct<StatsGetResult>("StatsGetResult", "A nonblocking copy of host observations; unavailable services are identified explicitly.")
			.Field("fps", &StatsGetResult::Fps, "Approximate observed host frames per second.")
			.Field("cpuMilliseconds", &StatsGetResult::CpuMilliseconds, "Approximate observed host CPU frame time in milliseconds.")
			.Field("droppedSeconds", &StatsGetResult::DroppedSeconds, "Approximate accumulated scheduler time dropped in this session.")
			.Field("entities", &StatsGetResult::Entities, "Active target scene entity count.")
			.Field("bodies", &StatsGetResult::Bodies, "Live physics body count.")
			.Field("voices", &StatsGetResult::Voices, "Live audio voice count.")
			.Field("scriptAvailable", &StatsGetResult::ScriptAvailable, "Whether the scripting heap service exists.")
			.Field("scriptHeapBytes", &StatsGetResult::ScriptHeapBytes, "Approximate current script heap bytes when available.")
			.Field("scriptSoftLimitBytes", &StatsGetResult::ScriptSoftLimitBytes, "Approximate script heap soft limit in bytes when available.")
			.Field("scriptHardLimitBytes", &StatsGetResult::ScriptHardLimitBytes, "Approximate script heap hard limit in bytes when available.")
			.Field("memoryAllocationCount", &StatsGetResult::MemoryAllocationCount, "Current Vulkan device memory allocation count.")
			.Field("maxMemoryAllocationCount", &StatsGetResult::MaxMemoryAllocationCount, "Vulkan device allocation limit, zero without a device.")
			.Field("views", &StatsGetResult::Views, "Displayed scene then game views; Runtime supplies game only.");
	}

	void RegisterStatsMethods(MethodRegistry& methods)
	{
		methods.Add({ .Name = "stats.get", .Description = "Reads host and displayed-view statistics without waiting for the GPU or advancing a frame.", .SupportsDryRun = true, .AvailableInRuntime = true, .AllowedInBatch = true, .Examples = { { .Description = "Inspect current host and renderer observations.", .Params = Json::object() } } }, &Automation::StatsGet);
	}

}
