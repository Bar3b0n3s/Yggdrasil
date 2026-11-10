#include "EditorPCH.h"
#include "EditorCore/Automation/UiAuthoringMethods.h"

#include "EditorCore/Automation/EditorMethodContext.h"
#include "EditorCore/Commands/SceneEdit.h"
#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/UiButtonComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiLayoutComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Components/UiScrollViewComponent.h"
#include "Engine/Scene/Components/UiSliderComponent.h"
#include "Engine/Scene/Components/UiTextComponent.h"
#include "Engine/Scene/Components/UiTextInputComponent.h"
#include "Engine/Scene/Components/UiToggleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <nlohmann/json.hpp>

#include <format>
#include <optional>
#include <string_view>

// ui.createWidget (UiAuthoringMethods.h; Docs/Decisions/0033-m22-contract.md decision 32): the templates of the header's
// table, built in the addressed edit scene as one SceneEdit.

namespace Engine {

	namespace Utils {

		// The kind's name: the default name of the widget's root and its label's default text.
		static std::string_view GetUiWidgetKindName(UiWidgetKind kind)
		{
			switch (kind)
			{
				case UiWidgetKind::Canvas:
					return "Canvas";
				case UiWidgetKind::Panel:
					return "Panel";
				case UiWidgetKind::Image:
					return "Image";
				case UiWidgetKind::Text:
					return "Text";
				case UiWidgetKind::Button:
					return "Button";
				case UiWidgetKind::Toggle:
					return "Toggle";
				case UiWidgetKind::Slider:
					return "Slider";
				case UiWidgetKind::ScrollView:
					return "ScrollView";
				case UiWidgetKind::TextInput:
					return "TextInput";
			}
			return "Widget";
		}

		// The kinds that take `text`: a Text, a Button's or a Toggle's label, and a TextInput's placeholder (C22.3).
		static bool HasUiWidgetLabel(UiWidgetKind kind)
		{
			return kind == UiWidgetKind::Text || kind == UiWidgetKind::Button || kind == UiWidgetKind::Toggle || kind == UiWidgetKind::TextInput;
		}

		// The default root name: the kind's name, spelt as words where it joins two.
		static std::string GetUiWidgetDefaultName(UiWidgetKind kind)
		{
			switch (kind)
			{
				case UiWidgetKind::ScrollView:
					return "Scroll View";
				case UiWidgetKind::TextInput:
					return "Text Input";
				case UiWidgetKind::Canvas:
				case UiWidgetKind::Panel:
				case UiWidgetKind::Image:
				case UiWidgetKind::Text:
				case UiWidgetKind::Button:
				case UiWidgetKind::Toggle:
				case UiWidgetKind::Slider:
					break;
			}
			return std::string(GetUiWidgetKindName(kind));
		}

		// The default `text`: the kind's name, or a TextInput's placeholder "Enter text" (C22.3, decision 53).
		static std::string GetUiWidgetDefaultText(UiWidgetKind kind)
		{
			return kind == UiWidgetKind::TextInput ? std::string("Enter text") : std::string(GetUiWidgetKindName(kind));
		}

		// Whether `entity` or one of its ancestors holds a UiCanvas.
		static bool IsInUiCanvas(ConstEntity entity)
		{
			for (; entity.IsValid(); entity = entity.GetParent())
			{
				if (entity.HasComponent<UiCanvasComponent>())
					return true;
			}
			return false;
		}

		// A rectangle stretched over its parent element, its Size added to the stretched span (a negative one insets it).
		static UiRectComponent MakeStretchedUiRect(glm::vec2 size = glm::vec2(0.0f))
		{
			return UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(1.0f), .Size = size };
		}

		// Builds the template of `kind` (UiAuthoringMethods.h's table) on the entity `root`, its children created below it and
		// appended to `created` in the table's order. `text` is the label of a Text, Button or Toggle, or a TextInput's
		// placeholder.
		static void BuildUiWidget(Scene& scene, Entity root, UiWidgetKind kind, const std::string& text, std::vector<UUID>& created)
		{
			const auto child = [&scene, root, &created](std::string_view name)
			{
				const Entity entity = scene.CreateEntity(name, root);
				created.push_back(entity.GetUUID());
				return entity;
			};
			switch (kind)
			{
				case UiWidgetKind::Canvas:
					root.AddComponent<UiCanvasComponent>();
					return;
				case UiWidgetKind::Panel:
					root.AddComponent<UiRectComponent>(MakeStretchedUiRect());
					root.AddComponent<UiImageComponent>(UiImageComponent{ .Color = glm::vec4(0.1f, 0.1f, 0.1f, 0.8f) });
					return;
				case UiWidgetKind::Image:
					root.AddComponent<UiRectComponent>(UiRectComponent{ .Size = glm::vec2(100.0f) });
					root.AddComponent<UiImageComponent>();
					return;
				case UiWidgetKind::Text:
					root.AddComponent<UiRectComponent>(UiRectComponent{ .Size = glm::vec2(200.0f, 50.0f) });
					root.AddComponent<UiTextComponent>(UiTextComponent{ .Text = text,
						.Size = 32.0f,
						.Alignment = TextAlignment::Center,
						.VerticalAlignment = UiTextVerticalAlignment::Middle });
					return;
				case UiWidgetKind::Button:
				{
					root.AddComponent<UiRectComponent>(UiRectComponent{ .Size = glm::vec2(200.0f, 50.0f) });
					root.AddComponent<UiImageComponent>(UiImageComponent{ .Color = glm::vec4(0.9f, 0.9f, 0.9f, 1.0f) });
					root.AddComponent<UiButtonComponent>();
					const Entity label = child("Label");
					label.AddComponent<UiRectComponent>(MakeStretchedUiRect());
					label.AddComponent<UiTextComponent>(UiTextComponent{ .Text = text,
						.Size = 28.0f,
						.Color = glm::vec4(0.05f, 0.05f, 0.05f, 1.0f),
						.Alignment = TextAlignment::Center,
						.VerticalAlignment = UiTextVerticalAlignment::Middle });
					return;
				}
				case UiWidgetKind::Toggle:
				{
					root.AddComponent<UiRectComponent>(UiRectComponent{ .Size = glm::vec2(32.0f) });
					root.AddComponent<UiImageComponent>(UiImageComponent{ .Color = glm::vec4(0.9f, 0.9f, 0.9f, 1.0f) });
					const Entity check = child("Check");
					check.AddComponent<UiRectComponent>(MakeStretchedUiRect(glm::vec2(-12.0f)));
					check.AddComponent<UiImageComponent>(UiImageComponent{ .Color = glm::vec4(0.1f, 0.1f, 0.1f, 1.0f) });
					const Entity label = child("Label");
					label.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(1.0f, 0.5f),
						.AnchorMax = glm::vec2(1.0f, 0.5f),
						.Pivot = glm::vec2(0.0f, 0.5f),
						.Position = glm::vec2(8.0f, 0.0f),
						.Size = glm::vec2(160.0f, 32.0f) });
					label.AddComponent<UiTextComponent>(
						UiTextComponent{ .Text = text, .Size = 24.0f, .VerticalAlignment = UiTextVerticalAlignment::Middle });
					root.AddComponent<UiToggleComponent>(UiToggleComponent{ .Graphic = check.GetUUID() });
					return;
				}
				case UiWidgetKind::Slider:
				{
					root.AddComponent<UiRectComponent>(UiRectComponent{ .Size = glm::vec2(200.0f, 20.0f) });
					root.AddComponent<UiImageComponent>(UiImageComponent{ .Color = glm::vec4(0.3f, 0.3f, 0.3f, 1.0f) });
					const Entity fill = child("Fill");
					fill.AddComponent<UiRectComponent>(
						UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(0.0f, 1.0f), .Size = glm::vec2(0.0f) });
					fill.AddComponent<UiImageComponent>(UiImageComponent{ .Color = glm::vec4(0.3f, 0.6f, 1.0f, 1.0f) });
					const Entity handle = child("Handle");
					handle.AddComponent<UiRectComponent>(
						UiRectComponent{ .AnchorMin = glm::vec2(0.0f, 0.5f), .AnchorMax = glm::vec2(0.0f, 0.5f), .Size = glm::vec2(20.0f, 32.0f) });
					handle.AddComponent<UiImageComponent>(UiImageComponent{ .Color = glm::vec4(0.95f, 0.95f, 0.95f, 1.0f) });
					root.AddComponent<UiSliderComponent>(UiSliderComponent{ .Fill = fill.GetUUID(), .Handle = handle.GetUUID() });
					return;
				}
				case UiWidgetKind::ScrollView:
				{
					root.AddComponent<UiRectComponent>(UiRectComponent{ .Size = glm::vec2(300.0f, 200.0f) });
					root.AddComponent<UiImageComponent>(UiImageComponent{ .Color = glm::vec4(0.1f, 0.1f, 0.1f, 0.8f) });
					const Entity content = child("Content");
					content.AddComponent<UiRectComponent>(UiRectComponent{ .AnchorMin = glm::vec2(0.0f),
						.AnchorMax = glm::vec2(1.0f, 0.0f),
						.Pivot = glm::vec2(0.5f, 0.0f),
						.Size = glm::vec2(0.0f, 400.0f) });
					content.AddComponent<UiLayoutComponent>(UiLayoutComponent{ .Kind = UiLayoutKind::Stack,
						.Direction = UiLayoutDirection::Vertical,
						.Padding = UiPadding{ .Left = 8.0f, .Top = 8.0f, .Right = 8.0f, .Bottom = 8.0f },
						.Spacing = glm::vec2(0.0f, 8.0f) });
					root.AddComponent<UiScrollViewComponent>(UiScrollViewComponent{ .Content = content.GetUUID() });
					return;
				}
				case UiWidgetKind::TextInput:
				{
					// C22.3 (decision 53): the field's Text and Placeholder children share one inset single-line layout.
					root.AddComponent<UiRectComponent>(UiRectComponent{ .Size = glm::vec2(240.0f, 32.0f) });
					root.AddComponent<UiImageComponent>(UiImageComponent{ .Color = glm::vec4(0.15f, 0.15f, 0.15f, 1.0f) });
					const Entity shown = child("Text");
					shown.AddComponent<UiRectComponent>(MakeStretchedUiRect(glm::vec2(-12.0f)));
					shown.AddComponent<UiTextComponent>(
						UiTextComponent{ .Size = 20.0f, .Color = glm::vec4(1.0f), .VerticalAlignment = UiTextVerticalAlignment::Middle, .Wrap = false });
					const Entity placeholder = child("Placeholder");
					placeholder.AddComponent<UiRectComponent>(MakeStretchedUiRect(glm::vec2(-12.0f)));
					placeholder.AddComponent<UiTextComponent>(UiTextComponent{ .Text = text,
						.Size = 20.0f,
						.Color = glm::vec4(0.6f, 0.6f, 0.6f, 1.0f),
						.VerticalAlignment = UiTextVerticalAlignment::Middle,
						.Wrap = false });
					root.AddComponent<UiTextInputComponent>(UiTextInputComponent{ .TextElement = shown.GetUUID(), .Placeholder = placeholder.GetUUID() });
					return;
				}
			}
		}

	}

	namespace Automation {

		Result<UiCreateWidgetResult> UiCreateWidget(EditorMethodContext& context, const UiCreateWidgetParams& params)
		{
			// The edit scene of the addressed open scene: the play scene takes entity.create.
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(SceneTarget::Edit, true, true));
			const std::string_view kindName = Utils::GetUiWidgetKindName(params.Kind);
			if (context.HasParam("name") && params.Name.empty())
				return std::unexpected(
					Utils::MakeParamError(ErrorCode::InvalidArgument, "/name", "a widget name must not be empty", "omit name to use the kind's name"));
			if (context.HasParam("text") && !Utils::HasUiWidgetLabel(params.Kind))
			{
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/text",
					std::format("a {} has no label to take a text", kindName),
					"only a Text, Button, Toggle or TextInput takes text: omit it, or create a Text below the widget"));
			}
			Entity parent;
			if (!params.Parent.empty())
			{
				ENGINE_TRY_ASSIGN(parent, context.ResolveEntity(*scene, params.Parent, "/parent"));
			}
			std::optional<uint32_t> index;
			if (context.HasParam("index"))
			{
				const size_t siblings = parent.IsValid() ? parent.GetChildren().size() : scene->GetRootEntities().size();
				if (params.Index > siblings)
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/index",
						std::format("index {} is past the {} {}", params.Index, siblings,
							parent.IsValid() ? "children of the parent" : "root entities of the scene"),
						std::format("use an index from 0 to {}, or omit it to place the widget last", siblings)));
				}
				index = params.Index;
			}

			const std::string name = context.HasParam("name") ? params.Name : Utils::GetUiWidgetDefaultName(params.Kind);
			const std::string text = context.HasParam("text") ? params.Text : Utils::GetUiWidgetDefaultText(params.Kind);
			// A widget needs a canvas at or above its parent; without one it goes below a new canvas, which then takes the
			// created root's place below `parent` (at `index`), so the command never builds UI outside a canvas.
			const bool needsCanvas = params.Kind != UiWidgetKind::Canvas && !Utils::IsInUiCanvas(parent);

			SceneEdit edit(context.GetEditor(), std::format("Create UI {} '{}'", kindName, name));
			std::vector<UUID> created;
			Entity canvas;
			if (needsCanvas)
			{
				canvas = parent.IsValid() ? scene->CreateEntity("Canvas", parent) : scene->CreateEntity("Canvas");
				canvas.AddComponent<UiCanvasComponent>();
				if (index)
					ENGINE_TRY(scene->SetParent(canvas, parent, index, false));
				created.push_back(canvas.GetUUID());
			}
			const Entity widgetParent = needsCanvas ? canvas : parent;
			const Entity root = widgetParent.IsValid() ? scene->CreateEntity(name, widgetParent) : scene->CreateEntity(name);
			if (!needsCanvas && index)
				ENGINE_TRY(scene->SetParent(root, parent, index, false));
			created.push_back(root.GetUUID());
			Utils::BuildUiWidget(*scene, root, params.Kind, text, created);
			const UUID rootId = root.GetUUID();
			ENGINE_TRY_ASSIGN(const uint64_t undoIndex, edit.Commit());

			UiCreateWidgetResult result;
			result.Entity = context.MakeEntitySummary(scene->FindEntityByID(rootId));
			for (const UUID id : created)
				result.Entities.push_back(id.ToString());
			result.UndoIndex = ToAutomationCounter(undoIndex);
			return result;
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
			.Entry(UiWidgetKind::ScrollView, "ScrollView", "A vertical scroll view whose Content child stacks its children.")
			.Entry(UiWidgetKind::TextInput, "TextInput", "A single-line text field with a Text child that shows what is typed and a Placeholder child.");

		registry.Struct<UiCreateWidgetParams>("UiCreateWidgetParams", "The params of ui.createWidget.")
			.Field("kind", &UiCreateWidgetParams::Kind, "The widget to build.")
			.Field("parent", &UiCreateWidgetParams::Parent,
				"The parent entity (16 hex digits, a unique id prefix of at least 6, or a path); a kind other than Canvas below a parent "
				"without a UiCanvas at or above it goes below a new canvas created there. Omitted: a Canvas is a root, any other kind goes "
				"below a new root canvas.")
			.Field("index", &UiCreateWidgetParams::Index,
				"The created root's position among the parent's children (among the scene's roots without a parent); omitted: last.")
			.Field("name", &UiCreateWidgetParams::Name, "The widget root's name; omitted: the kind's name.")
			.Field("text", &UiCreateWidgetParams::Text,
				"The label of a Text, Button or Toggle, or a TextInput's placeholder; omitted: the kind's name (\"Enter text\" for a TextInput).");

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
				.Description = "Builds a UI widget (a canvas, panel, image, text, button, toggle, slider, scroll view or text input) with its child "
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
