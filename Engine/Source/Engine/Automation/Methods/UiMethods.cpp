#include "EnginePCH.h"
#include "Engine/Automation/Methods/UiMethods.h"

#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"

// The ui domain (UiMethods.h): the types and the registration are final; the handler is a stub of the M22 contract (C22.1,
// Docs/Decisions/0033-m22-contract.md decision 8) that stream E implements.

namespace Engine {

	namespace Automation {

		Result<UiTreeResult> UiTree(AutomationMethodContext& /*context*/, const UiTreeParams& /*params*/)
		{
			ENGINE_CONTRACT_STUB();
			return MakeError(ErrorCode::Unsupported, "ui.tree (Docs/Decisions/0033-m22-contract.md decision 8, stream E)");
		}

	}

	void RegisterUiMethodTypes(TypeRegistry& registry)
	{
		registry.Struct<UiPixelRect>("UiPixelRect", "A rectangle in scene pixels: x right and y down from the scene image's top-left corner.")
			.Field("x", &UiPixelRect::X, "The left edge.", { .Unit = "px" })
			.Field("y", &UiPixelRect::Y, "The top edge.", { .Unit = "px" })
			.Field("width", &UiPixelRect::Width, "The width.", { .Unit = "px" })
			.Field("height", &UiPixelRect::Height, "The height.", { .Unit = "px" });

		registry.Struct<UiTreeEntry>("UiTreeEntry", "One canvas or UI element of ui.tree's JSON list, in draw order.")
			.Field("id", &UiTreeEntry::Id, "The entity's id.")
			.Field("name", &UiTreeEntry::Name, "The entity's name.")
			.Field("path", &UiTreeEntry::Path, "The entity's path.")
			.Field("canvas", &UiTreeEntry::Canvas, "The id of the canvas the element belongs to (a canvas's own id).")
			.Field("parent", &UiTreeEntry::Parent, "The parent element's id, or the canvas's for a top-level element; empty for a canvas.")
			.Field("depth", &UiTreeEntry::Depth, "0 for a canvas, 1 for a top-level element, one more per level below it.")
			.Field("components", &UiTreeEntry::Components, "The UI components the entity holds, in registry order.")
			.Field("rect", &UiTreeEntry::Rect, "The laid-out rectangle in scene pixels; a canvas's is the whole scene image.")
			.Field("sortOrder", &UiTreeEntry::SortOrder, "A canvas's UiCanvas SortOrder (canvases draw in ascending order); 0 for an element.")
			.Field("scale", &UiTreeEntry::Scale,
				"A canvas's scale: the scene pixels per canvas unit at the resolution laid out at, which turns a pixel measurement into "
				"UiRect's canvas units; 0 for an element.")
			.Field("clipped", &UiTreeEntry::Clipped, "Whether a clipping ancestor (UiRect.ClipChildren) clips the element to clip.")
			.Field("clip", &UiTreeEntry::Clip, "The clip rectangle in whole scene pixels, when clipped.")
			.Field("visible", &UiTreeEntry::Visible, "Whether the element covers a pixel of the scene image once clipped.")
			.Field("text", &UiTreeEntry::Text, "Its UiText's text, cut to 256 bytes; empty without one.")
			.Field("interactable", &UiTreeEntry::Interactable, "Whether the element takes pointer and navigation input (W22.2; false before).")
			.Field("focused", &UiTreeEntry::Focused, "Whether the element has the focus (W22.2; false before).")
			.Field("hovered", &UiTreeEntry::Hovered, "Whether the pointer is over the element (W22.2; false before).")
			.Field("pressed", &UiTreeEntry::Pressed, "Whether the element is pressed (W22.2; false before).")
			.Field("widget", &UiTreeEntry::Widget, "The element's widget kind, such as Button or Slider (W22.2); empty for none.");

		registry.Struct<UiTreeParams>("UiTreeParams", "The params of ui.tree.")
			.Field("canvas", &UiTreeParams::Canvas, "An entity with a UiCanvas whose UI alone is listed; omitted: every canvas.")
			.Field("depth", &UiTreeParams::Depth, "The most levels listed below each canvas; 0: unlimited.")
			.Field("format", &UiTreeParams::Format, "Text: a compact outline; Json: a flat list of entries.")
			.Field("width", &UiTreeParams::Width,
				"The scene resolution's width to lay out at, with height; omitted: the play session's own, or the project's window size for "
				"the edit scene.",
				{ .Min = 0.0, .Max = 16384.0, .Unit = "px" })
			.Field("height", &UiTreeParams::Height, "The scene resolution's height to lay out at, with width.", { .Min = 0.0, .Max = 16384.0, .Unit = "px" })
			.Field("target", &UiTreeParams::Target, "The scene to read; omitted: the play scene while playing, else the edit scene.");

		registry.Struct<UiTreeResult>("UiTreeResult", "A scene's UI laid out in scene pixels.")
			.Field("scene", &UiTreeResult::Scene, "The scene read.")
			.Field("width", &UiTreeResult::Width, "The scene resolution's width laid out at.", { .Unit = "px" })
			.Field("height", &UiTreeResult::Height, "The scene resolution's height laid out at.", { .Unit = "px" })
			.Field("text", &UiTreeResult::Text, "The outline (format text).")
			.Field("elements", &UiTreeResult::Elements, "The canvases in draw order, each followed by its elements in draw order (format json).");
	}

	void RegisterUiMethods(MethodRegistry& methods)
	{
		Json treeExample = Json::object();
		treeExample["format"] = "Text";
		methods.Add(
			{
				.Name = "ui.tree",
				.Description = "Shows the scene's UI laid out in scene pixels: every canvas and element with its rectangle, clip, visibility, "
							   "components and text, as a compact text outline or a flat JSON list, optionally for one canvas and to a depth.",
				.ExposeAsTool = true,
				.SupportsSceneTarget = true,
				.AvailableInRuntime = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Outline every canvas.", .Params = treeExample } },
			},
			&Automation::UiTree);
	}

}
