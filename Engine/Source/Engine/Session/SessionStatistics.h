#pragma once

#include <cstdint>

// M28 (Docs/Decisions/0043-m28-game-services.md decision 4): the play session's counters that stats.get and the
// performance readout (Application.GetStats) report, in Session so that Dist has them (the Automation directory's symbols
// are a family the Dist map forbids).

namespace Engine {

	class PlaySession;
	struct RenderStats;
	struct ScriptPerformanceStats;

	struct SessionStatistics
	{
		uint32_t Entities = 0;     // the session scene's entities
		uint32_t Bodies = 0;       // CountSessionBodies
		uint32_t ActiveBodies = 0; // the awake bodies of both physics worlds
		uint32_t Particles = 0;    // the live particles after the last Particles phase (ParticleSystem::GetStats)
		// M43 (Docs/Decisions/0061-m43-static-scenery.md decision 3): the last tick's work (PlaySession::GetLastTickWork, each
		// saturating at the uint32 range: the static cells rebuilt or removed are its frame phase's extraction's, never an
		// editor view's) and the items of the session scene's static set as its last render extraction refreshed it
		// (Scene::GetStaticCache; 0 without a cache).
		uint32_t TransformsUpdated = 0;
		uint32_t EntitiesVisited = 0;
		uint32_t StaticCellRebuilds = 0;
		uint32_t StaticItems = 0;
	};

	// The counters of `session` now. Main thread; reads only.
	[[nodiscard]] SessionStatistics ReadSessionStatistics(const PlaySession& session);

	// M21's milestone review: the live bodies of `session`'s physics worlds, the 3D world's and the 2D world's together:
	// stats.get's bodies (which the hardening soak's no-growth rule reads) and the Runtime's stats overlay.
	[[nodiscard]] uint32_t CountSessionBodies(const PlaySession& session);

	// The render part of the performance readout from one view's last RenderStats (IPlaySessionHost::ReadFrameStatistics):
	// RenderAvailable, GpuAvailable, the lights (visible, dropped, and the shadowed spot and point lights), the skinned
	// meshes, and the passes' draw calls and triangles summed and GPU milliseconds summed over the passes with a GPU sample.
	// A view without a render (RenderStats::Available false) leaves `stats` unchanged.
	void AddRenderStatistics(ScriptPerformanceStats& stats, const RenderStats& view);

}
