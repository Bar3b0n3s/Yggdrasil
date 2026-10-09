#include "EditorPCH.h"
#include "EditorCore/Automation/EditorMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/EditorUiState.h"
#include "EditorCore/Play/EditorPlayController.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Session/PlaySession.h"

namespace Engine {

	Result<EditorStateResult> Automation::EditorState(EditorMethodContext& context, const NoParams& /*params*/)
	{
		const EditorContext& editor = context.GetEditor();
		EditorStateResult result;
		for (const EditorPanel panel : editor.GetUiState().GetOpenPanels())
			result.OpenPanels.emplace_back(EditorPanelToString(panel));
		result.Mode = editor.GetPlay().GetPlayStateName();
		result.SelectionTarget = editor.GetSelectionTarget();
		result.UiFrame = ToAutomationCounter(editor.GetUiState().GetCompletedFrame());
		result.SceneChangedOnDisk = editor.IsSceneChangedOnDisk();
		if (const PlaySession* session = editor.GetPlay().GetSession(); session != nullptr && session->GetLockstepOwner() != NoClient)
			result.LockstepOwner = context.GetClientName(session->GetLockstepOwner());
		if (!editor.GetSelection().empty())
		{
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(result.SelectionTarget, true, false));
			for (const UUID id : editor.GetSelection())
			{
				const Entity entity = scene->FindEntityByID(id);
				if (entity.IsValid())
					result.Selection.push_back(context.MakeEntitySummary(entity));
			}
		}
		return result;
	}

	void RegisterEditorStateMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<EditorStateResult>("EditorStateResult", "The editor's current panels, play state and selection.")
			.Field("openPanels", &EditorStateResult::OpenPanels, "Actually shown panels in stable panel order.")
			.Field("mode", &EditorStateResult::Mode, "Edit, Play, Simulate or Paused.")
			.Field("selection", &EditorStateResult::Selection, "Selected entities that still exist in the selection's scene.")
			.Field("selectionTarget", &EditorStateResult::SelectionTarget, "The scene to which selection refers.")
			.Field("lockstepOwner", &EditorStateResult::LockstepOwner, "The lockstep owner's client name, empty when unowned.")
			.Field("uiFrame", &EditorStateResult::UiFrame, "Completed CPU UI frame count, saturated to 32 bits; zero without UI frames.")
			.Field("sceneChangedOnDisk", &EditorStateResult::SceneChangedOnDisk, "Whether the scene's source changed externally.");
	}

	void RegisterEditorStateMethods(MethodRegistry& methods)
	{
		methods.Add({ .Name = "editor.state", .Description = "Reports current editor panels, selection and play state without needing a renderer.", .AllowedInBatch = true, .Examples = { { .Description = "Read editor state.", .Params = Json::object() } } }, &Automation::EditorState);
	}

}
