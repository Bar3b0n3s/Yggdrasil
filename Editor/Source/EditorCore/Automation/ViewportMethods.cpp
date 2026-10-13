#include "EditorPCH.h"
#include "EditorCore/Automation/ViewportMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Viewport/EditorCamera.h"
#include "Engine/Asset/AssetManager.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/EntityBounds.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"

#include <algorithm>
#include <cmath>
#include <format>
#include <limits>
#include <optional>
#include <string>
#include <string_view>

namespace Engine {

	namespace Utils {

		static Error ViewportParamError(std::string_view pointer, std::string message)
		{
			return Error(ErrorCode::InvalidArgument, std::move(message)).WithLocation({ .File = {}, .JsonPointer = std::string(pointer), .Entity = {} });
		}

		static bool IsFiniteViewportVector(const glm::vec3& value)
		{
			return std::isfinite(value.x) && std::isfinite(value.y) && std::isfinite(value.z);
		}

		static ViewportCameraResult MakeViewportCameraResult(const ExplicitRenderCamera& camera)
		{
			return ViewportCameraResult{ .Position = camera.Position,
				.Target = camera.Target,
				.Projection = camera.Projection == RenderProjection::Orthographic ? ProjectionType::Orthographic : ProjectionType::Perspective,
				.VerticalFov = camera.VerticalFov,
				.OrthographicSize = camera.OrthographicSize,
				.NearClip = camera.NearClip,
				.FarClip = camera.FarClip };
		}

		static ViewportSetOptionsResult MakeViewportOptionsResult(const EditorViewportState& viewport)
		{
			const EditorViewportOptions& options = viewport.GetOptions();
			const glm::uvec2 resolution = viewport.GetGameResolution();
			return ViewportSetOptionsResult{ .Grid = options.Grid,
				.Gizmos = options.Gizmos,
				.Colliders = options.Colliders,
				.Icons = options.Icons,
				.Wireframe = options.Wireframe,
				.DebugView = std::string(RenderDebugViewToString(viewport.GetDebugView())),
				.GameResolution = { resolution.x, resolution.y } };
		}

	}

	Result<ViewportCameraResult> Automation::ViewportCamera(EditorMethodContext& context, const ViewportCameraParams& params)
	{
		EditorViewportState& viewport = context.GetEditor().GetViewportState();
		ExplicitRenderCamera camera = viewport.GetCamera();
		// Every member but the pose is validated and located on its own below, and the members not sent come from the
		// current camera, which SetCamera already accepted. So the whole-camera SetCamera can only refuse the pose (a
		// coincident or vertical direction): its error belongs to /target when it was sent, else /position. `pointer` is
		// the last member sent, the fallback should SetCamera ever refuse anything else.
		std::string_view posePointer;
		std::string_view pointer;
		if (context.HasParam("position"))
		{
			if (!Utils::IsFiniteViewportVector(params.Position))
				return std::unexpected(Utils::ViewportParamError("/position", "camera position must be finite"));
			camera.Position = params.Position;
			posePointer = "/position";
			pointer = "/position";
		}
		if (context.HasParam("target"))
		{
			if (!Utils::IsFiniteViewportVector(params.Target))
				return std::unexpected(Utils::ViewportParamError("/target", "camera target must be finite"));
			camera.Target = params.Target;
			posePointer = "/target";
			pointer = "/target";
		}
		if (context.HasParam("projection"))
		{
			camera.Projection = params.Projection == ProjectionType::Orthographic ? RenderProjection::Orthographic : RenderProjection::Perspective;
			pointer = "/projection";
		}
		if (context.HasParam("verticalFov"))
		{
			if (!std::isfinite(params.VerticalFov) || params.VerticalFov <= 0.0f || params.VerticalFov >= 180.0f)
				return std::unexpected(Utils::ViewportParamError("/verticalFov", "the vertical field of view must be between 0 and 180 degrees, exclusive"));
			camera.VerticalFov = params.VerticalFov;
			pointer = "/verticalFov";
		}
		if (context.HasParam("orthographicSize"))
		{
			if (!std::isfinite(params.OrthographicSize) || params.OrthographicSize <= 0.0f)
				return std::unexpected(Utils::ViewportParamError("/orthographicSize", "the orthographic size must be a positive half height in metres"));
			camera.OrthographicSize = params.OrthographicSize;
			pointer = "/orthographicSize";
		}
		if (context.HasParam("nearClip"))
		{
			camera.NearClip = params.NearClip;
			pointer = "/nearClip";
		}
		if (context.HasParam("farClip"))
		{
			camera.FarClip = params.FarClip;
			pointer = "/farClip";
		}
		if (context.HasParam("nearClip") || context.HasParam("farClip"))
		{
			if (!std::isfinite(camera.NearClip) || !std::isfinite(camera.FarClip) || camera.NearClip <= 0.0f || camera.FarClip <= camera.NearClip)
				return std::unexpected(Utils::ViewportParamError(context.HasParam("farClip") ? "/farClip" : "/nearClip",
					std::format("the clip range needs 0 < nearClip < farClip (got {} and {})", camera.NearClip, camera.FarClip)));
		}
		if (!pointer.empty())
		{
			Status applied = viewport.SetCamera(camera);
			if (!applied)
			{
				const std::string_view located = posePointer.empty() ? pointer : posePointer;
				return std::unexpected(std::move(applied).error().WithLocation({ .File = {}, .JsonPointer = std::string(located), .Entity = {} }));
			}
		}
		return Utils::MakeViewportCameraResult(viewport.GetCamera());
	}

	Result<ViewportFrameResult> Automation::ViewportFrame(EditorMethodContext& context, const ViewportFrameParams& params)
	{
		if (params.Entities.empty() || params.Entities.size() > 1000)
			return std::unexpected(Utils::ViewportParamError("/entities", "frame needs between 1 and 1000 entity references"));
		ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(SceneTarget::Edit, false, false));
		std::vector<Entity> entities;
		entities.reserve(params.Entities.size());
		for (size_t index = 0; index < params.Entities.size(); ++index)
		{
			ENGINE_TRY_ASSIGN(Entity entity, context.ResolveEntity(*scene, params.Entities[index], std::format("/entities/{}", index)));
			entities.push_back(entity);
		}
		AssetManager* assets = context.GetAssets();
		if (assets == nullptr)
			return MakeError(ErrorCode::Unsupported, "viewport.frame needs an asset manager to resolve mesh bounds");
		assets->WaitIdle();
		glm::vec3 minimum(std::numeric_limits<float>::max());
		glm::vec3 maximum(std::numeric_limits<float>::lowest());
		ViewportFrameResult result;
		for (const Entity entity : entities)
		{
			const std::optional<Aabb> bounds = ComputeEntityWorldBounds(entity, *assets);
			const glm::vec3 low = bounds ? bounds->Min : TransformSystem::GetWorldPosition(entity);
			const glm::vec3 high = bounds ? bounds->Max : low;
			if (!Utils::IsFiniteViewportVector(low) || !Utils::IsFiniteViewportVector(high))
				return std::unexpected(Utils::ViewportParamError("/entities", "entity bounds must be finite"));
			minimum = glm::min(minimum, low);
			maximum = glm::max(maximum, high);
			result.Entities.push_back(context.MakeEntitySummary(entity));
		}
		EditorViewportState& viewport = context.GetEditor().GetViewportState();
		const glm::uvec2 size = viewport.GetSceneSize();
		const float aspect = static_cast<float>(size.x) / static_cast<float>(size.y);
		ENGINE_TRY_ASSIGN(const ExplicitRenderCamera camera, EditorCamera::FrameBounds(viewport.GetCamera(), minimum, maximum, aspect));
		ENGINE_TRY(viewport.SetCamera(camera));
		result.Position = camera.Position;
		result.Target = camera.Target;
		return result;
	}

	Result<ViewportSetOptionsResult> Automation::ViewportSetOptions(EditorMethodContext& context, const ViewportSetOptionsParams& params)
	{
		EditorViewportState& viewport = context.GetEditor().GetViewportState();
		EditorViewportOptions options = viewport.GetOptions();
		if (context.HasParam("grid"))
			options.Grid = params.Grid;
		if (context.HasParam("gizmos"))
			options.Gizmos = params.Gizmos;
		if (context.HasParam("colliders"))
			options.Colliders = params.Colliders;
		if (context.HasParam("icons"))
			options.Icons = params.Icons;
		if (context.HasParam("wireframe"))
			options.Wireframe = params.Wireframe;
		// Validate every member before changing anything.
		std::optional<RenderDebugView> debugView;
		if (context.HasParam("debugView"))
		{
			debugView = ParseRenderDebugView(params.DebugView);
			if (!debugView)
			{
				std::string valid;
				for (uint32_t index = 0; index < RenderDebugViewCount; ++index)
					valid += std::format("{}{}", valid.empty() ? "" : ", ", RenderDebugViewToString(static_cast<RenderDebugView>(index)));
				return std::unexpected(Utils::ViewportParamError("/debugView", std::format("unknown debug view '{}'; the debug views are {}", params.DebugView, valid)));
			}
		}
		std::optional<glm::uvec2> resolution;
		if (context.HasParam("gameResolution"))
		{
			const auto& size = params.GameResolution;
			const bool freeResolution = size.size() == 2 && size[0] == 0 && size[1] == 0;
			const bool fixedResolution = size.size() == 2 && size[0] >= 1 && size[0] <= 8192 && size[1] >= 1 && size[1] <= 8192;
			if (!freeResolution && !fixedResolution)
				return std::unexpected(Utils::ViewportParamError("/gameResolution", "the game resolution is [width, height] with both sides in [1, 8192], or [0, 0] for free resolution"));
			resolution = glm::uvec2(size[0], size[1]);
		}
		if (debugView)
			ENGINE_TRY(viewport.SetDebugView(*debugView));
		if (resolution)
			ENGINE_TRY(viewport.SetGameResolution(*resolution));
		viewport.SetOptions(options);
		return Utils::MakeViewportOptionsResult(viewport);
	}

	void RegisterViewportMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<ViewportCameraParams>("ViewportCameraParams", "An optional patch of the editor scene camera; absent members are unchanged.")
			.Field("position", &ViewportCameraParams::Position, "World position; absent preserves the current position.")
			.Field("target", &ViewportCameraParams::Target, "World look-at target; absent preserves the current target.")
			.Field("projection", &ViewportCameraParams::Projection, "Perspective or Orthographic, as the scene view's Orthographic overlay toggle sets it.")
			.Field("verticalFov", &ViewportCameraParams::VerticalFov, "Perspective vertical field of view in degrees, between 0 and 180 exclusive.")
			.Field("orthographicSize", &ViewportCameraParams::OrthographicSize, "Orthographic half height in metres, positive.")
			.Field("nearClip", &ViewportCameraParams::NearClip, "Near clipping distance in metres, positive and below farClip.")
			.Field("farClip", &ViewportCameraParams::FarClip, "Far clipping and culling distance in metres, above nearClip.");
		registry.Struct<ViewportCameraResult>("ViewportCameraResult", "The complete editor scene camera.")
			.Field("position", &ViewportCameraResult::Position, "World position in metres.")
			.Field("target", &ViewportCameraResult::Target, "World look-at target in metres.")
			.Field("projection", &ViewportCameraResult::Projection, "Perspective or Orthographic.")
			.Field("verticalFov", &ViewportCameraResult::VerticalFov, "Perspective vertical field of view in degrees.")
			.Field("orthographicSize", &ViewportCameraResult::OrthographicSize, "Orthographic half height in metres.")
			.Field("nearClip", &ViewportCameraResult::NearClip, "Near clipping distance in metres.")
			.Field("farClip", &ViewportCameraResult::FarClip, "Far clipping and culling distance in metres.");
		registry.Struct<ViewportFrameParams>("ViewportFrameParams", "Entities to fit in the scene viewport.")
			.Field("entities", &ViewportFrameParams::Entities, "One to 1000 entity IDs, unique prefixes or paths in the shown scene.");
		registry.Struct<ViewportFrameResult>("ViewportFrameResult", "The fitted camera pose and resolved entities.")
			.Field("position", &ViewportFrameResult::Position, "The fitted world position.")
			.Field("target", &ViewportFrameResult::Target, "The fitted world look-at target.")
			.Field("entities", &ViewportFrameResult::Entities, "Resolved entities in request order.");
		registry.Struct<ViewportSetOptionsParams>("ViewportSetOptionsParams", "A presence-aware patch of scene viewport options.")
			.Field("grid", &ViewportSetOptionsParams::Grid, "Show the scene grid.")
			.Field("gizmos", &ViewportSetOptionsParams::Gizmos, "Enable transform gizmos.")
			.Field("colliders", &ViewportSetOptionsParams::Colliders, "Show collider geometry.")
			.Field("icons", &ViewportSetOptionsParams::Icons, "Show scene object icons.")
			.Field("wireframe", &ViewportSetOptionsParams::Wireframe, "Show mesh wireframes.")
			.Field("debugView", &ViewportSetOptionsParams::DebugView, "The scene view's debug view, any ASCII case: Lit, Albedo, Normals, Roughness, Metallic, Emissive, AO, ShadowCascades or Overdraw.")
			.Field("gameResolution", &ViewportSetOptionsParams::GameResolution, "The Game view's resolution [width, height], each 1 to 8192, or [0, 0] for free resolution.");
		registry.Struct<ViewportSetOptionsResult>("ViewportSetOptionsResult", "All current scene and game viewport options.")
			.Field("grid", &ViewportSetOptionsResult::Grid, "Whether the scene grid is shown.")
			.Field("gizmos", &ViewportSetOptionsResult::Gizmos, "Whether transform gizmos are enabled.")
			.Field("colliders", &ViewportSetOptionsResult::Colliders, "Whether collider geometry is shown.")
			.Field("icons", &ViewportSetOptionsResult::Icons, "Whether object icons are shown.")
			.Field("wireframe", &ViewportSetOptionsResult::Wireframe, "Whether mesh wireframes are shown.")
			.Field("debugView", &ViewportSetOptionsResult::DebugView, "The scene view's debug view (canonical name).")
			.Field("gameResolution", &ViewportSetOptionsResult::GameResolution, "The Game view's resolution [width, height]; [0, 0] is free resolution.");
	}

	void RegisterViewportMethods(MethodRegistry& methods)
	{
		methods.Add({ .Name = "viewport.camera", .Description = "Reads or patches the editor scene camera (pose, projection, field of view, orthographic size and clip range) without changing the scene or history.", .Examples = { { .Description = "Read the scene camera.", .Params = Json::object() }, { .Description = "Switch the scene view to an orthographic top-down camera.", .Params = Json{ { "position", { 0, 20, 0.01 } }, { "target", { 0, 0, 0 } }, { "projection", "Orthographic" }, { "orthographicSize", 8 } } } } }, &Automation::ViewportCamera);
		methods.Add({ .Name = "viewport.frame", .Description = "Frames every referenced entity in the scene viewport with ten percent padding.", .RequiredParams = { "entities" }, .Examples = { { .Description = "Frame the board.", .Params = Json{ { "entities", Json::array({ "/Board" }) } } } } }, &Automation::ViewportFrame);
		methods.Add({ .Name = "viewport.setOptions", .Description = "Patches the scene viewport's overlays and debug view and the Game view's resolution; absent members remain unchanged.", .Examples = { { .Description = "Show colliders.", .Params = Json{ { "colliders", true } } }, { .Description = "Show the scene view's normals and a fixed 1280x720 game view.", .Params = Json{ { "debugView", "Normals" }, { "gameResolution", { 1280, 720 } } } } } }, &Automation::ViewportSetOptions);
	}

}
