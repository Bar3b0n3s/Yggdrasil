#include "EditorPCH.h"
#include "EditorCore/EditorContext.h"

#include "EditorCore/Commands/CompositeCommand.h"
#include "EditorCore/Private/EditorFileError.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Core/Mounts/OverlayMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Platform/SecureRandom.h"
#include "Engine/Project/ProjectSerializer.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/SceneSerializer.h"

#include <nlohmann/json.hpp>

namespace Engine {

	namespace Utils {

		static constexpr std::string_view ProjectScheme = "project";
		static constexpr std::string_view CacheScheme = "cache";

		// Removes `scheme`'s mount, logging a failure (the mount is already gone then).
		static void UnmountLogged(VirtualFileSystem& vfs, std::string_view scheme)
		{
			if (Result<Scope<IMount>> removed = vfs.Unmount(scheme); !removed)
				ENGINE_WARN("Could not unmount {}://: {}", scheme, removed.error().ToString());
		}

	}

	// --- EditorContext ---------------------------------------------------------------------------------------------------

	EditorContext::EditorContext(ConstructionKey /*key*/, EngineContext& engine, const EditorContextSpecification& specification,
		const Random::State& idGeneratorState)
		: m_Engine(&engine), m_Specification(specification), m_IdGenerator(UUIDGenerator::CreateRandom(idGeneratorState)), m_History(specification.HistoryLimits)
	{
	}

	EditorContext::~EditorContext()
	{
		CloseProject();
	}

	Result<Scope<EditorContext>> EditorContext::Create(EngineContext& engine, const EditorContextSpecification& specification)
	{
		if (specification.IdGeneratorState.has_value())
			return CreateScope<EditorContext>(ConstructionKey(), engine, specification, *specification.IdGeneratorState);
		Result<Random::State> generated = SecureRandom::GenerateState();
		if (!generated)
			return std::unexpected(std::move(generated).error().WithContext("seeding the editor's id generator"));
		return CreateScope<EditorContext>(ConstructionKey(), engine, specification, *generated);
	}

	uint64_t EditorContext::GetRevision() const
	{
		// During a dry run the base may have wrapped around (EditorDryRunScope::Begin); unsigned arithmetic makes the sum the
		// real revision plus the sandbox's mutations all the same.
		return m_RevisionBase + (m_Scene != nullptr ? m_Scene->GetRevision() : 0);
	}

	uint64_t EditorContext::GetRevisionBeforeCommand() const
	{
		return m_PendingRevisionBefore.value_or(GetRevision());
	}

	const TypeRegistry& EditorContext::GetTypeRegistry() const
	{
		return m_Engine->GetTypeRegistry();
	}

	VirtualFileSystem& EditorContext::GetVfs() const
	{
		return m_Engine->GetVfs();
	}

	const LoadedProject& EditorContext::GetProject() const
	{
		ENGINE_ASSERT(m_Project != nullptr, "EditorContext::GetProject without an open project");
		return *m_Project;
	}

	bool EditorContext::IsReadOnly() const
	{
		return m_Project != nullptr && m_Project->IsReadOnly();
	}

	Result<Scope<IMount>> EditorContext::CreateProjectMount() const
	{
		ENGINE_ASSERT(m_Project != nullptr, "EditorContext::CreateProjectMount without an open project");
		const MountAccess access = m_Project->IsReadOnly() ? MountAccess::ReadOnly : MountAccess::ReadWrite;
		ENGINE_TRY_ASSIGN(Scope<NativeDirectoryMount> mount, NativeDirectoryMount::Create(m_Project->GetRoot(), access));
		return Scope<IMount>(std::move(mount));
	}

	Status EditorContext::OpenProject(Scope<LoadedProject> project)
	{
		ENGINE_ASSERT(project != nullptr, "EditorContext::OpenProject needs a project");
		ENGINE_ASSERT(m_DryRun == nullptr && m_Transaction == nullptr, "EditorContext::OpenProject inside a dry run or a transaction");
		if (HasProject())
		{
			return MakeError(ErrorCode::InvalidState, "the project '{}' is already open; close it before opening '{}'", m_Project->GetSettings().Name,
				FileSystem::PathToUtf8(project->GetProjectFile()));
		}

		VirtualFileSystem& vfs = GetVfs();
		const MountAccess access = project->IsReadOnly() ? MountAccess::ReadOnly : MountAccess::ReadWrite;
		ENGINE_TRY_ASSIGN(Scope<NativeDirectoryMount> projectMount, NativeDirectoryMount::Create(project->GetRoot(), access));
		ENGINE_TRY_ASSIGN(Scope<NativeDirectoryMount> cacheMount,
			NativeDirectoryMount::Create(project->GetCacheDirectory(), MountAccess::ReadWrite, AtomicWriteOptions{ .KeepBackup = false, .InjectFailure = AtomicWriteStep::None }));
		ENGINE_TRY(vfs.Mount(Utils::ProjectScheme, std::move(projectMount)));
		if (Status mounted = vfs.Mount(Utils::CacheScheme, std::move(cacheMount)); !mounted)
		{
			Utils::UnmountLogged(vfs, Utils::ProjectScheme);
			return mounted;
		}

		std::optional<ProvenanceRecorder> provenance;
		if (!project->IsReadOnly())
		{
			// A project without a provenance file yet (every new project until its first recorded write) gets an empty
			// recorder from Load.
			Result<ProvenanceRecorder> loaded = ProvenanceRecorder::Load(vfs);
			if (!loaded)
			{
				Utils::UnmountLogged(vfs, Utils::CacheScheme);
				Utils::UnmountLogged(vfs, Utils::ProjectScheme);
				return std::unexpected(Utils::ToEditorFileError(std::move(loaded).error()).WithContext("loading the project's provenance"));
			}
			provenance = std::move(*loaded);
		}

		if (vfs.IsMounted("user"))
		{
			if (Status added = ProjectManager::AddRecentProject(vfs, project->GetProjectFile()); !added)
				ENGINE_WARN("Could not add the project to the recent list: {}", added.error().ToString());
		}

		m_Project = std::move(project);
		m_Provenance = std::move(provenance);
		m_Scene.reset();
		m_ScenePath.reset();
		m_SceneDirty = false;
		m_History.Clear();
		m_Selection.clear();
		ENGINE_INFO("Opened project '{}' at '{}'{}", m_Project->GetSettings().Name, FileSystem::PathToUtf8(m_Project->GetRoot()),
			m_Project->IsReadOnly() ? " read-only" : "");
		return {};
	}

	void EditorContext::CloseProject()
	{
		if (!HasProject())
			return;
		ENGINE_ASSERT(m_DryRun == nullptr && m_Transaction == nullptr, "EditorContext::CloseProject inside a dry run or a transaction");
		CloseScene();
		VirtualFileSystem& vfs = GetVfs();
		Utils::UnmountLogged(vfs, Utils::CacheScheme);
		Utils::UnmountLogged(vfs, Utils::ProjectScheme);
		const std::string name = m_Project->GetSettings().Name;
		m_Provenance.reset();
		m_Project.reset(); // releases the project lock
		ENGINE_INFO("Closed project '{}'", name);
	}

	Status EditorContext::ApplyProjectSettings(const Json& document)
	{
		if (!HasProject())
			return MakeError(ErrorCode::InvalidState, "the project settings cannot change: no project is open");
		const TypeRegistry& registry = GetTypeRegistry();
		ProjectLoadReport report;
		ENGINE_TRY_ASSIGN(ProjectSettings settings, ProjectSerializer::FromJson(document, registry, {}, report));
		ENGINE_TRY_ASSIGN(const std::string text, ProjectSerializer::SaveToString(settings, registry));
		ENGINE_TRY_ASSIGN(const VfsPath path, VfsPath::Create(Utils::ProjectScheme, FileSystem::PathToUtf8(m_Project->GetProjectFile().filename())));
		ENGINE_TRY(WriteProjectFile(path, std::as_bytes(std::span(text.data(), text.size()))));
		m_Project->SetSettings(std::move(settings));
		return {};
	}

	Scene& EditorContext::GetScene()
	{
		ENGINE_ASSERT(m_Scene != nullptr, "EditorContext::GetScene without an open scene");
		return *m_Scene;
	}

	const Scene& EditorContext::GetScene() const
	{
		ENGINE_ASSERT(m_Scene != nullptr, "EditorContext::GetScene without an open scene");
		return *m_Scene;
	}

	Scope<Scene> EditorContext::CreateScene(std::string name)
	{
		return Scene::Create({ .Name = std::move(name), .Seed = 0, .Registry = &GetTypeRegistry(), .IdGenerator = &m_IdGenerator, .Runtime = false });
	}

	void EditorContext::SetScene(Scope<Scene> scene, std::optional<VfsPath> path, bool dirty)
	{
		ENGINE_ASSERT(HasProject(), "EditorContext::SetScene needs an open project");
		ENGINE_ASSERT(scene != nullptr, "EditorContext::SetScene needs a scene");
		ENGINE_ASSERT(&scene->GetTypeRegistry() == &GetTypeRegistry() && &scene->GetUUIDGenerator() == &m_IdGenerator,
			"EditorContext::SetScene: scene '{}' was not created with EditorContext::CreateScene", scene->GetName());
		ENGINE_ASSERT(m_Transaction == nullptr, "EditorContext::SetScene inside a transaction");

		// Past every value reported so far, so a revision of the previous scene never matches this one (ADR 0008 decision 28).
		const uint64_t nextBase = GetRevision() + 1;
		m_Scene = std::move(scene);
		m_RevisionBase = nextBase;
		m_ScenePath = std::move(path);
		m_SceneDirty = dirty;
		m_History.Clear();
		m_Selection.clear();

		const std::string pathText = m_ScenePath.has_value() ? std::string(m_ScenePath->GetPath()) : std::string();
		AppendEvent(EngineEvent{ .Seq = 0, .Tick = std::nullopt, .Type = EngineEventType::SceneOpened, .Id = UUID(), .Path = pathText, .Name = {}, .Message = {}, .Dirty = dirty });
		ENGINE_INFO("Opened scene '{}' ({} entities){}{}", m_Scene->GetName(), m_Scene->GetEntityCount(),
			pathText.empty() ? std::string(", not saved yet") : std::format(" from '{}'", pathText), dirty ? ", with unsaved repairs" : "");
	}

	void EditorContext::CloseScene()
	{
		if (m_Scene == nullptr)
			return;
		ENGINE_ASSERT(m_Transaction == nullptr, "EditorContext::CloseScene inside a transaction");
		const uint64_t nextBase = GetRevision() + 1;
		m_Scene.reset();
		m_RevisionBase = nextBase;
		m_ScenePath.reset();
		m_SceneDirty = false;
		m_History.Clear();
		m_Selection.clear();
	}

	bool EditorContext::IsSceneDirty() const
	{
		return m_Scene != nullptr && (m_SceneDirty || m_History.IsDirty());
	}

	void EditorContext::MarkSceneSaved(const VfsPath& path)
	{
		ENGINE_ASSERT(m_Scene != nullptr, "EditorContext::MarkSceneSaved without an open scene");
		m_ScenePath = path;
		m_SceneDirty = false;
		m_History.MarkSavePoint();
	}

	Result<uint64_t> EditorContext::Execute(Scope<Command> command)
	{
		ENGINE_ASSERT(command != nullptr, "EditorContext::Execute needs a command");
		if (IsReadOnly() && !IsDryRun())
		{
			return std::unexpected(Error(ErrorCode::PermissionDenied, std::format("'{}' cannot run: the project is open read-only", command->GetLabel()))
					.WithHint("open the project without --read-only, or close the editor that holds it"));
		}
		command->SetOrigin(GetCommandOrigin());
		if (m_Transaction != nullptr)
		{
			ENGINE_TRY(m_Transaction->ExecuteAndAppend(std::move(command)));
			return 0;
		}
		ENGINE_TRY_ASSIGN(const uint64_t sequence, m_History.Execute(std::move(command), *this));
		return IsDryRun() ? 0 : sequence;
	}

	CommandHistory& EditorContext::GetHistory()
	{
		return m_History;
	}

	const CommandHistory& EditorContext::GetHistory() const
	{
		return m_History;
	}

	void EditorContext::SetSelection(std::vector<UUID> selection)
	{
		std::vector<UUID> kept;
		kept.reserve(selection.size());
		for (const UUID id : selection)
		{
			if (m_Scene == nullptr || !std::as_const(*m_Scene).FindEntityByID(id).IsValid())
				continue;
			if (std::find(kept.begin(), kept.end(), id) == kept.end())
				kept.push_back(id);
		}
		m_Selection = std::move(kept);
	}

	Status EditorContext::WriteProjectFile(const VfsPath& path, std::span<const std::byte> data)
	{
		ENGINE_ASSERT(path.GetScheme() == Utils::ProjectScheme, "EditorContext::WriteProjectFile writes project:// paths, not '{}'", path.ToString());
		if (!HasProject())
			return MakeError(ErrorCode::InvalidState, "cannot write '{}': no project is open", path.ToString());
		if (IsReadOnly() && !IsDryRun())
		{
			return std::unexpected(Error(ErrorCode::PermissionDenied, std::format("cannot write '{}': the project is open read-only", path.ToString()))
					.WithHint("open the project without --read-only, or close the editor that holds it"));
		}

		VirtualFileSystem& vfs = GetVfs();
		if (const VfsPath parent = path.GetParent(); !parent.IsRoot() && !vfs.Exists(parent))
			ENGINE_TRY(Utils::ToEditorFileStatus(vfs.CreateDirectories(parent)));
		ENGINE_TRY(Utils::ToEditorFileStatus(vfs.WriteFileAtomic(path, data)));

		if (IsDryRun() || !m_Provenance.has_value() || !ProvenanceRecorder::IsRecordedPath(path.GetPath()))
			return {};
		m_Provenance->Record(path.GetPath(), XXH64(data), m_Attribution);
		if (Status saved = m_Provenance->Save(vfs); !saved)
		{
			return std::unexpected(Utils::ToEditorFileError(std::move(saved).error())
					.WithContext(std::format("'{}' was written, but its provenance could not be saved", path.ToString())));
		}
		return {};
	}

	void EditorContext::SetWriteAttribution(std::optional<WriteAttribution> attribution)
	{
		m_HasRequestAttribution = attribution.has_value();
		m_Attribution = attribution.has_value() ? std::move(*attribution) : WriteAttribution{};
	}

	const ProvenanceRecorder* EditorContext::GetProvenance() const
	{
		return m_Provenance.has_value() ? &*m_Provenance : nullptr;
	}

	void EditorContext::AppendEvent(EngineEvent event)
	{
		if (IsDryRun())
			return;
		static_cast<void>(m_Engine->GetEventLog().Append(std::move(event)));
	}

	void EditorContext::RequestShutdown(int exitCode)
	{
		if (!m_ShutdownRequest.has_value())
			m_ShutdownRequest = exitCode;
	}

	// --- EditorTransaction -----------------------------------------------------------------------------------------------

	struct EditorTransaction::State
	{
		EditorContext* Context = nullptr; // documented back-reference: outlives the transaction
		std::string Label{};
		EditorTransaction* Outermost = nullptr; // the transaction this one joined; null for the outermost
		std::vector<Scope<Command>> Executed{}; // the outermost's executed commands, oldest first
		size_t JoinedFirst = 0;                 // when joined: the index of this one's first command in the outermost's list
		size_t OpenJoined = 0;                  // the outermost: how many joined transactions are open
		size_t ClosedCount = 0;                 // GetCommandCount once closed
		uint64_t RevisionBefore = 0;            // EditorContext::GetRevision before the first command
	};

	namespace Utils {

		// Undoes commands[first ..] newest first and removes them. When one's Undo fails, it and the older ones of
		// commands[first ..] stay applied: they are replaced by one composite of them labelled "<label> (partially rolled
		// back)", appended to `commands`, and the Undo error is returned with the context "rolling back '<label>'".
		static Status UndoTransactionCommands(EditorContext& context, std::vector<Scope<Command>>& commands, size_t first, const std::string& label)
		{
			for (size_t index = commands.size(); index > first; --index)
			{
				Status undone = commands[index - 1]->Undo(context);
				if (undone)
				{
					commands.pop_back();
					continue;
				}
				Scope<CompositeCommand> partial = CreateScope<CompositeCommand>(std::format("{} (partially rolled back)", label));
				for (size_t applied = first; applied < index; ++applied)
					partial->AddExecuted(std::move(commands[applied]));
				partial->SetOrigin(context.GetCommandOrigin());
				commands.resize(first);
				commands.push_back(std::move(partial));
				return std::unexpected(std::move(undone).error().WithContext(std::format("rolling back '{}'", label)));
			}
			return {};
		}

	}

	EditorTransaction::EditorTransaction(EditorContext& context, std::string label)
		: m_State(CreateScope<State>())
	{
		m_State->Context = &context;
		m_State->Label = std::move(label);
		if (EditorTransaction* outermost = context.m_Transaction; outermost != nullptr)
		{
			m_State->Outermost = outermost;
			m_State->JoinedFirst = outermost->m_State->Executed.size();
			++outermost->m_State->OpenJoined;
		}
		else
		{
			context.m_Transaction = this;
			m_State->RevisionBefore = context.GetRevision();
		}
	}

	EditorTransaction::~EditorTransaction()
	{
		if (!m_IsOpen)
			return;
		if (Status rolledBack = Rollback(); !rolledBack)
			ENGINE_ERROR("Rolling back the abandoned transaction '{}' failed: {}", m_State->Label, rolledBack.error().ToString());
	}

	uint64_t EditorTransaction::Commit()
	{
		ENGINE_ASSERT(m_IsOpen, "EditorTransaction '{}' committed after it closed", m_State->Label);
		if (!m_IsOpen)
			return 0;
		State& state = *m_State;
		state.ClosedCount = GetCommandCount();
		m_IsOpen = false;
		if (state.Outermost != nullptr)
		{
			--state.Outermost->m_State->OpenJoined;
			return 0; // the outermost transaction records these commands with its own
		}

		ENGINE_ASSERT(state.OpenJoined == 0, "EditorTransaction '{}' committed while a transaction that joined it is open", state.Label);
		EditorContext& context = *state.Context;
		context.m_Transaction = nullptr;
		if (state.Executed.empty())
			return 0;
		Scope<CompositeCommand> composite = CreateScope<CompositeCommand>(state.Label);
		for (Scope<Command>& command : state.Executed)
			composite->AddExecuted(std::move(command));
		state.Executed.clear();
		composite->SetOrigin(context.GetCommandOrigin());
		const uint64_t sequence = context.m_History.RecordExecuted(std::move(composite), context, state.RevisionBefore);
		return context.IsDryRun() ? 0 : sequence;
	}

	Status EditorTransaction::Rollback()
	{
		if (!m_IsOpen)
			return {};
		State& state = *m_State;
		state.ClosedCount = GetCommandCount();
		m_IsOpen = false;
		EditorContext& context = *state.Context;
		if (state.Outermost != nullptr)
		{
			State& outer = *state.Outermost->m_State;
			--outer.OpenJoined;
			return Utils::UndoTransactionCommands(context, outer.Executed, state.JoinedFirst, state.Label);
		}

		ENGINE_ASSERT(state.OpenJoined == 0, "EditorTransaction '{}' rolled back while a transaction that joined it is open", state.Label);
		context.m_Transaction = nullptr;
		Status rolledBack = Utils::UndoTransactionCommands(context, state.Executed, 0, state.Label);
		if (!rolledBack)
		{
			// The commands still applied (now one composite) become an undo step, so the history matches the scene.
			ENGINE_ASSERT(state.Executed.size() == 1, "a failed rollback leaves exactly one partial composite");
			static_cast<void>(context.m_History.RecordExecuted(std::move(state.Executed.front()), context, state.RevisionBefore));
			state.Executed.clear();
		}
		return rolledBack;
	}

	bool EditorTransaction::IsJoined() const
	{
		return m_State->Outermost != nullptr;
	}

	Status EditorTransaction::ExecuteAndAppend(Scope<Command> command)
	{
		ENGINE_ASSERT(command != nullptr, "EditorTransaction::ExecuteAndAppend needs a command");
		ENGINE_ASSERT(m_IsOpen, "EditorTransaction '{}' used after it closed", m_State->Label);
		State& state = *m_State;
		if (state.Outermost != nullptr)
			return state.Outermost->ExecuteAndAppend(std::move(command));

		EditorContext& context = *state.Context;
		const uint64_t revisionBefore = context.GetRevisionBeforeCommand();
		ENGINE_TRY(command->Execute(context));
		if (state.Executed.empty())
			state.RevisionBefore = revisionBefore;
		state.Executed.push_back(std::move(command));
		return {};
	}

	size_t EditorTransaction::GetCommandCount() const
	{
		const State& state = *m_State;
		if (!m_IsOpen)
			return state.ClosedCount;
		if (state.Outermost == nullptr)
			return state.Executed.size();
		return state.Outermost->m_State->Executed.size() - state.JoinedFirst;
	}

	// --- EditorDryRunScope -----------------------------------------------------------------------------------------------

	struct EditorDryRunScope::State
	{
		EditorContext* Context = nullptr; // documented back-reference: outlives the dry run
		bool Active = false;              // the sandbox is swapped in (Begin succeeded)
		OverlayMount* Overlay = nullptr;  // the overlay mounted at project://, owned by the VFS while mounted
		// What the dry run swapped out, restored by the destructor.
		Scope<Scene> RealScene{};
		std::optional<VfsPath> RealScenePath{};
		bool RealSceneDirty = false;
		CommandHistory RealHistory{};
		std::optional<UUIDGenerator> RealIdGenerator{};
		uint64_t RealRevisionBase = 0;
		std::vector<UUID> RealSelection{};
		std::optional<ProjectSettings> RealSettings{};
	};

	EditorDryRunScope::EditorDryRunScope(ConstructionKey /*key*/, EditorContext& context)
		: m_State(CreateScope<State>())
	{
		m_State->Context = &context;
	}

	Result<Scope<EditorDryRunScope>> EditorDryRunScope::Begin(EditorContext& context)
	{
		ENGINE_ASSERT(context.m_DryRun == nullptr, "EditorDryRunScope::Begin while a dry run is open");
		ENGINE_ASSERT(context.m_Transaction == nullptr, "EditorDryRunScope::Begin inside a transaction");
		if (!context.HasProject())
			return MakeError(ErrorCode::InvalidState, "a dry run needs an open project");

		// The scene copy first: it is the step that can fail, and nothing is swapped yet.
		Scope<Scene> copy;
		if (context.HasScene())
		{
			const Scene& real = context.GetScene();
			Result<Json> document = SceneSerializer::ToJson(real);
			if (!document)
				return std::unexpected(std::move(document).error().WithContext("copying the open scene for a dry run"));
			copy = context.CreateScene(real.GetName());
			LoadReport report;
			if (Status loaded = SceneSerializer::FromJson(*copy, *document, LoadOptions{}, report); !loaded)
				return std::unexpected(std::move(loaded).error().WithContext("copying the open scene for a dry run"));
		}

		// project:// behind an overlay, while no job uses it (VirtualFileSystem class comment).
		context.GetEngine().GetJobSystem().WaitIdle();
		VirtualFileSystem& vfs = context.GetVfs();
		Result<Scope<IMount>> lower = vfs.Unmount(Utils::ProjectScheme);
		if (!lower)
			return std::unexpected(std::move(lower).error().WithContext("starting a dry run"));
		Scope<OverlayMount> overlay = CreateScope<OverlayMount>(std::move(*lower));
		OverlayMount* overlayMount = overlay.get();
		if (Status mounted = vfs.Mount(Utils::ProjectScheme, std::move(overlay)); !mounted)
		{
			// Mount consumed the overlay and the project mount inside it: mount the project root again.
			Result<Scope<IMount>> again = context.CreateProjectMount();
			Status remounted = again ? vfs.Mount(Utils::ProjectScheme, std::move(*again)) : Status(std::unexpected(std::move(again).error()));
			if (!remounted)
				ENGINE_ERROR("project:// could not be mounted again after a failed dry run: {}", remounted.error().ToString());
			return std::unexpected(std::move(mounted).error().WithContext("starting a dry run"));
		}

		Scope<EditorDryRunScope> scope = CreateScope<EditorDryRunScope>(ConstructionKey(), context);
		State& state = *scope->m_State;
		state.Overlay = overlayMount;
		const uint64_t realRevision = context.GetRevision();
		state.RealScenePath = context.m_ScenePath;
		state.RealSceneDirty = context.m_SceneDirty;
		state.RealIdGenerator = context.m_IdGenerator;
		state.RealRevisionBase = context.m_RevisionBase;
		state.RealSelection = context.m_Selection;
		state.RealSettings = context.m_Project->GetSettings();
		const bool realDirty = context.IsSceneDirty();
		state.RealScene = std::move(context.m_Scene);
		state.RealHistory = std::move(context.m_History);

		context.m_History = CommandHistory(context.m_Specification.HistoryLimits);
		context.m_Scene = std::move(copy);
		context.m_SceneDirty = realDirty; // the sandbox history starts clean; a dirty real scene stays dirty
		// GetRevision must report the real revision plus the sandbox's mutations. Loading the copy counted mutations of its
		// own, possibly more than the real revision, so the base may wrap around; the unsigned sum is still exact.
		context.m_RevisionBase = realRevision - (context.m_Scene != nullptr ? context.m_Scene->GetRevision() : 0);
		context.m_DryRun = scope.get();
		state.Active = true;
		return scope;
	}

	EditorDryRunScope::~EditorDryRunScope()
	{
		State& state = *m_State;
		if (!state.Active)
			return;
		EditorContext& context = *state.Context;
		ENGINE_ASSERT(context.m_DryRun == this, "EditorDryRunScope: dry runs close in the order they opened");
		ENGINE_ASSERT(context.m_Transaction == nullptr, "EditorDryRunScope closed while a transaction opened in it is still open");

		context.m_Scene = std::move(state.RealScene);
		context.m_ScenePath = std::move(state.RealScenePath);
		context.m_SceneDirty = state.RealSceneDirty;
		context.m_History = std::move(state.RealHistory);
		context.m_IdGenerator = *state.RealIdGenerator;
		context.m_RevisionBase = state.RealRevisionBase;
		context.m_Selection = std::move(state.RealSelection);
		if (context.m_Project != nullptr)
			context.m_Project->SetSettings(std::move(*state.RealSettings));

		context.GetEngine().GetJobSystem().WaitIdle();
		VirtualFileSystem& vfs = context.GetVfs();
		if (Result<Scope<IMount>> overlay = vfs.Unmount(Utils::ProjectScheme); overlay)
		{
			ENGINE_ASSERT(overlay->get() == state.Overlay, "EditorDryRunScope: project:// was replaced during the dry run");
			if (Status mounted = vfs.Mount(Utils::ProjectScheme, state.Overlay->ReleaseLower()); !mounted)
				ENGINE_ERROR("project:// could not be mounted again after a dry run: {}", mounted.error().ToString());
		}
		else
		{
			ENGINE_ERROR("project:// was unmounted during a dry run: {}", overlay.error().ToString());
		}
		state.Overlay = nullptr;
		context.m_DryRun = nullptr;
	}

	std::vector<std::string> EditorDryRunScope::GetChangedPaths() const
	{
		return m_State->Overlay != nullptr ? m_State->Overlay->GetChangedPaths() : std::vector<std::string>{};
	}

}
