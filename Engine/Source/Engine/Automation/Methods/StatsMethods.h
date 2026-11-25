#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	class AutomationMethodContext;
	class MethodRegistry;
	class TypeRegistry;
	struct RenderStats;

	struct StatsGetParams
	{
		// ADR 0038 (friction F09): false leaves the per-view render statistics out (StatsGetResult::Views stays empty), for a
		// counter read such as the soak check's; true, the default, reports them.
		bool Views = true;
	};

	// Approximate diagnostic telemetry, never simulation state. Converts nonnegative observations to finite float:
	// NaN and negative values become zero; positive overflow (including infinity) saturates to float's maximum.
	// Hosts use this before narrowing their double observations or byte counters. Frame identities stay exact strings.
	[[nodiscard]] float ToStatsTelemetry(double value);

	struct StatsPassSummary
	{
		std::string Name{};
		float CpuMilliseconds = 0.0f;
		float GpuMilliseconds = 0.0f;
		bool GpuAvailable = false;
		uint32_t DrawCalls = 0;
		uint32_t Dispatches = 0;
		uint32_t Triangles = 0;
	};

	struct StatsViewSummary
	{
		std::string Name{}; // "scene" or "game"; never the transient screenshot view
		bool Available = false;
		std::string Frame{}; // exact decimal uint64, not rounded through JSON's floating number
		std::string GpuFrame{};
		bool GpuAvailable = false;
		uint32_t Width = 0;
		uint32_t Height = 0;
		float CpuMilliseconds = 0.0f;
		std::vector<StatsPassSummary> Passes{};
		uint32_t ShadowDraws = 0;
		uint32_t ShadowedSpotLights = 0;
		uint32_t DroppedSpotShadows = 0;
		uint32_t ShadowedPointLights = 0; // M23 (R-PLS): RenderStats::ShadowedPointLights
		uint32_t DroppedPointShadows = 0;
		// The quality settings the recorded frame rendered with (RenderStats; the project's ProjectSettings.Rendering, in
		// the Editor and in an exported Runtime alike); 0 and false while unavailable.
		uint32_t ShadowMapSize = 0;
		bool SsaoHalfResolution = false;
		// M26 (W26.2's review): RenderStats::HistoryFrames, the renders since the view's temporal history started afresh.
		uint32_t HistoryFrames = 0;
		// M29 (Docs/Decisions/0044-m29-light-limits.md decision 5): RenderStats::MaxShadedLights, the shaded-light cap in force.
		uint32_t MaxShadedLights = 0;
		// M29 (decision 4): RenderStats::CachedPointShadows and CachedSpotShadows.
		uint32_t CachedPointShadows = 0;
		uint32_t CachedSpotShadows = 0;
		// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 13; ADR 0042's phase-5 ledger: appended in landing order):
		// RenderStats::SsrHalfResolution, and the SSR trace's area in the completed frame SsrAreaFrame (exact decimal, empty while
		// unavailable): the trace image's tiles, the tiles traced and the trace pixels that traced, 0 while unavailable.
		bool SsrHalfResolution = false;
		std::string SsrAreaFrame{};
		uint32_t SsrTiles = 0;
		uint32_t SsrTracedTiles = 0;
		uint32_t SsrTracedPixels = 0;
		// M28 (Docs/Decisions/0043-m28-game-services.md decision 4; appended after M29's and M32's rows, which landed first):
		// RenderStats::VisibleLights, DroppedLights and SkinnedMeshes.
		uint32_t Lights = 0;
		uint32_t DroppedLights = 0;
		uint32_t SkinnedMeshes = 0;
		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3; its ledger: appended after M28's rows):
		// RenderStats::PlanarReflections, DroppedPlanarReflections, CulledPlanarReflections and PlanarReflectionEntities (each
		// entity's id as UUID::ToString gives it, in slot order).
		uint32_t PlanarReflections = 0;
		uint32_t DroppedPlanarReflections = 0;
		uint32_t CulledPlanarReflections = 0;
		std::vector<std::string> PlanarReflectionEntities{};
		// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3; ADR 0060's ledger: first of phase 8's rows): RenderStats'
		// StaticItems, StaticCells, StaticCellsVisible and DynamicItems.
		uint32_t StaticItems = 0;
		uint32_t StaticCells = 0;
		uint32_t StaticCellsVisible = 0;
		uint32_t DynamicItems = 0;
	};

	struct StatsGetResult
	{
		float Fps = 0.0f;
		float CpuMilliseconds = 0.0f;
		float DroppedSeconds = 0.0f; // accumulated scheduler dropped time; reset with the session
		uint32_t Entities = 0;
		uint32_t Bodies = 0;
		uint32_t Voices = 0;
		bool ScriptAvailable = false; // false means there is no live script VM, not "zero memory used"
		float ScriptHeapBytes = 0.0f;
		float ScriptSoftLimitBytes = 0.0f;
		float ScriptHardLimitBytes = 0.0f;
		// M25's soak (Docs/Decisions/0036-m25-tetris-pixel-art.md, "The soak"): the committed bytes of the heap the C runtime
		// allocates from (ProcessHeap, Platform/ProcessHeap.h), which the hardening soak's memory rule measures; false and 0
		// where the platform has no measure (Linux and macOS, ADR 0028 decision 10) and when the query fails.
		bool ProcessHeapAvailable = false;
		float ProcessHeapCommittedBytes = 0.0f;
		uint32_t MemoryAllocationCount = 0;
		uint32_t MaxMemoryAllocationCount = 0;
		std::vector<StatsViewSummary> Views{}; // scene then game; Runtime game only
		// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 31; the plan's device-loss counter): the device losses the
		// host recovered from since it started (GraphicsDevice::GetRecoveryCount), in the Editor and the Runtime; 0 with
		// renderer none. Filled and registered ("deviceLosses") by W18.3's integration.
		uint32_t DeviceLosses = 0;
		// M28 (Docs/Decisions/0043-m28-game-services.md decision 4): the play session's awake bodies of both worlds and its
		// live particles (SessionStatistics); 0 without a session.
		uint32_t ActiveBodies = 0;
		uint32_t Particles = 0;
		// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3; first of phase 8's top-level fields): the play session's
		// last tick's work (PlaySession::GetLastTickWork: the world matrices recomputed in it and its frame phase, the
		// entities its per-tick passes visited, and the static cells its frame phase's extraction rebuilt or removed, never an
		// editor view's); 0 without a session.
		uint32_t TransformsUpdated = 0;
		uint32_t EntitiesVisited = 0;
		uint32_t StaticCellRebuilds = 0;
	};

	// Owned diagnostic copy of one displayed renderer's history, preserving both CPU and delayed GPU identities.
	// No rendering, waits, asset access or mutation. An unavailable history produces only the supplied view name.
	[[nodiscard]] StatsViewSummary MakeStatsViewSummary(std::string name, const RenderStats& stats);

	namespace Automation {

		// Copies host observations only: no GPU wait, render, frame advance, cache eviction, or simulation mutation.
		// Main thread. CPU/body/voice data remains available with renderer none; view.Available/GpuAvailable report absent
		// samples explicitly. InvalidState without an open project/host; Unsupported only for a host lacking the service.
		[[nodiscard]] Result<StatsGetResult> StatsGet(AutomationMethodContext& context, const StatsGetParams& params);

	}

	void RegisterStatsMethodTypes(TypeRegistry& registry);
	// stats.get: read-only, AllowedInBatch, SupportsDryRun, Runtime subset, not launcher, not MCP tool.
	void RegisterStatsMethods(MethodRegistry& methods);

}
