#include "Runtime/RuntimeApp.h"

#include "Engine/App/CommandLine.h"
#include "Engine/App/EngineContext.h"
#include "Engine/App/ExitCode.h"
#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Asset/PakMount.h"
#include "Engine/Asset/PakReader.h"
#include "Engine/Asset/RuntimeAssetManager.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GpuProfiler.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/RenderContext.h"
#include "Engine/Platform/Process.h"
#include "Engine/Platform/Window.h"
#include "Engine/Project/ProjectSerializer.h"
#include "Engine/Renderer/BlitPass.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/StaleMirrorSchedule.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Session/PlaySession.h"

#if !defined(ENGINE_DIST)
	#include "Engine/App/ProcessContext.h"
	#include "Engine/App/VulkanErrorHandler.h"
	#include "Engine/Automation/Methods/AutomationTypes.h"
	#include "Engine/Automation/Methods/RegisterSharedMethods.h"
	#include "Engine/Automation/Methods/RuntimeAutomationServer.h"
#endif

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <limits>
#include <system_error>
#include <utility>
#include <vector>

namespace Engine {

	namespace Utils {

#if !defined(ENGINE_DIST)
		constexpr std::string_view ManifestOption = "--manifest";
		constexpr std::string_view ScreenshotAtOption = "--screenshot-at";
		constexpr std::string_view AutomationOption = "--automation";
		constexpr std::string_view PausedOption = "--paused";

		constexpr std::array RuntimeCommandLineOptions = {
			CommandLineOption{ .Name = ManifestOption, .Value = CommandLineValue::Required, .ValueName = "path", .Description = "Run the game of this Game.json instead of the one next to the executable." },
			CommandLineOption{ .Name = ScreenshotAtOption, .Value = CommandLineValue::Required, .ValueName = "T:path", .Description = "Write the game view as a PNG after the frame in which tick T is reached." },
			CommandLineOption{ .Name = AutomationOption, .Value = CommandLineValue::Optional, .ValueName = "port", .Description = "Serve the Runtime's automation subset (127.0.0.1)." },
			CommandLineOption{ .Name = PausedOption, .Value = CommandLineValue::None, .ValueName = {}, .Description = "Start the play session paused at tick 0 (needs --automation)." },
		};

		// The types of the Runtime's automation subset (RuntimeAutomationServer.h), registered into the context's registry
		// before it freezes. Dist has no automation, so it registers nothing.
		static void RegisterRuntimeAutomationTypes(TypeRegistry& registry)
		{
			RegisterAutomationSharedTypes(registry);
			RegisterSharedMethodTypes(registry);
		}

		// A path option's value: non-empty, in UTF-8, made absolute against the working directory.
		static Result<std::filesystem::path> ReadPathValue(std::string_view option, std::string_view value)
		{
			if (value.empty())
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs a path", option);
			if (!IsValidUtf8(value))
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs a path in UTF-8", option);
			std::error_code error;
			std::filesystem::path path = std::filesystem::absolute(FileSystem::PathFromUtf8(value), error);
			if (error)
				return MakeError(ErrorCode::InvalidArgument, "option '{}': '{}' is not a usable path ({})", option, value, error.message());
			return path;
		}
#endif

		// Game.json next to the executable, or in the Resources folder of a macOS bundle's executable
		// (<Name>.app/Contents/MacOS/<Name>, §14.1).
		static std::filesystem::path GetDefaultManifestPath(const std::filesystem::path& executablePath)
		{
			const std::filesystem::path directory = executablePath.parent_path();
			if (directory.filename() == "MacOS" && directory.parent_path().filename() == "Contents")
				return directory.parent_path() / "Resources" / std::string(GameManifest::FileName);
			return directory / std::string(GameManifest::FileName);
		}

		// The XXH64 of the pak `pak.Path` (relative to `directory`) against the manifest's (§14.1: the exported paks, never
		// others). Streams the file. Errors: those of reading it; Validation naming the pak on a mismatch.
		static Status VerifyPakHash(const std::filesystem::path& directory, const GameManifestPak& pak)
		{
			ENGINE_TRY_ASSIGN(const Scope<NativeDirectoryMount> files, NativeDirectoryMount::Create(directory, MountAccess::ReadOnly));
			ENGINE_TRY_ASSIGN(const VfsPath path, VfsPath::Create("game", pak.Path));
			ENGINE_TRY_ASSIGN(const Scope<IFileStream> stream, files->Open(path));
			constexpr size_t ChunkBytes = size_t{ 1 } << 20;
			std::vector<std::byte> chunk(ChunkBytes);
			XXH64Hasher hasher;
			while (true)
			{
				ENGINE_TRY_ASSIGN(const size_t read, stream->Read(chunk));
				if (read == 0)
					break;
				hasher.Update(std::span<const std::byte>(chunk.data(), read));
			}
			const uint64_t hash = hasher.Digest();
			if (hash != pak.Hash)
			{
				return std::unexpected(Error(ErrorCode::Validation,
					std::format("'{}' does not match {} (XXH64 {:016x}, expected {:016x}): the game's files are damaged", pak.Path,
						GameManifest::FileName, hash, pak.Hash))
						.WithHint("reinstall the game, or export it again"));
			}
			return {};
		}

		// The project settings that ride in Game.pak's TOC (§14.1: Metadata {"Project": <the canonical .eproj document>}).
		static Result<ProjectSettings> ReadProjectSettings(const PakReader& pak, const TypeRegistry& registry)
		{
			const JsonReader metadata(pak.GetMetadata().Get());
			ENGINE_TRY_ASSIGN(const JsonReader project, metadata.GetMember("Project"));
			ProjectLoadOptions options;
			options.StrictUnknowns = true;
			options.SourcePath = std::format("{} Metadata/Project", pak.GetName());
			ProjectLoadReport report;
			return ProjectSerializer::FromJson(project.GetValue(), registry, options, report);
		}

		static bool IsSameSimulation(const SimulationSettings& left, const SimulationSettings& right)
		{
			return left.FixedHz == right.FixedHz && left.MaxStepsPerFrame == right.MaxStepsPerFrame && left.Seed == right.Seed
				&& left.MaxEntities == right.MaxEntities;
		}

		// A startup creation's result: a Gpu error is the device out of memory at startup, which ends the process (§8.14
		// item 7); other errors get the context `what`.
		template<typename T>
		static Result<T> CheckStartupCreation(Result<T> created, std::string_view what)
		{
			if (created.has_value())
				return created;
			if (created.error().GetCode() == ErrorCode::Gpu)
				FatalError(FatalErrorKind::OutOfMemory, std::format("Cannot create {}: {}", what, created.error().ToString()));
			return std::unexpected(std::move(created).error().WithContext(std::format("while creating {}", what)));
		}

		// The game view's size: the window's framebuffer, at least 1 x 1.
		static std::pair<uint32_t, uint32_t> GetViewSize(const Window& window)
		{
			return { std::max(window.GetFramebufferWidth(), 1u), std::max(window.GetFramebufferHeight(), 1u) };
		}

	}

	struct RuntimeApp::State
	{
		RuntimeOptions Options{};
		GameManifest Manifest{};
		Ref<const PakReader> GamePak;
		Scope<PlaySession> Session;
		uint32_t ViewWidth = 0; // the size last given to PlaySession::SetViewSize
		uint32_t ViewHeight = 0;
		// With a device: the renderer of the game view, its blit into the frame target (created for the target's format at
		// the first rendered frame, when the swapchain's format is known), and the capture of screenshots.
		Scope<GpuResourceCache> GpuCache;
		Scope<SceneRendererPipelines> Pipelines;
		Scope<SceneRenderer> Renderer;
		Scope<BlitPass> Blit;
		nvrhi::Format BlitFormat = nvrhi::Format::UNKNOWN;
		Scope<ViewportCapture> Capture;
		bool RenderFailing = false; // the last frame's render or blit failed, so the next failure is not logged again
		// M8 stale mirrors (SceneRenderer.h): when the collections also release what the start scene's first frame did not
		// use (CreateRenderers notes the start scene as the shown scene's change).
		StaleMirrorSchedule Mirrors{};
		bool Running = false; // OnInitialize succeeded: the frames ran
		bool ScreenshotWritten = false;
#if !defined(ENGINE_DIST)
		Scope<RuntimeAutomationServer> Server;
#endif
	};

	std::span<const CommandLineOption> GetRuntimeCommandLineOptions()
	{
#if !defined(ENGINE_DIST)
		return Utils::RuntimeCommandLineOptions;
#else
		return {};
#endif
	}

	Result<RuntimeOptions> ParseRuntimeOptions(const CommandLine& commandLine, const std::filesystem::path& executablePath)
	{
		RuntimeOptions options;
		options.ManifestPath = Utils::GetDefaultManifestPath(executablePath);
#if !defined(ENGINE_DIST)
		if (const std::optional<std::string_view> manifest = commandLine.GetValue(Utils::ManifestOption))
		{
			ENGINE_TRY_ASSIGN(options.ManifestPath, Utils::ReadPathValue(Utils::ManifestOption, *manifest));
		}

		if (const std::optional<std::string_view> screenshot = commandLine.GetValue(Utils::ScreenshotAtOption))
		{
			// "<tick>:<path>": the tick is digits only, so the first ':' ends it (a Windows path has its own ':').
			const size_t colon = screenshot->find(':');
			const std::string_view tick = screenshot->substr(0, std::min(colon, screenshot->size()));
			RuntimeOptions::ScreenshotAt at;
			const std::from_chars_result parsed = std::from_chars(tick.data(), tick.data() + tick.size(), at.Tick);
			const bool digitsOnly = !tick.empty() && std::ranges::all_of(tick, [](char character)
			{
				return character >= '0' && character <= '9';
			});
			if (colon == std::string_view::npos || !digitsOnly || parsed.ec != std::errc() || parsed.ptr != tick.data() + tick.size())
			{
				return MakeError(ErrorCode::InvalidArgument, "option '{}' needs '<tick>:<path>' with a decimal tick, such as \"60:Shot.png\"; got '{}'",
					Utils::ScreenshotAtOption, *screenshot);
			}
			ENGINE_TRY_ASSIGN(at.Path, Utils::ReadPathValue(Utils::ScreenshotAtOption, screenshot->substr(colon + 1)));
			options.Screenshot = std::move(at);
		}

		options.Automation = commandLine.Has(Utils::AutomationOption);
		if (commandLine.GetValue(Utils::AutomationOption).has_value())
		{
			ENGINE_TRY_ASSIGN(const std::optional<uint64_t> port, commandLine.GetUnsigned(Utils::AutomationOption, std::numeric_limits<uint16_t>::max()));
			if (!port.has_value() || *port == 0)
				return MakeError(ErrorCode::InvalidArgument, "option '{}' takes a port from 1 to 65535 (without one, the OS assigns it)", Utils::AutomationOption);
			options.AutomationPort = static_cast<uint16_t>(*port);
		}

		options.StartPaused = commandLine.Has(Utils::PausedOption);
		if (options.StartPaused && !options.Automation)
		{
			return MakeError(ErrorCode::InvalidArgument, "option '{}' needs '{}': only play.step and play.resume advance a paused game",
				Utils::PausedOption, Utils::AutomationOption);
		}
#else
		static_cast<void>(commandLine);
#endif
		return options;
	}

	RuntimeApp::RuntimeApp(ApplicationSpecification specification, RuntimeOptions options, GameManifest manifest)
		: Application(std::move(specification)), m_State(CreateScope<State>())
	{
		m_State->Options = std::move(options);
		m_State->Manifest = std::move(manifest);
	}

	// OnShutdown released the manager and the session while the engine context existed.
	RuntimeApp::~RuntimeApp() = default;

	Status RuntimeApp::OnInitialize()
	{
		const Status initialized = InitializeGame();
		// Run calls OnShutdown only after a successful OnInitialize and destroys the context next, so what was built (the
		// server, the GPU objects, the session, the asset manager) goes now, in OnShutdown's order.
		if (!initialized)
			OnShutdown();
		m_State->Running = initialized.has_value();
		return initialized;
	}

	Status RuntimeApp::InitializeGame()
	{
		EngineContext& context = GetContext();
		State& state = *m_State;
		const GameManifest& manifest = state.Manifest;
		const std::filesystem::path directory = state.Options.ManifestPath.parent_path();

		// The paks must be the ones the manifest was exported with (§14.1); the engine context opened Engine.pak already.
		for (const GameManifestPak& pak : manifest.Paks)
			ENGINE_TRY(Utils::VerifyPakHash(directory, pak));
		const Ref<const PakReader>& enginePak = context.GetEnginePak();
		if (enginePak == nullptr)
			return MakeError(ErrorCode::InvalidState, "the engine context has no Engine.pak (ApplicationSpecification::EnginePak)");
		ENGINE_TRY_ASSIGN(state.GamePak, PakReader::Open(directory / FileSystem::PathFromUtf8(manifest.Paks[1].Path)));
		ENGINE_TRY_ASSIGN(Scope<PakMount> projectFiles, PakMount::Create(state.GamePak));
		ENGINE_TRY(context.GetVfs().Mount("project", std::move(projectFiles)));

		// The asset manager over both paks, injected into the context (§3 rule 4).
		m_Loaders = CreateScope<AssetLoaderRegistry>();
		RegisterBuiltinLoaders(*m_Loaders);
		m_Assets = CreateScope<RuntimeAssetManager>(RuntimeAssetManagerSpecification{
			.Jobs = &context.GetJobSystem(),
			.MainThread = &context.GetMainThreadQueue(),
			.Registry = &context.GetTypeRegistry(),
			.Loaders = m_Loaders.get(),
		});
		ENGINE_TRY(m_Assets->AddPak(enginePak));
		ENGINE_TRY(m_Assets->AddPak(state.GamePak));
		context.SetAssetManager(m_Assets.get());

		// The project's settings ride in Game.pak; the manifest's Simulation set up the loop, so both must agree.
		ENGINE_TRY_ASSIGN(ProjectSettings project, Utils::ReadProjectSettings(*state.GamePak, context.GetTypeRegistry()));
		if (!Utils::IsSameSimulation(project.Simulation, manifest.Simulation))
		{
			return std::unexpected(Error(ErrorCode::Validation, std::format("the Simulation settings of {} differ from those in '{}'", GameManifest::FileName, state.GamePak->GetName()))
					.WithHint("export the game again"));
		}

		// The start scene, through the same SceneSerializer::FromJson path Play uses (§14.3), as a play session.
		const std::string startScene = manifest.StartScene.ToString();
		ENGINE_TRY_ASSIGN(const AssetRef<Asset> loaded, WithContext(m_Assets->Load(manifest.StartScene), std::format("while loading the start scene {}", startScene)));
		const AssetRef<SceneData> scene = AssetCast<SceneData>(loaded);
		if (scene == nullptr)
		{
			return MakeError(ErrorCode::Validation, "the start scene {} of {} is a {}, not a Scene", startScene, GameManifest::FileName,
				AssetTypeToString(loaded->GetAssetType()));
		}
		ENGINE_TRY_ASSIGN(const uint32_t sceneSeed, JsonReader(*scene->Document).ReadMember<uint32_t>("Seed"));

		const auto [viewWidth, viewHeight] = Utils::GetViewSize(*context.GetWindow());
		PlaySessionSpecification session;
		session.Registry = &context.GetTypeRegistry();
		session.Assets = m_Assets.get();
		session.Mode = PlayMode::Play;
		session.Seed = PlaySession::ComputeSessionSeed(project.Simulation.Seed, sceneSeed);
		session.Project = std::move(project);
		session.ViewWidth = viewWidth;
		session.ViewHeight = viewHeight;
		session.Serial = 1;                       // the Runtime's only session
		session.Audio = context.GetAudioEngine(); // M12
		ENGINE_TRY_ASSIGN(state.Session, PlaySession::Create(session, *scene->Document));
		state.ViewWidth = viewWidth;
		state.ViewHeight = viewHeight;
		if (state.Options.StartPaused)
			state.Session->SetPaused(true);
		ENGINE_INFO("Runtime: '{}' started with scene '{}' ({} entities, seed {}){}", manifest.Name, m_Assets->GetReferencePath(manifest.StartScene),
			state.Session->GetScene().GetEntityCount(), state.Session->GetSeed(), state.Options.StartPaused ? ", paused at tick 0" : "");

		if (context.GetGraphicsDevice() != nullptr)
			ENGINE_TRY(CreateRenderers());

#if !defined(ENGINE_DIST)
		if (state.Options.Automation)
		{
			RuntimeAutomationServerSpecification server;
			server.Listen = true;
			server.Port = state.Options.AutomationPort;
			server.GameName = manifest.Name;
			server.Headless = GetSpecification().Window == WindowMode::Headless;
			server.RendererName = std::string(RendererModeToCommandLine(GetSpecification().Renderer));
			server.SessionsDirectory = GetProcessContext().GetUserDataPaths().Root / "Automation" / "Sessions";
			server.Assets = m_Assets.get();
			server.Audio = context.GetAudioEngine(); // M12: audio.stats
			server.ScenePath = m_Assets->GetReferencePath(manifest.StartScene);
			if (state.Capture != nullptr)
			{
				// The capture and the asset manager belong to this application, which outlives the server (OnShutdown releases
				// the server first). §8.13: screenshots render after the asset manager has published every load requested so far.
				ViewportCapture* capture = state.Capture.get();
				RuntimeAssetManager* assets = m_Assets.get();
				server.View = [capture, assets](const RenderSnapshot& snapshot, const ViewportScreenshotRequest& request)
				{
					assets->WaitIdle();
					return capture->Capture(request, snapshot);
				};
				server.SystemErrors = MakeVulkanSystemErrorHandler(context);
			}
			ENGINE_TRY_ASSIGN(state.Server,
				RuntimeAutomationServer::Create(context.GetTypeRegistry(), context.GetEventLog(), context.GetVfs(), *state.Session, server));
		}
#endif
		return {};
	}

	Status RuntimeApp::CreateRenderers()
	{
		EngineContext& context = GetContext();
		State& state = *m_State;
		GraphicsDevice& device = *context.GetGraphicsDevice();
		// Every pipeline is created here, once (§8.12): the scene renderer's set, shared by the game view and the screenshot
		// capture. The blit pass waits for the frame target's format (OnRender).
		state.GpuCache = CreateScope<GpuResourceCache>(device, *m_Assets);
		ENGINE_TRY_ASSIGN(state.Pipelines,
			Utils::CheckStartupCreation(SceneRendererPipelines::Create(device, *context.GetPipelineFactory()), "the scene renderer's pipelines"));
		ENGINE_TRY_ASSIGN(state.Renderer, Utils::CheckStartupCreation(SceneRenderer::Create(device, *state.Pipelines, *state.GpuCache, *m_Assets, { .Width = state.ViewWidth, .Height = state.ViewHeight }), "the game view's renderer"));
		ENGINE_TRY_ASSIGN(state.Capture,
			Utils::CheckStartupCreation(ViewportCapture::CreateForScenes(device, *state.Pipelines, *state.GpuCache, *m_Assets), "the screenshot capture"));
		state.Mirrors.NoteSceneChange();
		return {};
	}

	void RuntimeApp::OnShutdown()
	{
		State& state = *m_State;
		if (state.Running && state.Options.Screenshot.has_value() && !state.ScreenshotWritten)
		{
			ENGINE_ERROR("The run ended at tick {} before --screenshot-at {} wrote '{}'", state.Session->GetTick(), state.Options.Screenshot->Tick,
				FileSystem::PathToUtf8(state.Options.Screenshot->Path));
		}

		// The server first (its pending operations are cancelled against the live session), then the GPU objects, the
		// session, the GPU cache, and the asset manager the others use, while the context still exists.
#if !defined(ENGINE_DIST)
		state.Server.reset();
#endif
		state.Capture.reset();
		state.Blit.reset();
		state.Renderer.reset();
		state.Pipelines.reset();
		state.Session.reset();
		state.GpuCache.reset();
		EngineContext& context = GetContext();
		if (context.GetAssetManager() == m_Assets.get())
			context.SetAssetManager(nullptr);
		// The manager finishes its loads on the context's job system and main-thread queue, so it goes before them.
		m_Assets.reset();
		m_Loaders.reset();
		state.GamePak.reset();
	}

	void RuntimeApp::OnEvent(Event& event)
	{
		// Every game input goes through the session's tick-stamped queue: real devices reach the next tick (§13.6).
		if (m_State->Session != nullptr)
			m_State->Session->GetInput().QueueDeviceEvent(event);
	}

	void RuntimeApp::OnSafePoint()
	{
		State& state = *m_State;
		// M8 stale mirrors (SceneRenderer.h, "Stale mirrors"; Docs/Decisions/0013-m8-decisions.md decision 7): the previous
		// frame's renders were executed, so the mirrors of replaced asset versions are released; after the first frame the
		// start scene rendered, also everything that frame did not use (StaleMirrorSchedule).
		if (state.GpuCache != nullptr && state.Pipelines != nullptr)
		{
			const bool releaseUnused = state.Mirrors.TakeReleaseUnused();
			state.GpuCache->CollectStale(releaseUnused);
			state.Pipelines->CollectStale(*m_Assets, releaseUnused);
		}
#if !defined(ENGINE_DIST)
		if (state.Server != nullptr)
		{
			state.Server->Pump();
			if (const std::optional<int> exitCode = state.Server->GetShutdownRequest())
				RequestExit(*exitCode);
		}
#endif
		// The session's time scale and the unthrottled frames of a running play.step reach the loop here, after the requests
		// that changed them (§4.2, §13.6).
		if (state.Session != nullptr)
		{
			SetFrameTimeScale(state.Session->GetTimeScale());
			SetFrameThrottleSuspended(state.Session->IsStepping());
		}
	}

	void RuntimeApp::OnFixedStep(const SimStep& /*step*/)
	{
		// A running session advances one tick per loop step (§4.2 step 5); a paused one, or one a client steps in lockstep,
		// does not. The session counts its own ticks.
		if (m_State->Session != nullptr)
			m_State->Session->AdvanceLoopStep();
	}

	void RuntimeApp::OnUpdate(const FrameTime& frame)
	{
		State& state = *m_State;
		if (state.Session == nullptr)
			return;

		// The game view follows the window's framebuffer; a minimized window keeps the last size.
		const Window& window = *GetContext().GetWindow();
		if (!window.IsMinimized())
		{
			const auto [width, height] = Utils::GetViewSize(window);
			if (width != state.ViewWidth || height != state.ViewHeight)
			{
				state.Session->SetViewSize(width, height);
				state.ViewWidth = width;
				state.ViewHeight = height;
			}
		}
		state.Session->AdvanceLoopFrame(frame);

		if (!state.Options.Screenshot.has_value() || state.ScreenshotWritten)
			return;
		if (state.Session->GetTick() >= state.Options.Screenshot->Tick)
		{
			if (!WriteScreenshot())
				RequestExit(ExitCode::Failed);
			return;
		}
		const std::optional<uint64_t> maxFrames = GetSpecification().MaxFrames;
		if (maxFrames.has_value() && frame.FrameIndex + 1 == *maxFrames)
		{
			ENGINE_ERROR("--frames {} ends the run at tick {}, before --screenshot-at {}", *maxFrames, state.Session->GetTick(),
				state.Options.Screenshot->Tick);
			state.ScreenshotWritten = true; // reported here, not again at shutdown
			RequestExit(ExitCode::Failed);
		}
	}

	void RuntimeApp::OnRender(RenderContext& context)
	{
		State& state = *m_State;
		if (state.Renderer == nullptr || state.Session == nullptr || context.Width == 0 || context.Height == 0)
			return;
		GpuProfileScope scope(*context.Profiler, *context.CommandList, "GameView");

		// Render targets are created at startup or resize, and one the device has no memory for is fatal (§8.14 item 7), as
		// in the editor.
		const Status resized = state.Renderer->Resize(context.Width, context.Height);
		if (!resized.has_value())
			FatalError(FatalErrorKind::OutOfMemory, std::format("Cannot resize the game view's targets: {}", resized.error().ToString()));

		// The blit's pipeline is made for the frame target's format, which the first rendered frame tells (the swapchain's
		// surface format); a swapchain recreated with another format gets a new one. Creating it is startup work: a Gpu error
		// is fatal (CheckStartupCreation).
		const nvrhi::FramebufferInfo& target = context.Framebuffer->getFramebufferInfo();
		const nvrhi::Format format = target.colorFormats.empty() ? nvrhi::Format::UNKNOWN : target.colorFormats.front();
		if (state.Blit == nullptr || format != state.BlitFormat)
		{
			state.Blit.reset();
			Result<Scope<BlitPass>> blit = Utils::CheckStartupCreation(BlitPass::Create(*context.Device, *GetContext().GetPipelineFactory(), target),
				"the game view's blit");
			if (!blit.has_value())
			{
				if (!state.RenderFailing)
					ENGINE_ERROR("Cannot render the game view: {}", blit.error().ToString());
				state.RenderFailing = true;
				return;
			}
			state.Blit = std::move(*blit);
			state.BlitFormat = format;
		}

		// The session's last snapshot (its frame phase extracted it at the view size), rendered at the frame's size. A render
		// error names a draw it skipped (a non-finite matrix); the rest of the view rendered and is shown.
		const Status rendered = state.Renderer->Render(*context.CommandList, state.Session->GetLastExtraction());
		state.Mirrors.NoteRendered();
		const Status blitted = state.Blit->Record(*context.CommandList, *state.Renderer->GetFinalTexture(), *context.Framebuffer);
		// A binding set the device has no memory for is fatal like any GPU object (§8.14 item 7); anything else is logged
		// once per run of failing frames.
		if (!blitted.has_value() && blitted.error().GetCode() == ErrorCode::Gpu)
			FatalError(FatalErrorKind::OutOfMemory, std::format("Cannot present the game view: {}", blitted.error().ToString()));
		const Status& drawn = blitted.has_value() ? rendered : blitted;
		if (!drawn.has_value() && !state.RenderFailing)
			ENGINE_ERROR("Cannot render the game view: {}", drawn.error().ToString());
		state.RenderFailing = !drawn.has_value();
	}

	bool RuntimeApp::WriteScreenshot()
	{
		State& state = *m_State;
		const RuntimeOptions::ScreenshotAt& screenshot = *state.Options.Screenshot;
		state.ScreenshotWritten = true;
		const std::string path = FileSystem::PathToUtf8(screenshot.Path);
		if (state.Capture == nullptr)
		{
			ENGINE_ERROR("Cannot write the screenshot '{}': the game renders nothing with --renderer none", path);
			return false;
		}

		// The session's view as it is now, at the window's framebuffer size (§8.13: re-rendered, never the presented image,
		// after the asset manager has published every load requested so far).
		m_Assets->WaitIdle();
		const auto [width, height] = Utils::GetViewSize(*GetContext().GetWindow());
		RenderExtractionRequest request;
		request.Camera = RenderCameraSource::Primary;
		request.Width = width;
		request.Height = height;
		Result<RenderSnapshot> snapshot = state.Session->ExtractView(request);
		Result<Image> image = snapshot.has_value() ? state.Capture->Capture({ .Width = width, .Height = height, .MaxDimension = 0 }, *snapshot)
												   : Result<Image>(std::unexpected(snapshot.error()));
		Status written = image.has_value() ? FileSystem::CreateDirectories(screenshot.Path.parent_path()) : Status(std::unexpected(image.error()));
		if (written.has_value())
			written = WritePng(screenshot.Path, *image);
		if (!written.has_value())
		{
			ENGINE_ERROR("Cannot write the screenshot of tick {} to '{}': {}", state.Session->GetTick(), path, written.error().ToString());
			return false;
		}
		ENGINE_INFO("Screenshot of tick {} ({}x{}) written to '{}'", state.Session->GetTick(), width, height, path);
		return true;
	}

	Result<Scope<Application>> CreateRuntimeApp(std::span<const std::string> arguments)
	{
		const std::span<const CommandLineOption> engineOptions = GetEngineCommandLineOptions();
		const std::span<const CommandLineOption> runtimeOptions = GetRuntimeCommandLineOptions();
		std::vector<CommandLineOption> options(engineOptions.begin(), engineOptions.end());
		options.insert(options.end(), runtimeOptions.begin(), runtimeOptions.end());
		ENGINE_TRY_ASSIGN(CommandLine commandLine, CommandLine::Parse(arguments, options));
		if (!commandLine.GetPositional().empty())
		{
			return MakeError(ErrorCode::InvalidArgument, "unexpected argument '{}': the runtime takes no positional arguments",
				commandLine.GetPositional().front());
		}

		ApplicationSpecification specification;
		ENGINE_TRY(ApplyEngineCommandLine(commandLine, specification));
		ENGINE_TRY_ASSIGN(const std::filesystem::path executable, Process::GetCurrentExecutablePath());
		ENGINE_TRY_ASSIGN(RuntimeOptions runtime, ParseRuntimeOptions(commandLine, executable));
		if (runtime.Screenshot.has_value() && specification.Renderer == RendererMode::None)
			return MakeError(ErrorCode::InvalidArgument, "option '--screenshot-at' needs a renderer: it cannot be combined with '--renderer none'");

		// The manifest names the application before anything else exists: logs, crash reports and user:// go to
		// <UserData>/<Name>/ (§4.4, §14.3). Its errors are InitFailed (3).
		Result<GameManifest> manifest = GameManifestSerializer::LoadFromFile(runtime.ManifestPath);
		if (!manifest)
		{
			const bool missing = manifest.error().GetCode() == ErrorCode::NotFound;
			return std::unexpected(std::move(manifest).error().WithHint(missing ? "run the game from its exported folder, next to its Game.json"
																				: "export the game again"));
		}

		specification.Name = manifest->Name;
		specification.WindowSettings = WindowSpecification{
			.Title = manifest->Window.Title,
			.Width = manifest->Window.Width,
			.Height = manifest->Window.Height,
			.Resizable = manifest->Window.Resizable,
			.Fullscreen = manifest->Window.Fullscreen,
		};
		specification.Graphics.VSync = manifest->Window.VSync;
		specification.Loop.FixedHz = manifest->Simulation.FixedHz;
		specification.Loop.MaxStepsPerFrame = manifest->Simulation.MaxStepsPerFrame;
		specification.EnginePak = runtime.ManifestPath.parent_path() / FileSystem::PathFromUtf8(manifest->Paks[0].Path);
#if !defined(ENGINE_DIST)
		specification.RegisterTypes = &Utils::RegisterRuntimeAutomationTypes;
#endif
		return CreateScope<RuntimeApp>(std::move(specification), std::move(runtime), std::move(*manifest));
	}

}
