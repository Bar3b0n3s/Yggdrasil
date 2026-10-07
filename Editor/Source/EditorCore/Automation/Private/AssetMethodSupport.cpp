#include "EditorPCH.h"
#include "EditorCore/Automation/Private/AssetMethodSupport.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Automation/Private/MethodSupport.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Private/PrefabInstances.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/AssetReference.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Reflection/FuzzySuggest.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/PrefabInstantiator.h"
#include "Engine/Scene/Scene.h"

namespace Engine {

	namespace Utils {

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

			std::vector<std::string> candidates;
			for (const AssetRecord* record : assets.GetRegistry().GetRecords())
			{
				if (record->Metadata.Kind == AssetMetaKind::Asset)
					candidates.push_back(std::string(record->SourcePath.GetPath()));
			}
			const std::vector<std::string> suggestions = FuzzySuggest(reference, candidates);
			return std::unexpected(Error(ErrorCode::NotFound, std::format("no asset '{}'", reference))
					.WithHint(suggestions.empty() ? std::string("asset.list lists the project's assets") : MakeDidYouMeanHint(suggestions)));
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
			SceneEdit edit(editor, "Record Prefab Overrides");
			const PrefabOptions options{ .Schemas = nullptr };
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

	}

}
