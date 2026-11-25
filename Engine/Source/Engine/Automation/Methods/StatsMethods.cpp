#include "EnginePCH.h"
#include "Engine/Automation/Methods/StatsMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Platform/ProcessHeap.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Session/PlaySession.h"
#include "Engine/Session/SessionStatistics.h"

#include <cstdint>
#include <limits>

namespace Engine {

	float ToStatsTelemetry(double value)
	{
		if (!(value > 0.0))
			return 0.0f;
		constexpr float Maximum = std::numeric_limits<float>::max();
		return value >= static_cast<double>(Maximum) ? Maximum : static_cast<float>(value);
	}

	Result<StatsGetResult> Automation::StatsGet(AutomationMethodContext& context, const StatsGetParams& params)
	{
		ENGINE_TRY_ASSIGN(StatsGetResult result, context.GetHostStatistics());
		if (!params.Views)
			result.Views.clear();
		// M21's milestone review: the bodies of both physics worlds, so the hardening soak's no-growth rule sees 2D bodies too;
		// M28: their awake bodies and the live particles (SessionStatistics).
		if (const PlaySession* session = context.GetPlaySession(); session != nullptr)
		{
			const SessionStatistics counters = ReadSessionStatistics(*session);
			result.Bodies = counters.Bodies;
			result.ActiveBodies = counters.ActiveBodies;
			result.Particles = counters.Particles;
			// M43: the last tick's work and the last extraction's static cell rebuilds.
			result.TransformsUpdated = counters.TransformsUpdated;
			result.EntitiesVisited = counters.EntitiesVisited;
			result.StaticCellRebuilds = counters.StaticCellRebuilds;
		}
		if (const PlaySession* session = context.GetPlaySession(); session != nullptr && session->GetScripts() != nullptr)
		{
			const ScriptMemoryState memory = session->GetScripts()->GetMemoryState();
			result.ScriptAvailable = true;
			result.ScriptHeapBytes = ToStatsTelemetry(static_cast<double>(memory.UsedBytes));
			result.ScriptSoftLimitBytes = ToStatsTelemetry(static_cast<double>(memory.SoftLimitBytes));
			result.ScriptHardLimitBytes = ToStatsTelemetry(static_cast<double>(memory.HardLimitBytes));
		}
		// M25's soak (ADR 0036, "The soak"): the C runtime heap's committed bytes, in both hosts, from the process itself.
		if (const Result<uint64_t> heap = ProcessHeap::GetCommittedBytes(); heap.has_value())
		{
			result.ProcessHeapAvailable = true;
			result.ProcessHeapCommittedBytes = ToStatsTelemetry(static_cast<double>(*heap));
		}
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
		result.ShadowedPointLights = stats.ShadowedPointLights;
		result.DroppedPointShadows = stats.DroppedPointShadows;
		result.ShadowMapSize = stats.ShadowMapSize;
		result.SsaoHalfResolution = stats.SsaoHalfResolution;
		result.HistoryFrames = stats.HistoryFrames;
		result.MaxShadedLights = stats.MaxShadedLights;
		result.CachedPointShadows = stats.CachedPointShadows;
		result.CachedSpotShadows = stats.CachedSpotShadows;
		result.SsrHalfResolution = stats.SsrHalfResolution;
		result.SsrAreaFrame = stats.SsrAreaAvailable ? std::to_string(stats.SsrAreaFrameIndex) : std::string();
		result.SsrTiles = stats.SsrTiles;
		result.SsrTracedTiles = stats.SsrTracedTiles;
		result.SsrTracedPixels = stats.SsrTracedPixels;
		result.Lights = stats.VisibleLights;
		result.DroppedLights = stats.DroppedLights;
		result.SkinnedMeshes = stats.SkinnedMeshes;
		result.PlanarReflections = stats.PlanarReflections;
		result.DroppedPlanarReflections = stats.DroppedPlanarReflections;
		result.CulledPlanarReflections = stats.CulledPlanarReflections;
		for (const UUID& entity : stats.PlanarReflectionEntities)
			result.PlanarReflectionEntities.push_back(entity.ToString());
		result.StaticItems = stats.StaticItems;
		result.StaticCells = stats.StaticCells;
		result.StaticCellsVisible = stats.StaticCellsVisible;
		result.DynamicItems = stats.DynamicItems;
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
		registry.Struct<StatsGetParams>("StatsGetParams", "Which statistics stats.get reports.")
			.Field("views", &StatsGetParams::Views,
				"Optional: false leaves out the per-view render statistics (views is empty), for a counter read such as a soak check's "
				"entities, bodies, voices and heap; true, the default, reports them.");
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
			.Field("droppedSpotShadows", &StatsViewSummary::DroppedSpotShadows, "Contributing spot shadows beyond atlas capacity.")
			.Field("shadowedPointLights", &StatsViewSummary::ShadowedPointLights, "Point lights the frame rendered with shadows.")
			.Field("droppedPointShadows", &StatsViewSummary::DroppedPointShadows,
				"Visible shadow-casting point lights beyond the budget of four, lit without shadows.")
			.Field("shadowMapSize", &StatsViewSummary::ShadowMapSize, "Shadow map size the frame rendered with (the project's Rendering.ShadowMapSize); 0 when unavailable.")
			.Field("ssaoHalfResolution", &StatsViewSummary::SsaoHalfResolution, "Whether the frame rendered with the project's Rendering.SsaoHalfResolution set.")
			.Field("historyFrames", &StatsViewSummary::HistoryFrames,
				"Renders since the view's temporal history (motion vectors, TAA's jitter and history, auto-exposure's adaptation) last started afresh: 0 at "
				"its first render, after a scene change, play starting or stopping, or a camera cut; one more each render after.")
			.Field("maxShadedLights", &StatsViewSummary::MaxShadedLights,
				"The most lights the frame shaded (the project's Rendering.MaxShadedLights: 256, 512, 1024 or 2048); 0 when unavailable.")
			.Field("cachedPointShadows", &StatsViewSummary::CachedPointShadows,
				"Shadowed point lights whose shadow maps the frame reused unchanged, drawing no caster (the static shadow cache).")
			.Field("cachedSpotShadows", &StatsViewSummary::CachedSpotShadows,
				"Shadowed spot lights whose atlas tiles the frame reused unchanged (the atlas is reused whole or redrawn whole).")
			.Field("ssrHalfResolution", &StatsViewSummary::SsrHalfResolution, "Whether the frame rendered with the project's Rendering.SsrHalfResolution set.")
			.Field("ssrAreaFrame", &StatsViewSummary::SsrAreaFrame,
				"Exact decimal source frame of the SSR area below, a completed GPU frame; empty while no completed frame recorded SSR.")
			.Field("ssrTiles", &StatsViewSummary::SsrTiles,
				"The SSR trace image's 8 x 8 tiles in the frame of ssrAreaFrame: the scene image's, or a quarter of them at half resolution; 0 when "
				"unavailable.")
			.Field("ssrTracedTiles", &StatsViewSummary::SsrTracedTiles,
				"The tiles the SSR trace covered, those holding a glossy pixel (roughness at most SsrMaxRoughness, the sky excluded); 0 when unavailable.")
			.Field("ssrTracedPixels", &StatsViewSummary::SsrTracedPixels,
				"The trace pixels that traced a reflection in the covered tiles (at half resolution each stands for four scene pixels); 0 when "
				"unavailable.")
			.Field("lights", &StatsViewSummary::Lights, "Lights the frame shaded after culling, at most maxShadedLights.")
			.Field("droppedLights", &StatsViewSummary::DroppedLights,
				"Visible lights beyond maxShadedLights, left unshaded (RENDER_LIGHT_LIMIT_EXCEEDED's).")
			.Field("skinnedMeshes", &StatsViewSummary::SkinnedMeshes, "Meshes with a skin the frame deformed (morph-only meshes are not counted).")
			.Field("planarReflections", &StatsViewSummary::PlanarReflections,
				"Planar reflections the frame drew, at most 4: PlanarReflection mirrors in view and facing the camera, each rendered from the "
				"camera mirrored in its plane; its cost is the pass row PlanarReflection<k> of its slot k.")
			.Field("droppedPlanarReflections", &StatsViewSummary::DroppedPlanarReflections,
				"Planar reflections in view and facing the camera beyond the 4 the frame draws (the smallest on screen), or whose GPU objects, "
				"or the view's probe constants, could not be created this render (the render reports that error); they reflect the probes and "
				"screen-space reflections instead.")
			.Field("culledPlanarReflections", &StatsViewSummary::CulledPlanarReflections,
				"Planar reflections the frame did not consider: out of view, seen from behind their reflecting side, or in a view that draws "
				"none (a debug view or the wireframe); they cost nothing.")
			.Field("planarReflectionEntities", &StatsViewSummary::PlanarReflectionEntities,
				"The entities of the drawn planar reflections, in slot order (the largest on screen first), their ids as text.")
			.Field("staticItems", &StatsViewSummary::StaticItems,
				"Mesh items of the frame's static set: the MeshRenderers of entities a Static makes static scenery, which render extraction "
				"does not visit and the renderer prepares once per cell.")
			.Field("staticCells", &StatsViewSummary::StaticCells, "Cells of the frame's static set: 32 m squares on the XZ plane, each holding the static items whose origin lies in it.")
			.Field("staticCellsVisible", &StatsViewSummary::StaticCellsVisible,
				"Static cells the view's camera did not cull whole: the view's static work follows them, testing only their items.")
			.Field("dynamicItems", &StatsViewSummary::DynamicItems, "Mesh items render extraction visited for the frame: every MeshRenderer that is not static scenery.");
		registry.Struct<StatsGetResult>("StatsGetResult", "A nonblocking copy of host observations; unavailable services are identified explicitly.")
			.Field("fps", &StatsGetResult::Fps, "Approximate observed host frames per second.")
			.Field("cpuMilliseconds", &StatsGetResult::CpuMilliseconds, "Approximate observed host CPU frame time in milliseconds.")
			.Field("droppedSeconds", &StatsGetResult::DroppedSeconds, "Approximate accumulated scheduler time dropped in this session.")
			.Field("entities", &StatsGetResult::Entities, "Active target scene entity count.")
			.Field("bodies", &StatsGetResult::Bodies, "Live physics body count of the play session: the 3D world's bodies and the 2D world's together; 0 without a session.")
			.Field("voices", &StatsGetResult::Voices, "Live audio voice count.")
			.Field("scriptAvailable", &StatsGetResult::ScriptAvailable, "Whether the scripting heap service exists.")
			.Field("scriptHeapBytes", &StatsGetResult::ScriptHeapBytes, "Approximate current script heap bytes when available.")
			.Field("scriptSoftLimitBytes", &StatsGetResult::ScriptSoftLimitBytes, "Approximate script heap soft limit in bytes when available.")
			.Field("scriptHardLimitBytes", &StatsGetResult::ScriptHardLimitBytes, "Approximate script heap hard limit in bytes when available.")
			.Field("processHeapAvailable", &StatsGetResult::ProcessHeapAvailable,
				"Whether the host measured the heap the C runtime allocates from: false on Linux and macOS, which have no measure, and "
				"when the query fails.")
			.Field("processHeapCommittedBytes", &StatsGetResult::ProcessHeapCommittedBytes,
				"Approximate committed bytes of the heap the C runtime allocates from (malloc and new: the engine's own memory and that of "
				"every library allocating through the C runtime; on Windows the process heap, its large blocks included), when available. "
				"What a graphics driver keeps in allocations of its own is not in it, nor, in an AddressSanitizer build, what the C "
				"runtime or HeapAlloc allocates, which the sanitizer's allocator serves. The hardening soak's memory rule measures it.")
			.Field("memoryAllocationCount", &StatsGetResult::MemoryAllocationCount,
				"Current Vulkan device memory allocation count, which counts what the frame released until the frame's garbage collection "
				"frees it at the frame's end.")
			.Field("maxMemoryAllocationCount", &StatsGetResult::MaxMemoryAllocationCount, "Vulkan device allocation limit, zero without a device.")
			.Field("views", &StatsGetResult::Views,
				"The scene then the game view, as the editor's displayed viewport panels: a view whose panel is not displayed is unavailable, and a "
				"headless editor displays only the Scene view of its default layout, so game is unavailable there unless the editor was started "
				"with --headless-game-view, whose game view renders every frame at the project's window size (or viewport.setOptions' "
				"gameResolution). The Runtime supplies game only.")
			.Field("deviceLosses", &StatsGetResult::DeviceLosses,
				"The GPU device losses this host recovered from since it started, each also a GpuDeviceLost event; 0 with renderer none.")
			.Field("activeBodies", &StatsGetResult::ActiveBodies, "Awake physics bodies of the play session, the 3D world's and the 2D world's together; 0 without a session.")
			.Field("particles", &StatsGetResult::Particles, "Live particles of the play session after its last Particles phase; 0 without a session.")
			.Field("transformsUpdated", &StatsGetResult::TransformsUpdated,
				"World matrices the play session's last tick and the frame phase after it recomputed: only the entities whose transforms "
				"changed and their descendants; 0 without a session.")
			.Field("entitiesVisited", &StatsGetResult::EntitiesVisited,
				"Entities the per-entity passes of the play session's last tick visited (physics, audio, UI, animation, particles and "
				"scripts), summed: 0 a tick in which nothing changes; 0 without a session.")
			.Field("staticCellRebuilds", &StatsGetResult::StaticCellRebuilds,
				"Static cells the session scene's last render extraction rebuilt or removed: a write to a static entity rebuilds its cell "
				"at the next extracted frame, so this counts per rendered frame, not per tick; 0 without a session.");
	}

	void RegisterStatsMethods(MethodRegistry& methods)
	{
		methods.Add({ .Name = "stats.get", .Description = "Reads host and displayed-view statistics without waiting for the GPU or advancing a frame; views: false leaves the per-view render statistics out.", .SupportsDryRun = true, .AvailableInRuntime = true, .AllowedInBatch = true, .Examples = { { .Description = "Inspect current host and renderer observations.", .Params = Json::object() }, { .Description = "Read the counters only, without the per-view render statistics.", .Params = Json{ { "views", false } } } } }, &Automation::StatsGet);
	}

}
