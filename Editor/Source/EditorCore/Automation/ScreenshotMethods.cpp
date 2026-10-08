#include "EditorPCH.h"
#include "EditorCore/Automation/ScreenshotMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/ResultOffload.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Renderer/ViewportCapture.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <span>

namespace Engine {

	namespace Utils {

		constexpr std::string_view PngExtension = "png";

		// The base64 length of `byteCount` bytes (RFC 4648, with padding).
		[[nodiscard]] static constexpr size_t GetBase64Length(size_t byteCount)
		{
			return (byteCount + 2) / 3 * 4;
		}

		// The inline data of the largest PNG leaves the other members of the result (the path, short names and numbers) at
		// least 8 KB below the offload threshold, so a screenshot result is never offloaded (§13.4).
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

		// A member that needs a later milestone, present in the params: Unsupported located at it (never ignored).
		static Error MakeLaterMilestoneError(std::string_view member, std::string_view reason)
		{
			return MakeParamError(ErrorCode::Unsupported, std::format("/{}", member), std::format("viewport.screenshot '{}' is not supported yet: {}", member, reason),
				std::format("leave out '{}'", member));
		}

		// Unsupported for the members of ViewportScreenshotParams that need later milestones (ScreenshotMethods.h).
		static Status CheckViewportParams(const EditorMethodContext& context, const ViewportScreenshotParams& params)
		{
			if (params.View == ViewportView::Game)
			{
				return std::unexpected(MakeParamError(ErrorCode::Unsupported, "/view",
					"viewport.screenshot view 'Game' is not supported yet: the game view renders the play scene through its camera, which "
					"arrives with play sessions and the scene renderer (M7)",
					"use view \"scene\""));
			}
			if (context.HasParam("camera"))
				return std::unexpected(MakeLaterMilestoneError("camera", "camera entities arrive with the scene renderer (M7)"));
			if (context.HasParam("debugView"))
				return std::unexpected(MakeLaterMilestoneError("debugView", "the debug views arrive with the PBR renderer (M8)"));
			if (context.HasParam("annotate"))
				return std::unexpected(MakeLaterMilestoneError("annotate", "annotations are drawn by the overlay pass (M9)"));
			return {};
		}

		// The error of a screenshot method without its capture (--renderer none, §13.9).
		static Error MakeNoRendererError(std::string_view method)
		{
			return Error(ErrorCode::Unsupported, std::format("{} needs a renderer, and this editor runs with --renderer none", method))
				.WithHint("start the editor without --renderer none (it needs a Vulkan 1.3 GPU)");
		}

		// The PNG of `image`, downscaled to `maxDimension`, written to the server's output directory.
		struct WrittenScreenshot
		{
			std::string Path{};
			uint32_t Width = 0;
			uint32_t Height = 0;
			Buffer Png{};
		};

		static Result<WrittenScreenshot> WriteScreenshot(EditorMethodContext& context, const Image& captured, uint32_t maxDimension)
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
			ENGINE_TRY_ASSIGN(written.Path, context.GetServer().WriteOutputFile(PngExtension, written.Png));
			written.Width = image.Width;
			written.Height = image.Height;
			return written;
		}

	}

	namespace Automation {

		Result<ViewportScreenshotResult> ViewportScreenshot(EditorMethodContext& context, const ViewportScreenshotParams& params)
		{
			ENGINE_TRY(Utils::CheckViewportParams(context, params));
			const ScreenshotCaptures& captures = context.GetServer().GetSpecification().Screenshots;
			if (!captures.Viewport)
				return std::unexpected(Utils::MakeNoRendererError("viewport.screenshot"));

			context.SetPhase("Rendering the viewport");
			ENGINE_TRY_ASSIGN(const Image captured, WithContext(captures.Viewport(params.Width, params.Height), "while rendering the viewport"));
			ENGINE_TRY_ASSIGN(const Utils::WrittenScreenshot written, Utils::WriteScreenshot(context, captured, params.MaxDimension));
			// Inline data only while the result stays below the offload threshold (MaxInlineScreenshotPngBytes).
			const bool fitsInline = written.Png.size() <= MaxInlineScreenshotPngBytes;
			return ViewportScreenshotResult{
				.View = params.View,
				.Path = written.Path,
				.Width = written.Width,
				.Height = written.Height,
				.Data = params.Inline && fitsInline ? Utils::EncodeBase64(written.Png) : std::string(),
				.InlineOmitted = params.Inline && !fitsInline,
			};
		}

		Result<EditorScreenshotResult> EditorScreenshot(EditorMethodContext& context, const EditorScreenshotParams& params)
		{
			const ScreenshotCaptures& captures = context.GetServer().GetSpecification().Screenshots;
			if (!captures.EditorUi)
				return std::unexpected(Utils::MakeNoRendererError("editor.screenshot"));

			context.SetPhase("Rendering the editor UI");
			ENGINE_TRY_ASSIGN(const Image captured, WithContext(captures.EditorUi(), "while rendering the editor UI"));
			ENGINE_TRY_ASSIGN(const Utils::WrittenScreenshot written, Utils::WriteScreenshot(context, captured, params.MaxDimension));
			return EditorScreenshotResult{ .Path = written.Path, .Width = written.Width, .Height = written.Height };
		}

	}

	void RegisterScreenshotMethodTypes(TypeRegistry& registry)
	{
		const FieldMeta dimensionMeta{ .Min = 1.0, .Max = static_cast<double>(MaxViewportScreenshotDimension) };

		registry.Enum<ViewportView>("ViewportView", "Which view viewport.screenshot renders.")
			.Entry(ViewportView::Scene, "Scene", "The editor viewport's view of the edit scene.")
			.Entry(ViewportView::Game, "Game", "The play scene through its game camera (not supported before M7).");

		registry.Struct<ViewportScreenshotParams>("ViewportScreenshotParams", "The params of viewport.screenshot: a fresh render of a view as a PNG.")
			.Field("view", &ViewportScreenshotParams::View, "The view to render: \"Scene\" (\"Game\" is not supported before M7).")
			.Field("width", &ViewportScreenshotParams::Width, "The rendered width in pixels, before maxDimension.", dimensionMeta)
			.Field("height", &ViewportScreenshotParams::Height, "The rendered height in pixels, before maxDimension.", dimensionMeta)
			.Field("camera", &ViewportScreenshotParams::Camera, "A camera entity to render through (not supported before M7; refused when present).")
			.Field("debugView", &ViewportScreenshotParams::DebugView, "A debug view to render (not supported before M8; refused when present).")
			.Field("annotate", &ViewportScreenshotParams::Annotate,
				"Labels, colliders, bounds and axes to draw (not supported before M9; refused when present).")
			.Field("maxDimension", &ViewportScreenshotParams::MaxDimension, "The larger side of the PNG at most this many pixels.", dimensionMeta)
			.Field("inline", &ViewportScreenshotParams::Inline,
				"Also return the PNG as base64 in \"data\" when it is at most 30 KB; a larger one is only written (inlineOmitted).");

		registry.Struct<ViewportScreenshotResult>("ViewportScreenshotResult", "A viewport screenshot written as a PNG.")
			.Field("view", &ViewportScreenshotResult::View, "The view rendered.")
			.Field("path", &ViewportScreenshotResult::Path, "The PNG's absolute path.")
			.Field("mimeType", &ViewportScreenshotResult::MimeType, "\"image/png\".")
			.Field("width", &ViewportScreenshotResult::Width, "The PNG's width in pixels.")
			.Field("height", &ViewportScreenshotResult::Height, "The PNG's height in pixels.")
			.Field("data", &ViewportScreenshotResult::Data, "With inline: the PNG, base64, when it is at most 30 KB; empty otherwise.")
			.Field("inlineOmitted", &ViewportScreenshotResult::InlineOmitted,
				"True when inline was asked for but the PNG is larger than 30 KB: data is empty and the PNG is at path.");

		registry.Struct<EditorScreenshotParams>("EditorScreenshotParams", "The params of editor.screenshot: the whole editor UI as a PNG.")
			.Field("maxDimension", &EditorScreenshotParams::MaxDimension, "The larger side of the PNG at most this many pixels.", dimensionMeta);

		registry.Struct<EditorScreenshotResult>("EditorScreenshotResult", "An editor UI screenshot written as a PNG.")
			.Field("path", &EditorScreenshotResult::Path, "The PNG's absolute path.")
			.Field("mimeType", &EditorScreenshotResult::MimeType, "\"image/png\".")
			.Field("width", &EditorScreenshotResult::Width, "The PNG's width in pixels.")
			.Field("height", &EditorScreenshotResult::Height, "The PNG's height in pixels.");
	}

	void RegisterScreenshotMethods(MethodRegistry& methods)
	{
		Json viewportExample = Json::object();
		viewportExample["view"] = "scene";
		viewportExample["maxDimension"] = 512;
		// The host type is named because Engine/Automation/Methods/ScreenshotMethods.h overloads the handler for
		// AutomationMethodContext (the shared one, M7).
		methods.Add<EditorMethodContext, ViewportScreenshotParams, ViewportScreenshotResult>(
			{
				.Name = "viewport.screenshot",
				.Description = "Renders the viewport afresh at width x height, downscales it to maxDimension and writes it as a PNG whose path "
							   "the result names. Needs a renderer (not --renderer none).",
				.RequiredParams = { "view" },
				.ExposeAsTool = true,
				.AvailableInRuntime = true,
				.Examples = { { .Description = "A 512-pixel screenshot of the scene view.", .Params = viewportExample } },
			},
			&Automation::ViewportScreenshot);

		methods.Add(
			{
				.Name = "editor.screenshot",
				.Description = "Re-renders the editor UI's last rendered frame (the whole editor as of that frame) at its framebuffer size, "
							   "downscales it to maxDimension and writes it as a PNG whose path the result names. Needs a renderer (not "
							   "--renderer none).",
				.ExposeAsTool = true,
				.Examples = { { .Description = "A screenshot of the editor at most 1024 pixels wide.", .Params = Json::object() } },
			},
			&Automation::EditorScreenshot);
	}

}
