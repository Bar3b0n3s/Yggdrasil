#include "EnginePCH.h"
#include "Engine/App/Application.h"

#include "Engine/App/EngineContext.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GpuDiagnostics.h"
#include "Engine/Graphics/GpuProfiler.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/Graphics/Swapchain.h"
#include "Engine/ImGui/ImGuiLayer.h"
#include "Engine/Platform/ErrorDialog.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <format>
#include <string>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

		constexpr std::string_view HeadlessOption = "--headless";
		constexpr std::string_view FramesOption = "--frames";
#if !defined(ENGINE_DIST)
		constexpr std::string_view UserDataDirectoryOption = "--user-data-dir";
		constexpr std::string_view RendererOption = "--renderer";
		constexpr std::string_view GpuValidationOption = "--gpu-validation";
		constexpr std::string_view GpuValidationSync = "sync";
		constexpr std::string_view ExpectNoGpuErrorsOption = "--expect-no-gpu-errors";
		constexpr std::string_view VulkanApiOption = "--vulkan-api";
		constexpr std::string_view GpuOption = "--gpu";
		constexpr std::string_view GpuInjectFaultOption = "--gpu-inject-fault";
#endif

		// Every option of GetEngineCommandLineOptions. Dist honours only the options §13.9 lists, so --user-data-dir is
		// an unknown option there.
		constexpr std::array EngineCommandLineOptions = {
			CommandLineOption{
				.Name = HeadlessOption,
				.Description = "Run without a display on GLFW's null platform, one fixed step per frame (ManualClock).",
			},
			CommandLineOption{
				.Name = FramesOption,
				.Value = CommandLineValue::Required,
				.ValueName = "N",
				.Description = "Exit with code 0 after N frames (N >= 1).",
			},
#if !defined(ENGINE_DIST)
			CommandLineOption{
				.Name = UserDataDirectoryOption,
				.Value = CommandLineValue::Required,
				.ValueName = "path",
				.Description = "Keep logs and crash reports under this absolute directory instead of the user's data folder.",
			},
			CommandLineOption{
				.Name = RendererOption,
				.Value = CommandLineValue::Required,
				.ValueName = "vulkan|none",
				.Description = "Render with Vulkan (default), or not at all (logic only, no GPU needed).",
			},
			CommandLineOption{
				.Name = GpuValidationOption,
				.Value = CommandLineValue::Optional,
				.ValueName = "sync",
				.Description = "Enable the Vulkan validation layer and NVRHI validation (on by default in Debug); =sync adds "
							   "synchronization validation.",
			},
			CommandLineOption{
				.Name = ExpectNoGpuErrorsOption,
				.Description = "Exit with code 1 when the GPU device reported any validation, NVRHI or resource-leak error or "
							   "warning during the run, its teardown included.",
			},
			CommandLineOption{
				.Name = VulkanApiOption,
				.Value = CommandLineValue::Required,
				.ValueName = "1.3|1.4",
				.Description = "Request at most this Vulkan API version (1.3 forces the fallbacks of the 1.4 paths).",
			},
			CommandLineOption{
				.Name = GpuOption,
				.Value = CommandLineValue::Required,
				.ValueName = "index|name",
				.Description = "Use the GPU with this index or a name containing this text (overrides ENGINE_GPU).",
			},
			CommandLineOption{
				.Name = GpuInjectFaultOption,
				.Value = CommandLineValue::Required,
				.ValueName = "device-lost|oom-texture|hang",
				.Description = "Force a GPU fault path for tests: device loss, texture out-of-memory or a GPU hang.",
			},
#endif
		};

		// The startup rule of §8.14 item 7 for the frame's rendering objects: a Gpu error creating one means the device is
		// out of memory and ends the process with FatalError(OutOfMemory) (exit code 4); any other error is returned with
		// `context`, and Run reports it as InitFailed.
		template<typename T>
		static Result<T> CheckRenderingObject(Result<T> result, std::string_view fatalMessage, std::string context)
		{
			if (!result.has_value() && result.error().GetCode() == ErrorCode::Gpu)
				FatalError(FatalErrorKind::OutOfMemory, std::format("{}: {}", fatalMessage, result.error().ToString()));
			return WithContext(std::move(result), std::move(context));
		}

		// A rendering object recreated during the frame loop (a recreated swapchain, a resized offscreen target) that fails
		// ends the process: OutOfMemory for a Gpu error (§8.14 item 7), Gpu for anything else.
		[[noreturn]] static void EndRenderingObjectFailure(const Error& error, std::string_view message)
		{
			const FatalErrorKind kind = error.GetCode() == ErrorCode::Gpu ? FatalErrorKind::OutOfMemory : FatalErrorKind::Gpu;
			FatalError(kind, std::format("{}: {}", message, error.ToString()));
		}

		// The rule of the --expect-no-gpu-errors check: any error or warning in the device's whole life fails the run.
		[[nodiscard]] static bool HasGpuMessages(const GpuMessageCounts& counts)
		{
			return counts.Errors > 0 || counts.Warnings > 0;
		}

	}

	// The frame's rendering objects with a device (Application.h, step 1), from InitializeRendering to ShutdownRendering.
	struct Application::FrameRendering
	{
		Scope<Swapchain> WindowSwapchain;      // windowed processes
		Scope<OffscreenTarget> OffscreenFrame; // headless processes (§8.13)
		Scope<FramePacer> Pacer;
		Scope<GpuProfiler> Profiler;
		nvrhi::CommandListHandle CommandList{};
		// Headless: per frame slot, the frame target (its framebuffer, which references its textures) of the slot's last
		// submission, dropped when the slot's next frame begins, after FramePacer::BeginFrame has waited for that
		// submission. NVRHI's command lists do not reference textures used only by clears (ADR 0009 decision 31), so
		// without this a resize could destroy a target that a frame still in flight clears. The swapchain needs none: its
		// recreation waits for idle.
		std::vector<nvrhi::FramebufferHandle> SlotTargets;
		// The frame clock's time since the last UI frame (ImGuiLayer::BeginFrame), accumulated over skipped frames.
		double UiDeltaSeconds = 0.0;
		bool IsImGuiFailing = false; // the last UI frame failed to render, so the next failure is not logged again
	};

	std::span<const CommandLineOption> GetEngineCommandLineOptions()
	{
		return Utils::EngineCommandLineOptions;
	}

	Status ApplyEngineCommandLine(const CommandLine& commandLine, ApplicationSpecification& specification)
	{
		if (commandLine.Has(Utils::HeadlessOption))
		{
			specification.Window = WindowMode::Headless;
			specification.Clock = ClockKind::Manual; // every headless run uses ManualClock (§13.9)
		}

		ENGINE_TRY_ASSIGN(const std::optional<uint64_t> frames, commandLine.GetUnsigned(Utils::FramesOption));
		if (frames.has_value())
		{
			if (*frames == 0)
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs a number of frames >= 1, got 0", Utils::FramesOption);
			specification.MaxFrames = *frames;
		}

#if !defined(ENGINE_DIST)
		if (const std::optional<std::string_view> directory = commandLine.GetValue(Utils::UserDataDirectoryOption))
		{
			// std::filesystem::path throws on ill-formed UTF-8 on Windows, and the option is external input.
			if (!IsValidUtf8(*directory))
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs a path in UTF-8", Utils::UserDataDirectoryOption);
			std::filesystem::path root(std::u8string_view(reinterpret_cast<const char8_t*>(directory->data()), directory->size()));
			if (!root.is_absolute())
			{
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs an absolute directory, got '{}'",
					Utils::UserDataDirectoryOption, *directory);
			}
			specification.UserDataRoot = std::move(root);
		}

		if (const std::optional<std::string_view> renderer = commandLine.GetValue(Utils::RendererOption))
		{
			const std::optional<RendererMode> mode = RendererModeFromCommandLine(*renderer);
			if (!mode.has_value())
				return MakeError(ErrorCode::InvalidArgument, "option '{}' takes 'vulkan' or 'none', got '{}'", Utils::RendererOption, *renderer);
			specification.Renderer = *mode;
		}

		if (commandLine.Has(Utils::GpuValidationOption))
		{
			const std::optional<std::string_view> validation = commandLine.GetValue(Utils::GpuValidationOption);
			if (validation.has_value() && *validation != Utils::GpuValidationSync)
			{
				return MakeError(ErrorCode::InvalidArgument, "option '{}' takes no value or '{}', got '{}'", Utils::GpuValidationOption,
					Utils::GpuValidationSync, *validation);
			}
			specification.Graphics.Validation = true;
			specification.Graphics.SynchronizationValidation = validation.has_value();
		}

		if (commandLine.Has(Utils::ExpectNoGpuErrorsOption))
			specification.ExpectNoGpuErrors = true;

		if (const std::optional<std::string_view> api = commandLine.GetValue(Utils::VulkanApiOption))
		{
			const std::optional<VulkanApiVersion> version = VulkanApiVersionFromString(*api);
			if (!version.has_value())
				return MakeError(ErrorCode::InvalidArgument, "option '{}' takes '1.3' or '1.4', got '{}'", Utils::VulkanApiOption, *api);
			specification.Graphics.MaxApiVersion = *version;
		}

		// CommandLine::Parse never yields an empty value for a Required option.
		if (const std::optional<std::string_view> gpu = commandLine.GetValue(Utils::GpuOption))
			specification.Graphics.GpuOverride = std::string(*gpu);

		if (const std::optional<std::string_view> fault = commandLine.GetValue(Utils::GpuInjectFaultOption))
		{
			const std::optional<GpuFault> injected = GpuFaultFromString(*fault);
			if (!injected.has_value() || *injected == GpuFault::None)
			{
				return MakeError(ErrorCode::InvalidArgument, "option '{}' takes 'device-lost', 'oom-texture' or 'hang', got '{}'",
					Utils::GpuInjectFaultOption, *fault);
			}
			specification.Graphics.InjectFault = *injected;
		}
#endif

		specification.Args = commandLine;
		return {};
	}

	Application::Application(ApplicationSpecification specification)
		: m_Specification(std::move(specification))
	{
	}

	Application::~Application() = default;

	int Application::Run()
	{
		ENGINE_CORE_VERIFY(!m_HasRun, "Application::Run may be called once per Application");
		m_HasRun = true;

		ProcessContext* process = ProcessContext::GetCurrent();
		ENGINE_CORE_VERIFY(process != nullptr, "Application::Run needs a live ProcessContext");
		ENGINE_CORE_ASSERT(process->GetSpecification().Window == m_Specification.Window,
			"the application is {} but the process is {}: a process is windowed or headless, never both",
			WindowModeToString(m_Specification.Window), WindowModeToString(process->GetSpecification().Window));

		if (m_Specification.Clock == ClockKind::Scripted)
		{
			ENGINE_CORE_ERROR("Cannot run the application '{}': the Scripted clock needs a delta table and is chosen per "
							  "FeatureTest suite, never through ApplicationSpecification::Clock",
				m_Specification.Name);
			return ExitCode::InitFailed;
		}

		// 1. Per-context initialization (§4.1 level 2), then the application's own.
		WindowSpecification window = m_Specification.WindowSettings;
		if (window.Title.empty())
			window.Title = m_Specification.Name;
		const EngineContextSpecification contextSpecification = {
			.WorkerCount = m_Specification.WorkerCount.value_or(JobSystem::GetDefaultWorkerCount()),
			.UserDataDirectory = process->GetUserDataPaths().Root,
			.EngineResourcesDirectory = m_Specification.EngineResourcesDirectory,
			.EngineCacheDirectory = m_Specification.EngineCacheDirectory,
			.Window = std::move(window),
			.RegisterTypes = m_Specification.RegisterTypes,
			.Graphics = m_Specification.Renderer == RendererMode::Vulkan ? std::optional<GraphicsSpecification>(m_Specification.Graphics)
																		 : std::nullopt,
		};
		Result<Scope<EngineContext>> context = EngineContext::Create(contextSpecification);
		if (!context.has_value())
		{
			ReportInitializationFailure(*process, context.error());
			return ExitCode::InitFailed;
		}
		m_Context = std::move(*context);

		const Status rendering = InitializeRendering();
		if (!rendering.has_value())
		{
			ReportInitializationFailure(*process, rendering.error());
			ShutdownRendering();
			m_Context.reset();
			return ExitCode::InitFailed;
		}

		const Status initialized = OnInitialize();
		if (!initialized.has_value())
		{
			ReportInitializationFailure(*process, initialized.error());
			m_PendingExitCode.reset();
			ShutdownRendering();
			m_Context.reset();
			return ExitCode::InitFailed;
		}

		// 2. The frame loop. An exit requested during OnInitialize makes it run no frame.
		const FrameLoopSpecification loopSpecification = {
			.Loop = m_Specification.Loop,
			.MaxFrames = m_Specification.MaxFrames,
			.ThrottleToFixedHz = m_Specification.Window == WindowMode::Headless && m_Specification.ThrottleHeadless,
		};
		IFrameLoopClient& client = *this; // the base is private, so convert here rather than inside CreateScope
		m_FrameLoop = CreateScope<FrameLoop>(*m_Context, client, CreateClock(), loopSpecification);
		if (m_PendingExitCode.has_value())
			m_FrameLoop->RequestExit(*m_PendingExitCode);
		int exitCode = m_FrameLoop->Run();
		m_FrameLoop.reset();

		// 3. Shutdown in reverse order. The GPU services go before the rest of the context, so a run that expects no GPU
		// messages fails when its device reported any error or warning in its whole life, its teardown included (leaks, the
		// validation layer's reports at vkDestroyDevice, §15.3); an earlier failure keeps its code.
		OnShutdown();
		m_PendingExitCode.reset();
		ShutdownRendering();
		const bool hadDevice = m_Context->GetGraphicsDevice() != nullptr;
		const GpuMessageCounts gpuMessages = m_Context->DestroyGraphics();
		if (m_Specification.ExpectNoGpuErrors && hadDevice && exitCode == ExitCode::Success && Utils::HasGpuMessages(gpuMessages))
		{
			ENGINE_CORE_ERROR("The GPU device reported {} error(s) and {} warning(s) during the run (--expect-no-gpu-errors)",
				gpuMessages.Errors, gpuMessages.Warnings);
			exitCode = ExitCode::Failed;
		}
		m_Context.reset();
		return exitCode;
	}

	void Application::RequestExit(int exitCode)
	{
		if (m_FrameLoop != nullptr)
			m_FrameLoop->RequestExit(exitCode);
		else if (!m_PendingExitCode.has_value())
			m_PendingExitCode = exitCode;
	}

	EngineContext& Application::GetContext()
	{
		ENGINE_CORE_ASSERT(m_Context != nullptr, "Application::GetContext outside initialization and shutdown");
		return *m_Context;
	}

	ProcessContext& Application::GetProcessContext()
	{
		ENGINE_CORE_ASSERT(m_Context != nullptr, "Application::GetProcessContext outside initialization and shutdown");
		ProcessContext* process = ProcessContext::GetCurrent();
		ENGINE_CORE_VERIFY(process != nullptr, "Application::GetProcessContext without a ProcessContext");
		return *process;
	}

	void Application::OnFrameEvent(Event& event)
	{
		// Resizes need no call here: the swapchain compares the framebuffer size with the size it was created for when the
		// next frame acquires its image, so the several events of one resize (window size, framebuffer size, restore)
		// cause one recreation, never one per event.
		OnEvent(event);
	}

	void Application::OnFrameSafePoint()
	{
		OnSafePoint();
	}

	void Application::OnFrameFixedStep(const SimStep& step)
	{
		OnFixedStep(step);
	}

	void Application::OnFrameUpdate(const FrameTime& frame)
	{
		OnUpdate(frame);
	}

	void Application::OnFrameRender(const FrameTime& frame)
	{
		// Without a device (RendererMode::None) there is nothing to render.
		if (m_Rendering == nullptr)
			return;
		FrameRendering& rendering = *m_Rendering;
		GraphicsDevice& device = *m_Context->GetGraphicsDevice();
		Window& window = *m_Context->GetWindow();

		// The UI clock runs on the frame clock, also across the frames that render nothing (ImGuiLayer::BeginFrame).
		rendering.UiDeltaSeconds += frame.UnscaledDeltaTime;

		// §8.2: wait (bounded) until this frame's slot is free, then pick the target. The slot's previous submission has
		// completed, so the target it held may go (FrameRendering::SlotTargets).
		rendering.Pacer->BeginFrame();
		const uint32_t frameSlot = rendering.Pacer->GetFrameSlot();
		rendering.Profiler->BeginFrame(frameSlot);
		if (frameSlot < rendering.SlotTargets.size())
			rendering.SlotTargets[frameSlot] = nullptr;

		nvrhi::IFramebuffer* framebuffer = nullptr;
		nvrhi::ITexture* target = nullptr;
		uint32_t width = 0;
		uint32_t height = 0;
		if (rendering.WindowSwapchain != nullptr)
		{
			Swapchain& swapchain = *rendering.WindowSwapchain;
			const Result<SwapchainAcquireStatus> status = swapchain.AcquireNextImage(frameSlot);
			if (!status.has_value())
				Utils::EndRenderingObjectFailure(status.error(), "Cannot recreate the swapchain");
			if (*status == SwapchainAcquireStatus::Acquired)
			{
				framebuffer = swapchain.GetCurrentFramebuffer();
				target = swapchain.GetCurrentTexture();
				width = swapchain.GetWidth();
				height = swapchain.GetHeight();
			}
		}
		else if (window.GetFramebufferWidth() > 0 && window.GetFramebufferHeight() > 0)
		{
			// Headless: the offscreen target follows the window's framebuffer size (a render target created at resize,
			// §8.14 item 7).
			OffscreenTarget& offscreen = *rendering.OffscreenFrame;
			const Status resized = offscreen.Resize(device, window.GetFramebufferWidth(), window.GetFramebufferHeight());
			if (!resized.has_value())
				Utils::EndRenderingObjectFailure(resized.error(), "Cannot resize the frame's offscreen target");
			framebuffer = offscreen.GetFramebuffer();
			target = offscreen.GetColorTexture();
			width = offscreen.GetWidth();
			height = offscreen.GetHeight();
			rendering.SlotTargets[frameSlot] = framebuffer;
		}

		// A skipped frame (minimized, a recreated swapchain, an acquire that timed out) records and submits nothing.
		if (framebuffer == nullptr || target == nullptr)
		{
			rendering.Pacer->EndFrame(device.GetLastSubmissionID());
			device.RunGarbageCollection();
			return;
		}

		nvrhi::ICommandList& commandList = *rendering.CommandList;
		commandList.open();
		commandList.clearTextureFloat(target, nvrhi::AllSubresources,
			nvrhi::Color(FrameClearColor[0], FrameClearColor[1], FrameClearColor[2], FrameClearColor[3]));

		RenderContext context = {
			.Device = &device,
			.CommandList = &commandList,
			.Framebuffer = framebuffer,
			.Target = target,
			.Width = width,
			.Height = height,
			.FrameSlot = frameSlot,
			.FrameIndex = rendering.Pacer->GetFrameIndex(),
			.Profiler = rendering.Profiler.get(),
		};
		OnRender(context);

		if (m_ImGuiLayer != nullptr)
		{
			m_ImGuiLayer->BeginFrame(rendering.UiDeltaSeconds, frameSlot);
			rendering.UiDeltaSeconds = 0.0;
			OnImGuiRender();
			m_ImGuiLayer->EndFrame();

			GpuProfileScope scope(*rendering.Profiler, commandList, "ImGui");
			const Status rendered = m_ImGuiLayer->Render(commandList, *framebuffer);
			// A Gpu error means that a pipeline (created per target format on first use, ADR 0009 decision 25), a geometry
			// buffer or a binding set could not be created: the device is out of memory, which ends the process (§8.14
			// item 7). Texture failures never come back here (the renderer logs them and skips their draws). Any other
			// failure is logged once per run of failing frames, because the UI is recorded every frame.
			if (!rendered.has_value() && rendered.error().GetCode() == ErrorCode::Gpu)
				FatalError(FatalErrorKind::OutOfMemory, std::format("Cannot render the ImGui frame: {}", rendered.error().ToString()));
			if (!rendered.has_value() && !rendering.IsImGuiFailing)
				ENGINE_CORE_ERROR("Cannot render the ImGui frame: {}", rendered.error());
			rendering.IsImGuiFailing = !rendered.has_value();
		}
		commandList.close();

		if (rendering.WindowSwapchain != nullptr)
			rendering.WindowSwapchain->QueueFrameSemaphores();
		const uint64_t submission = device.ExecuteCommandList(commandList);
		if (rendering.WindowSwapchain != nullptr)
			rendering.WindowSwapchain->Present();
		rendering.Pacer->EndFrame(submission);
		device.RunGarbageCollection();
	}

	Status Application::InitializeRendering()
	{
		// RendererMode::None renders nothing and needs none of the rendering objects.
		GraphicsDevice* device = m_Context->GetGraphicsDevice();
		if (device == nullptr)
			return {};
		Window* window = m_Context->GetWindow();
		ENGINE_CORE_VERIFY(window != nullptr, "the application's context has no window");
		const uint32_t framesInFlight = device->GetFramesInFlight();

		m_Rendering = CreateScope<FrameRendering>();
		FrameRendering& rendering = *m_Rendering;
		if (m_Specification.Window == WindowMode::Windowed)
		{
			ENGINE_TRY_ASSIGN(rendering.WindowSwapchain,
				Utils::CheckRenderingObject(Swapchain::Create(*device, *window,
												{
													.VSync = device->GetSpecification().VSync,
													.FramesInFlight = framesInFlight,
												}),
					"Cannot create the swapchain", "while creating the swapchain"));
		}
		else
		{
			// Headless (§8.13): the frames land in an offscreen target of the window's framebuffer size.
			const OffscreenTargetSpecification specification = {
				.Width = std::max(window->GetFramebufferWidth(), 1u),
				.Height = std::max(window->GetFramebufferHeight(), 1u),
				.ColorFormat = nvrhi::Format::RGBA8_UNORM,
				.Depth = false,
				.ClearColor = nvrhi::Color(FrameClearColor[0], FrameClearColor[1], FrameClearColor[2], FrameClearColor[3]),
				.DebugName = "FrameTarget",
			};
			ENGINE_TRY_ASSIGN(OffscreenTarget offscreen,
				Utils::CheckRenderingObject(OffscreenTarget::Create(*device, specification), "Cannot create the frame's offscreen target",
					"while creating the frame's offscreen target"));
			rendering.OffscreenFrame = CreateScope<OffscreenTarget>(std::move(offscreen));
			rendering.SlotTargets.resize(framesInFlight);
		}
		ENGINE_TRY_ASSIGN(rendering.CommandList,
			Utils::CheckRenderingObject(device->CreateCommandList(), "Cannot create the frame's command list",
				"while creating the frame's command list"));
		rendering.Pacer = CreateScope<FramePacer>(*device, framesInFlight);
		rendering.Profiler = CreateScope<GpuProfiler>(*device, framesInFlight);

		if (m_Specification.EnableImGui)
		{
			// imgui.ini lives under the user-data folder (user://) while no project is open (§8.11).
			std::filesystem::path iniFilePath;
			if (!m_Specification.ImGuiIniPath.empty())
			{
				ENGINE_CORE_ASSERT(IsValidUtf8(m_Specification.ImGuiIniPath), "ApplicationSpecification::ImGuiIniPath must be UTF-8");
				const std::string& iniPath = m_Specification.ImGuiIniPath;
				iniFilePath = GetProcessContext().GetUserDataPaths().Root
					/ std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(iniPath.data()), iniPath.size()));
			}
			ENGINE_TRY_ASSIGN(m_ImGuiLayer,
				Utils::CheckRenderingObject(ImGuiLayer::Create(*window, *device, *m_Context->GetPipelineFactory(),
												{
													.IniFilePath = std::move(iniFilePath),
													.FramesInFlight = framesInFlight,
												}),
					"Cannot create the ImGui layer", "while creating the ImGui layer"));
		}
		return {};
	}

	void Application::ShutdownRendering()
	{
		if (m_Rendering == nullptr && m_ImGuiLayer == nullptr)
			return;
		// §8.14 item 4: nothing the rendering objects own may still be in use by the GPU when they go.
		if (GraphicsDevice* device = m_Context->GetGraphicsDevice())
			device->WaitForIdle();
		m_ImGuiLayer.reset();
		if (m_Rendering != nullptr)
		{
			m_Rendering->Profiler.reset();
			m_Rendering->CommandList = nullptr;
			m_Rendering->WindowSwapchain.reset();
			m_Rendering->SlotTargets.clear();
			m_Rendering->OffscreenFrame.reset();
			m_Rendering->Pacer.reset();
			m_Rendering.reset();
		}
	}

	void Application::ReportInitializationFailure(const ProcessContext& process, const Error& error) const
	{
		ENGINE_CORE_ERROR("Cannot initialize the application '{}': {}", m_Specification.Name, error);
		if (process.GetSpecification().ShowErrorDialogs && m_Specification.Window == WindowMode::Windowed)
			ShowErrorDialog(m_Specification.Name, std::format("Cannot start: {}", error.ToString()));
	}

	Scope<Clock> Application::CreateClock() const
	{
		if (m_Specification.Clock == ClockKind::Manual)
			return CreateScope<ManualClock>(m_Specification.Loop.GetFixedDelta());
		return CreateScope<SystemClock>();
	}

}
