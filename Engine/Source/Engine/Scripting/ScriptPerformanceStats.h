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
	};

}
