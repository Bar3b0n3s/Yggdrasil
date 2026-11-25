#pragma once

#include <cstdint>

// M28 (Docs/Decisions/0043-m28-game-services.md decision 4): the performance readout's struct, in a header of M28's own so
// that the readout rows phase 5's ledger gives M29 (its light rows) and M32 (its SSR row) join it at their merges, appended
// in landing order, without touching Scripting/ScriptHost.h, whose ledger row closes it to them. ScriptHost.h includes it.

namespace Engine {

	// M28 (Docs/Decisions/0043-m28-game-services.md decision 4): Application.GetStats, the performance readout. Frame-paced
	// observations, never simulation state: a fixed-step gameplay callback cannot read them (ParticleEmitter:GetParticleCount's
	// rule). The render counters are the game view's last rendered frame (its passes' draw calls, triangles and GPU times,
	// the GPU times of the frame the profiler last read back); without a renderer RenderAvailable is false and they are 0.
	struct ScriptPerformanceStats
	{
		bool RenderAvailable = false;
		bool GpuAvailable = false;
		double Fps = 0.0;
		double CpuMilliseconds = 0.0;
		double GpuMilliseconds = 0.0;
		uint32_t VisibleLights = 0;
		uint32_t DroppedLights = 0;
		uint32_t ShadowedLights = 0; // the shadowed spot and point lights
		uint32_t DrawCalls = 0;
		uint64_t Triangles = 0;
		uint32_t Particles = 0; // the live particles after the last Particles phase
		uint32_t SkinnedMeshes = 0;
		uint32_t Bodies = 0; // the 3D and 2D worlds' bodies
		uint32_t ActiveBodies = 0;
		uint32_t Entities = 0;
		uint64_t ScriptHeapBytes = 0;
		// M29's light rows (Docs/Decisions/0044-m29-light-limits.md decisions 4 and 5; ADR 0042's phase-5 ledger, carried by M28
		// at its later landing): the shaded-light cap the frame rendered with, and the shadowed point and spot lights whose maps
		// it reused from the static shadow cache.
		uint32_t MaxShadedLights = 0;
		uint32_t CachedPointShadows = 0;
		uint32_t CachedSpotShadows = 0;
		// M32's SSR row (Docs/Decisions/0047-m32-fog-ssr.md decision 13, carried the same way): the area the SSR trace covered
		// in the last completed GPU frame that recorded it: the trace image's 8 x 8 tiles, those it traced and the trace
		// pixels that traced; 0 while unavailable (no completed frame with SSR).
		uint32_t SsrTiles = 0;
		uint32_t SsrTracedTiles = 0;
		uint32_t SsrTracedPixels = 0;
		// M42 (Docs/Decisions/0059-m42-planar-reflections.md decision 3): the planar reflections the frame drew and those it dropped
		// past the cap of 4 or for want of their GPU objects or the view's probe constants (RenderStats' PlanarReflections and
		// DroppedPlanarReflections).
		uint32_t PlanarReflections = 0;
		uint32_t DroppedPlanarReflections = 0;
		// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3; ADR 0060's ledger: first of phase 8's members): the
		// session's last tick's work (PlaySession::GetLastTickWork: TransformsUpdated, the world matrices recomputed;
		// EntitiesVisited, the entities its per-tick passes visited; StaticCellRebuilds, the static cells its frame phase's
		// extraction rebuilt or removed, never an editor view's), the static items of its scene's last render extraction's set
		// (StaticItems; both in every mode, a renderer or not: extraction runs either way), and the static cells the game
		// view's camera did not cull whole (StaticCellsVisible, RenderStats').
		uint32_t TransformsUpdated = 0;
		uint32_t EntitiesVisited = 0;
		uint32_t StaticCellRebuilds = 0;
		uint32_t StaticItems = 0;
		uint32_t StaticCellsVisible = 0;
		// M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; ADR 0060's phase-8 ledger: after M43's members): 1 when
		// the frame regenerated its Sky's image-based light, else 0 (RenderStats::SkyUpdates).
		uint32_t SkyUpdates = 0;
	};

}
