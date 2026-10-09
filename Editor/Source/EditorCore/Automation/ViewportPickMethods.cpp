#include "EditorPCH.h"
#include "EditorCore/Automation/ViewportPickMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/SceneRaycast.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"

namespace Engine {

	Result<ViewportPickResult> Automation::ViewportPick(EditorMethodContext& context, const ViewportPickParams& params)
	{
		if (params.View != ViewportView::Scene && params.View != ViewportView::Game)
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/view", "expected scene or game"));
		ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), false));
		AssetManager* assets = context.GetAssets();
		if (assets == nullptr)
			return MakeError(ErrorCode::Unsupported, "this host has no asset manager for visual queries");
		ENGINE_TRY_ASSIGN(const ViewportPixelSize size, context.GetViewportPixelSize(params.View));
		if (size.Width == 0 || size.Height == 0)
			return MakeError(ErrorCode::InvalidState, "viewport framebuffer is unavailable");
		if (params.X >= size.Width)
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/x", "pixel is outside the viewport width"));
		if (params.Y >= size.Height)
			return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/y", "pixel is outside the viewport height"));
		RenderExtractionRequest request;
		request.Width = size.Width;
		request.Height = size.Height;
		if (params.View == ViewportView::Scene)
		{
			const auto camera = context.GetSceneViewCamera();
			if (!camera)
				return MakeError(ErrorCode::InvalidState, "scene view camera is unavailable");
			request.Camera = RenderCameraSource::Explicit;
			request.ExplicitCamera = *camera;
		}
		const ProjectSettings* settings = context.GetProjectSettings();
		if (settings == nullptr)
			return MakeError(ErrorCode::InvalidState, "no project settings available for visual queries");
		PlaySession* session = context.GetPlaySession();
		const bool playTarget = session != nullptr && &session->GetScene() == scene;
		if (!playTarget)
			TransformSystem::Update(*scene);
		ENGINE_TRY_ASSIGN(const RenderSnapshot snapshot, playTarget ? session->ExtractView(request) : ExtractRenderSnapshot(*scene, request));
		if (!snapshot.HasCamera)
			return MakeError(ErrorCode::InvalidState, "SCENE_NO_PRIMARY_CAMERA: target scene has no active primary camera");
		ENGINE_TRY_ASSIGN(const RenderRayInterval ray, ComputeViewPixelRayInterval(snapshot.Camera, params.X, params.Y));
		ENGINE_TRY_ASSIGN(const PhysicsLayerTable layers, PhysicsLayerTable::Create(settings->Physics.Layers, settings->Physics.Collisions));
		ENGINE_TRY_ASSIGN(const auto hit, RaycastScene(*scene, *assets, layers, { .Ray = ray.Ray, .MaxDistance = ray.MaxDistance, .MinDistance = ray.MinDistance, .Alpha = snapshot.Alpha }));
		ViewportPickResult result{ .View = params.View, .Width = size.Width, .Height = size.Height };
		if (hit)
		{
			result.Raycast.Hit = true;
			result.Raycast.Entity = context.MakeEntitySummary(scene->FindEntityByID(hit->Entity));
			result.Raycast.Position = hit->Position;
			result.Raycast.Normal = hit->Normal;
			result.Raycast.Barycentric = hit->Barycentric;
			result.Raycast.Distance = hit->Distance;
			result.Raycast.Submesh = hit->Submesh;
			result.Raycast.Triangle = hit->Triangle;
		}
		return result;
	}

	Result<ViewportPixelSize> EditorMethodContext::GetViewportPixelSize(ViewportView view) const
	{
		ENGINE_TRY_ASSIGN(const glm::uvec2 size, GetEditor().GetViewportState().GetPixelSize(view));
		return ViewportPixelSize{ .Width = size.x, .Height = size.y };
	}

	void RegisterViewportPickMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<ViewportPickParams>("ViewportPickParams", "A CPU visual query through the centre of a current viewport framebuffer pixel.")
			.Field("x", &ViewportPickParams::X, "Horizontal pixel from the left; must be below the actual framebuffer width.")
			.Field("y", &ViewportPickParams::Y, "Vertical pixel from the top; must be below the actual framebuffer height.")
			.Field("view", &ViewportPickParams::View, "Scene camera or the target scene's primary game camera.")
			.Field("target", &ViewportPickParams::Target, "Defaults to play while playing, edit otherwise.");
		registry.Struct<ViewportPickResult>("ViewportPickResult", "A camera-clipped visual ray hit without changing selection or history.")
			.Field("view", &ViewportPickResult::View, "The viewport queried.")
			.Field("width", &ViewportPickResult::Width, "Actual framebuffer width used for the projection.")
			.Field("height", &ViewportPickResult::Height, "Actual framebuffer height used for the projection.")
			.Field("raycast", &ViewportPickResult::Raycast, "Nearest triangle in the camera's near-to-far interval.");
	}

	void RegisterViewportPickMethods(MethodRegistry& methods)
	{
		methods.Add({ .Name = "viewport.pick", .Description = "Queries visual triangles at a viewport pixel without a GPU, clipping to the camera's near and far planes. Returns the hit without changing selection.", .RequiredParams = { "x", "y", "view" }, .SupportsDryRun = true, .AllowedInBatch = true, .Examples = { { .Description = "Query the centre of the default scene viewport.", .Params = Json{ { "x", 320 }, { "y", 180 }, { "view", "scene" } } } } },
			&Automation::ViewportPick);
	}

}
