#include "EditorPCH.h"
#include "Editor/EditorApp.h"

#include "Engine/App/CommandLine.h"
#include "Engine/App/EngineContext.h"
#include "Engine/App/ExitCode.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/ImGui/ImGuiLayer.h"
#include "Engine/ImGui/ImGuiScreenshot.h"
#include "Engine/Renderer/ViewportCapture.h"

#include <imgui.h>

#include <array>
#include <vector>

namespace Engine {

	namespace Utils {

		constexpr std::string_view ViewportScreenshotOption = "--viewport-screenshot";
		constexpr std::string_view EditorScreenshotOption = "--editor-screenshot";

		constexpr std::array EditorCommandLineOptions = {
			CommandLineOption{
				.Name = ViewportScreenshotOption,
				.Value = CommandLineValue::Required,
				.ValueName = "path",
				.Description = "After the last frame of a --frames run, write a 640x360 viewport screenshot to this PNG file.",
			},
			CommandLineOption{
				.Name = EditorScreenshotOption,
				.Value = CommandLineValue::Required,
				.ValueName = "path",
				.Description = "After the last frame of a --frames run (2 or more), write a screenshot of the editor UI to this PNG file.",
			},
		};

		// The engine's options followed by the editor's. The views refer to string literals (CommandLineOption's rule).
		static std::vector<CommandLineOption> GetEditorCommandLineOptions()
		{
			std::vector<CommandLineOption> options(GetEngineCommandLineOptions().begin(), GetEngineCommandLineOptions().end());
			options.insert(options.end(), EditorCommandLineOptions.begin(), EditorCommandLineOptions.end());
			return options;
		}

		// The value of path option `name` as a path; an empty path when the option was not given.
		static Result<std::filesystem::path> GetPathOption(const CommandLine& commandLine, std::string_view name)
		{
			const std::optional<std::string_view> value = commandLine.GetValue(name);
			if (!value.has_value())
				return std::filesystem::path();
			// std::filesystem::path throws on ill-formed UTF-8 on Windows, and the option is external input.
			if (value->empty() || !IsValidUtf8(*value))
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs a file path in UTF-8", name);
			return std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(value->data()), value->size()));
		}

		// The editor's fatal-error hook (ProcessContext::SetFatalErrorHook): the place of the autosave (§4.13, §8.14 item 6).
		static void OnEditorFatalError(void* /*userData*/, FatalErrorKind kind, std::string_view /*message*/)
		{
			ENGINE_WARN("Editor fatal-error hook ({}): no project is open, nothing to autosave", FatalErrorKindToString(kind));
		}

	}

	EditorApp::EditorApp(ApplicationSpecification specification, EditorAppOptions options)
		: Application(std::move(specification)), m_Options(std::move(options))
	{
	}

	Status EditorApp::OnInitialize()
	{
		GetProcessContext().SetFatalErrorHook({ .Function = &Utils::OnEditorFatalError, .UserData = nullptr });

		// The viewport capture owns its pipeline from startup (§8.12); creating a pipeline at startup that the device has no
		// memory for is fatal (§8.14 item 7).
		EngineContext& context = GetContext();
		if (GraphicsDevice* device = context.GetGraphicsDevice())
		{
			Result<Scope<ViewportCapture>> capture = ViewportCapture::Create(*device, *context.GetPipelineFactory());
			if (!capture.has_value())
			{
				if (capture.error().GetCode() == ErrorCode::Gpu)
					FatalError(FatalErrorKind::OutOfMemory, std::format("Cannot create the viewport capture: {}", capture.error().ToString()));
				return std::unexpected(std::move(capture).error().WithContext("while creating the viewport capture"));
			}
			m_ViewportCapture = std::move(*capture);
		}
		return {};
	}

	void EditorApp::OnShutdown()
	{
		m_ViewportCapture.reset();
		GetProcessContext().SetFatalErrorHook({});
	}

	void EditorApp::OnUpdate(const FrameTime& frame)
	{
		const std::optional<uint64_t> maxFrames = GetSpecification().MaxFrames;
		if (maxFrames.has_value() && frame.FrameIndex + 1 == *maxFrames && !WriteScreenshots())
			RequestExit(ExitCode::Failed);
	}

	void EditorApp::OnImGuiRender()
	{
		ImGui::ShowDemoWindow();
	}

	bool EditorApp::WriteScreenshots()
	{
		const auto write = [](std::string_view what, const std::filesystem::path& path, const Result<Image>& image)
		{
			if (!image.has_value())
			{
				ENGINE_ERROR("Cannot capture the {} screenshot: {}", what, image.error());
				return false;
			}
			const Status written = WritePng(path, *image);
			if (!written.has_value())
			{
				ENGINE_ERROR("Cannot write the {} screenshot: {}", what, written.error());
				return false;
			}
			ENGINE_INFO("Wrote the {} screenshot ({}x{})", what, image->Width, image->Height);
			return true;
		};

		GraphicsDevice* device = GetContext().GetGraphicsDevice();
		bool succeeded = true;
		if (!m_Options.ViewportScreenshotPath.empty())
		{
			const Result<Image> image = m_ViewportCapture != nullptr
				? m_ViewportCapture->Capture({})
				: Result<Image>(MakeError(ErrorCode::Unsupported, "screenshots need a renderer (not --renderer none)"));
			succeeded = write("viewport", m_Options.ViewportScreenshotPath, image) && succeeded;
		}
		if (!m_Options.EditorScreenshotPath.empty())
		{
			ImGuiLayer* imgui = GetImGuiLayer();
			const Result<Image> image = device != nullptr && imgui != nullptr
				? CaptureImGuiScreenshot(*device, *imgui, {})
				: Result<Image>(MakeError(ErrorCode::Unsupported, "screenshots need a renderer (not --renderer none)"));
			succeeded = write("editor", m_Options.EditorScreenshotPath, image) && succeeded;
		}
		return succeeded;
	}

	Result<Scope<Application>> CreateEditorApp(std::span<const std::string> arguments)
	{
		const std::vector<CommandLineOption> options = Utils::GetEditorCommandLineOptions();
		ENGINE_TRY_ASSIGN(CommandLine commandLine, CommandLine::Parse(arguments, options));
		if (!commandLine.GetPositional().empty())
		{
			return MakeError(ErrorCode::InvalidArgument, "unexpected argument '{}': the editor takes no positional arguments",
				commandLine.GetPositional().front());
		}
		ApplicationSpecification specification;
		specification.Name = ENGINE_PRODUCT_NAME;
		specification.EnableImGui = true;
		specification.ImGuiIniPath = "Editor/imgui.ini";
		ENGINE_TRY(ApplyEngineCommandLine(commandLine, specification));

		EditorAppOptions editorOptions;
		ENGINE_TRY_ASSIGN(editorOptions.ViewportScreenshotPath, Utils::GetPathOption(commandLine, Utils::ViewportScreenshotOption));
		ENGINE_TRY_ASSIGN(editorOptions.EditorScreenshotPath, Utils::GetPathOption(commandLine, Utils::EditorScreenshotOption));
		const bool anyScreenshot = !editorOptions.ViewportScreenshotPath.empty() || !editorOptions.EditorScreenshotPath.empty();
		if (anyScreenshot && !specification.MaxFrames.has_value())
			return MakeError(ErrorCode::InvalidArgument, "the screenshot options are written after the last frame and need --frames");
		if (!editorOptions.EditorScreenshotPath.empty() && specification.MaxFrames.value_or(0) < 2)
		{
			return MakeError(ErrorCode::InvalidArgument, "option '{}' shows the UI of the frame before the last and needs --frames 2 or more",
				Utils::EditorScreenshotOption);
		}
		return CreateScope<EditorApp>(std::move(specification), std::move(editorOptions));
	}

}
