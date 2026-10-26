#include "EditorPCH.h"
#include "EditorCore/Automation/Private/AssetMethodSupport.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/PrefabInstances.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/AssetReference.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/AssetPipeline/AssetDependencyGraph.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/Prefab.h"
#include "Engine/Scene/PrefabAsset.h"
#include "Engine/Scene/PrefabInstantiator.h"
#include "Engine/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <optional>
#include <utility>

namespace Engine {

	namespace {

		// CreatePrefabPropagationCommand's command: it makes the other open scenes follow on its execution (and redo) when it
		// closes an undo step's writes, or on its undo when it opens them, so that it always comes after every write, or
		// every restore, of the step.
		class PrefabPropagationCommand final : public Command
		{
		public:
			PrefabPropagationCommand(std::vector<AssetHandle> prefabs, bool afterUndo)
				: m_Prefabs(std::move(prefabs)), m_AfterUndo(afterUndo)
			{
			}

			[[nodiscard]] Status Execute(EditorContext& context) override
			{
				if (!m_AfterUndo)
					Follow(context);
				return {};
			}

			[[nodiscard]] Status Undo(EditorContext& context) override
			{
				if (m_AfterUndo)
					Follow(context);
				return {};
			}

			[[nodiscard]] std::string_view GetLabel() const override { return "Update Prefab Instances of the Other Open Scenes"; }
			[[nodiscard]] bool ChangesScene() const override { return false; }
			[[nodiscard]] size_t GetMemorySize() const override { return sizeof(*this) + m_Prefabs.size() * sizeof(AssetHandle); }
		private:
			// EditorContext::PropagatePrefabUpdate once the prefabs' files are imported: a write, or the restore of an undo,
			// only schedules the import of what it wrote. Only while another scene is open, so editing one scene imports
			// nothing more. Never fails: a failed import is logged, and the scenes follow the last good version.
			void Follow(EditorContext& context) const
			{
				if (context.IsDryRun())
					return;
				const OpenSceneId addressed = context.GetAddressedSceneId();
				const std::vector<OpenSceneId> scenes = context.GetOpenScenes();
				const bool othersOpen = std::ranges::any_of(scenes, [addressed](OpenSceneId id)
				{
					return id != addressed;
				});
				if (!othersOpen)
					return;
				EditorAssetManager& assets = context.GetAssets();
				for (const AssetHandle prefab : m_Prefabs)
				{
					if (const Result<AssetImportOutcome> imported = assets.Reimport(prefab); !imported)
					{
						ENGINE_ERROR("The other open scenes follow the last good version of the prefab '{}': {}", assets.GetReferencePath(prefab),
							imported.error().ToString());
					}
				}
				context.PropagatePrefabUpdate(m_Prefabs);
			}
		private:
			std::vector<AssetHandle> m_Prefabs;
			bool m_AfterUndo = false;
		};

	}

	namespace Utils {

		// Whether the canonical document of `prefab` holds a prefab instance (a Prefab component on one of its entities).
		static bool NestsPrefabInstances(const Prefab& prefab)
		{
			return std::ranges::any_of(prefab.GetEntities(), [](const Json& entity)
			{
				const auto components = entity.find("Components");
				return components != entity.end() && components->is_object() && components->contains("Prefab");
			});
		}

		AssetSummary MakeAssetSummary(const EditorAssetManager& assets, AssetHandle handle)
		{
			AssetSummary summary;
			summary.Id = handle.ToString();
			summary.Path = assets.GetReferencePath(handle);
			summary.Type = assets.GetAssetType(handle);
			return summary;
		}

		AssetDiagnosticInfo MakeAssetDiagnosticInfo(const AssetDiagnostic& diagnostic)
		{
			AssetDiagnosticInfo info;
			info.Severity = diagnostic.Severity;
			info.Code = diagnostic.Code;
			info.Path = diagnostic.Path;
			info.Message = diagnostic.Message;
			info.Hint = diagnostic.Hint;
			return info;
		}

		Status RefreshAssets(EditorContext& editor)
		{
			ENGINE_TRY_ASSIGN(const AssetRefreshReport report, editor.GetAssets().Refresh());
			static_cast<void>(report);
			return {};
		}

		Result<AssetHandle> ResolveAssetReference(EditorContext& editor, std::string_view reference)
		{
			if (reference.empty())
			{
				return std::unexpected(Error(ErrorCode::InvalidArgument, "an asset reference must not be empty")
						.WithHint("give 16 hex digits, a path such as \"Assets/Materials/Red.material\" (\"#<key>\" for a sub-asset), or an engine path"));
			}
			ENGINE_TRY_ASSIGN(const AssetReference parsed, ParseAssetReference(reference));
			if (parsed.Kind == AssetReferenceKind::ProjectPath)
				ENGINE_TRY(RefreshAssets(editor));

			const EditorAssetManager& assets = editor.GetAssets();
			if (const std::optional<AssetHandle> handle = assets.Resolve(reference); handle.has_value())
				return *handle;

			// An engine path is compared with the built-in paths (M17 friction F02), any other reference with the project's.
			const bool enginePath = parsed.Kind == AssetReferenceKind::EnginePath;
			std::vector<std::string> candidates;
			if (enginePath)
			{
				for (const BuiltinAssetEntry& entry : assets.GetBuiltins().GetEntries())
					candidates.push_back(entry.Path);
			}
			else
			{
				for (const AssetRecord* record : assets.GetRegistry().GetRecords())
				{
					if (record->Metadata.Kind == AssetMetaKind::Asset)
						candidates.push_back(std::string(record->SourcePath.GetPath()));
				}
			}
			const std::vector<std::string> suggestions = FuzzySuggest(reference, candidates);
			std::string hint = MakeDidYouMeanHint(suggestions);
			if (hint.empty())
				hint = enginePath ? "asset.list {dir: \"engine://\"} lists the built-in assets" : "asset.list lists the project's assets";
			return std::unexpected(Error(ErrorCode::NotFound, std::format("no asset '{}'", reference)).WithHint(std::move(hint)));
		}

		Result<AssetHandle> ResolveAssetParam(EditorMethodContext& context, std::string_view reference, std::string_view pointer)
		{
			Result<AssetHandle> handle = ResolveAssetReference(context.GetEditor(), reference);
			if (!handle)
				return std::unexpected(LocateAtParam(handle.error(), pointer));
			return *handle;
		}

		Result<VfsPath> ResolveAssetsPath(const EditorMethodContext& context, std::string_view path, std::string_view pointer, std::string_view extension,
			bool allowAssetsRoot)
		{
			ENGINE_TRY_ASSIGN(VfsPath resolved, context.ResolveProjectPath(path, pointer, extension));
			ENGINE_TRY_ASSIGN(const VfsPath assets, VfsPath::Create("project", "Assets"));
			if (!resolved.IsUnder(assets) || (resolved == assets && !allowAssetsRoot))
			{
				return std::unexpected(MakeParamError(ErrorCode::InvalidArgument, pointer, std::format("'{}' is not below the Assets folder", path),
					"assets live under Assets/, such as \"Assets/Materials/Red.material\""));
			}
			return resolved;
		}

		void AddMissingDirectoryEdits(const VirtualFileSystem& vfs, const VfsPath& directory, std::vector<AssetFileEdit>& edits)
		{
			std::vector<VfsPath> missing;
			for (VfsPath current = directory; !current.IsRoot() && !vfs.Exists(current); current = current.GetParent())
				missing.push_back(current);
			for (auto path = missing.rbegin(); path != missing.rend(); ++path)
				edits.push_back(AssetFileEdit{ .Path = *path, .Kind = AssetFileKind::Directory, .Before = std::nullopt, .After = Buffer() });
		}

		Status CheckAssetPathFree(const VirtualFileSystem& vfs, const VfsPath& path, std::string_view pointer)
		{
			if (vfs.Exists(path))
			{
				return std::unexpected(MakeParamError(ErrorCode::AlreadyExists, pointer, std::format("'{}' already exists", ToProjectRelative(path)),
					"choose another path, or change the existing asset"));
			}
			ENGINE_TRY_ASSIGN(const VfsPath meta, GetMetaPath(path));
			if (vfs.Exists(meta))
			{
				return std::unexpected(MakeParamError(ErrorCode::AlreadyExists, pointer, std::format("'{}' already exists", ToProjectRelative(meta)),
					"choose another path, or fix the stray .meta with project.validate {fix: true}"));
			}
			return {};
		}

		Status RecordPrefabOverrides(EditorContext& editor, AssetHandle prefab, const Prefab& current)
		{
			if (!editor.HasScene())
				return {};
			Scene& scene = editor.GetScene();
			const std::vector<std::pair<UUID, AssetHandle>> instances = FindPrefabInstances(scene, std::span<const AssetHandle>(&prefab, 1));
			if (instances.empty())
				return {};
			ENGINE_TRY_ASSIGN(const auto schemas, editor.GetScriptSchemaSnapshot());
			SceneEdit edit(editor, "Record Prefab Overrides");
			const PrefabOptions options{ .Schemas = schemas.get() };
			for (const std::pair<UUID, AssetHandle>& instance : instances)
			{
				const Entity root = scene.FindEntityByID(instance.first);
				ENGINE_TRY(WithContext(PrefabInstantiator::RefreshOverrides(root, current, options),
					std::format("recording the overrides of the prefab instance '{}'", scene.GetEntityPath(root))));
			}
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, edit.Commit());
			static_cast<void>(undoIndex);
			return {};
		}

		bool IsReferencedByPrefab(const EditorAssetManager& assets, AssetHandle prefab)
		{
			const AssetRegistry& registry = assets.GetRegistry();
			return std::ranges::any_of(assets.GetDependencyGraph().GetDependents(prefab), [&registry](AssetHandle dependent)
			{
				const std::optional<AssetRegistry::Location> location = registry.Locate(dependent);
				return location.has_value() && location->Type == AssetType::Prefab;
			});
		}

		bool UpdatesPrefabInstances(const EditorContext& editor, AssetHandle asset)
		{
			const EditorAssetManager& assets = editor.GetAssets();
			if (assets.GetAssetType(asset) != AssetType::Prefab)
				return false;
			const AssetHandle prefabs[] = { asset };
			return IsInstancedInOpenScenes(editor, prefabs) || IsReferencedByPrefab(assets, asset);
		}

		Result<std::vector<AssetHandle>> FindNestingPrefabs(const EditorContext& editor, AssetHandle prefab, const Prefab& current)
		{
			if (!IsReferencedByPrefab(editor.GetAssets(), prefab) && !NestsPrefabInstances(current))
				return std::vector<AssetHandle>{};
			const AssetHandle prefabs[] = { prefab };
			return editor.GetNestingPrefabs(prefabs);
		}

		Scope<Command> CreatePrefabPropagationCommand(std::vector<AssetHandle> prefabs, bool afterUndo)
		{
			return CreateScope<PrefabPropagationCommand>(std::move(prefabs), afterUndo);
		}

		Result<PrefabVersionUpdate> UpdatePrefabVersion(EditorContext& editor, AssetHandle prefab, const Prefab& current, Scope<Command> write,
			std::span<const AssetHandle> nesting, std::string label)
		{
			EditorAssetManager& assets = editor.GetAssets();
			std::vector<AssetHandle> rebuilt(nesting.begin(), nesting.end());
			rebuilt.push_back(prefab);
			std::ranges::sort(rebuilt);

			EditorTransaction transaction(editor, std::move(label));
			ENGINE_TRY(RecordPrefabOverrides(editor, prefab, current));
			if (!nesting.empty())
			{
				ENGINE_TRY_ASSIGN(const auto schemas, editor.GetScriptSchemaSnapshot());
				for (const AssetHandle nestingPrefab : nesting)
				{
					LoadReport report;
					ENGINE_TRY_ASSIGN(const Prefab nestingCurrent, LoadPrefabAsset(assets, nestingPrefab, editor.GetTypeRegistry(), schemas.get(), report));
					ENGINE_TRY(RecordPrefabOverrides(editor, nestingPrefab, nestingCurrent));
				}
			}

			// The step's writes reach the other open scenes (decision 15): one bracket opens them, the other closes the step.
			bool propagating = false;
			const auto beginPropagation = [&editor, prefab, &propagating]() -> Status
			{
				if (propagating)
					return {};
				propagating = true;
				ENGINE_TRY(editor.Execute(CreatePrefabPropagationCommand({ prefab }, true)));
				return {};
			};
			if (write != nullptr)
			{
				ENGINE_TRY(beginPropagation());
				ENGINE_TRY(editor.Execute(std::move(write)));
				ENGINE_TRY_ASSIGN(const AssetImportOutcome outcome, assets.Reimport(prefab));
				static_cast<void>(outcome);
			}
			PrefabVersionUpdate update;
			// The prefabs nesting it whose documents the rewrite changes: only those are written, and only those are reported.
			if (!nesting.empty())
			{
				const AssetHandle prefabs[] = { prefab };
				ENGINE_TRY_ASSIGN(NestingPrefabUpdate rewrite, editor.CreateNestingPrefabUpdateCommand(prefabs));
				if (rewrite.Rewrite != nullptr)
				{
					ENGINE_TRY(beginPropagation());
					ENGINE_TRY(editor.Execute(std::move(rewrite.Rewrite)));
				}
				update.UpdatedPrefabs = std::move(rewrite.Prefabs);
			}
			if (editor.HasScene())
			{
				update.UpdatedInstances = FindPrefabInstances(editor.GetScene(), rebuilt).size();
				ENGINE_TRY_ASSIGN(Scope<Command> instances, editor.CreatePrefabUpdateCommand(rebuilt));
				if (instances != nullptr)
					ENGINE_TRY(editor.Execute(std::move(instances)));
			}
			if (propagating)
				ENGINE_TRY(editor.Execute(CreatePrefabPropagationCommand({ prefab }, false)));
			update.UndoIndex = transaction.Commit();
			return update;
		}

		Result<uint64_t> ExecuteImportSettingsWrite(EditorContext& editor, AssetHandle asset, Scope<Command> write, std::string label)
		{
			if (!UpdatesPrefabInstances(editor, asset))
				return editor.Execute(std::move(write));
			// The overrides are recorded against the version the instances were built from, so it is loaded before the write.
			EditorAssetManager& assets = editor.GetAssets();
			LoadReport report;
			ENGINE_TRY_ASSIGN(const auto schemas, editor.GetScriptSchemaSnapshot());
			ENGINE_TRY_ASSIGN(const Prefab current, LoadPrefabAsset(assets, asset, editor.GetTypeRegistry(), schemas.get(), report));
			ENGINE_TRY_ASSIGN(const std::vector<AssetHandle> nesting, FindNestingPrefabs(editor, asset, current));
			ENGINE_TRY_ASSIGN(const PrefabVersionUpdate update, UpdatePrefabVersion(editor, asset, current, std::move(write), nesting, std::move(label)));
			return update.UndoIndex;
		}

	}

}
