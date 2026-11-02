#include "EditorPCH.h"
#include "EditorCore/Automation/UiAuthoringMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

#include <nlohmann/json.hpp>

// ui.createWidget (UiAuthoringMethods.h; Docs/Decisions/0033-m22-contract.md decision 32). The types and the registration are
// final; the handler is a stub of the M22 contract (C22.2) that stream D implements.

namespace Engine {

	namespace Automation {

		Result<UiCreateWidgetResult> UiCreateWidget(EditorMethodContext& /*context*/, const UiCreateWidgetParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return std::unexpected(Error(ErrorCode::Unsupported, "ui.createWidget is not implemented yet (Docs/Decisions/0033-m22-contract.md decision 32, stream D)"));
		}

	}

	void RegisterUiAuthoringMethodTypes(TypeRegistry& registry)
	{
		registry.Enum<UiWidgetKind>("UiWidgetKind", "The UI widget ui.createWidget builds.")
			.Entry(UiWidgetKind::Canvas, "Canvas", "A UI canvas, the root of a UI tree.")
			.Entry(UiWidgetKind::Panel, "Panel", "An image stretched over its parent: a background for a group of elements.")
			.Entry(UiWidgetKind::Image, "Image", "A 100 by 100 image.")
			.Entry(UiWidgetKind::Text, "Text", "A centred text.")
			.Entry(UiWidgetKind::Button, "Button", "A button with an image and a Label child.")
			.Entry(UiWidgetKind::Toggle, "Toggle", "A toggle box with a Check child that shows while it is on, and a Label child.")
			.Entry(UiWidgetKind::Slider, "Slider", "A horizontal slider with Fill and Handle children.")
			.Entry(UiWidgetKind::ScrollView, "ScrollView", "A vertical scroll view whose Content child stacks its children.");

		registry.Struct<UiCreateWidgetParams>("UiCreateWidgetParams", "The params of ui.createWidget.")
			.Field("kind", &UiCreateWidgetParams::Kind, "The widget to build.")
			.Field("parent", &UiCreateWidgetParams::Parent,
				"The parent entity (16 hex digits, a unique id prefix of at least 6, or a path); a kind other than Canvas below a parent "
				"without a UiCanvas at or above it goes below a new canvas created there. Omitted: a Canvas is a root, any other kind goes "
				"below a new root canvas.")
			.Field("index", &UiCreateWidgetParams::Index,
				"The created root's position among the parent's children (among the scene's roots without a parent); omitted: last.")
			.Field("name", &UiCreateWidgetParams::Name, "The widget root's name; omitted: the kind's name.")
			.Field("text", &UiCreateWidgetParams::Text, "The label of a Text, Button or Toggle; omitted: the kind's name.");

		registry.Struct<UiCreateWidgetResult>("UiCreateWidgetResult", "The created widget.")
			.Field("entity", &UiCreateWidgetResult::Entity, "The widget's root.")
			.Field("entities", &UiCreateWidgetResult::Entities, "The id of every created entity, in creation order (a new canvas first).")
			.Field("undoIndex", &UiCreateWidgetResult::UndoIndex, "The command's undo index; 0 in a dry run or a batch.");
	}

	void RegisterUiAuthoringMethods(MethodRegistry& methods)
	{
		Json buttonExample = Json::object();
		buttonExample["kind"] = "Button";
		buttonExample["parent"] = "/Canvas";
		buttonExample["name"] = "Play";
		buttonExample["text"] = "Play";
		methods.Add(
			{
				.Name = "ui.createWidget",
				.Description = "Builds a UI widget (a canvas, panel, image, text, button, toggle, slider or scroll view) with its child "
							   "entities and components in the edit scene, as one undoable command; the editor's Create UI menu submits it.",
				.RequiredParams = { "kind" },
				.ExposeAsTool = true,
				.Mutates = true,
				.SupportsDryRun = true,
				.SupportsSceneTarget = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Add a Play button to the canvas.", .Params = buttonExample } },
			},
			&Automation::UiCreateWidget);
	}

}
