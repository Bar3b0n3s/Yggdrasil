#include "EnginePCH.h"
#include "Engine/Automation/Methods/ScreenshotMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/ResultOffload.h"
#include "Engine/Core/Assert.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/RenderAnnotations.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <span>
#include <string_view>

namespace Engine {

	namespace Utils {

		constexpr std::string_view ScreenshotPngExtension = "png";
		// The project validator's code for a scene without a primary camera (EditorCore's SceneNoPrimaryCameraCode, which
		// Engine cannot include), named by a game view that has none.
		constexpr std::string_view NoPrimaryCameraCode = "SCENE_NO_PRIMARY_CAMERA";

		// The base64 length of `byteCount` bytes (RFC 4648, with padding).
		[[nodiscard]] static constexpr size_t GetBase64Length(size_t byteCount)
		{
			return (byteCount + 2) / 3 * 4;
		}

		// The inline data of the largest PNG leaves the other members of the result (the path, the camera summary, short
		// names and numbers) at least 8 KB below the offload threshold, so a screenshot result is never offloaded (§13.4).
		static_assert(GetBase64Length(MaxInlineScreenshotPngBytes) + 8 * 1024 <= DefaultOffloadThresholdBytes);

		// RFC 4648 base64 with padding: the "data" of an inline screenshot.
		static std::string EncodeBase64(std::span<const std::byte> bytes)
		{
			constexpr std::string_view Alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
			std::string text;
			text.reserve(GetBase64Length(bytes.size()));
			for (size_t offset = 0; offset < bytes.size(); offset += 3)
			{
				const size_t count = std::min<size_t>(3, bytes.size() - offset);
				uint32_t group = 0;
				for (size_t index = 0; index < 3; ++index)
				{
					const uint32_t value = index < count ? std::to_integer<uint32_t>(bytes[offset + index]) : 0U;
					group = (group << 8U) | value;
				}
				for (size_t index = 0; index < 4; ++index)
				{
					const uint32_t sextet = (group >> (18U - 6U * static_cast<uint32_t>(index))) & 0x3FU;
					text.push_back(index <= count ? Alphabet[sextet] : '=');
				}
			}
			return text;
		}

		static Result<RenderDebugView> ParseDebugViewParam(std::string_view name)
		{
			if (name.empty())
				return RenderDebugView::Lit;
			if (const auto view = ParseRenderDebugView(name))
				return *view;
			std::string valid;
			for (uint32_t index = 0; index < RenderDebugViewCount; ++index)
				valid += std::format("{}{}", valid.empty() ? "" : ", ", RenderDebugViewToString(static_cast<RenderDebugView>(index)));
			return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/debugView",
				std::format("unknown debug view '{}'; the debug views are {}", name, valid), "leave out 'debugView' for the lit image"));
		}

		// The camera of the view: the "camera" param's entity, the primary camera of the game view, or the host's scene-view
		// camera (`sceneViewCamera`, not null for the scene view). `cameraSummary` receives the camera entity's summary (empty
		// for the scene-view camera).
		static Status SelectCamera(AutomationMethodContext& context, const ViewportScreenshotParams& params, Scene& scene,
			const ExplicitRenderCamera* sceneViewCamera, RenderExtractionRequest& request, EntitySummary& cameraSummary)
		{
			if (context.HasParam("camera"))
			{
				ENGINE_TRY_ASSIGN(const Entity camera, context.ResolveEntity(scene, params.Camera, "/camera"));
				if (!camera.HasComponent<CameraComponent>())
				{
					return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, "/camera",
						std::format("entity '{}' has no Camera component, so it cannot render a view", scene.GetEntityPath(camera)),
						"name a camera entity, or leave out 'camera' to use the view's own camera"));
				}
				request.Camera = RenderCameraSource::Entity;
				request.CameraEntity = camera.GetUUID();
				cameraSummary = context.MakeEntitySummary(camera);
				return {};
			}
			if (params.View == ViewportView::Game)
			{
				const ConstEntity primary = FindPrimaryCamera(scene);
				if (!primary.IsValid())
				{
					return std::unexpected(Error(ErrorCode::InvalidState,
						std::format("{}: the scene has no effectively enabled camera with Primary set, so it has no game view", NoPrimaryCameraCode))
							.WithHint("set Primary on a camera (entity.update {components: {Camera: {Primary: true}}}), or render the scene view "
									  "or a camera entity (\"camera\")"));
				}
				request.Camera = RenderCameraSource::Primary;
				cameraSummary = context.MakeEntitySummary(primary);
				return {};
			}
			ENGINE_CORE_ASSERT(sceneViewCamera != nullptr, "the scene view needs the host's scene-view camera");
			request.Camera = RenderCameraSource::Explicit;
			request.ExplicitCamera = *sceneViewCamera;
			return {};
		}

		// The PNG of `captured`, downscaled to `maxDimension`, written to the host's output directory.
		struct WrittenScreenshot
		{
			std::string Path{};
			uint32_t Width = 0;
			uint32_t Height = 0;
			Buffer Png{};
		};

		static Result<WrittenScreenshot> WriteScreenshot(AutomationMethodContext& context, const Image& captured, uint32_t maxDimension)
		{
			// A capture that already fits is encoded as it is; only a larger one is copied, downscaled. Screenshots may be up to
			// MaxViewportScreenshotDimension square, so no full-size copy is made.
			std::optional<Image> downscaled;
			if (std::max(captured.Width, captured.Height) > maxDimension)
			{
				ENGINE_TRY_ASSIGN(downscaled, DownscaleImage(captured, maxDimension));
			}
			const Image& image = downscaled.has_value() ? *downscaled : captured;
			WrittenScreenshot written;
			ENGINE_TRY_ASSIGN(written.Png, EncodePng(image));
			ENGINE_TRY_ASSIGN(written.Path, context.WriteOutputFile(ScreenshotPngExtension, written.Png));
			written.Width = image.Width;
			written.Height = image.Height;
			return written;
		}

	}

	namespace Automation {

		Result<ViewportScreenshotResult> ViewportScreenshot(AutomationMethodContext& context, const ViewportScreenshotParams& params)
		{
			ENGINE_TRY_ASSIGN(const RenderDebugView debugView, Utils::ParseDebugViewParam(params.DebugView));
			ViewportAnnotationOptions annotations;
			if (context.HasParam("annotate"))
			{
				const auto parsed = ParseViewportAnnotations(params.Annotate);
				if (!parsed)
				{
					const Error& error = parsed.error();
					return std::unexpected(Error(error).WithIssue({
						.JsonPointer = error.GetLocation().JsonPointer.value_or("/annotate"),
						.Message = error.GetMessageText(),
						.Hint = error.GetHint(),
						.Suggestions = {},
					}));
				}
				annotations = *parsed;
			}
			const std::optional<ExplicitRenderCamera> sceneViewCamera = context.GetSceneViewCamera();
			if (params.View == ViewportView::Scene && !sceneViewCamera.has_value())
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::Unsupported, "/view",
					"viewport.screenshot view 'scene' needs the editor's scene view; the Runtime serves the game view only",
					"use view \"game\" (with \"camera\" for another camera entity)"));
			}

			// Reads default to the play scene while playing (§13.4); the game view looks at the edit scene otherwise.
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), false));
			PlaySession* session = context.GetPlaySession();
			const bool isPlayScene = session != nullptr && &session->GetScene() == scene;

			RenderExtractionRequest request{ .Width = params.Width, .Height = params.Height, .Alpha = 1.0f };
			const ProjectSettings* settings = context.GetProjectSettings();
			if (settings == nullptr)
				return MakeError(ErrorCode::Unsupported, "viewport.screenshot needs the host's project rendering settings");
			request.Quality = { .ShadowMapSize = settings->Rendering.ShadowMapSize, .SsaoHalfResolution = settings->Rendering.SsaoHalfResolution };
			request.SelectedEntities = context.GetSelectedEntities();
			ENGINE_TRY_ASSIGN(request.Annotations, ResolveViewportAnnotations(context, *scene, annotations));
			request.Flags = annotations.Colliders ? RenderViewFlags::Colliders : RenderViewFlags::None;
			EntitySummary cameraSummary;
			ENGINE_TRY(Utils::SelectCamera(context, params, *scene, sceneViewCamera.has_value() ? &*sceneViewCamera : nullptr, request, cameraSummary));

			// The view is extracted afresh (§8.13): the play scene between ticks through the session, which brings its render
			// state up to date and uses its view alpha; the edit scene with its world matrices recomputed (runtime-only
			// components, so its revision does not change) and no interpolation.
			context.SetPhase("Rendering the viewport");
			RenderSnapshot snapshot;
			if (isPlayScene)
			{
				ENGINE_TRY_ASSIGN(snapshot, session->ExtractView(request));
			}
			else
			{
				TransformSystem::Update(*scene);
				ENGINE_TRY_ASSIGN(snapshot, ExtractRenderSnapshot(*scene, request));
			}
			snapshot.DebugView = debugView;
			snapshot.SceneRevision = scene->GetRevision();
			// Data views admit only the annotations this call requests, never ordinary session debug primitives.
			if (debugView == RenderDebugView::AO || debugView == RenderDebugView::ShadowCascades || debugView == RenderDebugView::Overdraw)
				snapshot.DebugDraw.Clear();
			if (annotations.Colliders || request.Annotations.Labels != RenderAnnotationLabels::None || request.Annotations.Bounds || request.Annotations.Axes)
			{
				AssetManager* assets = context.GetAssets();
				if (assets == nullptr)
					return MakeError(ErrorCode::Unsupported, "viewport annotations need the host's asset manager");
				ENGINE_TRY_ASSIGN(const PhysicsLayerTable layers, PhysicsLayerTable::Create(settings->Physics.Layers, settings->Physics.Collisions));
				ENGINE_TRY(AppendRenderAnnotations(*scene, *assets, layers, isPlayScene ? &session->GetPhysics() : nullptr, snapshot));
			}

			const ViewportScreenshotRequest capture{ .Width = params.Width, .Height = params.Height, .MaxDimension = 0 };
			ENGINE_TRY_ASSIGN(const Image captured, WithContext(context.CaptureView(snapshot, capture), "while rendering the viewport"));
			ENGINE_TRY_ASSIGN(const Utils::WrittenScreenshot written, Utils::WriteScreenshot(context, captured, params.MaxDimension));
			// Inline data only while the result stays below the offload threshold (MaxInlineScreenshotPngBytes).
			const bool fitsInline = written.Png.size() <= MaxInlineScreenshotPngBytes;
			return ViewportScreenshotResult{
				.View = params.View,
				.Target = isPlayScene ? SceneTarget::Play : SceneTarget::Edit,
				.Camera = std::move(cameraSummary),
				.Path = written.Path,
				.Width = written.Width,
				.Height = written.Height,
				.Data = params.Inline && fitsInline ? Utils::EncodeBase64(written.Png) : std::string(),
				.InlineOmitted = params.Inline && !fitsInline,
			};
		}

	}

	void RegisterViewportScreenshotMethodTypes(TypeRegistry& registry)
	{
		const FieldMeta dimensionMeta{ .Min = 1.0, .Max = static_cast<double>(MaxViewportScreenshotDimension) };

		registry.Enum<ViewportView>("ViewportView", "Which view viewport.screenshot renders.")
			.Entry(ViewportView::Scene, "Scene", "The target scene through the editor's scene-view camera (the editor only).")
			.Entry(ViewportView::Game, "Game", "The target scene through its primary camera: what the game shows.");

		registry.Struct<ViewportScreenshotParams>("ViewportScreenshotParams", "The params of viewport.screenshot: a fresh render of a view as a PNG.")
			.Field("view", &ViewportScreenshotParams::View,
				"The view to render: \"Game\" (the primary camera) or \"Scene\" (the editor's scene-view camera; not in the Runtime).")
			.Field("target", &ViewportScreenshotParams::Target, "The scene to render; omitted: the play scene while playing, else the edit scene.")
			.Field("width", &ViewportScreenshotParams::Width, "The rendered width in pixels, before maxDimension.", dimensionMeta)
			.Field("height", &ViewportScreenshotParams::Height, "The rendered height in pixels, before maxDimension.", dimensionMeta)
			.Field("camera", &ViewportScreenshotParams::Camera,
				"A camera entity of the target scene (an EntityRef) to render through instead of the view's camera.")
			.Field("debugView", &ViewportScreenshotParams::DebugView,
				"A debug view to render instead of the lit image, ignoring case: \"Lit\" (the default, also for \"\"), \"Albedo\", "
				"\"Normals\", \"Roughness\", \"Metallic\", \"Emissive\", \"AO\", \"ShadowCascades\" or \"Overdraw\". A data view stores each value v as round(255 v).")
			.Field("annotate", &ViewportScreenshotParams::Annotate,
				"Capture-only annotations: labels=all, selection (selected), none or an EntityRef array; colliders, bounds and axes are booleans. Unknown members and invalid references are rejected.")
			.Field("maxDimension", &ViewportScreenshotParams::MaxDimension, "The larger side of the PNG at most this many pixels.", dimensionMeta)
			.Field("inline", &ViewportScreenshotParams::Inline,
				"Also return the PNG as base64 in \"data\" when it is at most 30 KB; a larger one is only written (inlineOmitted).");

		registry.Struct<ViewportScreenshotResult>("ViewportScreenshotResult", "A viewport screenshot written as a PNG.")
			.Field("view", &ViewportScreenshotResult::View, "The view rendered.")
			.Field("target", &ViewportScreenshotResult::Target, "The scene rendered.")
			.Field("camera", &ViewportScreenshotResult::Camera,
				"The camera entity the view was rendered through; every member empty for the editor's scene-view camera.")
			.Field("path", &ViewportScreenshotResult::Path, "The PNG's absolute path.")
			.Field("mimeType", &ViewportScreenshotResult::MimeType, "\"image/png\".")
			.Field("width", &ViewportScreenshotResult::Width, "The PNG's width in pixels.")
			.Field("height", &ViewportScreenshotResult::Height, "The PNG's height in pixels.")
			.Field("data", &ViewportScreenshotResult::Data, "With inline: the PNG, base64, when it is at most 30 KB; empty otherwise.")
			.Field("inlineOmitted", &ViewportScreenshotResult::InlineOmitted,
				"True when inline was asked for but the PNG is larger than 30 KB: data is empty and the PNG is at path.");
	}

	void RegisterViewportScreenshotMethods(MethodRegistry& methods)
	{
		Json gameExample = Json::object();
		gameExample["view"] = "game";
		gameExample["maxDimension"] = 512;
		Json cameraExample = Json::object();
		cameraExample["view"] = "game";
		cameraExample["camera"] = "/SideCamera";
		cameraExample["width"] = 320;
		cameraExample["height"] = 180;
		methods.Add(
			{
				.Name = "viewport.screenshot",
				.Description = "Renders a view of the target scene afresh at width x height (the game view through the primary camera, the "
							   "editor's scene view, or a camera entity), downscales it to maxDimension and writes it as a PNG whose path the "
							   "result names. Needs a renderer (not --renderer none).",
				.RequiredParams = { "view" },
				.ExposeAsTool = true,
				.AvailableInRuntime = true,
				.Examples = {
					{ .Description = "A 512-pixel screenshot of the game view.", .Params = gameExample },
					{ .Description = "A 320x180 view through the camera entity /SideCamera.", .Params = cameraExample },
				},
			},
			&Automation::ViewportScreenshot);
	}

}
