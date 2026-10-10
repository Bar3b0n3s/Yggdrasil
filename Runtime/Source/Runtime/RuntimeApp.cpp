#include "Runtime/RuntimeApp.h"

#include "Engine/App/CommandLine.h"
#include "Engine/App/EngineContext.h"
#include "Engine/App/ExitCode.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Asset/Asset.h"
#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Asset/PakMount.h"
#include "Engine/Asset/PakReader.h"
#include "Engine/Asset/ReplayData.h"
#include "Engine/Asset/RuntimeAssetManager.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
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
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Session/PlaySession.h"
#include "Engine/Session/ReplayPlayer.h"
#include "Engine/Testing/FeatureTestRunner.h"

#if !defined(ENGINE_DIST)
	#include "Engine/App/VulkanErrorHandler.h"
	#include "Engine/Automation/Methods/AutomationTypes.h"
	#include "Engine/Automation/Methods/RegisterSharedMethods.h"
	#include "Engine/Automation/Methods/RuntimeAutomationServer.h"
	#include "Engine/Automation/Methods/StatsMethods.h"
	#include "Engine/Scripting/ScriptCompiler.h"
#endif

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <format>
#include <limits>
#include <map>
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
		constexpr std::string_view ReplayOption = "--replay";
		constexpr std::string_view VerifyOption = "--verify";
#endif
		constexpr std::string_view FeatureTestOption = "--feature-test";
		constexpr std::string_view FilterOption = "--filter";

		constexpr std::array RuntimeCommandLineOptions = {
			CommandLineOption{ .Name = FeatureTestOption, .Value = CommandLineValue::None, .Description = "Run the exported testing suites and cooked replays, then exit." },
			CommandLineOption{ .Name = FilterOption, .Value = CommandLineValue::Required, .ValueName = "substring", .Description = "Select feature test suite/case names and replay paths." },
#if !defined(ENGINE_DIST)
			CommandLineOption{ .Name = ManifestOption, .Value = CommandLineValue::Required, .ValueName = "path", .Description = "Run the game of this Game.json instead of the one next to the executable." },
			CommandLineOption{ .Name = ScreenshotAtOption, .Value = CommandLineValue::Required, .ValueName = "T:path", .Description = "Write the game view as a PNG after the frame in which tick T is reached." },
			CommandLineOption{ .Name = AutomationOption, .Value = CommandLineValue::Optional, .ValueName = "port", .Description = "Serve the Runtime's automation subset (127.0.0.1)." },
			CommandLineOption{ .Name = PausedOption, .Value = CommandLineValue::None, .ValueName = {}, .Description = "Start the play session paused at tick 0 (needs --automation)." },
			CommandLineOption{ .Name = ReplayOption, .Value = CommandLineValue::Required, .ValueName = "path", .Description = "Run a replay from its recorded initial scene, parameters and seed, then exit." },
			CommandLineOption{ .Name = VerifyOption, .Value = CommandLineValue::None, .Description = "Verify replay expectations and its final state hash." },
#endif
		};

		// The types of the Runtime's automation subset (RuntimeAutomationServer.h), registered into the context's registry
		// before it freezes. Dist retains only the feature-test result schema.
		static void RegisterRuntimeTypes(TypeRegistry& registry)
		{
#if !defined(ENGINE_DIST)
			RegisterAutomationSharedTypes(registry);
			RegisterSharedMethodTypes(registry);
#endif
			RegisterTestResultTypes(registry);
		}

#if !defined(ENGINE_DIST)
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

	namespace {

#if defined(ENGINE_DIST)
		constexpr TestSuiteSettings::Mode RuntimeTestMode = TestSuiteSettings::Mode::Dist;
#else
		constexpr TestSuiteSettings::Mode RuntimeTestMode = TestSuiteSettings::Mode::Release;
#endif
		constexpr RunModes RuntimeScriptMode = RuntimeTestMode == TestSuiteSettings::Mode::Dist ? RunModes::Dist : RunModes::Release;

		Result<VfsPath> ProjectAssetPath(std::string_view text)
		{
			ENGINE_TRY_ASSIGN(auto path, text.find("://") == std::string_view::npos ? VfsPath::Create("project", text) : VfsPath::Parse(text));
			if (path.GetScheme() != "project" || !path.GetPath().starts_with("Assets/"))
				return MakeError(ErrorCode::Validation, "asset paths must be confined below project://Assets/");
			return path;
		}

		bool MatchesGlob(std::string_view pattern, std::string_view text)
		{
			// The export vocabulary: '*' and '?' stay within a path segment; '**' crosses separators and
			// '**/' also matches no directory. Dynamic programming avoids recursive/exponential backtracking.
			const size_t width = text.size() + 1;
			std::vector<uint8_t> matches((pattern.size() + 1) * width, 0);
			matches[pattern.size() * width + text.size()] = 1;
			for (size_t p = pattern.size(); p-- > 0;)
			{
				for (size_t t = width; t-- > 0;)
				{
					bool matched = false;
					if (pattern[p] == '*')
					{
						const bool recursive = p + 1 < pattern.size() && pattern[p + 1] == '*';
						const size_t next = p + (recursive ? 2 : 1);
						matched = matches[next * width + t] != 0;
						if (recursive && next < pattern.size() && pattern[next] == '/')
							matched |= matches[(next + 1) * width + t] != 0;
						if (t < text.size() && (recursive || text[t] != '/'))
							matched |= matches[p * width + t + 1] != 0;
					}
					else if (t < text.size() && (pattern[p] == text[t] || (pattern[p] == '?' && text[t] != '/')))
						matched = matches[(p + 1) * width + t + 1] != 0;
					matches[p * width + t] = matched ? uint8_t{ 1 } : uint8_t{ 0 };
				}
			}
			return matches.front() != 0;
		}

	}

	struct RuntimeApp::State final : IPlaySessionHost, IFeatureTestHost
	{
		explicit State(RuntimeApp& app)
			: App(app)
		{
		}
		RuntimeApp& App; // Borrowed owning application, outliving all sessions and the runner.
		RuntimeOptions Options{};
		GameManifest Manifest{};
		Ref<const PakReader> GamePak;
		Scope<PlaySession> Session;
		PlaySession* TestSession = nullptr; // Borrowed from Runner, unpublished before destruction.
		ProjectSettings Project{};
		Ref<const ScriptFieldSchemaSource> Schemas{};
		ScriptErrorStream Errors{};
		std::map<uint64_t, ReplayHeader> Headers{};
		uint64_t NextSerial = 1;
		uint64_t ReplayInputSerial = 0;
		uint64_t RenderedSerial = 0;
		uint64_t RenderedGeneration = 0;
		FeatureTestRunner Runner{};
		ReplayPlayer Player{};
		ScriptApiCoverage CoverageStart{};
		bool TestsComplete = false;
		bool ReplayComplete = false;
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
		std::optional<uint64_t> PendingRenderFrame{};
		// M8 stale mirrors (SceneRenderer.h): when the collections also release what the start scene's first frame did not
		// use (CreateRenderers notes the start scene as the shown scene's change).
		StaleMirrorSchedule Mirrors{};
		bool Running = false; // OnInitialize succeeded: the frames ran
		bool ScreenshotWritten = false;
#if !defined(ENGINE_DIST)
		Scope<RuntimeAutomationServer> Server;
#endif
		[[nodiscard]] PlaySession* ActiveSession() const { return Options.FeatureTest ? TestSession : Session.get(); }
		[[nodiscard]] ScriptEnvironment GetScriptEnvironment() const override;
		[[nodiscard]] Status SetScriptCursorMode(CursorMode mode) override;
		[[nodiscard]] CursorMode GetScriptCursorMode() const override;
		void OnScriptError(const ScriptError& error, bool fatal) override;
		[[nodiscard]] const ProjectSettings& GetProjectSettings() const override { return Project; }
		[[nodiscard]] Result<AssetHandle> ResolveTestScript(std::string_view path) override;
		[[nodiscard]] Result<std::vector<std::string>> ExpandReplayPaths(std::span<const std::string> globs) override;
		[[nodiscard]] Result<AssetRef<ReplayData>> LoadReplay(std::string_view path) override;
		[[nodiscard]] Result<ReplayHeader> DescribeReplayHeader(const PlaySession& session) const override;
		[[nodiscard]] Result<Scope<PlaySession>> CreateSuiteSession(const TestSuiteSettings& suite, TestSuiteSettings::Mode mode,
			IPlaySessionTestHook& hook, IScriptTestHost& testHost) override;
		[[nodiscard]] Result<Scope<PlaySession>> CreateReplaySession(const ReplayHeader& header, TestSuiteSettings::Mode mode) override;
		void SetActiveTestSession(PlaySession* session) override
		{
			TestSession = session;
			Mirrors.NoteSceneChange();
		}
		[[nodiscard]] AudioEngine* GetAudioEngine() const override { return App.GetContext().GetAudioEngine(); }
		[[nodiscard]] Status CaptureScreenshot(PlaySession& session, std::string_view name) override;
		[[nodiscard]] Status ReloadScript(PlaySession&, AssetHandle) override { return MakeError(ErrorCode::Unsupported, "exported scripts cannot be reloaded"); }
		[[nodiscard]] Result<TestCoverageReport> GetCoverage() const override;
		[[nodiscard]] Result<Json> Evaluate(PlaySession& session, const ReplayBytecodeExpectation& expectation) override;
		[[nodiscard]] Result<Scope<PlaySession>> PrepareSession(AssetHandle scene, ProjectSettings project, const PlayStartOptions& options,
			bool ownsAudio = false, IPlaySessionTestHook* hook = nullptr, IScriptTestHost* testHost = nullptr, bool deferActivation = false);
		[[nodiscard]] Result<PlaySession*> Restart(const ReplayHeader& header, const PlayStartOptions& options, bool replay);
		[[nodiscard]] Result<std::string> ValidateReplayOutput(std::string_view path) const;
		[[nodiscard]] Result<std::string> WriteReplay(std::string_view path, const ReplayDocument& document);
		void ReleaseReplayInput(uint64_t serial)
		{
			if (ReplayInputSerial == serial)
				ReplayInputSerial = 0;
		}
		[[nodiscard]] Status WriteTestResults();
	};

	ScriptEnvironment RuntimeApp::State::GetScriptEnvironment() const
	{
		const Window& window = *App.GetContext().GetWindow();
		ScriptEnvironment environment;
		environment.IsHeadless = App.GetSpecification().Window == WindowMode::Headless;
		environment.IsFocused = window.IsFocused();
		environment.Version = Manifest.EngineVersion;
		environment.WindowSize = { static_cast<float>(window.GetWidth()), static_cast<float>(window.GetHeight()) };
#if defined(ENGINE_PLATFORM_WINDOWS)
		environment.Platform = "Windows";
#elif defined(ENGINE_PLATFORM_LINUX)
		environment.Platform = "Linux";
#else
		environment.Platform = "macOS";
#endif
		return environment;
	}

	Status RuntimeApp::State::SetScriptCursorMode(CursorMode mode)
	{
		App.GetContext().GetWindow()->SetCursorMode(mode);
		return {};
	}
	CursorMode RuntimeApp::State::GetScriptCursorMode() const
	{
		return App.GetContext().GetWindow()->GetCursorMode();
	}
	void RuntimeApp::State::OnScriptError(const ScriptError& error, bool /*fatal*/)
	{
		static_cast<void>(Errors.Add(error));
		App.GetContext().GetEventLog().Append({ .Seq = 0, .Tick = error.Tick, .Type = EngineEventType::ScriptErrorRaised, .Id = error.Entity, .Path = error.Script, .Name = {}, .Message = error.Message, .Dirty = false });
	}

	Result<AssetHandle> RuntimeApp::State::ResolveTestScript(std::string_view path)
	{
		ENGINE_TRY_ASSIGN(auto canonical, ProjectAssetPath(path));
		const auto handle = App.m_Assets->Resolve(canonical.GetPath());
		if (!handle)
			return MakeError(ErrorCode::NotFound, "test script '{}' is missing from the game pak", path);
		ENGINE_TRY_ASSIGN(auto asset, App.m_Assets->Load(*handle));
		const auto script = AssetCast<ScriptData>(asset);
		if (!script || script->Kind != ScriptKind::TestSuite)
			return MakeError(ErrorCode::Validation, "'{}' is not a cooked test suite", path);
		return *handle;
	}

	Result<std::vector<std::string>> RuntimeApp::State::ExpandReplayPaths(std::span<const std::string> globs)
	{
		std::vector<std::string> result;
		for (const auto& glob : globs)
		{
			std::string checked = glob;
			std::replace(checked.begin(), checked.end(), '*', 'x');
			std::replace(checked.begin(), checked.end(), '?', 'x');
			ENGINE_TRY(ProjectAssetPath(checked));
			std::string_view pattern = glob;
			if (pattern.starts_with("project://"))
				pattern.remove_prefix(10);
			bool matched = false;
			for (const auto& entry : GamePak->GetEntries())
				if (entry.Type == "Replay" && MatchesGlob(pattern, entry.Path))
				{
					matched = true;
					result.push_back(entry.Path);
				}
			if (!matched && glob.find_first_of("*?") == std::string::npos)
				return MakeError(ErrorCode::NotFound, "replay '{}' is missing from the game pak", glob);
		}
		std::ranges::sort(result);
		result.erase(std::unique(result.begin(), result.end()), result.end());
		return result;
	}

	Result<AssetRef<ReplayData>> RuntimeApp::State::LoadReplay(std::string_view path)
	{
		if (path.starts_with("user://"))
		{
#if defined(ENGINE_DIST)
			return MakeError(ErrorCode::Unsupported, "Dist replay verification requires a cooked replay in the game pak");
#else
			ENGINE_TRY_ASSIGN(auto canonical, VfsPath::Parse(path));
			if (!canonical.GetPath().starts_with("Replays/") || canonical.GetExtension() != ".replay")
				return MakeError(ErrorCode::Validation, "user recordings must be below user://Replays/ with a .replay extension");
			ENGINE_TRY_ASSIGN(auto text, App.GetContext().GetVfs().ReadText(canonical));
			ReplayLoadReport report;
			ENGINE_TRY_ASSIGN(auto document, ReplayFromText(text, report, true));
			auto replay = CreateRef<ReplayData>();
			replay->Header = document.Header;
			replay->Events = document.Events;
			replay->FinalTick = document.FinalTick;
			replay->FinalStateHash = document.FinalStateHash;
			for (size_t i = 0; i < document.Expect.size(); ++i)
			{
				const auto& source = document.Expect[i];
				const std::string label = "=" + canonical.ToString();
				const std::string pointer = std::format("/Expect/{}/Luau", i);
				ENGINE_TRY_ASSIGN(auto compiled, ScriptCompiler::Compile({ .Source = source.Luau, .Mode = ScriptCompileMode::ExpressionOrChunk, .ChunkName = label, .JsonPointer = pointer }));
				ReplayBytecodeExpectation expectation;
				expectation.Tick = source.Tick;
				expectation.Script.Bytecode = std::move(compiled.Bytecode);
				expectation.Script.SourceMap = std::move(compiled.SourceMap);
				replay->Expect.push_back(std::move(expectation));
			}
			return AssetRef<ReplayData>(std::move(replay));
#endif
		}
		ENGINE_TRY_ASSIGN(auto canonical, ProjectAssetPath(path));
		const auto handle = App.m_Assets->Resolve(canonical.GetPath());
		if (!handle)
			return MakeError(ErrorCode::NotFound, "replay '{}' is missing from the game pak", path);
		ENGINE_TRY_ASSIGN(auto asset, App.m_Assets->Load(*handle));
		auto replay = AssetCast<ReplayData>(asset);
		if (!replay)
			return MakeError(ErrorCode::Validation, "'{}' is not a cooked Replay asset", path);
		return replay;
	}

	Result<ReplayHeader> RuntimeApp::State::DescribeReplayHeader(const PlaySession& session) const
	{
		const auto found = Headers.find(session.GetSerial());
		if (found == Headers.end() || !found->second.Scene.Handle.IsValid())
			return MakeError(ErrorCode::InvalidState, "recording requires a reproducible scene asset");
		return found->second;
	}

	Result<Scope<PlaySession>> RuntimeApp::State::PrepareSession(AssetHandle scene, ProjectSettings project, const PlayStartOptions& options,
		bool ownsAudio, IPlaySessionTestHook* hook, IScriptTestHost* testHost, bool deferActivation)
	{
		Json document;
		if (scene.IsValid())
		{
			ENGINE_TRY_ASSIGN(auto asset, App.m_Assets->Load(scene));
			const auto data = AssetCast<SceneData>(asset);
			if (!data || !data->Document)
				return MakeError(ErrorCode::Validation, "session start asset is not a Scene");
			document = *data->Document;
		}
		else
		{
			auto ids = UUIDGenerator::CreateDeterministic(0);
			auto empty = Scene::Create({ .Name = "Test", .Registry = &App.GetContext().GetTypeRegistry(), .IdGenerator = &ids });
			ENGINE_TRY_ASSIGN(document, SceneSerializer::ToJson(*empty));
		}
		ENGINE_TRY_ASSIGN(const uint32_t sceneSeed, JsonReader(document).ReadMember<uint32_t>("Seed"));
		if (options.PauseOnError)
			project.Scripting.PauseOnError = *options.PauseOnError;
		PlaySessionSpecification spec;
		spec.Registry = &App.GetContext().GetTypeRegistry();
		spec.Assets = App.m_Assets.get();
		spec.ScriptApi = &App.GetContext().GetScriptApiRegistry();
		spec.ScriptSchemas = Schemas;
		spec.Project = std::move(project);
		spec.Serial = NextSerial++;
		spec.Seed = options.Seed.value_or(PlaySession::ComputeSessionSeed(spec.Project.Simulation.Seed, sceneSeed));
		spec.Parameters = options.Parameters;
		spec.ViewWidth = ViewWidth;
		spec.ViewHeight = ViewHeight;
		spec.Audio = GetAudioEngine();
		spec.OwnsAudioTime = ownsAudio;
		spec.Host = this;
		spec.Environment = GetScriptEnvironment();
		spec.ScriptRunMode = RuntimeScriptMode;
		spec.TestMode = testHost != nullptr;
		spec.TestHook = hook;
		spec.TestHost = testHost;
		ENGINE_TRY_ASSIGN(auto session, PlaySession::Prepare(spec, document));
		// Runtime starts at the session's default scale; OnCreate/OnStart may change it. Recording and replay
		// options do not expose a time-scale override, and applying their default here would erase that gameplay write.
		session->SetLockstep(options.Lockstep, options.LockstepOwner);
		session->SetPaused(options.Paused || session->IsPaused());
		if (!deferActivation)
			session->Activate();
		ReplayHeader header;
		header.Scene = { scene, scene.IsValid() ? App.m_Assets->GetReferencePath(scene) : "" };
		header.Parameters.Set(session->GetLoadParameters());
		header.Seed = spec.Seed;
		header.FixedHz = spec.Project.Simulation.FixedHz;
		header.EngineVersion = Manifest.EngineVersion;
#if defined(ENGINE_DIST)
		header.Config = "Dist";
#elif defined(ENGINE_DEBUG)
		header.Config = "Debug";
#else
		header.Config = "Release";
#endif
		Headers[session->GetSerial()] = std::move(header);
		return session;
	}

	Result<Scope<PlaySession>> RuntimeApp::State::CreateSuiteSession(const TestSuiteSettings& suite, TestSuiteSettings::Mode mode,
		IPlaySessionTestHook& hook, IScriptTestHost& testHost)
	{
		if (mode != RuntimeTestMode)
			return MakeError(ErrorCode::InvalidArgument, "test mode does not match this Runtime");
		AssetHandle scene;
		if (!suite.Scene.empty())
		{
			ENGINE_TRY_ASSIGN(auto path, ProjectAssetPath(suite.Scene));
			const auto found = App.m_Assets->Resolve(path.GetPath());
			if (!found)
				return MakeError(ErrorCode::NotFound, "suite scene '{}' is missing", suite.Scene);
			scene = *found;
		}
		ProjectSettings project = Project;
		if (suite.Overrides.CallbackBudgetMs)
			project.Scripting.CallbackBudgetMs = suite.Overrides.CallbackBudgetMs;
		if (suite.Overrides.MemoryLimitMB)
			project.Scripting.MemoryLimitMB = suite.Overrides.MemoryLimitMB;
		if (suite.Overrides.PauseOnError != TestSuiteOverrides::PauseOnErrorOverride::Inherit)
			project.Scripting.PauseOnError = suite.Overrides.PauseOnError == TestSuiteOverrides::PauseOnErrorOverride::Pause;
		PlayStartOptions options;
		options.Parameters = suite.Parameters;
		options.Lockstep = suite.Clock.empty();
		return PrepareSession(scene, std::move(project), options, true, &hook, &testHost);
	}

	Result<Scope<PlaySession>> RuntimeApp::State::CreateReplaySession(const ReplayHeader& header, TestSuiteSettings::Mode mode)
	{
		if (mode != RuntimeTestMode)
			return MakeError(ErrorCode::InvalidArgument, "replay mode does not match this Runtime");
		ProjectSettings project = Project;
		project.Simulation.FixedHz = header.FixedHz;
		PlayStartOptions options;
		options.Seed = header.Seed;
		options.Parameters = header.Parameters;
		options.Lockstep = true;
		return PrepareSession(header.Scene.Handle, std::move(project), options, true);
	}

	Result<Json> RuntimeApp::State::Evaluate(PlaySession& session, const ReplayBytecodeExpectation& expectation)
	{
		if (!session.GetScripts())
			return MakeError(ErrorCode::InvalidState, "replay session has no script VM");
		ENGINE_TRY_ASSIGN(auto result, session.GetScripts()->ExecuteBytecode(expectation.Script));
		return result.Value.Get();
	}

	Result<PlaySession*> RuntimeApp::State::Restart(const ReplayHeader& header, const PlayStartOptions& options, bool replay)
	{
		// Preparation has no callbacks or audio effects. Failure preserves the old session, including its run state.
		ProjectSettings project = Project;
		project.Simulation.FixedHz = header.FixedHz;
		ENGINE_TRY_ASSIGN(auto prepared, PrepareSession(header.Scene.Handle, std::move(project), options, false, nullptr, nullptr, true));
		Session.reset();
		Session = std::move(prepared);
		Session->Activate();
		ReplayInputSerial = replay ? Session->GetSerial() : 0;
		Mirrors.NoteSceneChange();
		return Session.get();
	}

	Result<std::string> RuntimeApp::State::ValidateReplayOutput(std::string_view path) const
	{
		// Writes accept a relative destination, reads also accept the returned canonical identity (ADR 0019).
		ENGINE_TRY_ASSIGN(auto relative, VfsPath::Create("user", path));
		if (relative.GetExtension() != ".replay")
			return MakeError(ErrorCode::Validation, "recording output needs a .replay extension");
		ENGINE_TRY_ASSIGN(auto canonical, VfsPath::Create("user", "Replays/" + std::string(relative.GetPath())));
		const auto& vfs = App.GetContext().GetVfs();
		if (!vfs.IsMounted("user"))
			return MakeError(ErrorCode::PermissionDenied, "the Runtime user-data mount is unavailable");
		// Check existing components without creating directories or consuming a recorder. The VFS applies its case
		// policy and reports inaccessible ancestors. The writer repeats this before its atomic replacement.
		for (VfsPath current = canonical; !current.IsRoot(); current = current.GetParent())
		{
			auto info = vfs.GetInfo(current);
			if (!info && info.error().GetCode() != ErrorCode::NotFound)
				return std::unexpected(info.error());
			if (info && (current == canonical ? info->IsDirectory : !info->IsDirectory))
				return MakeError(ErrorCode::Validation, "recording output '{}' conflicts with an existing file or directory", current.ToString());
		}
		return canonical.ToString();
	}

	Result<std::string> RuntimeApp::State::WriteReplay(std::string_view path, const ReplayDocument& document)
	{
		ENGINE_TRY_ASSIGN(auto canonical, ValidateReplayOutput(path));
		ENGINE_TRY_ASSIGN(auto file, VfsPath::Parse(canonical));
		ENGINE_TRY_ASSIGN(auto text, ReplayToText(document));
		auto& vfs = App.GetContext().GetVfs();
		ENGINE_TRY(vfs.CreateDirectories(file.GetParent()));
		ENGINE_TRY(vfs.WriteFileAtomic(file, std::as_bytes(std::span(text.data(), text.size()))));
		return canonical;
	}

	Status RuntimeApp::State::CaptureScreenshot(PlaySession& session, std::string_view name)
	{
		ENGINE_TRY_ASSIGN(auto file, VfsPath::Create("user", "TestResults/" + std::string(name) + ".png"));
		if (!Capture)
			return MakeError(ErrorCode::Unsupported, "test screenshots require a renderer");
		App.m_Assets->WaitIdle();
		RenderExtractionRequest request;
		request.Width = ViewWidth;
		request.Height = ViewHeight;
		ENGINE_TRY_ASSIGN(auto snapshot, session.ExtractView(request));
		ENGINE_TRY_ASSIGN(auto image, Capture->Capture({ .Width = ViewWidth, .Height = ViewHeight, .MaxDimension = 0 }, snapshot));
		ENGINE_TRY_ASSIGN(auto png, EncodePng(image));
		auto& vfs = App.GetContext().GetVfs();
		ENGINE_TRY(vfs.CreateDirectories(file.GetParent()));
		return vfs.WriteFileAtomic(file, png);
	}

	Result<TestCoverageReport> RuntimeApp::State::GetCoverage() const
	{
		const auto& api = App.GetContext().GetScriptApiRegistry();
		ENGINE_TRY_ASSIGN(auto coverage, api.GetCoverage(RuntimeScriptMode));
		ENGINE_TRY_ASSIGN(auto definitions, api.GenerateDefinitions());
		TestCoverageReport report;
		report.RegistryFingerprint = std::format("{:016x}", XXH64(definitions));
		for (auto counter : coverage.Members)
		{
			const auto previous = std::ranges::find_if(CoverageStart.Members, [&counter](const auto& old)
			{
				return old.Owner == counter.Owner && old.Member == counter.Member && old.Kind == counter.Kind;
			});
			if (previous != CoverageStart.Members.end())
			{
				counter.Calls -= std::min(counter.Calls, previous->Calls);
				counter.Reads -= std::min(counter.Reads, previous->Reads);
				counter.Writes -= std::min(counter.Writes, previous->Writes);
				for (auto& value : counter.EnumValues)
				{
					const auto old = std::ranges::find_if(previous->EnumValues, [&value](const auto& item)
					{
						return item.ArgumentIndex == value.ArgumentIndex && item.EnumName == value.EnumName && item.ValueName == value.ValueName;
					});
					if (old != previous->EnumValues.end())
						value.Count -= std::min(value.Count, old->Count);
				}
			}
			std::vector<TestSuiteSettings::Mode> modes;
			if (HasFlag(counter.Modes, RunModes::Editor))
				modes.push_back(TestSuiteSettings::Mode::Editor);
			if (HasFlag(counter.Modes, RunModes::Release))
				modes.push_back(TestSuiteSettings::Mode::Release);
			if (HasFlag(counter.Modes, RunModes::Dist))
				modes.push_back(TestSuiteSettings::Mode::Dist);
			const auto count = [](uint64_t value)
			{
				return static_cast<uint32_t>(std::min<uint64_t>(value, std::numeric_limits<uint32_t>::max()));
			};
			const std::string id = counter.Owner + "." + counter.Member;
			if (counter.RequiresRead)
				report.Counters.push_back({ .Id = id + "/read", .Kind = "read", .Modes = modes, .Count = count(counter.Reads) });
			if (counter.RequiresWrite)
				report.Counters.push_back({ .Id = id + "/write", .Kind = "write", .Modes = modes, .Count = count(counter.Writes) });
			if (!counter.RequiresRead && !counter.RequiresWrite && counter.Kind != ScriptApiMemberKind::Constant)
				report.Counters.push_back({ .Id = id, .Kind = counter.Kind == ScriptApiMemberKind::Callback ? "callback" : "call", .Modes = modes, .Count = count(counter.Calls) });
			for (const auto& value : counter.EnumValues)
				report.Counters.push_back({ .Id = std::format("{}/argument{}/{}/{}", id, value.ArgumentIndex, value.EnumName, value.ValueName), .Kind = "enum", .Modes = modes, .Count = count(value.Count) });
		}
		std::ranges::sort(report.Counters, {}, &TestCoverageCounter::Id);
		return report;
	}

	Status RuntimeApp::State::WriteTestResults()
	{
		ENGINE_TRY_ASSIGN(auto report, Runner.GetResult());
		const auto directory = std::filesystem::path("bin") / "TestResults";
		const std::string name = std::format("Runtime-{}", Process::GetCurrentId());
		const auto jsonPath = directory / (name + ".json");
		const auto junitPath = directory / (name + ".xml");
		report.JsonPath = FileSystem::PathToUtf8(jsonPath);
		report.JunitPath = FileSystem::PathToUtf8(junitPath);
		ENGINE_TRY_ASSIGN(auto json, TestRunResultToJson(report, App.GetContext().GetTypeRegistry()));
		ENGINE_TRY_ASSIGN(auto text, JsonWriter::Write(json));
		ENGINE_TRY_ASSIGN(auto junit, TestRunResultToJUnit(report));
		ENGINE_TRY(FileSystem::CreateDirectories(directory));
		ENGINE_TRY(FileSystem::WriteFileAtomic(jsonPath, std::as_bytes(std::span(text.data(), text.size()))));
		ENGINE_TRY(FileSystem::WriteFileAtomic(junitPath, std::as_bytes(std::span(junit.data(), junit.size()))));
		ENGINE_INFO("Feature tests {}: {} cases; results '{}'", report.Passed ? "passed" : "failed", report.Cases.size(), report.JsonPath);
		return {};
	}

	std::span<const CommandLineOption> GetRuntimeCommandLineOptions()
	{
		return Utils::RuntimeCommandLineOptions;
	}

	Result<RuntimeOptions> ParseRuntimeOptions(const CommandLine& commandLine, const std::filesystem::path& executablePath)
	{
		RuntimeOptions options;
		options.ManifestPath = Utils::GetDefaultManifestPath(executablePath);
		options.FeatureTest = commandLine.Has(Utils::FeatureTestOption);
		if (auto filter = commandLine.GetValue(Utils::FilterOption))
		{
			if (!options.FeatureTest)
				return MakeError(ErrorCode::InvalidArgument, "--filter requires --feature-test");
			options.Filter = *filter;
		}
#if !defined(ENGINE_DIST)
		if (auto replay = commandLine.GetValue(Utils::ReplayOption))
		{
			if (replay->empty())
				return MakeError(ErrorCode::InvalidArgument, "--replay requires a path");
			options.Replay = *replay;
		}
		options.VerifyReplay = commandLine.Has(Utils::VerifyOption);
		if (options.VerifyReplay && options.Replay.empty())
			return MakeError(ErrorCode::InvalidArgument, "--verify requires --replay");
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
#endif
		if (options.FeatureTest && (!options.Replay.empty() || options.Automation || options.Screenshot))
			return MakeError(ErrorCode::InvalidArgument, "--feature-test owns the run and cannot be combined with --replay, --automation or --screenshot-at");
		if (!options.Replay.empty() && (options.Automation || options.Screenshot))
			return MakeError(ErrorCode::InvalidArgument, "--replay owns the run and cannot be combined with --automation or --screenshot-at");
		return options;
	}

	RuntimeApp::RuntimeApp(ApplicationSpecification specification, RuntimeOptions options, GameManifest manifest)
		: Application(std::move(specification)), m_State(CreateScope<State>(*this))
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

		state.Project = std::move(project);
		// Strict scene and prefab loading need every cooked script's field schema, including assets reached later
		// through Scene.Load or Instantiate. Pin one immutable snapshot for the entire exported run.
		std::map<AssetHandle, AssetRef<ScriptData>> scripts;
		for (const auto& pak : { enginePak, state.GamePak })
			for (const auto& entry : pak->GetEntries())
				if (entry.Type == "Script")
				{
					ENGINE_TRY_ASSIGN(auto asset, m_Assets->Load(entry.Handle));
					auto script = AssetCast<ScriptData>(asset);
					if (!script)
						return MakeError(ErrorCode::Validation, "pak Script '{}' decoded with the wrong type", entry.Path);
					scripts.emplace(entry.Handle, std::move(script));
				}
		ENGINE_TRY_ASSIGN(state.Schemas, ScriptFieldSchemaSource::Create(std::move(scripts)));
		const auto [viewWidth, viewHeight] = Utils::GetViewSize(*context.GetWindow());
		state.ViewWidth = viewWidth;
		state.ViewHeight = viewHeight;
		if (!state.Options.FeatureTest && state.Options.Replay.empty())
		{
			PlayStartOptions start;
			start.Paused = state.Options.StartPaused;
			ENGINE_TRY_ASSIGN(state.Session, state.PrepareSession(manifest.StartScene, state.Project, start));
			ENGINE_INFO("Runtime: '{}' started with scene '{}' ({} entities, seed {}){}", manifest.Name, m_Assets->GetReferencePath(manifest.StartScene),
				state.Session->GetScene().GetEntityCount(), state.Session->GetSeed(), state.Options.StartPaused ? ", paused at tick 0" : "");
		}

		if (context.GetGraphicsDevice() != nullptr)
			ENGINE_TRY(CreateRenderers());
		if (state.Options.FeatureTest)
		{
			ENGINE_TRY_ASSIGN(state.CoverageStart, context.GetScriptApiRegistry().GetCoverage(RuntimeScriptMode));
			ENGINE_TRY(state.Runner.Begin(state, { .Mode = RuntimeTestMode, .Filter = state.Options.Filter }));
		}
		else if (!state.Options.Replay.empty())
		{
			ENGINE_TRY_ASSIGN(auto replay, state.LoadReplay(state.Options.Replay));
			ENGINE_TRY_ASSIGN(state.Session, state.CreateReplaySession(replay->Header, RuntimeTestMode));
			state.ReplayInputSerial = state.Session->GetSerial();
			ENGINE_TRY(state.Player.Begin(std::move(replay), *state.Session, { state.Options.VerifyReplay, state.Options.VerifyReplay }));
		}

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
			server.ScriptErrors = &state.Errors;
			server.DescribeReplayHeader = [this]()
			{
				return m_State->DescribeReplayHeader(*m_State->Session);
			};
			server.LoadReplay = [this](std::string_view path)
			{
				return m_State->LoadReplay(path);
			};
			server.ValidateReplayOutput = [this](std::string_view path)
			{
				return m_State->ValidateReplayOutput(path);
			};
			server.WriteReplay = [this](std::string_view path, const ReplayDocument& document)
			{
				return m_State->WriteReplay(path, document);
			};
			server.ReleaseReplayInput = [this](uint64_t serial)
			{
				m_State->ReleaseReplayInput(serial);
			};
			server.StartRecordingSession = [this](const PlayStartOptions& options, bool restart) -> Result<PlaySession*>
			{
				if (m_State->Session && !restart)
					return MakeError(ErrorCode::InvalidState, "recording needs restart:true");
				ENGINE_TRY_ASSIGN(auto header, m_State->DescribeReplayHeader(*m_State->Session));
				if (!options.ScenePath.empty())
				{
					ENGINE_TRY_ASSIGN(auto path, ProjectAssetPath(options.ScenePath));
					const auto handle = m_Assets->Resolve(path.GetPath());
					if (!handle)
						return MakeError(ErrorCode::NotFound, "recording scene '{}' is missing", options.ScenePath);
					header.Scene.Handle = *handle;
				}
				return m_State->Restart(header, options, false);
			};
			server.RestartForReplay = [this](const ReplayHeader& header) -> Result<PlaySession*>
			{
				PlayStartOptions options;
				options.Seed = header.Seed;
				options.Parameters = header.Parameters;
				options.Lockstep = true;
				options.Paused = true;
				return m_State->Restart(header, options, true);
			};
			server.ScenePath = m_Assets->GetReferencePath(manifest.StartScene);
			// The server is released before this application's renderers and context. It calls this on the main thread
			// at the safe point; getters copy observations without advancing the game or polling the GPU.
			server.ReadHostStatistics = [this]()
			{
				StatsGetResult result;
				const FrameLoopStatistics frame = GetFrameStatistics();
				result.Fps = ToStatsTelemetry(frame.Fps);
				result.CpuMilliseconds = ToStatsTelemetry(frame.CpuMilliseconds);
				result.DroppedSeconds = ToStatsTelemetry(frame.DroppedSeconds);
				if (const GraphicsDevice* device = GetContext().GetGraphicsDevice())
				{
					result.MemoryAllocationCount = device->GetMemoryAllocationCount();
					result.MaxMemoryAllocationCount = device->GetInfo().MaxMemoryAllocationCount;
				}
				result.Views.push_back(MakeStatsViewSummary("game", m_State->Renderer != nullptr ? m_State->Renderer->GetRenderStats() : RenderStats{}));
				return result;
			};
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
		state.Runner.Cancel();
		state.Player.Cancel();
		state.Capture.reset();
		state.Blit.reset();
		state.Renderer.reset();
		state.PendingRenderFrame.reset();
		state.Pipelines.reset();
		state.Session.reset();
		state.Schemas.reset();
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
		if (m_State->Session != nullptr && !m_State->Options.FeatureTest && m_State->ReplayInputSerial == 0)
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
		if (state.Options.FeatureTest)
		{
			SetFrameThrottleSuspended(true);
			if (!state.TestsComplete)
			{
				const auto advanced = state.Runner.Advance();
				if (!advanced || *advanced)
				{
					state.TestsComplete = true;
					if (!advanced)
						ENGINE_ERROR("Feature test run failed: {}", advanced.error());
					const auto written = state.WriteTestResults();
					if (!written)
						ENGINE_ERROR("Cannot write feature test results: {}", written.error());
					const auto result = state.Runner.GetResult();
					RequestExit(advanced && written && result && result->Passed ? ExitCode::Success : ExitCode::Failed);
				}
			}
			return;
		}
		if (!state.Options.Replay.empty())
		{
			SetFrameThrottleSuspended(true);
			if (!state.ReplayComplete)
			{
				const auto advanced = state.Player.Advance(*state.Session, state);
				if (!advanced || *advanced)
				{
					state.ReplayComplete = true;
					state.ReleaseReplayInput(state.Session->GetSerial());
					if (!advanced)
					{
						ENGINE_ERROR("Replay failed: {}", advanced.error());
						RequestExit(ExitCode::Failed);
					}
					else
					{
						const auto result = state.Player.GetResult();
						if (auto* scripts = state.Session->GetScripts())
							scripts->Stop();
						const bool passed = result && result->Passed && state.Session->GetScriptErrors().GetCursor() == 0;
						if (passed)
							ENGINE_INFO("Replay passed at tick {} with state hash {}", result->FinalTick, result->StateHash);
						else if (result)
							ENGINE_ERROR("Replay verification failed at tick {}: expected {}, observed {}", result->FinalTick, result->ExpectedStateHash, result->StateHash);
						else
							ENGINE_ERROR("Replay result unavailable: {}", result.error());
						RequestExit(passed ? ExitCode::Success : ExitCode::Failed);
					}
				}
			}
			return;
		}
		// The session's time scale and the unthrottled frames of a running play.step reach the loop here, after the requests
		// that changed them (§4.2, §13.6).
		if (state.Session != nullptr)
		{
			if (const auto quit = state.Session->GetQuitRequest())
				RequestExit(*quit);
			FrameLoopConfig config;
			config.FixedHz = state.Session->GetProjectSettings().Simulation.FixedHz;
			config.MaxStepsPerFrame = state.Session->GetProjectSettings().Simulation.MaxStepsPerFrame;
			SetFrameLoopConfig(config);
			SetFrameTimeScale(state.Session->GetTimeScale());
			SetFrameThrottleSuspended(state.Session->IsStepping());
		}
	}

	void RuntimeApp::OnFixedStep(const SimStep& /*step*/)
	{
		// A running session advances one tick per loop step (§4.2 step 5); a paused one, or one a client steps in lockstep,
		// does not. The session counts its own ticks.
		if (m_State->Session != nullptr && !m_State->Options.FeatureTest && m_State->Options.Replay.empty())
			m_State->Session->AdvanceLoopStep();
	}

	void RuntimeApp::OnUpdate(const FrameTime& frame)
	{
		State& state = *m_State;
		if (state.Options.FeatureTest || !state.Options.Replay.empty())
		{
			if (const auto max = GetSpecification().MaxFrames; max && frame.FrameIndex + 1 == *max && !state.TestsComplete && !state.ReplayComplete)
			{
				if (state.Options.FeatureTest)
				{
					state.Runner.Cancel();
					const auto written = state.WriteTestResults();
					if (!written)
						ENGINE_ERROR("Cannot write cancelled test results: {}", written.error());
				}
				ENGINE_ERROR("--frames ended the run before {} completed", state.Options.FeatureTest ? "feature tests" : "replay");
				RequestExit(ExitCode::Failed);
			}
			return;
		}
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
		if (const auto quit = state.Session->GetQuitRequest())
			RequestExit(*quit);

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
		PlaySession* session = state.ActiveSession();
		if (state.Renderer == nullptr || session == nullptr || context.Width == 0 || context.Height == 0)
			return;
		if (state.RenderedSerial != session->GetSerial() || state.RenderedGeneration != session->GetSceneGeneration())
		{
			state.RenderedSerial = session->GetSerial();
			state.RenderedGeneration = session->GetSceneGeneration();
			state.Mirrors.NoteSceneChange();
		}
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
		// Render frame identities differ from simulation ticks and completed loop-frame counts, especially while paused
		// or minimized. Keep the session's snapshot unchanged, and pair this copy with the actual submission below.
		ENGINE_ASSERT(!state.PendingRenderFrame.has_value(), "the previous game view has not been submitted");
		RenderSnapshot snapshot = session->GetLastExtraction();
		snapshot.FrameIndex = context.FrameIndex;
		snapshot.SceneRevision = session->GetScene().GetRevision();
		state.PendingRenderFrame = context.FrameIndex;
		const Status rendered = state.Renderer->Render(*context.CommandList, snapshot);
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

	void RuntimeApp::OnRenderSubmitted(uint64_t frameIndex, uint64_t submissionId)
	{
		State& state = *m_State;
		if (!state.PendingRenderFrame.has_value())
			return;
		ENGINE_VERIFY(*state.PendingRenderFrame == frameIndex && state.Renderer != nullptr, "game view submission does not match its recorded frame");
		// Partial/error renders can also hold target references and timer queries. The renderer ignores attempts that
		// failed before recording. Captures use their own renderer and notify it directly.
		state.Renderer->OnSubmitted(frameIndex, submissionId);
		state.PendingRenderFrame.reset();
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
		const RenderingSettings& quality = state.Session->GetProjectSettings().Rendering;
		request.Quality = { .ShadowMapSize = quality.ShadowMapSize, .SsaoHalfResolution = quality.SsaoHalfResolution };
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
		if (runtime.FeatureTest && !manifest->Testing)
			return MakeError(ErrorCode::InvalidArgument, "--feature-test requires a testing export (Game.json Testing:true)");
		if (runtime.FeatureTest && manifest->Testing && specification.UserDataRoot.empty())
		{
			// Testing exports keep logs/crashes and user:// beside their manifest, including Dist without a CLI override.
			// ProcessContext appends the manifest's game name; an explicit development root always takes precedence.
			std::error_code error;
			specification.UserDataRoot = std::filesystem::absolute(runtime.ManifestPath.parent_path() / "bin" / "TestUserData", error);
			if (error)
				return MakeError(ErrorCode::Io, "cannot resolve testing user-data directory beside '{}': {}", FileSystem::PathToUtf8(runtime.ManifestPath), error.message());
		}
		if (runtime.FeatureTest || !runtime.Replay.empty())
		{
			auto audio = specification.Audio.value_or(GetDefaultAudioSpecification(specification.Window));
			audio.Decoding = AudioDecoding::Deterministic;
			specification.Audio = audio;
			specification.ThrottleHeadless = false;
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
		specification.RegisterTypes = &Utils::RegisterRuntimeTypes;
		return CreateScope<RuntimeApp>(std::move(specification), std::move(runtime), std::move(*manifest));
	}

}
