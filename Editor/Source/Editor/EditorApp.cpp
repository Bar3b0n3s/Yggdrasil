#include "EditorPCH.h"
#include "Editor/EditorApp.h"

#include "EditorCore/Automation/AutomationServer.h"
#include "EditorCore/Automation/BatchRunner.h"
#include "EditorCore/Automation/RegisterMethods.h"
#include "EditorCore/EditorCommandLine.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/App/CommandLine.h"
#include "Engine/App/ProcessContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Core/Log.h"
#include "Engine/Platform/Process.h"

#include <format>
#include <vector>

#if !defined(ENGINE_REPO_ROOT)
	#error "ENGINE_REPO_ROOT must be defined for the editor (Dependencies.lua, ApplyFirstPartySettings): it finds Resources and Docs there"
#endif

namespace Engine {

	struct EditorApp::State
	{
		EditorLaunchOptions Options{};
		Scope<EditorContext> Editor; // destroyed last (reverse member order): the server and the run use it
		Scope<AutomationServer> Server;
		Scope<BatchRunner> Batch; // --batch or --upgrade

		// OnInitialize's work for `app`; on failure the caller releases what was built.
		[[nodiscard]] Status Initialize(EditorApp& app);
		// Releases the run, the server and the editor, in that order, while the engine context still exists.
		void Release();
	};

	namespace Utils {

		// The repository root of development builds: Resources and Docs live there (§2.2; the editor has no Dist build).
		[[nodiscard]] static std::filesystem::path GetEditorRepositoryRoot()
		{
			return std::filesystem::path(ENGINE_REPO_ROOT);
		}

		// Writes `document` canonically (JsonWriter, pretty) to `file`.
		[[nodiscard]] static Status WriteReferenceFile(const std::filesystem::path& file, const Json& document)
		{
			ENGINE_TRY_ASSIGN(const std::string text, JsonWriter::Write(document, JsonStyle::Pretty));
			return FileSystem::WriteFileAtomic(file, std::as_bytes(std::span(text.data(), text.size())), { .KeepBackup = false });
		}

		// --dump-reference <dir>: Methods.json and catalog.json (ADR 0008 decision 9), from every method but the test hooks.
		[[nodiscard]] static Status DumpReference(const TypeRegistry& types, const std::filesystem::path& directory)
		{
			MethodRegistry methods(types);
			RegisterEditorMethods(methods, EditorMethodOptions{ .TestHooks = false });
			methods.Freeze();
			ENGINE_TRY(FileSystem::CreateDirectories(directory));
			ENGINE_TRY(WriteReferenceFile(directory / "Methods.json", methods.BuildMethodCatalog()));
			ENGINE_TRY(WriteReferenceFile(directory / "catalog.json", methods.BuildToolCatalog()));
			ENGINE_INFO("Wrote the method catalogue ({} methods) and the MCP tool catalogue", methods.GetMethods().size());
			return {};
		}

	}

	EditorApp::EditorApp(ApplicationSpecification specification)
		: Application(std::move(specification)), m_State(CreateScope<State>())
	{
	}

	EditorApp::~EditorApp() = default;

	Status EditorApp::State::Initialize(EditorApp& app)
	{
		State& state = *this;
		ENGINE_TRY_ASSIGN(state.Options, ParseEditorLaunchOptions(app.GetSpecification().Args));
		const EditorLaunchOptions& options = state.Options;
		const bool headless = app.GetSpecification().Window == WindowMode::Headless;

		if (options.DumpReferenceDirectory.has_value())
		{
			ENGINE_TRY(Utils::DumpReference(app.GetContext().GetTypeRegistry(), *options.DumpReferenceDirectory));
			app.RequestExit(ExitCode::Success);
			return {};
		}

		const std::filesystem::path repository = Utils::GetEditorRepositoryRoot();
		const UserDataPaths& userData = app.GetProcessContext().GetUserDataPaths();
		EditorContextSpecification editorSpecification;
		editorSpecification.TemplatesDirectory = repository / "Resources" / "Templates" / "Projects";
		editorSpecification.ReadOnlyCacheRoot = userData.Root / "ReadOnlyCache";
		ENGINE_TRY_ASSIGN(state.Editor, EditorContext::Create(app.GetContext(), editorSpecification));

		if (options.Project.has_value())
		{
			const ProjectOpenOptions openOptions{
				.ReadOnly = options.ReadOnly,
				.StrictUnknowns = false,
				.ReadOnlyCacheDirectory = options.ReadOnly ? editorSpecification.ReadOnlyCacheRoot / std::to_string(Process::GetCurrentId())
														   : std::filesystem::path(),
			};
			ENGINE_TRY_ASSIGN(Scope<LoadedProject> project, ProjectManager::OpenProject(*options.Project, openOptions, app.GetContext().GetTypeRegistry()));
			ENGINE_TRY(state.Editor->OpenProject(std::move(project)));
		}

		const bool listens = options.ListensForAutomation(headless);
		if (listens || options.IsOneShot())
		{
			AutomationServerSpecification serverSpecification;
			serverSpecification.Listen = listens;
			serverSpecification.Port = options.AutomationPort.value_or(0);
			serverSpecification.TestHooks = options.AutomationTestHooks;
			serverSpecification.Headless = headless;
			serverSpecification.RendererName = std::string(EditorRendererToString(options.Renderer));
			serverSpecification.SessionsDirectory = userData.Root / "Automation" / "Sessions";
			serverSpecification.DocsRoot = repository;
			ENGINE_TRY_ASSIGN(state.Server, AutomationServer::Create(*state.Editor, serverSpecification));
		}

		if (options.BatchFile.has_value())
		{
			ENGINE_TRY_ASSIGN(std::vector<BatchRequest> requests, BatchRunner::LoadFile(*options.BatchFile));
			state.Batch = CreateScope<BatchRunner>(std::move(requests), BatchRunOptions{ .ClientName = "batch", .WriteTranscript = false });
		}
		else if (options.Upgrade)
		{
			std::vector<BatchRequest> requests = { BatchRequest{ .Method = "project.upgrade", .Params = {}, .Line = 0 } };
			state.Batch = CreateScope<BatchRunner>(std::move(requests), BatchRunOptions{ .ClientName = "cli", .WriteTranscript = true });
		}
		return {};
	}

	void EditorApp::State::Release()
	{
		Batch.reset();
		Server.reset();
		Editor.reset();
	}

	Status EditorApp::OnInitialize()
	{
		Status initialized = m_State->Initialize(*this);
		if (!initialized)
			m_State->Release(); // Run destroys the engine context next, which the editor refers to
		return initialized;
	}

	void EditorApp::OnShutdown()
	{
		// The server first (its pending operations are cancelled against a live editor), then the editor (the project lock).
		m_State->Release();
	}

	void EditorApp::OnSafePoint()
	{
		State& state = *m_State;
		if (state.Server != nullptr)
			state.Server->Pump();

		if (state.Batch != nullptr && state.Server != nullptr)
		{
			state.Batch->Advance(*state.Server);
			if (state.Batch->IsFinished())
			{
				if (const std::optional<Error>& failure = state.Batch->GetFailure())
				{
					ENGINE_ERROR("The {} run failed: {}", state.Options.Upgrade ? "upgrade" : "batch", failure->ToString());
					RequestExit(ExitCode::Failed);
				}
				else
				{
					ENGINE_INFO("The {} run completed {} request(s)", state.Options.Upgrade ? "upgrade" : "batch", state.Batch->GetResults().size());
					RequestExit(ExitCode::Success);
				}
			}
		}

		if (state.Editor != nullptr)
		{
			if (const std::optional<int> exitCode = state.Editor->GetShutdownRequest())
				RequestExit(*exitCode);
		}
	}

	Result<Scope<Application>> CreateEditorApp(std::span<const std::string> arguments)
	{
		std::vector<CommandLineOption> options(GetEngineCommandLineOptions().begin(), GetEngineCommandLineOptions().end());
		options.insert(options.end(), GetEditorCommandLineOptions().begin(), GetEditorCommandLineOptions().end());
		ENGINE_TRY_ASSIGN(CommandLine commandLine, CommandLine::Parse(arguments, options));
		if (!commandLine.GetPositional().empty())
		{
			return MakeError(ErrorCode::InvalidArgument, "unexpected argument '{}': the editor takes no positional arguments",
				commandLine.GetPositional().front());
		}
		ENGINE_TRY_ASSIGN(const EditorLaunchOptions launch, ParseEditorLaunchOptions(commandLine));

		ApplicationSpecification specification;
		specification.Name = ENGINE_PRODUCT_NAME;
		// The editor's automation param and result structs join the context's type registry before it freezes (§5.4, §13.4).
		specification.RegisterTypes = &RegisterEditorMethodTypes;
		ENGINE_TRY(ApplyEngineCommandLine(commandLine, specification));
		// Batch runs are among §4.2's unthrottled modes (ADR 0008 decision 4).
		if (launch.BatchFile.has_value() || launch.Upgrade)
			specification.ThrottleHeadless = false;
		return CreateScope<EditorApp>(std::move(specification));
	}

}
