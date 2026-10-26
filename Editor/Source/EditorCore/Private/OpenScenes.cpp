#include "EditorPCH.h"
#include "EditorCore/Private/OpenScenes.h"

#include "EditorCore/EditorContext.h"

#include <format>
#include <vector>

namespace Engine {

	namespace Utils {

		std::vector<OpenSceneState> ListOpenScenes(const EditorContext& editor)
		{
			const OpenSceneId addressed = editor.GetAddressedSceneId();
			std::vector<OpenSceneState> scenes;
			for (const OpenSceneId id : editor.GetOpenScenes())
			{
				const Result<OpenSceneInfo> info = editor.DescribeOpenScene(id);
				if (info)
					scenes.push_back(OpenSceneState{ .Id = id, .Addressed = id == addressed, .Path = info->Path, .Name = info->Name, .Dirty = info->Dirty });
			}
			return scenes;
		}

		std::vector<OpenSceneState> ListDirtyOpenScenes(const EditorContext& editor)
		{
			std::vector<OpenSceneState> scenes = ListOpenScenes(editor);
			std::erase_if(scenes, [](const OpenSceneState& scene)
			{
				return !scene.Dirty;
			});
			return scenes;
		}

		std::string FormatOpenSceneNames(std::span<const OpenSceneState> scenes)
		{
			std::string names;
			for (const OpenSceneState& scene : scenes)
				names += std::format("{}'{}'", names.empty() ? "" : ", ", scene.Name);
			return names;
		}

		Result<Scope<EditorSceneTargetScope>> AddressOpenScene(EditorContext& editor, const OpenSceneState& scene)
		{
			// The addressed scene now, which an operation that replaced a scene since the list was read may have changed.
			if (scene.Id == editor.GetAddressedSceneId())
				return Scope<EditorSceneTargetScope>();
			return EditorSceneTargetScope::Begin(editor, scene.Id);
		}

	}

}
