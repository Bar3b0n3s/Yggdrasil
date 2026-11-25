#include "EnginePCH.h"
#include "Engine/Session/SessionStatistics.h"

#include "Engine/Renderer/RenderStats.h"
#include "Engine/Scene/ParticleSystem.h"
#include "Engine/Scene/Physics2DSystem.h"
#include "Engine/Scene/PhysicsSystem.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/ScriptHost.h"
#include "Engine/Session/PlaySession.h"

namespace Engine {

	SessionStatistics ReadSessionStatistics(const PlaySession& session)
	{
		SessionStatistics result;
		result.Entities = static_cast<uint32_t>(session.GetScene().GetEntityCount());
		result.Bodies = CountSessionBodies(session);
		const Physics2DSystem* physics2D = session.GetPhysics2D();
		result.ActiveBodies = session.GetPhysics().GetStats().ActiveBodyCount + (physics2D != nullptr ? physics2D->GetStats().ActiveBodyCount : 0);
		if (const ParticleSystem* particles = session.GetParticles(); particles != nullptr)
			result.Particles = particles->GetStats().Particles;
		return result;
	}

	uint32_t CountSessionBodies(const PlaySession& session)
	{
		const Physics2DSystem* physics2D = session.GetPhysics2D();
		return session.GetPhysics().GetStats().BodyCount + (physics2D != nullptr ? physics2D->GetStats().BodyCount : 0);
	}

	void AddRenderStatistics(ScriptPerformanceStats& stats, const RenderStats& view)
	{
		if (!view.Available)
			return;
		stats.RenderAvailable = true;
		stats.GpuAvailable = view.GpuAvailable;
		stats.VisibleLights = view.VisibleLights;
		stats.DroppedLights = view.DroppedLights;
		stats.ShadowedLights = view.ShadowedSpotLights + view.ShadowedPointLights;
		stats.SkinnedMeshes = view.SkinnedMeshes;
		stats.MaxShadedLights = view.MaxShadedLights;
		stats.CachedPointShadows = view.CachedPointShadows;
		stats.CachedSpotShadows = view.CachedSpotShadows;
		stats.PlanarReflections = view.PlanarReflections;
		stats.DroppedPlanarReflections = view.DroppedPlanarReflections;
		stats.SkyUpdates = view.SkyUpdates;
		if (view.SsrAreaAvailable)
		{
			stats.SsrTiles = view.SsrTiles;
			stats.SsrTracedTiles = view.SsrTracedTiles;
			stats.SsrTracedPixels = view.SsrTracedPixels;
		}
		for (const RenderPassStats& pass : view.Passes)
		{
			stats.DrawCalls += pass.DrawCalls;
			stats.Triangles += pass.Triangles;
			if (view.GpuAvailable && pass.GpuAvailable)
				stats.GpuMilliseconds += pass.GpuMilliseconds;
		}
	}

}
