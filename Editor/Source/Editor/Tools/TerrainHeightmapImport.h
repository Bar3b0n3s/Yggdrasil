#pragma once

#include "Editor/FileDialog.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

// The terrain inspector's Import Heightmap action (M26, C-TERR; Docs/Decisions/0037-m26-contract.md decisions 20 and 31 (j)):
// a file of its own beside the terrain brush that InspectorPanel (M24's in phase 3) only calls. The action is
// terrain.importHeightmap, its method (Roadmap rule 5): the heightmap is chosen through the FileDialog API (a 16-bit .png, or
// a .raw or .r16 of 16-bit samples), which only fills the method's source; then, when the terrain had no heightfield of the
// project to import into, entity.update assigns the imported one, a second undo step.

namespace Engine {

	class EditorContext;
	struct EditorPanelContext;
	struct TerrainComponent;

	// Where the action imports a heightmap for a Terrain.
	struct TerrainHeightmapImportTarget
	{
		std::string Path{};  // the .heightfield terrain.importHeightmap writes, project-relative
		bool Assign = false; // the Terrain does not name it yet: entity.update assigns it after the import
	};

	// The target of importing the heightmap `source` (a file path) for `terrain`: the source of its Heightfield when that is a
	// Heightfield of the project (replaced in place, so the terrain, and any other naming it, keeps the handle and follows),
	// else the first free `Assets/Terrain/<source's stem>.heightfield`, `-<n>` appended from n = 2 (free: neither the file
	// nor its .meta exists, so no file is ever replaced), to assign. Errors: InvalidArgument for a source without a file
	// stem; AlreadyExists when every suffixed name is taken; the path rules' (Validation).
	[[nodiscard]] Result<TerrainHeightmapImportTarget> FindTerrainHeightmapImportTarget(const EditorContext& editor, const TerrainComponent& terrain,
		std::string_view source);

	// The action. Draw it under the Terrain component's header while one entity of the edit scene is selected: an "Import
	// heightmap..." button, disabled while the editor is read-only or playing or an import it queued is pending, that opens the
	// dialog; the chosen file queues terrain.importHeightmap {source, path} (FindTerrainHeightmapImportTarget) and, once it
	// succeeded, entity.update {entity, components: {Terrain: {Heightfield}}} when the target says so. Drawn for another entity,
	// it cancels an open dialog. Main thread inside ImGui; keeps only UI state (the dialog, its entity and the queued tickets).
	class TerrainHeightmapImport
	{
	public:
		// Draws the button and the open dialog for `entity` and collects the queued actions' results. Errors: the dialog's,
		// FindTerrainHeightmapImportTarget's and the queued methods' (each once).
		[[nodiscard]] Status Draw(EditorPanelContext& context, UUID entity);
	private:
		// Takes the queued actions' results: an import that succeeded queues its assignment.
		[[nodiscard]] Status CollectResults(EditorPanelContext& context);

		FileDialog m_Dialog{};
		UUID m_DialogEntity{};                  // the entity the open dialog imports for
		std::optional<uint64_t> m_Import{};     // the queued terrain.importHeightmap
		UUID m_ImportEntity{};                  // its entity
		bool m_Assign = false;                  // its target is assigned after it
		std::optional<uint64_t> m_Assignment{}; // the queued entity.update
	};

}
