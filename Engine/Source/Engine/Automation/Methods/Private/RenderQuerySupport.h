#pragma once

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/RaycastMethods.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneRaycast.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"

namespace Engine::Utils {

	// Request-local values only. ExtractView refreshes interpolation-reset tags after writes between ticks as well as
	// world transforms; querying a play scene must use the same refresh path as rendering it.
	inline Result<RenderSnapshot> ExtractQueryView(AutomationMethodContext& context, Scene& scene, const RenderExtractionRequest& request)
	{
		PlaySession* session = context.GetPlaySession();
		if (session != nullptr && &session->GetScene() == &scene)
			return session->ExtractView(request);
		TransformSystem::Update(scene);
		return ExtractRenderSnapshot(scene, request);
	}

	inline Result<PhysicsLayerTable> GetQueryLayers(const AutomationMethodContext& context)
	{
		const ProjectSettings* settings = context.GetProjectSettings();
		if (settings == nullptr)
			return MakeError(ErrorCode::InvalidState, "no project settings available for visual queries");
		return PhysicsLayerTable::Create(settings->Physics.Layers, settings->Physics.Collisions);
	}

	inline SceneRaycastResult SummarizeVisualHit(const AutomationMethodContext& context, const Scene& scene, const std::optional<SceneRaycastHit>& hit)
	{
		SceneRaycastResult result;
		if (hit)
		{
			result.Hit = true;
			result.Entity = context.MakeEntitySummary(scene.FindEntityByID(hit->Entity));
			result.Position = hit->Position;
			result.Normal = hit->Normal;
			result.Barycentric = hit->Barycentric;
			result.Distance = hit->Distance;
			result.Submesh = hit->Submesh;
			result.Triangle = hit->Triangle;
		}
		return result;
	}

}
