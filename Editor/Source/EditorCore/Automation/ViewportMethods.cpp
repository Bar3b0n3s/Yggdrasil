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

	}

	Result<ViewportCameraResult> Automation::ViewportCamera(EditorMethodContext& context, const ViewportCameraParams& params)
	{
		EditorViewportState& viewport = context.GetEditor().GetViewportState();
		ExplicitRenderCamera camera = viewport.GetCamera();
		if (context.HasParam("position"))
		{
			if (!Utils::IsFiniteViewportVector(params.Position))
				return std::unexpected(Utils::ViewportParamError("/position", "camera position must be finite"));
			camera.Position = params.Position;
		}
		if (context.HasParam("target"))
		{
			if (!Utils::IsFiniteViewportVector(params.Target))
				return std::unexpected(Utils::ViewportParamError("/target", "camera target must be finite"));
			camera.Target = params.Target;
		}
		if (context.HasParam("position") || context.HasParam("target"))
		{
			Status applied = viewport.SetCamera(camera);
			if (!applied)
				return std::unexpected(std::move(applied).error().WithLocation({ .File = {}, .JsonPointer = context.HasParam("target") ? "/target" : "/position", .Entity = {} }));
		}
		return ViewportCameraResult{ .Position = camera.Position, .Target = camera.Target };
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
		viewport.SetOptions(options);
		return ViewportSetOptionsResult{ .Grid = options.Grid, .Gizmos = options.Gizmos, .Colliders = options.Colliders, .Icons = options.Icons, .Wireframe = options.Wireframe };
	}

	void RegisterViewportMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<ViewportCameraParams>("ViewportCameraParams", "An optional patch of the editor scene camera pose.")
			.Field("position", &ViewportCameraParams::Position, "World position; absent preserves the current position.")
			.Field("target", &ViewportCameraParams::Target, "World look-at target; absent preserves the current target.");
		registry.Struct<ViewportCameraResult>("ViewportCameraResult", "The complete editor scene camera pose.")
			.Field("position", &ViewportCameraResult::Position, "World position in metres.")
			.Field("target", &ViewportCameraResult::Target, "World look-at target in metres.");
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
			.Field("wireframe", &ViewportSetOptionsParams::Wireframe, "Show mesh wireframes.");
		registry.Struct<ViewportSetOptionsResult>("ViewportSetOptionsResult", "All current scene viewport options.")
			.Field("grid", &ViewportSetOptionsResult::Grid, "Whether the scene grid is shown.")
			.Field("gizmos", &ViewportSetOptionsResult::Gizmos, "Whether transform gizmos are enabled.")
			.Field("colliders", &ViewportSetOptionsResult::Colliders, "Whether collider geometry is shown.")
			.Field("icons", &ViewportSetOptionsResult::Icons, "Whether object icons are shown.")
			.Field("wireframe", &ViewportSetOptionsResult::Wireframe, "Whether mesh wireframes are shown.");
	}

	void RegisterViewportMethods(MethodRegistry& methods)
	{
		methods.Add({ .Name = "viewport.camera", .Description = "Reads or patches the editor scene camera pose without changing the scene or history.", .Examples = { { .Description = "Read the scene camera.", .Params = Json::object() } } }, &Automation::ViewportCamera);
		methods.Add({ .Name = "viewport.frame", .Description = "Frames every referenced entity in the scene viewport with ten percent padding.", .RequiredParams = { "entities" }, .Examples = { { .Description = "Frame the board.", .Params = Json{ { "entities", Json::array({ "/Board" }) } } } } }, &Automation::ViewportFrame);
		methods.Add({ .Name = "viewport.setOptions", .Description = "Patches scene viewport options; absent members remain unchanged.", .Examples = { { .Description = "Show colliders.", .Params = Json{ { "colliders", true } } } } }, &Automation::ViewportSetOptions);
	}

}
