#include "EditorPCH.h"
#include "EditorCore/Automation/ScreenshotMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Renderer/ViewportCapture.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <optional>

namespace Engine {

	namespace Utils {

		constexpr std::string_view EditorScreenshotPngExtension = "png";

		// The error of editor.screenshot without its capture (--renderer none, §13.9).
		static Error MakeNoEditorUiError()
		{
			return Error(ErrorCode::Unsupported, "editor.screenshot needs a renderer, and this editor runs with --renderer none")
				.WithHint("start the editor without --renderer none (it needs a Vulkan 1.3 GPU)");
		}

		// The PNG of `image`, downscaled to `maxDimension`, written to the server's output directory.
		struct WrittenEditorScreenshot
		{
			std::string Path{};
			uint32_t Width = 0;
			uint32_t Height = 0;
		};

		static Result<WrittenEditorScreenshot> WriteEditorScreenshot(EditorMethodContext& context, const Image& captured, uint32_t maxDimension)
		{
			// A capture that already fits is encoded as it is; only a larger one is copied, downscaled.
			std::optional<Image> downscaled;
			if (std::max(captured.Width, captured.Height) > maxDimension)
			{
				ENGINE_TRY_ASSIGN(downscaled, DownscaleImage(captured, maxDimension));
			}
			const Image& image = downscaled.has_value() ? *downscaled : captured;
			ENGINE_TRY_ASSIGN(const Buffer png, EncodePng(image));
			WrittenEditorScreenshot written;
			ENGINE_TRY_ASSIGN(written.Path, context.GetServer().WriteOutputFile(EditorScreenshotPngExtension, png));
			written.Width = image.Width;
			written.Height = image.Height;
			return written;
		}

	}

	namespace Automation {

		Result<EditorScreenshotResult> EditorScreenshot(EditorMethodContext& context, const EditorScreenshotParams& params)
		{
			const ScreenshotCaptures& captures = context.GetServer().GetSpecification().Screenshots;
			if (!captures.EditorUi)
				return std::unexpected(Utils::MakeNoEditorUiError());

			context.SetPhase("Rendering the editor UI");
			ENGINE_TRY_ASSIGN(const Image captured, WithContext(captures.EditorUi(), "while rendering the editor UI"));
			ENGINE_TRY_ASSIGN(const Utils::WrittenEditorScreenshot written, Utils::WriteEditorScreenshot(context, captured, params.MaxDimension));
			return EditorScreenshotResult{ .Path = written.Path, .Width = written.Width, .Height = written.Height };
		}

	}

	void RegisterScreenshotMethodTypes(TypeRegistry& registry)
	{
		const FieldMeta dimensionMeta{ .Min = 1.0, .Max = static_cast<double>(MaxViewportScreenshotDimension) };

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
