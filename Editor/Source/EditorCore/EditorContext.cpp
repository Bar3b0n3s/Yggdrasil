#include "EditorPCH.h"
#include "EditorCore/EditorContext.h"

#include "EditorCore/Audio/AudioPreview.h"
#include "EditorCore/Commands/AssetEditCommand.h"
#include "EditorCore/Commands/CompositeCommand.h"
#include "EditorCore/Commands/SceneEditCommand.h"
#include "EditorCore/EngineAssetGenerators.h"
#include "EditorCore/Native/NativeModuleHost.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Private/EditorFileError.h"
#include "EditorCore/Private/PrefabInstances.h"
#include "EditorCore/Private/SceneEditRollback.h"
#include "EditorCore/Project/ProjectValidator.h"
#include "EditorCore/Scripting/EditorScriptService.h"
#include "EditorCore/Scripting/ScriptTypeChecker.h"
#include "Engine/App/EngineContext.h"
#include "Engine/Asset/AssetLoaderRegistry.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/AssetPipeline/ImporterRegistry.h"
#include "Engine/AssetPipeline/Importers/PrefabImporter.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Jobs/JobSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Core/Mounts/OverlayMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Native/NativeComponents.h"
#include "Engine/Native/NativeModule.h"
#include "Engine/Platform/SecureRandom.h"
#include "Engine/Platform/SharedLibrary.h"
#include "Engine/Project/ProjectSerializer.h"
#include "Engine/Scene/ChangeTracker.h"
#include "Engine/Scene/Components/PrefabInstanceComponent.h"
#include "Engine/Scene/Components/UnknownComponentsComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/PrefabAsset.h"
#include "Engine/Scene/PrefabInstantiator.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Session/PlaySession.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <format>
#include <functional>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <string_view>
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

		// The project-relative path that the file `destination` (at or below `to`) had before a move from `from` to `to`.
		static std::string GetMoveSource(std::string_view from, std::string_view to, std::string_view destination)
		{
			if (destination.size() == to.size())
				return std::string(from);
			return std::format("{}{}", from, destination.substr(to.size()));
		}

		// Brings `provenance` in step with one AssetWriter event (§13.4, §13.12 rules 1 and 3): a written file under a recorded
		// path is recorded with `attribution`; the entries of a removed path, or of a move's source, are forgotten. A move
		// carries a source's entry to the file's new recorded path, with `attribution`, only when the source had an entry whose
		// hash equals the moved bytes: a move writes no content, so a file the editor did not write (no entry, or bytes changed
		// since its entry) gets no entry at its destination and the audit still reports it. Returns whether anything changed.
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
				{
					// The recorded hash of every source file, taken before the source's entries are forgotten.
					std::map<std::string, uint64_t, std::less<>> sourceHashes;
					for (const std::string& path : FindProvenanceEntries(provenance, event.From.GetPath()))
					{
						if (const ProvenanceEntry* entry = provenance.Find(path); entry != nullptr)
							sourceHashes.emplace(path, entry->Hash);
						changed = provenance.Remove(path) || changed;
					}
					for (const VfsPath& file : ListFilesAt(vfs, event.Path))
					{
						if (!ProvenanceRecorder::IsRecordedPath(file.GetPath()))
							continue;
						const auto recorded = sourceHashes.find(GetMoveSource(event.From.GetPath(), event.Path.GetPath(), file.GetPath()));
						if (recorded == sourceHashes.end())
							continue;
						Result<Buffer> bytes = vfs.ReadFile(file);
						if (!bytes)
						{
							ENGINE_WARN("Provenance could not record the moved file '{}': {}", file.ToString(), bytes.error().ToString());
							continue;
						}
						if (XXH64(*bytes) != recorded->second)
							continue;
						provenance.Record(file.GetPath(), recorded->second, attribution);
						changed = true;
					}
					break;
				}
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

		// `prefabs` with every prefab nesting one of them (Docs/Decisions/0031-m20-contract.md decision 8), sorted without
		// duplicates: the prefabs whose outermost instances an update of `prefabs` rebuilds. Empty for empty `prefabs`.
		// Errors: Validation PREFAB_CYCLE when the propagation from `prefabs` meets a nesting cycle
		// (EditorContext::GetNestingPrefabs).
		static Result<std::vector<AssetHandle>> AddNestingPrefabs(const EditorContext& context, std::span<const AssetHandle> prefabs)
		{
			std::vector<AssetHandle> rebuilt(prefabs.begin(), prefabs.end());
			if (!prefabs.empty() && context.HasProject())
			{
				// The prefabs that nest them (a deleted prefab nests nothing and is nested by nothing any more).
				const AssetRegistry& registry = context.GetAssets().GetRegistry();
				std::vector<AssetHandle> registered;
				std::ranges::copy_if(prefabs, std::back_inserter(registered), [&registry](AssetHandle prefab)
				{
					const std::optional<AssetRegistry::Location> location = registry.Locate(prefab);
					return location.has_value() && location->Type == AssetType::Prefab;
				});
				ENGINE_TRY_ASSIGN(const std::vector<AssetHandle> nesting, context.GetNestingPrefabs(registered));
				rebuilt.insert(rebuilt.end(), nesting.begin(), nesting.end());
			}
			std::ranges::sort(rebuilt);
			const auto [first, last] = std::ranges::unique(rebuilt);
			rebuilt.erase(first, last);
			return rebuilt;
		}

		// EditorContext::UpdatePrefabInstances without its atomicity: every outermost instance of `prefabs` (any prefab when
		// empty), or of a prefab nesting one of them (Docs/Decisions/0031-m20-contract.md decision 8), rebuilt from the
		// prefab's current version, its overrides refreshed afterwards; nested instances follow their outermost ones.
		// Returns whether anything changed. Errors: Validation PREFAB_CYCLE when the propagation from `prefabs` meets a nesting
		// cycle (EditorContext::GetNestingPrefabs); those of the rebuild.
		static Result<bool> RebuildPrefabInstances(EditorContext& context, Scene& scene,
			std::span<const AssetHandle> prefabs)
		{
			ENGINE_TRY_ASSIGN(const std::vector<AssetHandle> rebuilt, AddNestingPrefabs(context, prefabs));
			const std::vector<std::pair<UUID, AssetHandle>> instances = Utils::FindPrefabInstances(scene, rebuilt);
			if (instances.empty())
				return false;
			ENGINE_TRY_ASSIGN(const Ref<const ScriptFieldSchemaSource> schemas, context.GetScriptSchemaSnapshot());
			EditorAssetManager& assets = context.GetAssets();
			const TypeRegistry& registry = context.GetTypeRegistry();

			// Each prefab once, at its current version; a prefab that is not registered (deleted, or never imported) leaves its
			// instances as they are (PREFAB_MISSING_ASSET, ProjectValidator).
			std::map<AssetHandle, Prefab> loaded;
			std::set<AssetHandle> missing;
			const PrefabOptions options{ .Schemas = schemas.get() };
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
					Result<Prefab> current = LoadPrefabAsset(assets, handle, registry, options.Schemas, prefabReport);
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

		// The overrides of `scene`'s outermost instances of the prefabs `versions` holds, recorded against the version it holds
		// for each (PrefabInstantiator::RefreshOverrides), as Utils::RecordPrefabOverrides records the addressed scene's
		// before a prefab write. Errors: those of the schema snapshot and of the refresh.
		static Status RecordPrefabOverridesAgainst(EditorContext& context, Scene& scene, const std::map<AssetHandle, Prefab>& versions)
		{
			std::vector<AssetHandle> prefabs;
			prefabs.reserve(versions.size());
			for (const auto& [handle, prefab] : versions)
			{
				static_cast<void>(prefab);
				prefabs.push_back(handle); // sorted, as FindPrefabInstances needs
			}
			const std::vector<std::pair<UUID, AssetHandle>> instances = Utils::FindPrefabInstances(scene, prefabs);
			if (instances.empty())
				return {};
			ENGINE_TRY_ASSIGN(const Ref<const ScriptFieldSchemaSource> schemas, context.GetScriptSchemaSnapshot());
			const PrefabOptions options{ .Schemas = schemas.get() };
			for (const auto& [rootID, handle] : instances)
			{
				const Entity root = scene.FindEntityByID(rootID);
				const auto version = versions.find(handle);
				if (!root.IsValid() || version == versions.end())
					continue;
				ENGINE_TRY(WithContext(PrefabInstantiator::RefreshOverrides(root, version->second, options),
					std::format("recording the overrides of the prefab instance '{}'", scene.GetEntityPath(root))));
			}
			return {};
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

		// "Update Nesting Prefabs" (EditorContext::CreateNestingPrefabUpdateCommand, Docs/Decisions/0031-m20-contract.md
		// decision 8): the rewritten documents of the prefabs nesting a changed prefab, written by an AssetEditCommand and
		// imported at once, in nesting order, so the prefab update composed after it rebuilds instances from them. Undo
		// writes the previous documents back and imports them too. Both directions are atomic (Command.h): when an import
		// fails after the files were written, the other version of every file is written back and imported again, so a
		// failed Execute leaves the previous documents and a failed Undo leaves the command applied, and the import's error
		// is returned; a restore that fails too is logged at Error.
		class NestingPrefabUpdateCommand final : public Command
		{
		public:
			NestingPrefabUpdateCommand(Scope<AssetEditCommand> writes, std::vector<AssetHandle> prefabs)
				: m_Writes(std::move(writes)), m_Prefabs(std::move(prefabs))
			{
			}

			[[nodiscard]] Status Execute(EditorContext& context) override
			{
				ENGINE_TRY(m_Writes->Execute(context));
				if (Status imported = Import(context); !imported)
				{
					// The documents go back to their previous versions, imported again.
					Status restored = m_Writes->Undo(context);
					if (restored)
						restored = Import(context);
					if (!restored)
						ENGINE_ERROR("'{}' could not restore the prefabs after a failed import: {}", GetLabel(), restored.error().ToString());
					return imported;
				}
				return {};
			}

			[[nodiscard]] Status Undo(EditorContext& context) override
			{
				ENGINE_TRY(m_Writes->Undo(context));
				if (Status imported = Import(context); !imported)
				{
					// The command stays applied: the rewritten documents are written again and imported again.
					Status restored = m_Writes->Execute(context);
					if (restored)
						restored = Import(context);
					if (!restored)
						ENGINE_ERROR("'{}' could not restore the rewritten prefabs after a failed import: {}", GetLabel(), restored.error().ToString());
					return imported;
				}
				return {};
			}

			[[nodiscard]] std::string_view GetLabel() const override { return m_Writes->GetLabel(); }
			[[nodiscard]] bool ChangesScene() const override { return false; }
			[[nodiscard]] size_t GetMemorySize() const override { return sizeof(*this) + m_Writes->GetMemorySize() + m_Prefabs.size() * sizeof(AssetHandle); }
		private:
			// Imports every rewritten prefab, in nesting order. Errors: the first import's that fails.
			[[nodiscard]] Status Import(EditorContext& context) const
			{
				for (const AssetHandle prefab : m_Prefabs)
				{
					ENGINE_TRY_ASSIGN(const AssetImportOutcome outcome, context.GetAssets().Reimport(prefab));
					static_cast<void>(outcome);
				}
				return {};
			}
		private:
			Scope<AssetEditCommand> m_Writes;
			std::vector<AssetHandle> m_Prefabs; // in nesting order
		};

		// The prefab assets `prefab`'s document nests directly (the instances in it name them), from its current version;
		// `cache` keeps each document read. Errors: those of LoadPrefabAsset.
		static Result<std::vector<AssetHandle>> ReadNestedPrefabs(EditorAssetManager& assets, const TypeRegistry& registry, AssetHandle prefab,
			std::map<AssetHandle, std::vector<AssetHandle>>& cache)
		{
			if (const auto found = cache.find(prefab); found != cache.end())
				return found->second;
			LoadReport report;
			ENGINE_TRY_ASSIGN(const Prefab current, LoadPrefabAsset(assets, prefab, registry, nullptr, report));
			std::vector<AssetHandle> nested = GetNestedPrefabAssets(current, registry);
			cache.emplace(prefab, nested);
			return nested;
		}

		// The Validation error PREFAB_CYCLE naming the prefabs of `cycle` (one issue each, by path).
		static Error MakePrefabCycleError(const EditorAssetManager& assets, std::span<const AssetHandle> cycle)
		{
			std::vector<std::string> paths;
			for (const AssetHandle prefab : cycle)
				paths.push_back(assets.GetReferencePath(prefab));
			std::ranges::sort(paths);
			std::string names;
			std::vector<ErrorIssue> issues;
			for (const std::string& path : paths)
			{
				names += std::format("{}'{}'", names.empty() ? "" : ", ", path);
				issues.push_back(ErrorIssue{ "", std::format("{}: '{}' is on a nesting cycle", PrefabCycleCode, path), {}, {} });
			}
			const std::string cycleText = paths.size() == 1 ? std::format("the prefab {} nests itself", names)
															: std::format("the prefabs {} nest each other in a cycle", names);
			return Error(ErrorCode::Validation, std::format("{}: {}, so a change of it could never finish propagating", PrefabCycleCode, cycleText))
				.WithIssues(std::move(issues))
				.WithHint("remove a nested instance from one of the prefab files so that no prefab nests itself, directly or through others");
		}

		// The canonicalization of an opened scene's native components against the loaded module (decision 27), which
		// InstallScene runs for every way a scene opens (SetScene, AddScene, a recovery or reload), each finding logged; a
		// component its type rejects or the module does not register stays as written. Returns whether the scene changed.
		static bool CanonicalizeOpenedSceneNatives(Scene& scene, const NativeModuleTypes& types)
		{
			Result<NativeSceneReport> report = CanonicalizeNativeComponents(scene, &types, false);
			if (!report)
			{
				ENGINE_WARN("The native components of scene '{}' stay as written: {}", scene.GetName(), report.error());
				return false;
			}
			for (const LoadDiagnostic& diagnostic : report->Diagnostics)
			{
				ENGINE_WARN("Scene '{}', entity {} {}: {} ({})", scene.GetName(), diagnostic.Entity.ToString(), diagnostic.JsonPointer, diagnostic.Message,
					diagnostic.Code);
			}
			if (report->Changed)
				ENGINE_INFO("Scene '{}': its native components were made canonical for the loaded native module; save the scene to keep them", scene.GetName());
			return report->Changed;
		}

		// The --strict document check's native findings for one scene or prefab document (decision 27): every "Native."
		// component of every entity checked as CanonicalizeNativeComponents checks it with StrictUnknowns, so a component the
		// loaded module does not register (every one while none is loaded), one whose value its type rejects and one that
		// would migrate (a member dropped or reset) are each an error, located in the file. Read from the document itself, so
		// it holds whatever the serializer's own strict load reports.
		static std::vector<LoadDiagnostic> FindNativeDocumentProblems(const Json& document, const NativeModuleTypes* types)
		{
			std::vector<LoadDiagnostic> problems;
			const JsonReader reader(document);
			const std::optional<JsonReader> entities = reader.FindMember("Entities");
			const Result<size_t> count = entities.has_value() ? entities->GetArraySize() : Result<size_t>(0);
			for (size_t index = 0; count.has_value() && index < *count; ++index)
			{
				// A document whose structure is broken is the strict load's finding, not this check's.
				const Result<JsonReader> entity = entities->GetElement(index);
				if (!entity.has_value() || !entity->IsObject())
					continue;
				const std::optional<JsonReader> components = entity->FindMember("Components");
				if (!components.has_value() || !components->IsObject())
					continue;
				const Result<std::vector<std::string>> names = components->GetMemberNames();
				if (!names.has_value())
					continue;
				const std::optional<JsonReader> idMember = entity->FindMember("ID");
				const Result<UUID> read = idMember.has_value() ? idMember->ReadUUID() : Result<UUID>(UUID());
				const UUID id = read.value_or(UUID());
				for (const std::string& name : *names)
				{
					if (!IsNativeComponentName(name))
						continue;
					const Result<JsonReader> component = components->GetMember(name);
					if (!component.has_value())
						continue;
					const NativeTypeInfo* type = types != nullptr ? types->FindComponent(name) : nullptr;
					if (type == nullptr)
					{
						problems.push_back(LoadDiagnostic{ .Severity = DiagnosticSeverity::Error,
							.Code = std::string(NativeComponentUnregisteredCode),
							.Message = types == nullptr ? std::format("no native module is loaded to read '{}'", name)
														: std::format("the loaded native module registers no component '{}'", name),
							.JsonPointer = component->GetPointer(),
							.Entity = id });
						continue;
					}
					const Result<NativeCanonicalValue> canonical = CanonicalizeNativeValue(*type, component->GetValue(), component->GetPointer());
					if (!canonical)
					{
						const std::vector<ErrorIssue>& issues = canonical.error().GetIssues();
						if (issues.empty())
						{
							problems.push_back(LoadDiagnostic{ .Severity = DiagnosticSeverity::Error,
								.Code = std::string(NativeComponentInvalidCode),
								.Message = std::format("{} holds a value its type rejects: {}", name, canonical.error().GetMessageText()),
								.JsonPointer = component->GetPointer(),
								.Entity = id });
						}
						for (const ErrorIssue& issue : issues)
						{
							problems.push_back(LoadDiagnostic{ .Severity = DiagnosticSeverity::Error,
								.Code = std::string(NativeComponentInvalidCode),
								.Message = std::format("{} holds a value its type rejects: {}", name, issue.Message),
								.JsonPointer = issue.JsonPointer,
								.Entity = id });
						}
						continue;
					}
					for (const NativeValueNote& note : canonical->Report.Notes)
					{
						problems.push_back(LoadDiagnostic{ .Severity = DiagnosticSeverity::Error,
							.Code = note.Code,
							.Message = note.Message,
							.JsonPointer = JsonReader::AppendPointer(component->GetPointer(), note.Field),
							.Entity = id });
					}
				}
			}
			return problems;
		}

		// The hint of the --strict document check's native findings while no module is loaded (decision 27): why none is, as
		// the host knows it (the platform, the user's preference, the project opening's failed load), else that no build of
		// the sources as they are now exists (decision 31: a project's opening builds nothing).
		[[nodiscard]] static std::string MakeUnloadedNativeHint(const NativeModuleHost& host, const Status& opened)
		{
			constexpr std::string_view Needs = "a --strict editor reads native components only with the project's native module loaded";
			if (const Status supported = SharedLibrary::CheckSupported(); !supported)
				return std::format("{}, which this platform cannot load ({}): open the project without --strict", Needs, supported.error().GetMessageText());
			if (!host.IsAllowed())
				return std::format("{}: turn on Allow native code, build the module (native.build) and open the project again", Needs);
			if (!opened)
			{
				return std::format("{}, and it did not load when the project opened: {}; correct the module or the components named, build it again "
								   "(native.build) and open the project again",
					Needs, opened.error().ToString());
			}
			return std::format("{}, and no build of its sources as they are now exists: build the module (native.build) and open the project again", Needs);
		}

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
			.EngineAssetGenerators = GetEngineAssetGenerators(),
		});
		// Borrowed editor back-reference, cleared after CloseProject drains the manager and before members are destroyed.
		m_Assets->GetWriter().SetMutationGuard([this]()
		{
			return CheckMutationPermission();
		});
		engine.SetAssetManager(m_Assets.get());
		// M12: the asset browser's audio preview, when the engine context has an audio engine.
		if (AudioEngine* audio = engine.GetAudioEngine(); audio != nullptr)
			m_AudioPreview = CreateScope<AudioPreview>(*audio, *m_Assets);
		// M19 (Docs/Decisions/0030-m19-contract.md decisions 29 to 31): the native module host, which reads the user's Allow
		// native code preference now and again when a project opens; the project's open and close and every frame reach it.
		m_Native = CreateScope<NativeModuleHost>(MakeEditorNativeModuleHostSpecification(specification.NativeBuild, specification.NativeLoader));
		if (Status preference = m_Native->ReadPreference(*this); !preference)
			ENGINE_WARN("{}", preference.error());

		// Provenance for every write of the manager's AssetWriter, the editor's own and the manager's .meta writes alike
		// (§13.12 rule 1); never during a dry run, and not for read-only projects (no provenance). A write an import's
		// publication made carries the write context the import started under, whose attribution it is recorded with
		// (Docs/Decisions/0026-m16-contract.md decision 10); every other write gets the current attribution.
		m_Assets->SetWriteObserver([this](const AssetWriteEvent& event)
		{
			if (IsDryRun() || !m_Provenance.has_value())
				return;
			VirtualFileSystem& vfs = GetVfs();
			const WriteAttribution* attribution = &m_Attribution;
			if (event.WriteContextId != 0)
			{
				if (const auto context = m_WriteContexts.find(event.WriteContextId); context != m_WriteContexts.end())
					attribution = &context->second.Attribution;
			}
			if (!Utils::ApplyWriteToProvenance(*m_Provenance, vfs, *attribution, event))
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
		m_Assets->SetReloadListener([this](AssetHandle source)
		{
			m_Play->OnAssetReload(source);
		});
		// Open scenes (M20, Docs/Decisions/0031-m20-contract.md decision 15): every open scene the change concerns is flagged
		// and reported on its own, in tab order.
		m_Assets->SetExternalChangeListener([this](const AssetExternalChange& change)
		{
			const std::string path(change.Path.GetPath());
			const auto report = [this, &change, &path](const Scene& scene, bool dirty)
			{
				AppendEvent(EngineEvent{ .Seq = 0, .Tick = std::nullopt, .Type = EngineEventType::SceneChangedOnDisk, .Id = change.Handle, .Path = path, .Name = {}, .Message = {}, .Dirty = dirty });
				ENGINE_WARN("'{}' changed on disk; the open scene '{}' was left as it is{} (scene.open {{reload: true}} adopts the disk version)", path,
					scene.GetName(), dirty ? " with its unsaved changes" : "");
			};
			const auto reportMembers = [this, &change, &report]()
			{
				if (m_Scene == nullptr || !Utils::AffectsOpenScene(*m_Scene, m_ScenePath, change))
					return;
				m_SceneChangedOnDisk = true;
				report(*m_Scene, IsSceneDirty());
			};
			for (OpenSceneDocument& document : m_OpenScenes)
			{
				if (document.Id == m_AddressedScene)
				{
					reportMembers();
					continue;
				}
				if (document.SceneObject == nullptr || !Utils::AffectsOpenScene(*document.SceneObject, document.Path, change))
					continue;
				document.ChangedOnDisk = true;
				report(*document.SceneObject, document.Dirty || document.History.IsDirty());
			}
			// The members can hold a scene that is no open scene: a dry run's scene.new while none is open.
			if (m_AddressedScene == NoOpenScene)
				reportMembers();
		});

		// The UI's write context, so an import the UI starts before any request keeps "ui" (decision 10).
		SetWriteAttribution(std::nullopt);
	}

	EditorContext::~EditorContext()
	{
		const Status closed = CloseProject();
		ENGINE_VERIFY(closed.has_value(), "EditorContext destroyed before its project could close: {}", closed ? std::string() : closed.error().ToString());
		// The manager outlives this object's other members: it must not call back into them.
		m_Assets->GetWriter().SetMutationGuard({});
		m_Assets->SetWriteObserver({});
		m_Assets->SetExternalChangeListener({});
		m_Assets->SetReloadListener({});
		if (m_Engine->GetAssetManager() == m_Assets.get())
			m_Engine->SetAssetManager(nullptr);
	}

	void EditorContext::Update(double nowSeconds)
	{
		if (HasProject() && !IsAssetScanDeferred())
		{
			if (!m_Play->IsTestRunActive() && !IsDryRun() && nowSeconds >= m_NextScriptConfigurationCheck)
			{
				m_NextScriptConfigurationCheck = nowSeconds + 0.5;
				static_cast<void>(GetScriptService());
			}
			m_Assets->Update(nowSeconds);
			PruneSelection();
		}
		if (m_AudioPreview != nullptr)
			m_AudioPreview->Update();
		// M19 (decision 29): the Native panel's queued preference change, and the unload of a module it no longer allows.
		m_Native->Update(*this);
	}

	Status EditorContext::CheckProjectWrite(const VfsPath& path, std::string_view action) const
	{
		ENGINE_TRY(CheckMutationPermission());
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
		if (!info->IsDirectory)
		{
			Utils::ProvenanceErrorScope provenance(m_CollectProvenanceErrors, m_ProvenanceError);
			ENGINE_TRY(Utils::ToEditorFileStatus(m_Assets->GetWriter().Remove(path)));
			return provenance.Take();
		}

		// A folder goes only while it is empty, never with anything below it (never a permanent delete): the listing names
		// what the editor can see, and the non-recursive removal refuses a folder that still holds an entry the listing
		// leaves out, a host name no VfsPath can spell (NativeDirectoryMount::List).
		Result<std::vector<VfsEntry>> entries = vfs.List(path, false);
		if (!entries)
			return std::unexpected(Utils::ToEditorFileError(std::move(entries).error()));
		if (!entries->empty())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, std::format("cannot remove '{}': the folder is not empty", path.ToString()))
					.WithHint("delete the assets in it first (asset.delete moves them to the trash)"));
		}
		Utils::ProvenanceErrorScope provenance(m_CollectProvenanceErrors, m_ProvenanceError);
		Status removed = m_Assets->GetWriter().RemoveEmptyDirectory(path);
		if (!removed && removed.error().GetCode() == ErrorCode::InvalidState)
		{
			return std::unexpected(Error(ErrorCode::InvalidState,
				std::format("cannot remove '{}': the folder still holds an entry whose name no project path can spell, so the editor cannot list it (a "
							"reserved device name such as 'nul.txt', a name ending in '.' or ' ', or one with a character such as ':', '?' or '*'); nothing was removed",
					path.ToString()))
					.WithHint("remove or rename that entry outside the editor, then delete the folder again"));
		}
		ENGINE_TRY(Utils::ToEditorFileStatus(std::move(removed)));
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
			return Utils::RebuildPrefabInstances(*this, scene, prefabs);
		tracker.Begin();
		Result<bool> updated = Utils::RebuildPrefabInstances(*this, scene, prefabs);
		const std::vector<EntityChange> tracked = tracker.End();
		if (!updated)
			Utils::RollBackTrackedChanges(scene, tracked, "Update Prefab Instances");
		return updated;
	}

	Result<std::vector<AssetHandle>> EditorContext::GetNestingPrefabs(std::span<const AssetHandle> prefabs) const
	{
		if (!HasProject())
			return MakeError(ErrorCode::InvalidState, "the prefabs nesting a prefab cannot be found: no project is open");
		EditorAssetManager& assets = *m_Assets;
		const AssetRegistry& registry = assets.GetRegistry();
		const TypeRegistry& types = GetTypeRegistry();
		const auto isPrefab = [&registry](AssetHandle handle)
		{
			const std::optional<AssetRegistry::Location> location = registry.Locate(handle);
			return location.has_value() && location->Type == AssetType::Prefab;
		};
		for (const AssetHandle prefab : prefabs)
		{
			if (!isPrefab(prefab))
				return MakeError(ErrorCode::NotFound, "{} names no prefab asset of the project", prefab);
		}

		// The nesting graph upwards from `prefabs`, read from the import dependencies (a prefab's nested instances name
		// their prefabs, which its import lists) and confirmed in the documents, since a prefab may reference another
		// without nesting it (a field naming a prefab to spawn). A document may nest its own prefab, which the dependency
		// graph does not record (it keeps no self-edges), so every reached prefab's own document is read too.
		std::map<AssetHandle, std::vector<AssetHandle>> documents; // prefab -> the prefabs its document nests
		std::map<AssetHandle, std::set<AssetHandle>> nestedBy;     // prefab -> the prefabs nesting it directly
		std::set<AssetHandle> reached(prefabs.begin(), prefabs.end());
		std::vector<AssetHandle> pending(reached.begin(), reached.end());
		while (!pending.empty())
		{
			const AssetHandle nested = pending.back();
			pending.pop_back();
			std::vector<AssetHandle> candidates = assets.GetDependencyGraph().GetDependents(nested);
			candidates.push_back(nested);
			for (const AssetHandle candidate : candidates)
			{
				if (!isPrefab(candidate))
					continue;
				Result<std::vector<AssetHandle>> nests = Utils::ReadNestedPrefabs(assets, types, candidate, documents);
				if (!nests)
				{
					ENGINE_WARN("The prefab '{}' is left out of the prefabs nesting '{}': {}", assets.GetReferencePath(candidate),
						assets.GetReferencePath(nested), nests.error().ToString());
					continue;
				}
				if (!std::ranges::binary_search(*nests, nested))
					continue;
				nestedBy[nested].insert(candidate);
				if (reached.insert(candidate).second)
					pending.push_back(candidate);
			}
		}

		// Nesting order (Kahn's algorithm): each prefab after every reached prefab it nests, ties in asset path order. The
		// prefabs left over are on a cycle, or nest one.
		std::map<AssetHandle, size_t> waiting; // prefab -> the reached prefabs it nests that are not ordered yet
		for (const AssetHandle prefab : reached)
			waiting.emplace(prefab, 0);
		for (const auto& [nested, nesting] : nestedBy)
		{
			for (const AssetHandle prefab : nesting)
				++waiting[prefab];
		}
		std::set<std::pair<std::string, AssetHandle>> ready;
		for (const auto& [prefab, count] : waiting)
		{
			if (count == 0)
				ready.emplace(assets.GetReferencePath(prefab), prefab);
		}
		std::vector<AssetHandle> ordered;
		std::set<AssetHandle> done;
		while (!ready.empty())
		{
			const AssetHandle prefab = ready.begin()->second;
			ready.erase(ready.begin());
			done.insert(prefab);
			if (std::ranges::find(prefabs, prefab) == prefabs.end())
				ordered.push_back(prefab);
			if (const auto nesting = nestedBy.find(prefab); nesting != nestedBy.end())
			{
				for (const AssetHandle next : nesting->second)
				{
					if (--waiting[next] == 0)
						ready.emplace(assets.GetReferencePath(next), next);
				}
			}
		}
		if (done.size() == reached.size())
			return ordered;

		// The prefabs on a cycle: those of the rest that reach themselves through the prefabs nesting them.
		std::vector<AssetHandle> cycle;
		for (const AssetHandle prefab : reached)
		{
			if (done.contains(prefab))
				continue;
			std::set<AssetHandle> seen;
			std::vector<AssetHandle> stack{ prefab };
			bool returns = false;
			while (!stack.empty() && !returns)
			{
				const AssetHandle current = stack.back();
				stack.pop_back();
				const auto nesting = nestedBy.find(current);
				if (nesting == nestedBy.end())
					continue;
				for (const AssetHandle next : nesting->second)
				{
					returns = returns || next == prefab;
					if (!done.contains(next) && seen.insert(next).second)
						stack.push_back(next);
				}
			}
			if (returns)
				cycle.push_back(prefab);
		}
		return std::unexpected(Utils::MakePrefabCycleError(assets, cycle));
	}

	Result<NestingPrefabUpdate> EditorContext::CreateNestingPrefabUpdateCommand(std::span<const AssetHandle> prefabs)
	{
		ENGINE_TRY_ASSIGN(const std::vector<AssetHandle> nesting, GetNestingPrefabs(prefabs));
		if (nesting.empty())
			return NestingPrefabUpdate{};
		ENGINE_TRY_ASSIGN(const Ref<const ScriptFieldSchemaSource> schemas, GetScriptSchemaSnapshot());
		const PrefabOptions options{ .Schemas = schemas.get() };
		EditorAssetManager& assets = *m_Assets;
		const TypeRegistry& types = GetTypeRegistry();

		// Each nesting prefab's document with its nested instances rebuilt from the versions current now, in nesting order,
		// so a prefab nesting another that is rewritten here takes that one's new version.
		std::map<AssetHandle, Prefab> versions;
		for (const AssetHandle prefab : prefabs)
		{
			LoadReport report;
			ENGINE_TRY_ASSIGN(Prefab current, LoadPrefabAsset(assets, prefab, types, options.Schemas, report));
			versions.emplace(prefab, std::move(current));
		}
		std::vector<std::pair<VfsPath, Buffer>> files;
		std::vector<AssetHandle> written;
		for (const AssetHandle prefab : nesting)
		{
			const AssetRecord* record = assets.GetRegistry().Find(prefab);
			if (record == nullptr || record->Metadata.Importer != PrefabImporter::Id)
				continue; // only a .prefab file nests other prefabs
			LoadReport report;
			ENGINE_TRY_ASSIGN(const Prefab current, LoadPrefabAsset(assets, prefab, types, options.Schemas, report));
			std::map<AssetHandle, Prefab> nested;
			for (const AssetHandle handle : GetNestedPrefabAssets(current, types))
			{
				if (const auto version = versions.find(handle); version != versions.end())
					nested.emplace(handle, version->second);
			}
			LoadReport updateReport;
			ENGINE_TRY_ASSIGN(Prefab updated, WithContext(UpdateNestedPrefabInstances(current, types, nested, options, updateReport), std::format("rewriting the prefab '{}' that nests a changed prefab", assets.GetReferencePath(prefab))));
			for (const LoadDiagnostic& diagnostic : updateReport.Diagnostics)
				ENGINE_WARN("Rewriting the prefab '{}': {} ({})", assets.GetReferencePath(prefab), diagnostic.Message, diagnostic.Code);
			ENGINE_TRY_ASSIGN(const std::string before, current.SaveToString());
			ENGINE_TRY_ASSIGN(const std::string after, updated.SaveToString());
			versions.insert_or_assign(prefab, std::move(updated));
			if (after == before)
				continue;
			const std::span<const std::byte> bytes = AsBytes(after);
			files.emplace_back(record->SourcePath, Buffer(bytes.begin(), bytes.end()));
			written.push_back(prefab);
		}
		if (files.empty())
			return NestingPrefabUpdate{};
		ENGINE_TRY_ASSIGN(Scope<AssetEditCommand> writes, AssetEditCommand::CreateForWrites(*this, files, "Update Nesting Prefabs"));
		NestingPrefabUpdate update;
		update.Prefabs = written;
		update.Rewrite = CreateScope<Utils::NestingPrefabUpdateCommand>(std::move(writes), std::move(written));
		return update;
	}

	Result<Scope<Command>> EditorContext::CreatePrefabUpdateCommand(std::span<const AssetHandle> prefabs, const std::map<AssetHandle, Prefab>* recordAgainst)
	{
		if (!HasScene())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "the prefab instances cannot be updated: no scene is open")
					.WithHint("open a scene with scene.open first"));
		}
		Scene& scene = GetScene();
		ENGINE_ASSERT(!scene.GetChangeTracker().IsTracking(), "EditorContext::CreatePrefabUpdateCommand while a SceneEdit is active");

		// The update runs once under the change tracker to find what it changes, and is then rolled back: the command applies
		// those changes when it is executed (PrefabUpdateCommand).
		scene.GetChangeTracker().Begin();
		const Status recordedBefore = recordAgainst != nullptr ? Utils::RecordPrefabOverridesAgainst(*this, scene, *recordAgainst) : Status();
		Result<bool> updated = recordedBefore ? UpdatePrefabInstances(scene, prefabs) : Result<bool>(std::unexpected(recordedBefore.error()));
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

	void EditorContext::PropagatePrefabUpdate(std::span<const AssetHandle> prefabs, const std::map<AssetHandle, Prefab>* recordAgainst)
	{
		if (IsDryRun() || !HasProject() || prefabs.empty())
			return;
		const OpenSceneId addressed = GetAddressedSceneId();
		std::vector<OpenSceneId> others;
		for (const OpenSceneId id : GetOpenScenes())
		{
			if (id != addressed)
				others.push_back(id);
		}
		if (others.empty())
			return;
		std::vector<AssetHandle> changed(prefabs.begin(), prefabs.end());
		std::ranges::sort(changed);
		changed.erase(std::ranges::unique(changed).begin(), changed.end());
		const Result<std::vector<AssetHandle>> rebuilt = Utils::AddNestingPrefabs(*this, changed);
		if (!rebuilt)
		{
			ENGINE_ERROR("The other open scenes keep their prefab instances: {}", rebuilt.error().ToString());
			return;
		}
		// The other open scenes that instantiate a changed prefab, directly or through nesting, in tab order.
		std::vector<OpenSceneId> following;
		for (const OpenSceneId id : others)
		{
			const Scene* scene = std::as_const(*this).FindScene(id);
			if (scene != nullptr && !Utils::FindPrefabInstances(*scene, *rebuilt).empty())
				following.push_back(id);
		}
		if (following.empty())
			return;

		// Each scene's rebuild is an entry of that scene's own history, never part of the transaction of the action that
		// changed the prefabs, so undo stays isolated per scene. Each scene is addressed for its rebuild and the scene
		// addressed before comes back after it, directly rather than through an EditorSceneTargetScope: a targeted request's
		// own scope may be open, and the addressed scene's history may be undoing or redoing the step that propagates
		// (CommandHistory keeps its state behind a pointer, which parking moves out and unparking brings back unchanged).
		EditorTransaction* const transaction = std::exchange(m_Transaction, nullptr);
		for (const OpenSceneId id : following)
		{
			ParkAddressedScene();
			UnparkScene(id);
			const Status followed = [this, &changed, recordAgainst]() -> Status
			{
				ENGINE_TRY_ASSIGN(Scope<Command> update, CreatePrefabUpdateCommand(changed, recordAgainst));
				if (update == nullptr)
					return {};
				ENGINE_TRY_ASSIGN(const uint64_t index, Execute(std::move(update)));
				static_cast<void>(index);
				return {};
			}();
			if (!followed)
				ENGINE_ERROR("The open scene '{}' keeps its prefab instances after a prefab update: {}", m_Scene->GetName(), followed.error().ToString());
			ParkAddressedScene();
			if (addressed != NoOpenScene)
				UnparkScene(addressed);
		}
		m_Transaction = transaction;
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

	EditorScriptService* EditorContext::GetScriptService()
	{
		// While the asset scan is deferred, the script services wait for OpenDeferredAssets too (decision 11).
		if (!HasProject() || IsAssetScanDeferred())
			return nullptr;
		if (const Status refreshed = RefreshScriptServices(); !refreshed)
		{
			const std::string message = refreshed.error().ToString();
			if (message != m_ScriptConfigurationError)
				ENGINE_ERROR("Cannot refresh script diagnostics: {}", message);
			m_ScriptConfigurationError = message;
			return nullptr;
		}
		m_ScriptConfigurationError.clear();
		return m_ScriptService.get();
	}

	Result<EditorScriptService*> EditorContext::AcquireScriptService()
	{
		if (!HasProject())
			return MakeError(ErrorCode::InvalidState, "no project is open");
		if (IsAssetScanDeferred())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "the project's asset scan is deferred (Editor --upgrade)")
					.WithHint("project.refreshAssets runs the scan and starts the script services"));
		}
		ENGINE_TRY(RefreshScriptServices());
		m_ScriptConfigurationError.clear();
		return m_ScriptService.get();
	}

	const EditorScriptService* EditorContext::GetScriptService() const
	{
		return m_ScriptService.get();
	}

	Status EditorContext::RefreshScriptServices()
	{
		ENGINE_TRY_ASSIGN(ScriptTypeCheckerConfiguration configuration, ScriptTypeChecker::CaptureConfiguration(GetVfs()));
		XXH64Hasher hash;
		for (const auto& file : configuration.Files)
		{
			hash.UpdateU64(file.Path.ToString().size());
			hash.Update(file.Path.ToString());
			hash.UpdateU64(file.Source.has_value());
			if (file.Source)
			{
				hash.UpdateU64(file.Source->size());
				hash.Update(*file.Source);
			}
		}
		if (m_ScriptChecker && hash.Digest() == m_ScriptConfigurationHash)
			return {};
		ENGINE_TRY_ASSIGN(auto checker, ScriptTypeChecker::Create(m_Engine->GetScriptApiRegistry(), std::move(configuration)));
		m_Assets->WaitIdle();
		m_ScriptService.reset();
		m_Assets->SetScriptDiagnosticsProvider(checker.get());
		m_ScriptChecker = std::move(checker);
		m_ScriptService = CreateScope<EditorScriptService>(*this, *m_ScriptChecker);
		m_ScriptConfigurationHash = hash.Digest();
		return {};
	}

	Result<Ref<const ScriptFieldSchemaSource>> EditorContext::GetScriptSchemaSnapshot() const
	{
		std::map<AssetHandle, AssetRef<ScriptData>> scripts;
		std::vector<AssetHandle> handles;
		for (const AssetRecord* record : m_Assets->GetRegistry().GetRecords())
		{
			if (record->Metadata.Type != AssetType::Script || record->Metadata.Kind != AssetMetaKind::Asset)
				continue;
			handles.push_back(record->Metadata.Handle);
		}
		for (const AssetHandle handle : handles)
		{
			const auto loaded = m_Assets->Load(handle);
			if (!loaded)
				continue;
			if (const auto script = AssetCast<ScriptData>(*loaded))
				scripts.emplace(handle, script);
		}
		return ScriptFieldSchemaSource::Create(std::move(scripts));
	}

	Status EditorContext::ReimportScriptForTest(AssetHandle script)
	{
		if (!m_Play->IsTestRunActive())
			return MakeError(ErrorCode::InvalidState, "Test.ReloadScript requires the active test lease");
		const AssetRecord* record = m_Assets->GetRegistry().Find(script);
		if (record == nullptr || record->Metadata.Type != AssetType::Script)
			return MakeError(ErrorCode::InvalidArgument, "Test.ReloadScript requires a script asset");
		// Only this synchronous import may write its sidecar while the runner owns the editor. The normal read-only and
		// agent-policy guards still apply. End the exception before invoking any script callback.
		ENGINE_ASSERT(!m_TestImportActive, "test imports cannot nest");
		m_TestImportActive = true;
		const auto imported = m_Assets->Reimport(script);
		m_TestImportActive = false;
		if (!imported)
			return std::unexpected(imported.error());
		return {};
	}

	uint64_t EditorContext::GetRevision() const
	{
		// During a dry run the base may have wrapped around (EditorDryRunScope::Begin); unsigned arithmetic makes the sum the
		// real revision plus the sandbox's mutations all the same.
		const uint64_t sceneRevision = m_Scene != nullptr ? m_Scene->GetRevision() : 0;
		// A scene unparked behind a newer revision of another open scene keeps its own until it changes (UnparkScene).
		if (m_KeptRevision.has_value() && sceneRevision == m_KeptSceneRevision)
			return *m_KeptRevision;
		return m_RevisionBase + sceneRevision;
	}

	uint64_t EditorContext::GetRevisionHighWater() const
	{
		return std::max(m_RevisionHighWater, GetRevision());
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
		m_OpenScenes.clear();
		m_ActiveScene = NoOpenScene;
		m_AddressedScene = NoOpenScene;
		m_PlayedScene = NoOpenScene;
		m_Scene.reset();
		m_KeptRevision.reset();
		m_ScenePath.reset();
		m_SceneDirty = false;
		m_SceneChangedOnDisk = false;
		m_History.Clear();
		m_Selection.clear();
		m_SelectionTarget = SceneTarget::Edit;
		m_SelectionSession = 0;
		m_SelectionSceneGeneration = 0;
		m_UiState.SetSelectedAsset({});
		m_AssetScanDeferred = true;

		// --upgrade (Docs/Decisions/0026-m16-contract.md decision 11): the rest waits for OpenDeferredAssets, which
		// project.refreshAssets runs under its own attribution, so opening the project writes nothing.
		if (m_Specification.DeferAssetScan)
		{
			ENGINE_INFO("Opened project '{}' at '{}'{}; its asset scan waits for project.refreshAssets", m_Project->GetSettings().Name,
				FileSystem::PathToUtf8(m_Project->GetRoot()), m_Project->IsReadOnly() ? " read-only" : "");
			return {};
		}

		// A project the editor cannot index stays closed.
		if (Result<AssetRefreshReport> assets = OpenDeferredAssets(); !assets)
		{
			m_AssetScanDeferred = false;
			m_Provenance.reset();
			m_Project.reset();
			Utils::UnmountLogged(vfs, Utils::CacheScheme);
			Utils::UnmountLogged(vfs, Utils::ProjectScheme);
			return std::unexpected(std::move(assets).error());
		}
		return {};
	}

	bool EditorContext::IsAssetScanDeferred() const
	{
		return m_Project != nullptr && m_AssetScanDeferred;
	}

	Result<AssetRefreshReport> EditorContext::OpenDeferredAssets()
	{
		if (!HasProject())
			return MakeError(ErrorCode::InvalidState, "no project is open");
		if (!m_AssetScanDeferred)
		{
			return std::unexpected(Error(ErrorCode::InvalidState, std::format("the assets of the project '{}' are already open", m_Project->GetSettings().Name))
					.WithHint("only an editor started with --upgrade defers its asset scan, to its first project.refreshAssets"));
		}
		VirtualFileSystem& vfs = GetVfs();

		// The project's assets (M6): scan, .meta files for new sources (recorded in provenance through the write observer,
		// with the current attribution) and hot reload. A failure undoes only this part: the project stays open.
		Result<AssetRefreshReport> assets = OpenProjectAssets();
		if (!assets)
		{
			m_Assets->SetScriptDiagnosticsProvider(nullptr);
			m_ScriptService.reset();
			m_ScriptChecker.reset();
			return std::unexpected(std::move(assets).error().WithContext("opening the project's assets"));
		}

		// M19 (Docs/Decisions/0030-m19-contract.md decision 31): the project's native module, before its scenes load, with the
		// preference as the user left it. A module that does not load leaves the project open without it.
		if (Status preference = m_Native->ReadPreference(*this); !preference)
			ENGINE_WARN("{}", preference.error());
		const Status native = m_Native->OnProjectOpened(*this);
		if (!native)
			ENGINE_WARN("The project '{}' opens without its native module: {}", m_Project->GetSettings().Name, native.error());

		// --strict (§6 "Strict reader"): the project's scenes and prefabs, read once now, whatever the asset cache holds.
		if (m_Specification.StrictUnknowns)
		{
			if (Status strict = CheckProjectDocumentsStrictly(native); !strict)
			{
				m_Native->OnProjectClosed();
				m_Assets->CloseProject();
				m_Assets->SetScriptDiagnosticsProvider(nullptr);
				m_ScriptService.reset();
				m_ScriptChecker.reset();
				return std::unexpected(std::move(strict).error().WithContext("opening the project with --strict"));
			}
		}

		m_AssetScanDeferred = false;
		if (vfs.IsMounted("user"))
		{
			if (Status added = ProjectManager::AddRecentProject(vfs, m_Project->GetProjectFile()); !added)
				ENGINE_WARN("Could not add the project to the recent list: {}", added.error().ToString());
		}
		ENGINE_INFO("Opened project '{}' at '{}'{} ({} asset .meta files, {} new)", m_Project->GetSettings().Name,
			FileSystem::PathToUtf8(m_Project->GetRoot()), m_Project->IsReadOnly() ? " read-only" : "", assets->MetaCount, assets->CreatedMetas.size());
		return assets;
	}

	Result<AssetRefreshReport> EditorContext::OpenProjectAssets()
	{
		ENGINE_TRY_ASSIGN(const VfsPath assetsRoot, VfsPath::Create(Utils::ProjectScheme, "Assets"));
		ENGINE_TRY_ASSIGN(const VfsPath cacheRoot, VfsPath::Parse("cache://"));
		VirtualFileSystem& vfs = GetVfs();
		// Every project has an Assets folder (ProjectManager's templates); one made by hand without it gets it.
		if (!m_Project->IsReadOnly() && !vfs.Exists(assetsRoot))
			ENGINE_TRY(Utils::ToEditorFileStatus(vfs.CreateDirectories(assetsRoot)));
		ENGINE_TRY(RefreshScriptServices());
		return m_Assets->OpenProject(AssetProjectSpecification{
			.AssetsRoot = assetsRoot,
			.CacheRoot = cacheRoot,
			.ReadOnly = m_Project->IsReadOnly(),
			.HotReload = true,
		});
	}

	Status EditorContext::CheckProjectDocumentsStrictly(const Status& nativeOpened) const
	{
		ENGINE_TRY_ASSIGN(const VfsPath assetsRoot, VfsPath::Create(Utils::ProjectScheme, "Assets"));
		const VirtualFileSystem& vfs = GetVfs();
		if (!vfs.Exists(assetsRoot))
			return {};
		Result<std::vector<VfsEntry>> listed = vfs.List(assetsRoot, true);
		if (!listed)
			return std::unexpected(Utils::ToEditorFileError(std::move(listed).error()));
		std::vector<VfsEntry> entries = std::move(*listed);
		std::ranges::sort(entries, [](const VfsEntry& left, const VfsEntry& right)
		{
			return left.Path.GetPath() < right.Path.GetPath();
		});
		// Script field overrides resolve against the declared fields, as a play session's load does, so a value that does not
		// match its declaration is an error here too, and one that merely lacks a schema (no script asset) is not mistaken
		// for an unknown key.
		ENGINE_TRY_ASSIGN(const Ref<const ScriptFieldSchemaSource> schemas, GetScriptSchemaSnapshot());
		const TypeRegistry& registry = GetTypeRegistry();
		// M19 (decision 27): native components are checked against the module the project's opening loaded, if any; without
		// one, each finding's hint says why none is loaded.
		const Ref<const NativeModuleTypes> nativeTypes = m_Native->GetLoadedTypes();
		const std::string unloadedHint = nativeTypes == nullptr ? Utils::MakeUnloadedNativeHint(*m_Native, nativeOpened) : std::string();
		std::vector<ErrorIssue> issues;
		size_t failedFiles = 0;
		for (const VfsEntry& entry : entries)
		{
			const std::string_view extension = entry.Path.GetExtension();
			if (entry.Info.IsDirectory || (extension != ".scene" && extension != ".prefab"))
				continue;
			LoadOptions options;
			options.Mode = LoadMode::Strict;
			options.StrictUnknowns = true;
			options.Schemas = schemas.get();
			options.SourcePath = std::string(entry.Path.GetPath());
			LoadReport report;
			Status loaded;
			if (extension == ".scene")
			{
				UUIDGenerator scratchIds = UUIDGenerator::CreateDeterministic(0);
				const Scope<Scene> scratch = Scene::Create({ .Name = std::string(entry.Path.GetStem()), .Seed = 0, .Registry = &registry, .IdGenerator = &scratchIds, .Runtime = false });
				loaded = SceneSerializer::LoadFromFile(*scratch, vfs, entry.Path, options, report);
			}
			else if (Result<Prefab> prefab = Prefab::LoadFromFile(vfs, entry.Path, registry, options, report); !prefab)
			{
				loaded = std::unexpected(std::move(prefab).error());
			}
			if (!loaded)
			{
				++failedFiles;
				const Error& error = loaded.error();
				issues.push_back(ErrorIssue{ .JsonPointer = error.GetLocation().JsonPointer.value_or(std::string()),
					.Message = std::format("{}: {}", entry.Path.GetPath(), error.ToString()),
					.Hint = error.GetHint(),
					.Suggestions = {} });
				// Script field values are named with the code and place project.validate reports them under (ADR 0026).
				ENGINE_TRY_ASSIGN(const std::vector<LoadDiagnostic> problems, ProjectValidator::FindScriptFieldProblems(*this, entry.Path));
				for (const LoadDiagnostic& problem : problems)
				{
					issues.push_back(ErrorIssue{ .JsonPointer = problem.JsonPointer,
						.Message = std::format("{} {}: {} ({}, entity {})", entry.Path.GetPath(), problem.JsonPointer, problem.Message, problem.Code,
							problem.Entity.ToString()),
						.Hint = "project.validate reports it under the same code; correct or remove the override",
						.Suggestions = {} });
				}
			}
			// A document that cannot be read or parsed has its load's error above; its native components are not checked.
			const Result<std::string> text = vfs.ReadText(entry.Path);
			const Result<Json> document = text.has_value() ? JsonReader::Parse(*text) : Result<Json>(std::unexpected(text.error()));
			if (!document.has_value())
				continue;
			const std::vector<LoadDiagnostic> natives = Utils::FindNativeDocumentProblems(*document, nativeTypes.get());
			if (!natives.empty() && loaded)
				++failedFiles;
			for (const LoadDiagnostic& problem : natives)
			{
				issues.push_back(ErrorIssue{ .JsonPointer = problem.JsonPointer,
					.Message = std::format("{} {}: {} ({}, entity {})", entry.Path.GetPath(), problem.JsonPointer, problem.Message, problem.Code, problem.Entity.ToString()),
					.Hint = nativeTypes == nullptr ? unloadedHint : std::string("correct or remove the native component, or change the module so it registers and accepts it"),
					.Suggestions = {} });
			}
		}
		if (issues.empty())
			return {};
		std::string message = failedFiles == 1 ? issues.front().Message : std::format("{} scene and prefab files do not load strictly", failedFiles);
		return std::unexpected(Error(ErrorCode::Validation, std::move(message))
				.WithIssues(std::move(issues))
				.WithHint("--strict needs every scene and prefab to load strictly, unknown keys, fields and components being errors: correct the files "
						  "named (project.validate and scene.open name each problem), or open the project without --strict"));
	}

	void EditorContext::SetLifecycleCallbacks(EditorLifecycleCallbacks callbacks)
	{
		m_Lifecycle = std::move(callbacks);
	}

	Status EditorContext::PrepareForPlay()
	{
		return !IsDryRun() && m_Lifecycle.BeforePlay ? m_Lifecycle.BeforePlay() : Status{};
	}

	Status EditorContext::CloseProject()
	{
		if (m_Play->IsTestRunActive())
			return MakeError(ErrorCode::InvalidState, "a test run owns the project");
		if (!HasProject())
			return {};
		ENGINE_ASSERT(m_DryRun == nullptr && m_Transaction == nullptr, "EditorContext::CloseProject inside a dry run or a transaction");
		if (m_Lifecycle.BeforeProjectClose)
			ENGINE_TRY(m_Lifecycle.BeforeProjectClose());
		// A play session refers to the project's settings and assets: it stops before they go (M7).
		if (m_Play->IsPlaying())
		{
			const Status stopped = m_Play->Stop();
			ENGINE_ASSERT(stopped.has_value(), "stopping the play session of a closing project failed: {}", stopped ? std::string() : stopped.error().ToString());
		}
		// Every open scene closes with the project, which says so itself: no SceneClosed events.
		if (m_Scene != nullptr || !m_OpenScenes.empty())
			ClearAddressedScene();
		m_OpenScenes.clear();
		m_ActiveScene = NoOpenScene;
		m_AddressedScene = NoOpenScene;
		m_PlayedScene = NoOpenScene;
		// M19 (decision 31): a running module build is cancelled and the module unloaded, after the session that ran it.
		m_Native->OnProjectClosed();
		// M12: a preview plays a clip of the closing project.
		if (m_AudioPreview != nullptr)
			m_AudioPreview->Stop();
		// The manager's jobs and hot reload use project:// and cache://: they stop before the mounts go.
		m_Assets->CloseProject();
		m_Assets->SetScriptDiagnosticsProvider(nullptr);
		m_ScriptService.reset();
		m_ScriptChecker.reset();
		VirtualFileSystem& vfs = GetVfs();
		Utils::UnmountLogged(vfs, Utils::CacheScheme);
		Utils::UnmountLogged(vfs, Utils::ProjectScheme);
		const std::string name = m_Project->GetSettings().Name;
		m_AssetScanDeferred = false;
		m_Provenance.reset();
		m_Project.reset(); // releases the project lock
		ENGINE_INFO("Closed project '{}'", name);
		return {};
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
		ENGINE_TRY(CheckProjectWrite(path, "write"));
		const auto existing = GetVfs().ReadText(path);
		if (!existing && existing.error().GetCode() != ErrorCode::NotFound)
			return std::unexpected(existing.error());
		// Rewriting identical settings changes the file identity without a settings change for autosave to admit.
		// Preserve that identity, including across undo/redo of a no-op patch, while keeping command revisions intact.
		if (!existing || *existing != text)
			ENGINE_TRY(WriteProjectFile(path, std::as_bytes(std::span(text.data(), text.size()))));
		m_Project->SetSettings(std::move(settings));
		// A change of the addressed scene's revision: one it kept since it was unparked continues past every revision
		// reported meanwhile (UnparkScene).
		m_KeptRevision.reset();
		++m_RevisionBase;
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
		if (m_Play->IsTestRunActive())
			return;
		ENGINE_ASSERT(HasProject(), "EditorContext::SetScene needs an open project");
		ENGINE_ASSERT(scene != nullptr, "EditorContext::SetScene needs a scene");
		ENGINE_ASSERT(&scene->GetTypeRegistry() == &GetTypeRegistry() && &scene->GetUUIDGenerator() == &m_IdGenerator,
			"EditorContext::SetScene: scene '{}' was not created with EditorContext::CreateScene", scene->GetName());
		ENGINE_ASSERT(m_Transaction == nullptr, "EditorContext::SetScene inside a transaction");

		// A dry run replaces only its sandbox scene: the open scenes, their ids and the active one stay as they are.
		if (IsDryRun())
		{
			InstallScene(std::move(scene), std::move(path), dirty);
			return;
		}
		// The addressed scene is replaced in its tab position with a new id (the first open scene when none is addressed),
		// and becomes the active one, unless a target scope addresses a tab that is not active (project.upgrade's reload of
		// it): that replacement leaves the active scene as it is (the milestone review, M4).
		const OpenSceneId id = AllocateOpenSceneId();
		const bool activate = m_AddressedScene == NoOpenScene || m_AddressedScene == m_ActiveScene;
		if (OpenSceneDocument* replaced = FindDocument(m_AddressedScene); replaced != nullptr)
			replaced->Id = id;
		else
			m_OpenScenes.push_back(OpenSceneDocument{ .Id = id });
		m_AddressedScene = id;
		if (activate)
			m_ActiveScene = id;
		InstallScene(std::move(scene), std::move(path), dirty);
	}

	void EditorContext::InstallScene(Scope<Scene> scene, std::optional<VfsPath> path, bool dirty)
	{
		// M19 (Docs/Decisions/0030-m19-contract.md decision 27): with a native module loaded, a scene opens with its native
		// components canonical, migrated by field name; the scene is then modified until saved, like a repair. Every way a
		// scene opens installs it here, a scene opened in a new tab (AddScene) included.
		if (const Ref<const NativeModuleTypes> types = m_Native->GetLoadedTypes(); types != nullptr)
			dirty = Utils::CanonicalizeOpenedSceneNatives(*scene, *types) || dirty;

		// Past every value reported so far, in any open scene, so a revision of another scene never matches this one (ADR
		// 0008 decision 28; with one open scene: past the previous scene's).
		const uint64_t nextBase = GetRevisionHighWater() + 1;
		m_RevisionHighWater = nextBase - 1;
		m_Scene = std::move(scene);
		m_KeptRevision.reset();
		m_RevisionBase = nextBase;
		if (!IsDryRun())
			m_SceneDocumentEpoch = ++m_LastDocumentEpoch;
		m_ScenePath = std::move(path);
		m_SceneDirty = dirty;
		m_SceneChangedOnDisk = false;
		m_History.Clear();
		m_Selection.clear();
		m_SelectionTarget = SceneTarget::Edit;
		m_SelectionSession = 0;
		m_SelectionSceneGeneration = 0;
		m_UiState.SetSelectedAsset({});

		const std::string pathText = m_ScenePath.has_value() ? std::string(m_ScenePath->GetPath()) : std::string();
		AppendEvent(EngineEvent{ .Seq = 0, .Tick = std::nullopt, .Type = EngineEventType::SceneOpened, .Id = UUID(), .Path = pathText, .Name = {}, .Message = {}, .Dirty = dirty });
		ENGINE_INFO("Opened scene '{}' ({} entities){}{}", m_Scene->GetName(), m_Scene->GetEntityCount(),
			pathText.empty() ? std::string(", not saved yet") : std::format(" from '{}'", pathText), dirty ? ", with unsaved repairs" : "");
	}

	void EditorContext::ClearAddressedScene()
	{
		// Past every value reported so far, so the next scene's revisions never repeat this one's.
		const uint64_t nextBase = GetRevisionHighWater() + 1;
		m_RevisionHighWater = nextBase - 1;
		m_Scene.reset();
		m_KeptRevision.reset();
		m_RevisionBase = nextBase;
		if (!IsDryRun())
			m_SceneDocumentEpoch = ++m_LastDocumentEpoch;
		m_ScenePath.reset();
		m_SceneDirty = false;
		m_SceneChangedOnDisk = false;
		m_History.Clear();
		m_Selection.clear();
		m_SelectionTarget = SceneTarget::Edit;
		m_SelectionSession = 0;
		m_SelectionSceneGeneration = 0;
		m_UiState.SetSelectedAsset({});
	}

	void EditorContext::CloseScene()
	{
		if (m_Play->IsTestRunActive())
			return;
		if (m_Scene == nullptr)
			return;
		ENGINE_ASSERT(m_Transaction == nullptr, "EditorContext::CloseScene inside a transaction");
		// A dry run closes only its sandbox scene; so does a scene that is no open scene (a dry run's scene.new).
		if (IsDryRun() || m_AddressedScene == NoOpenScene)
		{
			ClearAddressedScene();
			return;
		}
		RemoveOpenScene(m_AddressedScene);
	}

	void EditorContext::RemoveOpenScene(OpenSceneId id)
	{
		const auto document = std::ranges::find(m_OpenScenes, id, &OpenSceneDocument::Id);
		ENGINE_ASSERT(document != m_OpenScenes.end(), "EditorContext::RemoveOpenScene of a scene that is not open");
		const size_t position = static_cast<size_t>(document - m_OpenScenes.begin());
		const bool active = id == m_ActiveScene;
		std::optional<VfsPath> path;
		if (id == m_AddressedScene)
		{
			path = m_ScenePath;
			ClearAddressedScene();
			m_AddressedScene = NoOpenScene;
		}
		else
		{
			path = document->Path; // its revision is already in the high-water mark (ParkAddressedScene)
		}
		m_OpenScenes.erase(document);
		if (active)
		{
			// The next tab becomes active, the previous one when the closed tab was the last.
			if (position < m_OpenScenes.size())
				m_ActiveScene = m_OpenScenes[position].Id;
			else
				m_ActiveScene = position > 0 ? m_OpenScenes[position - 1].Id : NoOpenScene;
		}
		// Inside a target scope the closed scene leaves the scope addressing no scene until it ends.
		if (m_TargetScope == nullptr && m_AddressedScene == NoOpenScene && m_ActiveScene != NoOpenScene)
			UnparkScene(m_ActiveScene);
		AppendOpenSceneEvent(EngineEventType::SceneClosed, path);
	}

	std::vector<OpenSceneId> EditorContext::GetOpenScenes() const
	{
		std::vector<OpenSceneId> ids;
		ids.reserve(m_OpenScenes.size());
		for (const OpenSceneDocument& document : m_OpenScenes)
			ids.push_back(document.Id);
		return ids;
	}

	OpenSceneId EditorContext::GetActiveSceneId() const
	{
		return m_ActiveScene;
	}

	OpenSceneId EditorContext::GetAddressedSceneId() const
	{
		return m_AddressedScene;
	}

	OpenSceneId EditorContext::FindOpenScene(const VfsPath& path) const
	{
		for (const OpenSceneDocument& document : m_OpenScenes)
		{
			const std::optional<VfsPath>& scenePath = document.Id == m_AddressedScene ? m_ScenePath : document.Path;
			if (scenePath.has_value() && *scenePath == path)
				return document.Id;
		}
		return NoOpenScene;
	}

	Scene* EditorContext::FindScene(OpenSceneId id)
	{
		OpenSceneDocument* document = FindDocument(id);
		if (document == nullptr)
			return nullptr;
		return id == m_AddressedScene ? m_Scene.get() : document->SceneObject.get();
	}

	const Scene* EditorContext::FindScene(OpenSceneId id) const
	{
		const OpenSceneDocument* document = FindDocument(id);
		if (document == nullptr)
			return nullptr;
		return id == m_AddressedScene ? m_Scene.get() : document->SceneObject.get();
	}

	Result<OpenSceneInfo> EditorContext::DescribeOpenScene(OpenSceneId id) const
	{
		const OpenSceneDocument* document = FindDocument(id);
		if (document == nullptr)
		{
			return std::unexpected(Error(ErrorCode::NotFound, std::format("no open scene has the id {}", id))
					.WithHint("scene.list lists the open scenes with their ids"));
		}
		OpenSceneInfo info;
		info.Id = id;
		info.Active = id == m_ActiveScene;
		const Scene* scene = nullptr;
		if (id == m_AddressedScene)
		{
			scene = m_Scene.get();
			info.Path = m_ScenePath;
			info.Revision = GetRevision();
			info.Dirty = IsSceneDirty();
			info.ChangedOnDisk = m_SceneChangedOnDisk;
		}
		else
		{
			scene = document->SceneObject.get();
			info.Path = document->Path;
			info.Revision = document->Revision;
			info.Dirty = document->Dirty || document->History.IsDirty();
			info.ChangedOnDisk = document->ChangedOnDisk;
		}
		info.Name = scene != nullptr ? scene->GetName() : std::string();
		info.EntityCount = scene != nullptr ? scene->GetEntityCount() : 0;
		return info;
	}

	bool EditorContext::IsAnySceneDirty() const
	{
		if (IsSceneDirty())
			return true;
		return std::ranges::any_of(m_OpenScenes, [this](const OpenSceneDocument& document)
		{
			return document.Id != m_AddressedScene && (document.Dirty || document.History.IsDirty());
		});
	}

	Status EditorContext::CheckOpenScenesChange(std::string_view action) const
	{
		if (m_Play->IsTestRunActive())
			return MakeError(ErrorCode::InvalidState, "cannot {}: a test run owns the editor", action);
		if (m_Transaction != nullptr)
			return MakeError(ErrorCode::InvalidState, "cannot {} inside a transaction", action);
		if (IsDryRun())
			return MakeError(ErrorCode::InvalidState, "cannot {} in a dry run", action);
		return {};
	}

	Result<OpenSceneId> EditorContext::AddScene(Scope<Scene> scene, std::optional<VfsPath> path, bool dirty)
	{
		ENGINE_TRY(CheckOpenScenesChange("open another scene"));
		ENGINE_ASSERT(HasProject(), "EditorContext::AddScene needs an open project");
		ENGINE_ASSERT(scene != nullptr, "EditorContext::AddScene needs a scene");
		ENGINE_ASSERT(&scene->GetTypeRegistry() == &GetTypeRegistry() && &scene->GetUUIDGenerator() == &m_IdGenerator,
			"EditorContext::AddScene: scene '{}' was not created with EditorContext::CreateScene", scene->GetName());
		if (m_OpenScenes.size() >= MaxOpenScenes)
		{
			return std::unexpected(Error(ErrorCode::InvalidState, std::format("cannot open '{}': {} scenes are open, the most the editor holds", scene->GetName(), MaxOpenScenes))
					.WithHint("close an open scene first (scene.close)"));
		}
		if (path.has_value())
		{
			if (const OpenSceneId open = FindOpenScene(*path); open != NoOpenScene)
			{
				return std::unexpected(Error(ErrorCode::AlreadyExists, std::format("'{}' is already open (open scene {})", path->GetPath(), open))
						.WithHint("scene.activate makes it the active scene"));
			}
		}
		// Inside a target scope the scope keeps addressing its scene; the new scene is active all the same.
		const OpenSceneId scoped = m_TargetScope != nullptr ? m_AddressedScene : NoOpenScene;
		ParkAddressedScene();
		const OpenSceneId id = AllocateOpenSceneId();
		m_OpenScenes.push_back(OpenSceneDocument{ .Id = id });
		m_AddressedScene = id;
		m_ActiveScene = id;
		InstallScene(std::move(scene), std::move(path), dirty);
		if (m_TargetScope != nullptr)
		{
			ParkAddressedScene();
			if (scoped != NoOpenScene)
				UnparkScene(scoped);
		}
		return id;
	}

	Status EditorContext::ActivateScene(OpenSceneId id)
	{
		ENGINE_TRY(CheckOpenScenesChange(std::format("activate open scene {}", id)));
		if (FindDocument(id) == nullptr)
		{
			return std::unexpected(Error(ErrorCode::NotFound, std::format("no open scene has the id {}", id))
					.WithHint("scene.list lists the open scenes with their ids"));
		}
		if (m_TargetScope != nullptr && id != m_AddressedScene)
			return MakeError(ErrorCode::InvalidState, "cannot activate open scene {} while a request addresses another open scene", id);
		if (id == m_ActiveScene)
			return {};
		// Inside the scope of `id` that scene is already the addressed one; otherwise the active document is parked.
		if (m_TargetScope == nullptr)
		{
			ParkAddressedScene();
			UnparkScene(id);
		}
		m_ActiveScene = id;
		const std::optional<VfsPath> path = id == m_AddressedScene ? m_ScenePath : FindDocument(id)->Path;
		AppendOpenSceneEvent(EngineEventType::SceneActivated, path);
		return {};
	}

	Status EditorContext::CloseScene(OpenSceneId id)
	{
		ENGINE_TRY(CheckOpenScenesChange(std::format("close open scene {}", id)));
		if (FindDocument(id) == nullptr)
		{
			return std::unexpected(Error(ErrorCode::NotFound, std::format("no open scene has the id {}", id))
					.WithHint("scene.list lists the open scenes with their ids"));
		}
		if (m_TargetScope != nullptr && id != m_AddressedScene)
			return MakeError(ErrorCode::InvalidState, "cannot close open scene {} while a request addresses another open scene", id);
		RemoveOpenScene(id);
		return {};
	}

	EditorContext::OpenSceneDocument* EditorContext::FindDocument(OpenSceneId id)
	{
		if (id == NoOpenScene)
			return nullptr;
		const auto document = std::ranges::find(m_OpenScenes, id, &OpenSceneDocument::Id);
		return document != m_OpenScenes.end() ? &*document : nullptr;
	}

	const EditorContext::OpenSceneDocument* EditorContext::FindDocument(OpenSceneId id) const
	{
		if (id == NoOpenScene)
			return nullptr;
		const auto document = std::ranges::find(m_OpenScenes, id, &OpenSceneDocument::Id);
		return document != m_OpenScenes.end() ? &*document : nullptr;
	}

	OpenSceneId EditorContext::AllocateOpenSceneId()
	{
		ENGINE_VERIFY(m_LastOpenSceneId < std::numeric_limits<OpenSceneId>::max(), "EditorContext: the open-scene ids are exhausted");
		return ++m_LastOpenSceneId;
	}

	void EditorContext::ParkAddressedScene()
	{
		OpenSceneDocument* document = FindDocument(m_AddressedScene);
		if (document == nullptr)
			return;
		ENGINE_ASSERT(!IsDryRun() && m_Transaction == nullptr, "EditorContext: an open scene is parked inside a dry run or a transaction");
		document->Revision = GetRevision();
		m_RevisionHighWater = std::max(m_RevisionHighWater, document->Revision);
		document->SceneObject = std::move(m_Scene);
		document->Path = std::exchange(m_ScenePath, std::nullopt);
		document->DocumentEpoch = m_SceneDocumentEpoch;
		document->Dirty = std::exchange(m_SceneDirty, false);
		document->ChangedOnDisk = std::exchange(m_SceneChangedOnDisk, false);
		document->History = std::exchange(m_History, CommandHistory(m_Specification.HistoryLimits));
		document->Selection = std::exchange(m_Selection, {});
		document->SelectionTarget = std::exchange(m_SelectionTarget, SceneTarget::Edit);
		document->SelectionSession = std::exchange(m_SelectionSession, 0);
		document->SelectionSceneGeneration = std::exchange(m_SelectionSceneGeneration, 0);
		// No scene is addressed until the next one is unparked: the revision then stays the newest reported.
		m_KeptRevision.reset();
		m_RevisionBase = m_RevisionHighWater;
		m_AddressedScene = NoOpenScene;
	}

	void EditorContext::UnparkScene(OpenSceneId id)
	{
		ENGINE_ASSERT(m_AddressedScene == NoOpenScene, "EditorContext::UnparkScene while another scene is addressed");
		OpenSceneDocument* document = FindDocument(id);
		ENGINE_ASSERT(document != nullptr, "EditorContext::UnparkScene of a scene that is not open");
		const uint64_t floor = GetRevisionHighWater();
		m_RevisionHighWater = floor;
		m_Scene = std::move(document->SceneObject);
		m_ScenePath = std::exchange(document->Path, std::nullopt);
		m_SceneDocumentEpoch = document->DocumentEpoch;
		m_SceneDirty = document->Dirty;
		m_SceneChangedOnDisk = document->ChangedOnDisk;
		m_History = std::move(document->History);
		m_Selection = std::exchange(document->Selection, {});
		m_SelectionTarget = document->SelectionTarget;
		m_SelectionSession = document->SelectionSession;
		m_SelectionSceneGeneration = document->SelectionSceneGeneration;
		m_AddressedScene = id;

		// Its revision stays what it was parked with until its scene changes. When another scene reported a newer one
		// meanwhile, the first change continues past that one, so a revision never names two states (decision 13).
		const uint64_t sceneRevision = m_Scene != nullptr ? m_Scene->GetRevision() : 0;
		if (document->Revision >= floor)
		{
			m_KeptRevision.reset();
			m_RevisionBase = document->Revision - sceneRevision;
		}
		else
		{
			m_KeptRevision = document->Revision;
			m_KeptSceneRevision = sceneRevision;
			m_RevisionBase = floor - sceneRevision;
		}

		// A selection made in a play session that ended while this scene was parked comes back to its edit scene when this is
		// the scene that session played, as Stop carries it back, and is dropped otherwise (one made after the played tab
		// closed: decision 13); one of a session that replaced it is dropped by PruneSelection as before.
		if (m_SelectionTarget == SceneTarget::Play && m_Play->GetSession() == nullptr)
		{
			const bool played = id == m_PlayedScene;
			std::erase_if(m_Selection, [this, played](UUID entity)
			{
				return !played || m_Scene == nullptr || !std::as_const(*m_Scene).FindEntityByID(entity).IsValid();
			});
			m_SelectionTarget = SceneTarget::Edit;
			m_SelectionSession = 0;
			m_SelectionSceneGeneration = 0;
		}
	}

	void EditorContext::AppendOpenSceneEvent(EngineEventType type, const std::optional<VfsPath>& path)
	{
		const std::string pathText = path.has_value() ? std::string(path->GetPath()) : std::string();
		AppendEvent(EngineEvent{ .Seq = 0, .Tick = std::nullopt, .Type = type, .Id = UUID(), .Path = pathText, .Name = {}, .Message = {}, .Dirty = false });
	}

	bool EditorContext::ShowsPlaySession(const PlaySession* session) const
	{
		return session != nullptr && (session->GetSerial() != m_PlayedSession || m_PlayedScene == m_AddressedScene);
	}

	bool EditorContext::ShowsPlayScene() const
	{
		if (m_Play->GetSession() == nullptr)
			return false;
		return m_Play->IsTestRunActive() || m_Play->GetPlayedScene() == m_ActiveScene;
	}

	bool EditorContext::IsSceneDirty() const
	{
		return m_Scene != nullptr && (m_SceneDirty || m_History.IsDirty());
	}

	void EditorContext::ApplySceneMigration(OpenSceneId scene, bool changed, bool clearHistory)
	{
		ENGINE_ASSERT(FindDocument(scene) != nullptr, "EditorContext::ApplySceneMigration of a scene that is not open");
		ENGINE_ASSERT(m_DryRun == nullptr && m_Transaction == nullptr, "EditorContext::ApplySceneMigration inside a dry run or a transaction");
		// Another open scene is addressed for its migration and the scene addressed before comes back after it, as
		// PropagatePrefabUpdate does.
		const OpenSceneId addressed = m_AddressedScene;
		if (scene != addressed)
		{
			ParkAddressedScene();
			UnparkScene(scene);
		}
		if (clearHistory)
		{
			// Clearing moves the save point to the empty history, which would read as saved.
			m_SceneDirty = IsSceneDirty();
			m_History.Clear();
		}
		if (changed)
		{
			m_SceneDirty = true;
			// The rewrite of the preserved components is no tracked change of the scene's own revision, so the revision moves
			// here: past every value any open scene reported (decision 13), also for a scene that keeps an older revision
			// until it changes (UnparkScene).
			const uint64_t sceneRevision = m_Scene != nullptr ? m_Scene->GetRevision() : 0;
			const uint64_t next = GetRevisionHighWater() + 1;
			m_KeptRevision.reset();
			m_RevisionBase = next - sceneRevision;
		}
		if (scene != addressed)
		{
			ParkAddressedScene();
			if (addressed != NoOpenScene)
				UnparkScene(addressed);
		}
	}

	Status EditorContext::MarkSceneSaved(const VfsPath& path)
	{
		ENGINE_ASSERT(m_Scene != nullptr, "EditorContext::MarkSceneSaved without an open scene");
		m_ScenePath = path;
		m_SceneDirty = false;
		m_SceneChangedOnDisk = false;
		m_History.MarkSavePoint();
		if (!IsDryRun() && m_Lifecycle.AfterSceneSaved)
		{
			Status cleaned = m_Lifecycle.AfterSceneSaved();
			if (!cleaned)
				return std::unexpected(std::move(cleaned).error().WithHint("the scene was saved; recovery cleanup failed and can be retried"));
		}
		return {};
	}

	Result<uint64_t> EditorContext::Execute(Scope<Command> command)
	{
		ENGINE_ASSERT(command != nullptr, "EditorContext::Execute needs a command");
		ENGINE_TRY(CheckMutationPermission());
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
		// Legacy callers address the shown scene. After Stop the same UUIDs are filtered against the edit scene. With several
		// open scenes the play scene is shown only in the tab of the scene the session plays (decision 13).
		const PlaySession* session = ShowsPlaySession(m_Play->GetSession()) ? m_Play->GetSession() : nullptr;
		const Scene* scene = session != nullptr ? &session->GetScene() : m_Scene.get();
		std::vector<UUID> kept;
		kept.reserve(selection.size());
		for (const UUID id : selection)
		{
			if (scene == nullptr || !scene->FindEntityByID(id).IsValid())
				continue;
			if (std::find(kept.begin(), kept.end(), id) == kept.end())
				kept.push_back(id);
		}
		m_Selection = std::move(kept);
		m_SelectionTarget = session != nullptr ? SceneTarget::Play : SceneTarget::Edit;
		m_SelectionSession = session != nullptr ? session->GetSerial() : 0;
		m_SelectionSceneGeneration = session != nullptr ? session->GetSceneGeneration() : 0;
		m_UiState.SetSelectedAsset({});
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

		// One write context per attribution (Docs/Decisions/0026-m16-contract.md decision 10): the imports started from now on
		// keep it until they are published, and the editor remembers what its Id means only while the token is current or
		// an import holds it.
		std::erase_if(m_WriteContexts, [](const auto& entry)
		{
			return entry.second.Token.expired();
		});
		Ref<const AssetWriteContext> context = CreateRef<const AssetWriteContext>(AssetWriteContext{ .Id = ++m_LastWriteContextId });
		m_WriteContexts.emplace(context->Id, WriteContextEntry{ .Token = context, .Attribution = m_Attribution });
		m_Assets->SetWriteContext(std::move(context));
	}

	Status EditorContext::CheckTestRunAdmission(std::string_view method, bool dryRun) const
	{
		if (!m_Play->IsTestRunActive())
			return {};
		// A test session is published for observation only. Some unflagged methods (play, eval, open, refresh, selection
		// and validation fixes) still mutate live host state, so admission is an explicit read subset.
		constexpr std::string_view Reads[] = { "session.hello", "session.info", "rpc.discover", "docs.get", "scene.tree", "scene.query",
			"scene.get", "entity.get", "entity.bounds", "log.read", "events.read", "play.state", "physics.bodyInfo", "audio.stats",
			"stats.get", "script.errors", "viewport.screenshot", "editor.screenshot" };
		if (dryRun || std::ranges::find(Reads, method) == std::end(Reads))
			return MakeError(ErrorCode::InvalidState, "a test run owns the editor; '{}' is unavailable until it finishes", method);
		return {};
	}

	Status EditorContext::CheckMutationPermission() const
	{
		if (m_Play->IsTestRunActive() && !m_TestImportActive)
			return MakeError(ErrorCode::InvalidState, "a test run owns the editor");
		if (m_AgentMutationsDenied && GetCommandOrigin() == CommandOrigin::Agent && !IsDryRun())
			return MakeError(ErrorCode::PermissionDenied, "agent mutations are disabled by the editor automation policy");
		return {};
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
		std::optional<uint64_t> RealKeptRevision{};
		uint64_t RealKeptSceneRevision = 0;
		uint64_t RealRevisionHighWater = 0;
		OpenSceneId RealPlayedScene = NoOpenScene;
		uint64_t RealPlayedSession = 0;
		std::vector<UUID> RealSelection{};
		SceneTarget RealSelectionTarget = SceneTarget::Edit;
		uint64_t RealSelectionSession = 0;
		uint64_t RealSelectionSceneGeneration = 0;
		AssetHandle RealSelectedAsset{};
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
		const uint64_t realSceneRevision = context.m_Scene != nullptr ? context.m_Scene->GetRevision() : 0;
		// The real scene still keeps the revision it was unparked with (UnparkScene): the copy keeps it until it changes.
		const bool keptRevision = context.m_KeptRevision.has_value() && realSceneRevision == context.m_KeptSceneRevision;
		state.RealScenePath = context.m_ScenePath;
		state.RealSceneDirty = context.m_SceneDirty;
		state.RealIdGenerator = context.m_IdGenerator;
		state.RealRevisionBase = context.m_RevisionBase;
		state.RealKeptRevision = context.m_KeptRevision;
		state.RealKeptSceneRevision = context.m_KeptSceneRevision;
		state.RealRevisionHighWater = context.m_RevisionHighWater;
		state.RealPlayedScene = context.m_PlayedScene;
		state.RealPlayedSession = context.m_PlayedSession;
		state.RealSelection = context.m_Selection;
		state.RealSelectionTarget = context.m_SelectionTarget;
		state.RealSelectionSession = context.m_SelectionSession;
		state.RealSelectionSceneGeneration = context.m_SelectionSceneGeneration;
		state.RealSelectedAsset = context.m_UiState.GetSelectedAsset();
		state.RealSettings = context.m_Project->GetSettings();
		const bool realDirty = context.IsSceneDirty();
		state.RealScene = std::move(context.m_Scene);
		state.RealHistory = std::move(context.m_History);

		context.m_History = CommandHistory(context.m_Specification.HistoryLimits);
		context.m_Scene = std::move(copy);
		context.m_SceneDirty = realDirty; // the sandbox history starts clean; a dirty real scene stays dirty
		// GetRevision must report the real revision plus the sandbox's mutations. Loading the copy counted mutations of its
		// own, possibly more than the real revision, so the base may wrap around; the unsigned sum is still exact. A kept
		// revision (UnparkScene) stays kept until the copy changes, and then continues as the real scene's would.
		const uint64_t copyRevision = context.m_Scene != nullptr ? context.m_Scene->GetRevision() : 0;
		context.m_RevisionBase = context.m_RevisionBase + realSceneRevision - copyRevision;
		if (keptRevision)
		{
			context.m_KeptRevision = realRevision;
			context.m_KeptSceneRevision = copyRevision;
		}
		else
		{
			context.m_KeptRevision.reset();
		}
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
		context.m_KeptRevision = state.RealKeptRevision;
		context.m_KeptSceneRevision = state.RealKeptSceneRevision;
		context.m_RevisionHighWater = state.RealRevisionHighWater;
		context.m_PlayedScene = state.RealPlayedScene;
		context.m_PlayedSession = state.RealPlayedSession;
		context.m_Selection = std::move(state.RealSelection);
		context.m_SelectionTarget = state.RealSelectionTarget;
		context.m_SelectionSession = state.RealSelectionSession;
		context.m_SelectionSceneGeneration = state.RealSelectionSceneGeneration;
		context.m_UiState.SetSelectedAsset(state.RealSelectedAsset);
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

	// --- EditorSceneTargetScope ------------------------------------------------------------------------------------------

	struct EditorSceneTargetScope::State
	{
		EditorContext* Context = nullptr; // documented back-reference: outlives the scope
		OpenSceneId Scene = NoOpenScene;
		bool Active = false; // Begin made it the context's scope
	};

	EditorSceneTargetScope::EditorSceneTargetScope(ConstructionKey /*key*/, EditorContext& context, OpenSceneId scene)
		: m_State(CreateScope<State>(State{ .Context = &context, .Scene = scene }))
	{
	}

	Result<Scope<EditorSceneTargetScope>> EditorSceneTargetScope::Begin(EditorContext& context, OpenSceneId scene)
	{
		if (context.IsDryRun() || context.m_Transaction != nullptr)
			return MakeError(ErrorCode::InvalidState, "a request cannot address open scene {} inside a dry run or a transaction", scene);
		if (context.m_TargetScope != nullptr)
			return MakeError(ErrorCode::InvalidState, "a request cannot address open scene {} while another request addresses an open scene", scene);
		if (context.FindDocument(scene) == nullptr)
		{
			return std::unexpected(Error(ErrorCode::NotFound, std::format("no open scene has the id {}", scene))
					.WithHint("scene.list lists the open scenes with their ids"));
		}
		Scope<EditorSceneTargetScope> scope = CreateScope<EditorSceneTargetScope>(ConstructionKey(), context, scene);
		if (scene != context.m_AddressedScene)
		{
			context.ParkAddressedScene();
			context.UnparkScene(scene);
		}
		context.m_TargetScope = scope.get();
		scope->m_State->Active = true;
		return scope;
	}

	EditorSceneTargetScope::~EditorSceneTargetScope()
	{
		State& state = *m_State;
		if (!state.Active)
			return;
		EditorContext& context = *state.Context;
		ENGINE_ASSERT(context.m_TargetScope == this, "EditorSceneTargetScope: scopes do not nest");
		ENGINE_ASSERT(context.m_DryRun == nullptr && context.m_Transaction == nullptr,
			"EditorSceneTargetScope closed while a dry run or a transaction opened in it is still open");
		context.m_TargetScope = nullptr;
		// The active scene is the addressed one again; the scope's scene, still open or not, keeps its document.
		if (context.m_AddressedScene != context.m_ActiveScene)
		{
			context.ParkAddressedScene();
			if (context.m_ActiveScene != NoOpenScene)
				context.UnparkScene(context.m_ActiveScene);
		}
	}

	OpenSceneId EditorSceneTargetScope::GetScene() const
	{
		// While it is open, the scene it addresses now: SetScene's replacement of it has a new id, and it addresses no scene
		// once its scene closed.
		return m_State->Active ? m_State->Context->m_AddressedScene : m_State->Scene;
	}

}
