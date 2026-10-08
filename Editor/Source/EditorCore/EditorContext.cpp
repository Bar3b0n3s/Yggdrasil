#include "EditorPCH.h"
#include "EditorCore/EditorContext.h"

#include "EditorCore/Commands/CompositeCommand.h"
#include "EditorCore/Commands/SceneEditCommand.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Private/EditorFileError.h"
#include "EditorCore/Private/PrefabInstances.h"
#include "EditorCore/Private/SceneEditRollback.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
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
#include "Engine/Scene/ChangeTracker.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/PrefabAsset.h"
#include "Engine/Scene/PrefabInstantiator.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <map>
#include <set>
#include <utility>
#include <vector>

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

		// One write path call of EditorContext (WriteProjectFile, MoveProjectFile, RemoveProjectFile): while the outermost
		// scope is open, the write observer keeps the first provenance save failure for Take instead of logging it. A write
		// path call nested in another (a write the asset manager makes while it processes one) reports to the outer one.
		class ProvenanceErrorScope
		{
		public:
			// `collecting` and `error` are documented back-references to EditorContext's members, which outlive the scope.
			ProvenanceErrorScope(bool& collecting, std::optional<Error>& error)
				: m_Collecting(&collecting), m_Error(&error), m_IsOutermost(!collecting)
			{
				if (!m_IsOutermost)
					return;
				m_Error->reset();
				*m_Collecting = true;
			}

			~ProvenanceErrorScope()
			{
				if (m_IsOutermost)
					*m_Collecting = false;
			}

			ProvenanceErrorScope(const ProvenanceErrorScope&) = delete;
			ProvenanceErrorScope& operator=(const ProvenanceErrorScope&) = delete;

			// The kept failure, taken (outermost scope only); success when there is none.
			[[nodiscard]] Status Take()
			{
				if (!m_IsOutermost || !m_Error->has_value())
					return {};
				Error error = std::move(**m_Error);
				m_Error->reset();
				return std::unexpected(std::move(error));
			}
		private:
			bool* m_Collecting = nullptr;
			std::optional<Error>* m_Error = nullptr;
			bool m_IsOutermost = false;
		};

		// The project-relative paths of the provenance entries at `path` or below it, in entry order.
		static std::vector<std::string> FindProvenanceEntries(const ProvenanceRecorder& provenance, std::string_view path)
		{
			std::vector<std::string> found;
			for (const ProvenanceEntry& entry : provenance.GetEntries())
			{
				const std::string_view entryPath = entry.Path;
				if (entryPath == path || (entryPath.size() > path.size() && entryPath.starts_with(path) && entryPath[path.size()] == '/'))
					found.push_back(entry.Path);
			}
			return found;
		}

		// The files at or below `path` (a file is itself, a directory every file below it), sorted; empty when it does not
		// exist.
		static std::vector<VfsPath> ListFilesAt(const VirtualFileSystem& vfs, const VfsPath& path)
		{
			const Result<FileInfo> info = vfs.GetInfo(path);
			if (!info)
				return {};
			if (!info->IsDirectory)
				return { path };
			std::vector<VfsPath> files;
			if (const Result<std::vector<VfsEntry>> entries = vfs.List(path, true); entries)
			{
				for (const VfsEntry& entry : *entries)
				{
					if (!entry.Info.IsDirectory)
						files.push_back(entry.Path);
				}
			}
			return files;
		}

		// Brings `provenance` in step with one AssetWriter event (§13.4, §13.12 rules 1 and 3): a written file under a recorded
		// path is recorded with `attribution`; the entries of a removed path, or of a move's source, are forgotten; every file
		// a move brought to a recorded path is recorded with its hash and `attribution`. Returns whether anything changed.
		static bool ApplyWriteToProvenance(ProvenanceRecorder& provenance, const VirtualFileSystem& vfs, const WriteAttribution& attribution,
			const AssetWriteEvent& event)
		{
			if (event.Path.GetScheme() != ProjectScheme)
				return false;
			bool changed = false;
			switch (event.Kind)
			{
				case AssetWriteKind::Written:
					if (ProvenanceRecorder::IsRecordedPath(event.Path.GetPath()))
					{
						provenance.Record(event.Path.GetPath(), event.ContentHash, attribution);
						changed = true;
					}
					break;
				case AssetWriteKind::Removed:
					for (const std::string& path : FindProvenanceEntries(provenance, event.Path.GetPath()))
						changed = provenance.Remove(path) || changed;
					break;
				case AssetWriteKind::Moved:
					for (const std::string& path : FindProvenanceEntries(provenance, event.From.GetPath()))
						changed = provenance.Remove(path) || changed;
					for (const VfsPath& file : ListFilesAt(vfs, event.Path))
					{
						if (!ProvenanceRecorder::IsRecordedPath(file.GetPath()))
							continue;
						Result<Buffer> bytes = vfs.ReadFile(file);
						if (!bytes)
						{
							ENGINE_WARN("Provenance could not record the moved file '{}': {}", file.ToString(), bytes.error().ToString());
							continue;
						}
						provenance.Record(file.GetPath(), XXH64(*bytes), attribution);
						changed = true;
					}
					break;
				case AssetWriteKind::DirectoryCreated:
					break;
			}
			return changed;
		}

		// True when the external change `change` concerns the open scene `scene` (opened from `scenePath`): its own file, or a
		// prefab asset one of its instances names (§7.5 race rule 3).
		static bool AffectsOpenScene(const Scene& scene, const std::optional<VfsPath>& scenePath, const AssetExternalChange& change)
		{
			if (scenePath.has_value() && *scenePath == change.Path)
				return true;
			if (!change.Handle.IsValid() || change.Type != AssetType::Prefab)
				return false;
			bool found = false;
			scene.ForEachCanonical([&found, &change](ConstEntity entity)
			{
				if (const PrefabInstanceComponent* instance = entity.TryGetComponent<PrefabInstanceComponent>(); instance != nullptr)
					found = found || instance->Prefab.GetHandle() == change.Handle;
			});
			return found;
		}

		// The canonical JSON of `root` and every entity below it, depth first in child order: everything an instance update
		// can change (members, user children and the root's override records).
		static Json CaptureSubtree(const Scene& scene, UUID root)
		{
			Json states = Json::array();
			std::vector<UUID> pending = { root };
			while (!pending.empty())
			{
				const ConstEntity entity = scene.FindEntityByID(pending.back());
				pending.pop_back();
				if (!entity.IsValid())
					continue;
				Result<Json> state = SceneSerializer::EntityToJson(entity);
				states.push_back(state ? std::move(*state) : Json());
				const std::span<const UUID> children = entity.GetChildren();
				for (auto child = children.rbegin(); child != children.rend(); ++child)
					pending.push_back(*child);
			}
			return states;
		}

		// The parent's UUID, or the invalid UUID for a root.
		static UUID GetParentUUID(ConstEntity entity)
		{
			const ConstEntity parent = entity.GetParent();
			return parent.IsValid() ? parent.GetUUID() : UUID();
		}

		// EditorContext::UpdatePrefabInstances without its atomicity: every instance of `prefabs` (any prefab when empty)
		// rebuilt from the prefab's current version, its overrides refreshed afterwards. Returns whether anything changed.
		static Result<bool> RebuildPrefabInstances(EditorAssetManager& assets, const TypeRegistry& registry, Scene& scene,
			std::span<const AssetHandle> prefabs)
		{
			const std::vector<std::pair<UUID, AssetHandle>> instances = Utils::FindPrefabInstances(scene, prefabs);
			if (instances.empty())
				return false;

			// Each prefab once, at its current version; a prefab that is not registered (deleted, or never imported) leaves its
			// instances as they are (PREFAB_MISSING_ASSET, ProjectValidator).
			std::map<AssetHandle, Prefab> loaded;
			std::set<AssetHandle> missing;
			const PrefabOptions options{ .Schemas = nullptr };
			LoadReport report;
			bool changed = false;
			for (const auto& [rootID, handle] : instances)
			{
				if (missing.contains(handle))
					continue;
				auto prefab = loaded.find(handle);
				if (prefab == loaded.end())
				{
					LoadReport prefabReport;
					Result<Prefab> current = LoadPrefabAsset(assets, handle, registry, prefabReport);
					if (!current)
					{
						if (current.error().GetCode() != ErrorCode::NotFound)
							return std::unexpected(std::move(current).error().WithContext("updating the prefab instances"));
						missing.insert(handle);
						continue;
					}
					prefab = loaded.emplace(handle, std::move(*current)).first;
				}

				Entity root = scene.FindEntityByID(rootID);
				if (!root.IsValid())
					continue;
				const Json before = Utils::CaptureSubtree(scene, rootID);
				ENGINE_TRY(WithContext(PrefabInstantiator::UpdateInstance(scene, root, prefab->second, options, report),
					std::format("updating the instance '{}' of '{}'", scene.GetEntityPath(root), assets.GetReferencePath(handle))));
				// The recorded overrides become exactly what still differs from the new version (§5.5: overrides are derived by
				// diffing, ADR 0006 decision 17). A reference this cannot store keeps the records as they were.
				if (Status refreshed = PrefabInstantiator::RefreshOverrides(root, prefab->second, options); !refreshed)
					ENGINE_WARN("The overrides of the prefab instance '{}' were kept as recorded: {}", scene.GetEntityPath(root), refreshed.error().ToString());
				changed = changed || Utils::CaptureSubtree(scene, rootID) != before;
			}
			for (const LoadDiagnostic& diagnostic : report.Diagnostics)
			{
				ENGINE_WARN("Updating the prefab instances of scene '{}': {}{}{}", scene.GetName(), diagnostic.Message,
					diagnostic.JsonPointer.empty() ? std::string() : std::format(" at {}", diagnostic.JsonPointer),
					diagnostic.Code.empty() ? std::string() : std::format(" ({})", diagnostic.Code));
			}
			return changed;
		}

		// "Update Prefab Instances" (EditorContext::CreatePrefabUpdateCommand): the entity changes of one prefab update,
		// applied by Execute and reverted by Undo through SceneEditCommand::ApplyChanges, the one scene-copy path (§1.3), like a
		// SceneEditCommand whose first Execute performs the edit. It is built unapplied, so the history records the revision
		// from before the update, and every execution appends the events of what it changed.
		class PrefabUpdateCommand final : public Command
		{
		public:
			PrefabUpdateCommand(std::string label, std::vector<SceneEntityChange> changes)
				: m_Label(std::move(label)), m_Changes(std::move(changes))
			{
				for (const SceneEntityChange& change : m_Changes)
				{
					m_MemorySize += sizeof(SceneEntityChange);
					m_MemorySize += change.Before != nullptr ? change.Before->dump().size() : 0;
					m_MemorySize += change.After != nullptr ? change.After->dump().size() : 0;
				}
			}

			[[nodiscard]] Status Execute(EditorContext& context) override
			{
				ENGINE_TRY(SceneEditCommand::ApplyChanges(context.GetScene(), m_Changes, true));
				SceneEditCommand::AppendChangeEvents(context, m_Changes, true);
				return {};
			}

			[[nodiscard]] Status Undo(EditorContext& context) override
			{
				// An in-memory restore of states that were valid when captured: a failure is a bug (Command.h).
				const Status restored = SceneEditCommand::ApplyChanges(context.GetScene(), m_Changes, false);
				ENGINE_ASSERT(restored.has_value(), "undoing '{}' failed: {}", m_Label, restored ? std::string() : restored.error().ToString());
				ENGINE_TRY(restored);
				SceneEditCommand::AppendChangeEvents(context, m_Changes, false);
				return {};
			}

			[[nodiscard]] std::string_view GetLabel() const override { return m_Label; }
			[[nodiscard]] size_t GetMemorySize() const override { return m_MemorySize; }

			[[nodiscard]] Status ReplayOnSceneCopy(Scene& scene, bool after) const override
			{
				return SceneEditCommand::ApplyChanges(scene, m_Changes, after);
			}
		private:
			std::string m_Label;
			std::vector<SceneEntityChange> m_Changes;
			size_t m_MemorySize = 0;
		};

	}

	// --- EditorContext ---------------------------------------------------------------------------------------------------

	EditorContext::EditorContext(ConstructionKey /*key*/, EngineContext& engine, const EditorContextSpecification& specification,
		const Random::State& idGeneratorState)
		: m_Engine(&engine), m_Specification(specification), m_IdGenerator(UUIDGenerator::CreateRandom(idGeneratorState)), m_Importers(CreateScope<ImporterRegistry>()), m_Loaders(CreateScope<AssetLoaderRegistry>()), m_History(specification.HistoryLimits), m_Play(CreateScope<EditorPlayController>(*this))
	{
		// The editor's asset services (§3 rule 4): the built-in importers and loaders, and the manager the engine context
		// serves to engine code.
		RegisterBuiltinImporters(*m_Importers);
		RegisterBuiltinLoaders(*m_Loaders);
		m_Assets = CreateScope<EditorAssetManager>(EditorAssetManagerSpecification{
			.Vfs = &engine.GetVfs(),
			.Jobs = &engine.GetJobSystem(),
			.MainThread = &engine.GetMainThreadQueue(),
			.Events = &engine.GetEventLog(),
			.Registry = &engine.GetTypeRegistry(),
			.IdGenerator = &m_IdGenerator,
			.Importers = m_Importers.get(),
			.Loaders = m_Loaders.get(),
			.EnvironmentBaker = specification.EnvironmentBaker,
			.ScriptDiagnostics = nullptr,
			.EngineAssetGenerators = {},
		});
		engine.SetAssetManager(m_Assets.get());

		// Provenance for every write of the manager's AssetWriter, the editor's own and the manager's .meta writes alike
		// (§13.12 rule 1); never during a dry run, and not for read-only projects (no provenance).
		m_Assets->SetWriteObserver([this](const AssetWriteEvent& event)
		{
			if (IsDryRun() || !m_Provenance.has_value())
				return;
			VirtualFileSystem& vfs = GetVfs();
			if (!Utils::ApplyWriteToProvenance(*m_Provenance, vfs, m_Attribution, event))
				return;
			Status saved = m_Provenance->Save(vfs);
			if (saved)
				return;
			Error error = Utils::ToEditorFileError(std::move(saved).error())
							  .WithContext(std::format("'{}' changed, but the project's provenance could not be saved", event.Path.ToString()));
			if (m_CollectProvenanceErrors)
			{
				if (!m_ProvenanceError.has_value())
					m_ProvenanceError = std::move(error);
				return;
			}
			ENGINE_ERROR("{}", error.ToString());
		});

		// §7.5 race rule 3: an external change of the open scene's file, or of a prefab it instantiates, is reported and never
		// applied to the scene in memory.
		// §7.5 race rule 4: a reload during ordinary play marks the session modified, whether an external change or the
		// editor's own write started it (a lockstep session defers reloads until it ends, EditorPlayController, so none reaches
		// it).
		m_Assets->SetReloadListener([this](AssetHandle /*source*/)
		{
			if (PlaySession* session = m_Play->GetSession(); session != nullptr)
				session->MarkModified();
		});
		m_Assets->SetExternalChangeListener([this](const AssetExternalChange& change)
		{
			if (m_Scene == nullptr || !Utils::AffectsOpenScene(*m_Scene, m_ScenePath, change))
				return;
			m_SceneChangedOnDisk = true;
			const bool dirty = IsSceneDirty();
			const std::string path(change.Path.GetPath());
			AppendEvent(EngineEvent{ .Seq = 0, .Tick = std::nullopt, .Type = EngineEventType::SceneChangedOnDisk, .Id = change.Handle, .Path = path, .Name = {}, .Message = {}, .Dirty = dirty });
			ENGINE_WARN("'{}' changed on disk; the open scene '{}' was left as it is{} (scene.open {{reload: true}} adopts the disk version)", path,
				m_Scene->GetName(), dirty ? " with its unsaved changes" : "");
		});
	}

	EditorContext::~EditorContext()
	{
		CloseProject();
		// The manager outlives this object's other members: it must not call back into them.
		m_Assets->SetWriteObserver({});
		m_Assets->SetExternalChangeListener({});
		m_Assets->SetReloadListener({});
		if (m_Engine->GetAssetManager() == m_Assets.get())
			m_Engine->SetAssetManager(nullptr);
	}

	void EditorContext::Update(double nowSeconds)
	{
		if (HasProject())
			m_Assets->Update(nowSeconds);
	}

	Status EditorContext::CheckProjectWrite(const VfsPath& path, std::string_view action) const
	{
		ENGINE_ASSERT(path.GetScheme() == Utils::ProjectScheme, "EditorContext: project writes take project:// paths, not '{}'", path.ToString());
		if (!HasProject())
			return MakeError(ErrorCode::InvalidState, "cannot {} '{}': no project is open", action, path.ToString());
		if (IsReadOnly() && !IsDryRun())
		{
			return std::unexpected(Error(ErrorCode::PermissionDenied, std::format("cannot {} '{}': the project is open read-only", action, path.ToString()))
					.WithHint("open the project without --read-only, or close the editor that holds it"));
		}
		return {};
	}

	Status EditorContext::MoveProjectFile(const VfsPath& from, const VfsPath& to)
	{
		ENGINE_ASSERT(to.GetScheme() == Utils::ProjectScheme, "EditorContext::MoveProjectFile moves to project:// paths, not '{}'", to.ToString());
		ENGINE_TRY(CheckProjectWrite(from, "move"));
		Utils::ProvenanceErrorScope provenance(m_CollectProvenanceErrors, m_ProvenanceError);
		ENGINE_TRY(Utils::ToEditorFileStatus(m_Assets->GetWriter().Move(from, to)));
		return provenance.Take();
	}

	Status EditorContext::RemoveProjectFile(const VfsPath& path)
	{
		ENGINE_TRY(CheckProjectWrite(path, "remove"));
		VirtualFileSystem& vfs = GetVfs();
		Result<FileInfo> info = vfs.GetInfo(path);
		if (!info)
			return std::unexpected(Utils::ToEditorFileError(std::move(info).error()));
		if (info->IsDirectory)
		{
			Result<std::vector<VfsEntry>> entries = vfs.List(path, false);
			if (!entries)
				return std::unexpected(Utils::ToEditorFileError(std::move(entries).error()));
			if (!entries->empty())
			{
				return std::unexpected(Error(ErrorCode::InvalidState, std::format("cannot remove '{}': the folder is not empty", path.ToString()))
						.WithHint("delete the assets in it first (asset.delete moves them to the trash)"));
			}
		}
		Utils::ProvenanceErrorScope provenance(m_CollectProvenanceErrors, m_ProvenanceError);
		ENGINE_TRY(Utils::ToEditorFileStatus(m_Assets->GetWriter().Remove(path)));
		return provenance.Take();
	}

	Status EditorContext::CreateProjectDirectory(const VfsPath& directory)
	{
		ENGINE_TRY(CheckProjectWrite(directory, "create"));
		return Utils::ToEditorFileStatus(m_Assets->GetWriter().CreateDirectories(directory));
	}

	Result<bool> EditorContext::UpdatePrefabInstances(Scene& scene, std::span<const AssetHandle> prefabs)
	{
		// Atomic for a caller that does not track the scene itself (scene.open's loaded scene): a failure restores every
		// instance it already rebuilt. CreatePrefabUpdateCommand tracks and rolls back on its own.
		ChangeTracker& tracker = scene.GetChangeTracker();
		if (tracker.IsTracking())
			return Utils::RebuildPrefabInstances(*m_Assets, GetTypeRegistry(), scene, prefabs);
		tracker.Begin();
		Result<bool> updated = Utils::RebuildPrefabInstances(*m_Assets, GetTypeRegistry(), scene, prefabs);
		const std::vector<EntityChange> tracked = tracker.End();
		if (!updated)
			Utils::RollBackTrackedChanges(scene, tracked, "Update Prefab Instances");
		return updated;
	}

	Result<Scope<Command>> EditorContext::CreatePrefabUpdateCommand(std::span<const AssetHandle> prefabs)
	{
		if (m_Scene == nullptr)
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "the prefab instances cannot be updated: no scene is open")
					.WithHint("open a scene with scene.open first"));
		}
		Scene& scene = *m_Scene;
		ENGINE_ASSERT(!scene.GetChangeTracker().IsTracking(), "EditorContext::CreatePrefabUpdateCommand while a SceneEdit is active");

		// The update runs once under the change tracker to find what it changes, and is then rolled back: the command applies
		// those changes when it is executed (PrefabUpdateCommand).
		scene.GetChangeTracker().Begin();
		Result<bool> updated = UpdatePrefabInstances(scene, prefabs);
		const std::vector<EntityChange> tracked = scene.GetChangeTracker().End();

		std::vector<SceneEntityChange> changes;
		changes.reserve(tracked.size());
		Status recorded;
		for (const EntityChange& change : tracked)
		{
			if (change.Kind != EntityChangeKind::Created && change.Before == nullptr)
			{
				recorded = MakeError(ErrorCode::Validation, "the prefab update cannot be recorded: the state of entity {} before it could not be serialized",
					change.EntityID);
				break;
			}
			SceneEntityChange entry{ .EntityID = change.EntityID,
				.Before = change.Kind == EntityChangeKind::Created ? nullptr : change.Before,
				.After = nullptr,
				.ParentBefore = change.ParentBefore,
				.SiblingIndexBefore = change.SiblingIndexBefore,
				.ParentAfter = UUID(),
				.SiblingIndexAfter = 0 };
			if (const ConstEntity entity = std::as_const(scene).FindEntityByID(change.EntityID); entity.IsValid())
			{
				Result<Json> after = SceneSerializer::EntityToJson(entity);
				if (!after)
				{
					recorded = std::unexpected(std::move(after).error().WithContext("recording the prefab update"));
					break;
				}
				entry.After = CreateRef<const Json>(std::move(*after));
				entry.ParentAfter = Utils::GetParentUUID(entity);
				entry.SiblingIndexAfter = entity.GetSiblingIndex();
			}
			if (entry.Before != nullptr || entry.After != nullptr)
				changes.push_back(std::move(entry));
		}

		// Back to the state before the update, whatever happened.
		Utils::RollBackTrackedChanges(scene, tracked, "Update Prefab Instances");

		if (!updated)
			return std::unexpected(std::move(updated).error());
		ENGINE_TRY(recorded);
		if (changes.empty())
			return Scope<Command>();
		return Scope<Command>(CreateScope<Utils::PrefabUpdateCommand>("Update Prefab Instances", std::move(changes)));
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

		m_Project = std::move(project);
		m_Provenance = std::move(provenance);
		m_Scene.reset();
		m_ScenePath.reset();
		m_SceneDirty = false;
		m_SceneChangedOnDisk = false;
		m_History.Clear();
		m_Selection.clear();

		// The project's assets (M6): scan, .meta files for new sources (recorded in provenance through the write observer,
		// with the current attribution) and hot reload. A project the editor cannot index stays closed.
		Result<AssetRefreshReport> assets = OpenProjectAssets();
		if (!assets)
		{
			m_Provenance.reset();
			m_Project.reset();
			Utils::UnmountLogged(vfs, Utils::CacheScheme);
			Utils::UnmountLogged(vfs, Utils::ProjectScheme);
			return std::unexpected(std::move(assets).error().WithContext("opening the project's assets"));
		}

		if (vfs.IsMounted("user"))
		{
			if (Status added = ProjectManager::AddRecentProject(vfs, m_Project->GetProjectFile()); !added)
				ENGINE_WARN("Could not add the project to the recent list: {}", added.error().ToString());
		}
		ENGINE_INFO("Opened project '{}' at '{}'{} ({} asset .meta files, {} new)", m_Project->GetSettings().Name,
			FileSystem::PathToUtf8(m_Project->GetRoot()), m_Project->IsReadOnly() ? " read-only" : "", assets->MetaCount, assets->CreatedMetas.size());
		return {};
	}

	Result<AssetRefreshReport> EditorContext::OpenProjectAssets()
	{
		ENGINE_TRY_ASSIGN(const VfsPath assetsRoot, VfsPath::Create(Utils::ProjectScheme, "Assets"));
		ENGINE_TRY_ASSIGN(const VfsPath cacheRoot, VfsPath::Parse("cache://"));
		VirtualFileSystem& vfs = GetVfs();
		// Every project has an Assets folder (ProjectManager's templates); one made by hand without it gets it.
		if (!m_Project->IsReadOnly() && !vfs.Exists(assetsRoot))
			ENGINE_TRY(Utils::ToEditorFileStatus(vfs.CreateDirectories(assetsRoot)));
		return m_Assets->OpenProject(AssetProjectSpecification{
			.AssetsRoot = assetsRoot,
			.CacheRoot = cacheRoot,
			.ReadOnly = m_Project->IsReadOnly(),
			.HotReload = true,
		});
	}

	void EditorContext::CloseProject()
	{
		if (!HasProject())
			return;
		ENGINE_ASSERT(m_DryRun == nullptr && m_Transaction == nullptr, "EditorContext::CloseProject inside a dry run or a transaction");
		// A play session refers to the project's settings and assets: it stops before they go (M7).
		if (m_Play->IsPlaying())
		{
			const Status stopped = m_Play->Stop();
			ENGINE_ASSERT(stopped.has_value(), "stopping the play session of a closing project failed: {}", stopped ? std::string() : stopped.error().ToString());
		}
		CloseScene();
		// The manager's jobs and hot reload use project:// and cache://: they stop before the mounts go.
		m_Assets->CloseProject();
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
		m_SceneChangedOnDisk = false;
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
		m_SceneChangedOnDisk = false;
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
		m_SceneChangedOnDisk = false;
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
		// While playing, the selection may name entities of the play scene too (the scene the editor shows, M10); Stop sets it
		// again, so it is restored by UUID to the entities of the edit scene (§5.6).
		const PlaySession* session = m_Play->GetSession();
		std::vector<UUID> kept;
		kept.reserve(selection.size());
		for (const UUID id : selection)
		{
			const bool inEditScene = m_Scene != nullptr && std::as_const(*m_Scene).FindEntityByID(id).IsValid();
			const bool inPlayScene = session != nullptr && session->GetScene().FindEntityByID(id).IsValid();
			if (!inEditScene && !inPlayScene)
				continue;
			if (std::find(kept.begin(), kept.end(), id) == kept.end())
				kept.push_back(id);
		}
		m_Selection = std::move(kept);
	}

	Status EditorContext::WriteProjectFile(const VfsPath& path, std::span<const std::byte> data)
	{
		ENGINE_TRY(CheckProjectWrite(path, "write"));

		// Through the asset manager's writer (§7.5 race rule 1): no echo from hot reload, the registry updated, reimports
		// scheduled, and provenance recorded by the write observer.
		AssetWriter& writer = m_Assets->GetWriter();
		Utils::ProvenanceErrorScope provenance(m_CollectProvenanceErrors, m_ProvenanceError);
		if (const VfsPath parent = path.GetParent(); !parent.IsRoot() && !GetVfs().Exists(parent))
			ENGINE_TRY(Utils::ToEditorFileStatus(writer.CreateDirectories(parent)));
		ENGINE_TRY(Utils::ToEditorFileStatus(writer.Write(path, data)));
		return provenance.Take();
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
		size_t TransientPlayFirst = 0;          // EditorContext::m_TransientPlayUndos' size when this transaction opened
	};

	namespace Utils {

		// Runs the transient play-scene undos[first ..] newest first and removes them (EditorContext::m_TransientPlayUndos).
		static void UndoTransientPlayEdits(std::vector<UniqueFunction<void()>>& undos, size_t first)
		{
			while (undos.size() > first)
			{
				UniqueFunction<void()> undo = std::move(undos.back());
				undos.pop_back();
				undo();
			}
		}

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
		m_State->TransientPlayFirst = context.m_TransientPlayUndos.size();
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
		// The play-scene changes stay (§13.4: transient, never recorded); a dry run still takes them back when it ends.
		if (!context.IsDryRun())
			context.m_TransientPlayUndos.resize(state.TransientPlayFirst);
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
		// The play-scene changes made inside the transaction go back with its commands (play-scene edits record none).
		Utils::UndoTransientPlayEdits(context.m_TransientPlayUndos, state.TransientPlayFirst);
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
		// The asset manager writes into the overlay and keeps a snapshot to restore (M6): registry updates, imports and
		// versions of the dry run leave no trace.
		context.m_Assets->BeginDryRun();
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

		// The play scene is real during a dry run: its changes are taken back here, newest first (§13.4).
		Utils::UndoTransientPlayEdits(context.m_TransientPlayUndos, 0);
		context.m_Assets->EndDryRun();
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
