#include "EditorPCH.h"
#include "EditorCore/Automation/ScreenshotMethods.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Automation/Protocol/PendingOperation.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Renderer/ViewportCapture.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <chrono>
#include <format>
#include <optional>
#include <utility>

namespace Engine {

	namespace Utils {

		constexpr std::string_view EditorScreenshotPngExtension = "png";

		// The error of editor.screenshot without its capture (--renderer none, §13.9).
		static Error MakeNoEditorUiError()
		{
			return Error(ErrorCode::Unsupported, "editor.screenshot needs a renderer, and this editor runs with --renderer none")
				.WithHint("start the editor without --renderer none (it needs a Vulkan 1.3 GPU)");
		}

		// The wire request has a bounded lifetime even if the host never publishes the requested frame.
		class EditorScreenshotDeadline final : public PendingOperation
		{
		public:
			EditorScreenshotDeadline(Scope<PendingOperation> operation, std::chrono::steady_clock::time_point deadline)
				: m_Operation(std::move(operation)), m_Deadline(deadline)
			{
			}

			std::optional<Result<Json>> Poll(MethodContext& context) override
			{
				if (m_Operation == nullptr)
					return MakeError(ErrorCode::Cancelled, "editor screenshot was cancelled");
				if (static_cast<EditorMethodContext&>(context).GetWallClockTime() >= m_Deadline)
				{
					Cancel(context);
					return MakeError(ErrorCode::Timeout, "editor screenshot timed out waiting for a fresh UI frame");
				}
				return m_Operation->Poll(context);
			}

			void Cancel(MethodContext& context) override
			{
				if (m_Operation != nullptr)
				{
					m_Operation->Cancel(context);
					m_Operation.reset();
				}
			}

			std::string GetPhase() const override { return m_Operation != nullptr ? m_Operation->GetPhase() : std::string(); }
		private:
			Scope<PendingOperation> m_Operation{};
			std::chrono::steady_clock::time_point m_Deadline{};
		};

		static Result<Scope<PendingOperation>> BeginRegisteredEditorScreenshot(EditorMethodContext& context, const EditorScreenshotParams& params)
		{
			if (!context.GetServer().GetSpecification().Screenshots.EditorUi)
				return std::unexpected(MakeNoEditorUiError());
			const auto deadline = context.GetWallClockTime() + std::chrono::seconds(context.GetMethod().Specification.TimeoutSeconds);
			ENGINE_TRY_ASSIGN(auto operation, Automation::BeginEditorScreenshot(context, params));
			return CreateScope<EditorScreenshotDeadline>(std::move(operation), deadline);
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
		methods.AddPending<EditorMethodContext, EditorScreenshotParams, EditorScreenshotResult>(
			{
				.Name = "editor.screenshot",
				.Description = "Requests a fresh editor UI frame, including while minimized, waits for that frame and captures the whole editor. "
							   "Downscales it to maxDimension and writes it as a PNG whose path the result names. Needs a renderer (not "
							   "--renderer none).",
				.ExposeAsTool = true,
				.Examples = { { .Description = "A screenshot of the editor at most 1024 pixels wide.", .Params = Json::object() } },
			},
			&Utils::BeginRegisteredEditorScreenshot);
	}

}
