#pragma once

#include "EditorCore/OpenSceneId.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"

#include <optional>
#include <span>
#include <string>
#include <vector>

// What the editor's operations on every open scene share (M20, E-SCENES; Docs/Decisions/0031-m20-contract.md decision 14:
// project.save, session.shutdown, project.upgrade, project.export, test.run, project.validate and play mode): reading the
// open scenes in tab order, and addressing one of them while the operation reads or writes it.

namespace Engine {

	class EditorContext;
	class EditorSceneTargetScope;

	namespace Utils {

		// One open scene as those operations read it.
		struct OpenSceneState
		{
			OpenSceneId Id = NoOpenScene;  // its EditorContext id (the addressed scene's is EditorContext::GetAddressedSceneId)
			bool Addressed = false;        // the addressed scene, which EditorContext's members that name "the open scene" reach
			std::optional<VfsPath> Path{}; // the file it was opened from or last saved to; nullopt for a scene never saved
			std::string Name{};
			bool Dirty = false;
		};

		// Every open scene in tab order (EditorContext::GetOpenScenes), as EditorContext::DescribeOpenScene describes it.
		// Empty without an open scene.
		[[nodiscard]] std::vector<OpenSceneState> ListOpenScenes(const EditorContext& editor);

		// The open scenes of ListOpenScenes with unsaved changes, in tab order.
		[[nodiscard]] std::vector<OpenSceneState> ListDirtyOpenScenes(const EditorContext& editor);

		// "'<name>'" for each of `scenes`, comma-separated: how an error names them.
		[[nodiscard]] std::string FormatOpenSceneNames(std::span<const OpenSceneState> scenes);

		// Makes `scene` the addressed scene until the returned scope ends: an EditorSceneTargetScope for an open scene other
		// than the one addressed when it is called, none (null) for that scene itself, which needs none. Errors: those of
		// EditorSceneTargetScope::Begin (NotFound when the scene closed; InvalidState inside a transaction, a dry run or
		// another scope).
		[[nodiscard]] Result<Scope<EditorSceneTargetScope>> AddressOpenScene(EditorContext& editor, const OpenSceneState& scene);

	}

}
