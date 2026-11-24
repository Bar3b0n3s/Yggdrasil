#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/UUID.h"
#include "Engine/Graphics/GpuProfiler.h"
#include "Engine/Renderer/RenderStage.h"

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>
#include <vector>

// Value snapshots for §8.2 and §13.7. Timings are diagnostic, never simulation input or part of a state hash.
namespace Engine {

	struct RenderPassStats
	{
		std::string Name{}; // fixed pass-list order, stable names in ADR 0017
		double CpuMilliseconds = 0.0;
		double GpuMilliseconds = 0.0;
		bool GpuAvailable = false; // false means no completed sample, not zero GPU cost
		uint32_t DrawCalls = 0;
		uint32_t Dispatches = 0;
		uint32_t Triangles = 0;
	};

	struct RenderStats
	{
		bool Available = false;     // no completed render/renderer none -> false and empty passes
		uint64_t FrameIndex = 0;    // CPU counters' frame
		uint64_t GpuFrameIndex = 0; // completed GPU timing frame; never relabel old samples as current
		bool GpuAvailable = false;
		uint32_t Width = 0;
		uint32_t Height = 0;
		// The snapshot's RenderQualitySettings this frame rendered with (RenderSnapshot::Quality: the project's
		// ProjectSettings.Rendering in every host); 0 and false while unavailable.
		uint32_t ShadowMapSize = 0;
		bool SsaoHalfResolution = false;
		// M26 (W26.2's review; Docs/Decisions/0037-m26-contract.md decision 10): the renders since the view's temporal
		// history (Renderer/TemporalHistory.h: its motion, TAA's jitter and history, auto-exposure's adaptation) last started
		// afresh: 0 for a render whose snapshot's history was reset (no camera or no CameraData::PreviousViewProjection: a
		// view's first render, a scene change, a camera cut, a capture's first render), else one more than this renderer's
		// previous render's, saturating. A host that applies its view's history on every render counts up; 0 every frame
		// means it does not.
		uint32_t HistoryFrames = 0;
		double CpuMilliseconds = 0.0;
		std::vector<RenderPassStats> Passes{};
		uint32_t VisibleMeshes = 0;
		uint32_t CulledSubmeshes = 0;
		uint32_t ShadowDraws = 0;
		uint32_t ShadowedSpotLights = 0;
		uint32_t DroppedSpotShadows = 0;
		// M23 (R-PLS, Docs/Decisions/0034-m23-contract.md decision 3): the point lights the frame shadowed (the plan's lights,
		// at most MaxPointShadowLights) and the visible shadow-casting point lights beyond that budget, lit unshadowed.
		uint32_t ShadowedPointLights = 0;
		uint32_t DroppedPointShadows = 0;
		uint32_t MemoryAllocationCount = 0; // tracked Vulkan device allocations, not NVRHI object count
		uint32_t MaxMemoryAllocationCount = 0;
		// M29 (Docs/Decisions/0044-m29-light-limits.md decision 5): the snapshot's RenderQualitySettings::MaxShadedLights the
		// frame shaded at most (the project's Rendering.MaxShadedLights in every host); 0 while unavailable.
		uint32_t MaxShadedLights = 0;
		// M29 (decision 4): the shadowed point and spot lights whose maps the frame reused from the static shadow cache, without
		// drawing a caster (of ShadowedPointLights and ShadowedSpotLights; the spot atlas is reused whole or not at all).
		uint32_t CachedPointShadows = 0;
		uint32_t CachedSpotShadows = 0;
		// M32 (Docs/Decisions/0047-m32-fog-ssr.md decisions 11 and 13; ADR 0042's phase-5 ledger: appended after M29's rows in
		// landing order, M28's following at its later landing): the snapshot's RenderQualitySettings::SsrHalfResolution this frame rendered with; and the SSR
		// trace's area in the completed GPU frame SsrAreaFrameIndex (read back, as the GPU timings are, once that frame's
		// submission completed): the trace image's 8 x 8 tiles (SsrTiles), those the trace covered (SsrTracedTiles) and the
		// trace pixels that traced in them (SsrTracedPixels; at half resolution each stands for four scene pixels), and the
		// trace's dispatch, its groups along x and its rows (SsrTraceGroups and SsrTraceRows, SsrPass::GetTraceGroups of
		// SsrTracedTiles; M32's milestone review). SsrAreaAvailable is false, and the counts 0, while no completed frame
		// recorded SSR, or the last completed frame recorded none.
		bool SsrHalfResolution = false;
		bool SsrAreaAvailable = false;
		uint64_t SsrAreaFrameIndex = 0;
		uint32_t SsrTiles = 0;
		uint32_t SsrTracedTiles = 0;
		uint32_t SsrTracedPixels = 0;
		uint32_t SsrTraceGroups = 0;
		uint32_t SsrTraceRows = 0;
		// M28 (Docs/Decisions/0043-m28-game-services.md decision 4; ADR 0042's phase-5 ledger: appended after M29's and M32's
		// rows, which landed first): the lights the frame shaded after culling (at most MaxShadedLights, M29's cap), the
		// visible lights beyond that cap (RENDER_LIGHT_LIMIT_EXCEEDED's), and the skinned meshes the Deform stage deformed for
		// the frame (a mesh with a skin; a morph-only mesh is not counted).
		uint32_t VisibleLights = 0;
		uint32_t DroppedLights = 0;
		uint32_t SkinnedMeshes = 0;
		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3; its ledger: appended after M28's rows): the frame's
		// planar reflections (Renderer/PlanarReflections.h). PlanarReflections: those drawn (at most
		// MaxPlanarReflectionsPerView); DroppedPlanarReflections: the candidates (in view, facing the camera, in a Lit view
		// outside the wireframe) past that cap, or whose GPU objects, or the view's probe constants, could not be created this
		// render (the render returns that error), which fall back to the probes and SSR; CulledPlanarReflections: the
		// snapshot's others (out of view, facing away, a plane that is not finite, or in a view that draws none);
		// PlanarReflectionEntities: the drawn reflections' entities in slot order, slot k's cost the pass row
		// PlanarReflection<k>.
		uint32_t PlanarReflections = 0;
		uint32_t DroppedPlanarReflections = 0;
		uint32_t CulledPlanarReflections = 0;
		std::vector<UUID> PlanarReflectionEntities{};
		// M47 (Docs/Decisions/0065-m47-water.md decision 3; phase 8's ledger: after M46's fields): WaterSurfaces, the snapshot's
		// waters when the Water pass's RecordSurface succeeds (it draws every one), 0 when it does not record or fails; Underwater,
		// whether the Underwater pass recorded (the camera's near plane may lie under a water). Both 0 until W47.2's render stream
		// and W47.3 record the passes.
		uint32_t WaterSurfaces = 0;
		bool Underwater = false;
	};

	// M32 (decision 13): the SSR area of one completed frame, which RenderStatsHistory::PublishSsrArea merges into the
	// latest stats: unavailable when the frame recorded no SSR.
	struct RenderSsrAreaSample
	{
		bool Available = false;
		uint64_t FrameIndex = 0;
		uint32_t Tiles = 0;
		uint32_t TracedTiles = 0;
		uint32_t TracedPixels = 0;
		uint32_t TraceGroups = 0;
		uint32_t TraceRows = 0;
	};

	struct RenderPassCounters
	{
		uint32_t DrawCalls = 0;
		uint32_t Dispatches = 0;
		uint32_t Triangles = 0;
	};

	// Borrowed per-view recording boundary, owned on the stack by SceneRenderer::Render. stats/profiler outlive it;
	// passes retain neither this object nor its command list. Main thread. No Graphics -> Renderer dependency.
	// nowSeconds is an owned callback to a monotonic CPU diagnostic clock, never a simulation clock. CPU-only tests
	// inject a manual clock and null profiler/list. Production passes supply the view's profiler and open list.
	class RenderRecordingContext
	{
	public:
		RenderRecordingContext(RenderStats& stats, std::function<double()> nowSeconds, GpuProfiler* profiler = nullptr);
		~RenderRecordingContext();
		RenderRecordingContext(const RenderRecordingContext&) = delete;
		RenderRecordingContext& operator=(const RenderRecordingContext&) = delete;
		// Flat, nonnested scopes; names/order are ADR0017's pass list. Copies name. A repeated name accumulates CPU time
		// and counters into the same row; GPU samples of that name sum. Null list means CPU-only (Prepare).
		// A non-null list always gets debug markers, even with a null profiler (busy timing slot); it outlives this scope.
		// Begin/End pairing, finite monotonic clock and no open scope at destruction are asserted programmer invariants.
		void BeginPass(std::string_view name, nvrhi::ICommandList* commandList = nullptr);
		// Ends GPU scope/marker and adds measured CPU duration plus saturating uint32 counters to the current CPU frame.
		// Every success/error path closes a begun scope; counts describe work actually recorded, not candidate draws.
		void EndPass(const RenderPassCounters& counters);
		// M18 (C18.1, the GPU-zone hook; Docs/Decisions/0029-m18-contract.md decision 4). SceneRenderer::Render calls
		// BeginStage before a stage's first pass and EndStage after its last, for every RenderStage in order, whether or not
		// the stage records a pass, so a hook sees every boundary of every Render. With a non-null `commandList` (null for
		// Prepare, which records no GPU work) and a profiler, the stage is one GPU zone named "Stage:<RenderStageToString>"
		// that the stage's pass scopes nest in (depth 1); with a non-null list it always gets its debug marker. Every stage is
		// also a CPU zone of the same name in the process profiler (Profiler::RecordZone), so profiler.capture shows the
		// stages on both timelines. A stage is never a stats row (no pass is named "Stage:..."). Stages do not nest, and
		// BeginPass and EndPass stay valid outside any stage (the context's CPU-only users). Asserted: no BeginStage while a
		// stage or a pass is open; EndStage only for an open stage without an open pass. No stage may be open at destruction,
		// unless an exception unwinds the recording (M18's milestone review): a vk::SystemError thrown out of NVRHI in the
		// middle of a pass or a stage (a device loss that an upload's first write meets) on its way to the frame boundary,
		// which recovers the device. The destructor then ends the open stage's CPU zone and leaves its GPU scopes open: the
		// device-loss release replaces the renderer's GpuProfiler, and the list is retired without being submitted.
		void BeginStage(RenderStage stage, nvrhi::ICommandList* commandList = nullptr);
		void EndStage();
	private:
		struct State;
		Scope<State> m_State{};
	};

	// A CPU copy of one view's samples. No global or shared "last viewport": scene, game and captures own separate
	// histories. The per-view renderer owns a GpuProfiler (reuse Graphics' query pool) for command recording; this value
	// collector merges completed samples without polling/waiting itself. Main thread, never retains caller-owned sample storage.
	class RenderStatsHistory
	{
	public:
		RenderStatsHistory() = default;
		void PublishCpu(RenderStats stats);
		// Consumes only GpuProfiler::GetLastFrameResult(), never an inferred slot/frame plus GetLastFrameSamples().
		// Copies values, retains no reference. Unavailable clears GPU availability/timings (FrameIndex is then ignored);
		// available older frames are ignored. Match by pass name, summing repeated scopes; absent names stay unavailable.
		// PublishCpu retains source-tagged GPU values only for matching names, never invents timing for newly enabled passes.
		// M18 (C18.1, R-PROF; decision 11): an available frame's samples (stages and passes) are also merged into the
		// process profiler's timeline (Profiler::SubmitGpuZones), each at its frame's recording start on the profiler's clock
		// plus its StartMilliseconds, with its Depth, so profiler.capture's Chrome trace shows them beside the CPU zones; a
		// frame is merged once, however often it is published.
		void PublishGpu(const GpuTimingFrame& frame);
		// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 13): the SSR area of a completed frame, as PublishGpu publishes its
		// timings: a sample older than the last published one is ignored; otherwise it sets RenderStats' SsrArea members (the
		// counts 0 when unavailable), which PublishCpu then keeps.
		void PublishSsrArea(const RenderSsrAreaSample& sample);
		// Owns this value; reference valid until the next Publish/Reset. A renderer keeps one history for its lifetime (a scene
		// replacement does not reset it: rows carry their frame indices); Reset returns it to the unavailable state.
		[[nodiscard]] const RenderStats& GetLatest() const;
		void Reset();
	private:
		RenderStats m_Latest{};
		uint64_t m_LastGpuFrameIndex = 0;
		bool m_HasGpuFrame = false;
		uint64_t m_LastSsrAreaFrameIndex = 0;
		bool m_HasSsrArea = false;
	};

}
