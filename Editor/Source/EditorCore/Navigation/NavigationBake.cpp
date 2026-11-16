#include "EditorPCH.h"
#include "EditorCore/Navigation/NavigationBake.h"

#include "EditorCore/Commands/SceneEdit.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Navigation/NavMeshBuilder.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Asset/AssetMetadata.h"
#include "Engine/Asset/AssetRegistry.h"
#include "Engine/Asset/NavMeshData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Physics/PhysicsLayers.h"
#include "Engine/Scene/Components/NavigationComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/NavigationBakeInput.h"
#include "Engine/Scene/Scene.h"

#include <format>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

// navigation.bake's work (NavigationBake.h; Docs/Decisions/0045-m30-navigation.md decision 8): the input and the mesh first,
// so a failure writes nothing; then the file, written only where its bytes changed, its .meta for a new source, and its
// import; then the component's NavMesh in one undoable edit, recorded only when it changes.

namespace Engine {

	namespace Utils {

		// The baked file of the scene saved at `scenePath`: Assets/Navigation/ followed by the scene's project path below
		// Assets/ with .navmesh for its extension (Assets/Scenes/Arena.scene bakes into Assets/Navigation/Scenes/Arena.navmesh).
		// Errors: those of the path rules (Validation).
		[[nodiscard]] static Result<VfsPath> GetNavMeshPath(const VfsPath& scenePath)
		{
			constexpr std::string_view AssetsPrefix = "Assets/";
			std::string_view relative = scenePath.GetPath();
			if (relative.starts_with(AssetsPrefix))
				relative.remove_prefix(AssetsPrefix.size());
			relative.remove_suffix(scenePath.GetExtension().size());
			return VfsPath::Create("project", std::format("Assets/Navigation/{}.navmesh", relative));
		}

		// Writes `bytes` to `path` where they changed and a .meta for a source the registry does not know (its importer's
		// defaults), then imports it when either was written. Sets the result's Path, Asset and Changed.
		[[nodiscard]] static Status CommitNavMeshFile(EditorContext& editor, const VfsPath& path, const Buffer& bytes, NavigationBakeResult& result)
		{
			EditorAssetManager& assets = editor.GetAssets();
			result.Path = std::string(path.GetPath());
			const Result<Buffer> existing = editor.GetVfs().ReadFile(path);
			result.Changed = !existing.has_value() || *existing != bytes;
			if (result.Changed)
			{
				ENGINE_TRY(WithContext(editor.CreateProjectDirectory(path.GetParent()), std::format("creating '{}'", path.GetParent().GetPath())));
				ENGINE_TRY(WithContext(editor.WriteProjectFile(path, bytes), std::format("writing '{}'", result.Path)));
			}

			bool newMeta = false;
			if (const AssetRecord* record = assets.GetRegistry().FindBySourcePath(path))
			{
				result.Asset = record->Metadata.Handle;
			}
			else
			{
				ENGINE_TRY_ASSIGN(const AssetMetadata metadata, assets.CreateMetadata(path));
				ENGINE_TRY_ASSIGN(const VfsPath metaPath, GetMetaPath(path));
				ENGINE_TRY(WithContext(editor.WriteProjectFile(metaPath, AsBytes(SerializeAssetMetadata(metadata))), std::format("writing the .meta of '{}'", result.Path)));
				result.Asset = metadata.Handle;
				newMeta = true;
			}
			if (result.Changed || newMeta)
			{
				const Result<AssetImportOutcome> imported = assets.Reimport(result.Asset);
				if (!imported.has_value())
					return std::unexpected(Error(imported.error()).WithContext(std::format("importing the baked '{}'", result.Path)));
			}
			return {};
		}

	}

	Result<NavigationBakeResult> BakeNavigation(EditorContext& editor, OpenSceneId scene)
	{
		if (!editor.HasProject())
			return MakeError(ErrorCode::InvalidState, "navigation.bake needs an open project");
		if (editor.FindScene(scene) == nullptr)
			return MakeError(ErrorCode::NotFound, "no open scene has the id {}", scene);
		// The bake addresses its scene for its whole stretch: the input and the undo step are that scene's.
		Scope<EditorSceneTargetScope> target;
		if (editor.GetAddressedSceneId() != scene)
		{
			ENGINE_TRY_ASSIGN(target, EditorSceneTargetScope::Begin(editor, scene));
		}
		// The mesh is baked from the edit scene and referenced by it; a play session runs the scene it copied.
		if (editor.GetPlay().IsPlaying())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "navigation is baked from the edit scene, not the play scene, and the editor is playing")
					.WithHint("stop playing (play.stop), then bake"));
		}
		const std::optional<VfsPath>& scenePath = editor.GetScenePath();
		if (!scenePath.has_value())
		{
			return std::unexpected(Error(ErrorCode::InvalidState, "the scene was never saved, and a bake is written beside its file")
					.WithHint("save the scene first with scene.save {path}"));
		}

		const PhysicsSettings& physics = editor.GetProject().GetSettings().Physics;
		ENGINE_TRY_ASSIGN(const PhysicsLayerTable layers, WithContext(PhysicsLayerTable::Create(physics.Layers, physics.Collisions), "reading the project's physics layers"));
		Scene& bakedScene = editor.GetScene();
		ENGINE_TRY_ASSIGN(const NavigationBakeInput input, CollectNavigationBakeInput(bakedScene, layers, editor.GetAssets()));
		ENGINE_TRY_ASSIGN(const NavMeshData mesh, BuildNavMesh(input));
		ENGINE_TRY_ASSIGN(const VfsPath path, Utils::GetNavMeshPath(*scenePath));

		NavigationBakeResult result;
		result.Entity = input.NavigationEntity;
		result.InputHash = mesh.InputHash;
		result.SkippedBodies = input.SkippedBodies;
		for (const NavMeshAgent& agent : mesh.Agents)
		{
			result.Agents.push_back({ .Name = agent.Name,
				.Polygons = static_cast<uint32_t>(agent.GetPolygonCount()),
				.Vertices = static_cast<uint32_t>(agent.GetVertexCount()) });
		}
		ENGINE_TRY(Utils::CommitNavMeshFile(editor, path, WriteNavMeshSource(mesh), result));
		editor.GetAssets().WaitIdle();

		SceneEdit edit(editor, "Bake Navigation");
		const Entity entity = bakedScene.FindEntityByID(input.NavigationEntity);
		const AssetHandle baked = result.Asset;
		if (entity.GetComponent<NavigationComponent>().NavMesh.GetHandle() != baked)
		{
			entity.Patch<NavigationComponent>([baked](NavigationComponent& navigation)
			{
				navigation.NavMesh = TypedAssetHandle<AssetType::NavMesh>(baked);
			});
			ENGINE_TRY(edit.Commit());
		}
		else
		{
			// A bake that changes no component records no undo step.
			edit.Cancel();
		}
		ENGINE_INFO("Baked the navigation of '{}' into '{}': {} agent type(s){}", scenePath->GetPath(), result.Path, result.Agents.size(),
			result.Changed ? "" : ", unchanged");
		return result;
	}

}
