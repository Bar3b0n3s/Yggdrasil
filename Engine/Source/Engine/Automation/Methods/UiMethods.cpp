#include "EnginePCH.h"
#include "Engine/Automation/Methods/UiMethods.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/ComponentInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiLayoutComponent.h"
#include "Engine/Scene/Components/UiScrollViewComponent.h"
#include "Engine/Scene/Components/UiSliderComponent.h"
#include "Engine/Scene/Components/UiTextComponent.h"
#include "Engine/Scene/Components/UiToggleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/UiInteraction.h"
#include "Engine/Scene/UiLayout.h"
#include "Engine/Scene/UiSystem.h"
#include "Engine/Scene/UiWidgets.h"
#include "Engine/Session/PlaySession.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <format>
#include <optional>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The ui domain (UiMethods.h; Docs/Decisions/0033-m22-contract.md decisions 8 and 31). ui.tree lays the target scene out with
// ComputeUiLayout, or reads the play session's current layout (UiSystem::GetLayout), and reports it in the table's draw order:
// each canvas, then its elements (canonical order, a parent before its children), with each element's widget and, in the play
// scene, the session's interaction state (UiSystem::GetInteraction).

namespace Engine {

	namespace Utils {

		// The registry category of the UI components (Scene/Registration/UiRegistration.cpp): an entry lists the ones its
		// entity holds, in registry order.
		constexpr std::string_view UiComponentCategory = "Ui";

		// The JSON entries' text is cut to this many bytes, the outline's to this many (as scene.tree's Text form).
		constexpr size_t MaxUiTreeTextBytes = 256;
		constexpr size_t MaxUiOutlineTextLength = 24;

		// `text` cut to at most `limit` bytes at a UTF-8 boundary, followed by "…" when it was longer.
		static std::string CutUiText(std::string_view text, size_t limit)
		{
			if (text.size() <= limit)
				return std::string(text);
			size_t cut = limit;
			while (cut > 0 && (static_cast<unsigned char>(text[cut]) & 0xc0) == 0x80)
				--cut;
			return std::format("{}…", text.substr(0, cut));
		}

		// A float as the outline prints it: the shortest text that reads back as the same float ("4.5", "20"), as scene.tree's.
		static std::string FormatUiOutlineNumber(float value)
		{
			return std::format("{}", value == 0.0f ? 0.0f : value);
		}

		// The outline's file name, as scene.tree's header names it: the last component of the summary's path, or
		// "<name> (unsaved)" for a scene never saved.
		static std::string FormatUiOutlineFile(const SceneSummary& summary)
		{
			if (summary.Path.empty())
				return std::format("{} (unsaved)", summary.Name);
			const size_t slash = summary.Path.rfind('/');
			return slash == std::string::npos ? summary.Path : summary.Path.substr(slash + 1);
		}

		static UiPixelRect MakeUiPixelRect(glm::vec2 min, glm::vec2 max)
		{
			return UiPixelRect{ .X = min.x, .Y = min.y, .Width = max.x - min.x, .Height = max.y - min.y };
		}

		static UiPixelRect MakeUiPixelRect(const UiClipRect& clip)
		{
			// Whole pixels within plus or minus 2^30 (ComputeUiClipRect), so each difference fits int64 and converts exactly
			// enough to float for a report.
			const int64_t width = static_cast<int64_t>(clip.MaxX) - clip.MinX;
			const int64_t height = static_cast<int64_t>(clip.MaxY) - clip.MinY;
			return UiPixelRect{ .X = static_cast<float>(clip.MinX),
				.Y = static_cast<float>(clip.MinY),
				.Width = static_cast<float>(std::max<int64_t>(width, 0)),
				.Height = static_cast<float>(std::max<int64_t>(height, 0)) };
		}

		// The registry names of the UI components `entity` holds, in registry order.
		static std::vector<const ComponentInfo*> GetUiComponents(ConstEntity entity)
		{
			std::vector<const ComponentInfo*> components = GetEntityComponents(entity);
			std::erase_if(components, [](const ComponentInfo* info)
			{
				return info->GetCategory() != UiComponentCategory;
			});
			return components;
		}

		static std::string_view GetUiLayoutKindName(UiLayoutKind kind)
		{
			switch (kind)
			{
				case UiLayoutKind::Stack:
					return "Stack";
				case UiLayoutKind::Grid:
					return "Grid";
				case UiLayoutKind::Flex:
					return "Flex";
			}
			return "Stack";
		}

		static std::string_view GetUiLayoutDirectionName(UiLayoutDirection direction)
		{
			return direction == UiLayoutDirection::Horizontal ? "Horizontal" : "Vertical";
		}

		// How the outline names a UiImage's frame: its sheet's project path without "Assets/" (or the first 8 hex digits of its
		// handle when `assets` cannot name it), '#' and the frame's index ("Ui/Buttons.spritesheet#2"); "none" for a plain
		// rectangle (a null sheet).
		static std::string FormatUiOutlineSheet(const UiImageComponent& image, const AssetManager* assets)
		{
			if (!image.Sheet.IsValid())
				return "none";
			const AssetHandle handle = image.Sheet.GetHandle();
			const std::string path = assets != nullptr ? assets->GetReferencePath(handle) : std::string();
			constexpr std::string_view AssetsFolder = "Assets/";
			std::string sheet = path.empty() ? handle.ToString().substr(0, 8) : path;
			if (sheet.starts_with(AssetsFolder))
				sheet.erase(0, AssetsFolder.size());
			return std::format("{}#{}", sheet, image.Frame);
		}

		// A scroll view's ScrollPosition as read (UiScrollViewComponent.h): clamped to its content's overflow in `table`, the
		// sizes being the laid-out rectangles in canvas units. A Content that is not an element below it leaves nothing to
		// scroll: its size is then the view's, as UiSystem.cpp's and UiWidgets.cpp's readers take it.
		static glm::vec2 ReadUiOutlineScrollPosition(const UiScrollViewComponent& scrollView, const UiLayoutTable& table, UUID element)
		{
			const UiLayoutNode* view = FindUiLayoutNode(table, element);
			if (view == nullptr)
				return glm::vec2(0.0f);
			const UiCanvasLayout* canvas = FindUiCanvasLayout(table, view->Canvas);
			const float scale = canvas != nullptr ? canvas->Scale : 1.0f;
			const glm::vec2 viewSize = (view->Max - view->Min) / scale;
			glm::vec2 contentSize = viewSize;
			const UiLayoutNode* content = scrollView.Content.IsValid() ? FindUiLayoutNode(table, scrollView.Content) : nullptr;
			for (const UiLayoutNode* parent = content != nullptr ? FindUiLayoutNode(table, content->Parent) : nullptr; parent != nullptr;
				parent = FindUiLayoutNode(table, parent->Parent))
			{
				if (parent->Entity == element)
				{
					contentSize = (content->Max - content->Min) / scale;
					break;
				}
			}
			return ClampUiScrollPosition(scrollView, viewSize, contentSize, scrollView.ScrollPosition);
		}

		// The short forms of an element's UI components in the outline: "UiImage(<sheet>#<frame>)", "UiText(\"<text>\")",
		// "UiLayout(<kind> <direction>)", C22.2's widgets "UiToggle(on|off)", "UiSlider(<value as read>)" and
		// "UiScrollView(<x>,<y>)" (its position as read in `table`), and the name of any other but UiRect, which every
		// element holds (UiButton, UiLayoutElement).
		static std::string FormatUiOutlineComponents(ConstEntity entity, const AssetManager* assets, const UiLayoutTable& table)
		{
			std::string components;
			const auto append = [&components](std::string_view text)
			{
				components += std::format("{}{}", components.empty() ? "" : " ", text);
			};
			for (const ComponentInfo* info : GetUiComponents(entity))
			{
				const std::string& name = info->GetName();
				if (name == "UiRect")
					continue;
				if (const UiImageComponent* image = name == "UiImage" ? entity.TryGetComponent<UiImageComponent>() : nullptr)
					append(std::format("UiImage({})", FormatUiOutlineSheet(*image, assets)));
				else if (const UiTextComponent* text = name == "UiText" ? entity.TryGetComponent<UiTextComponent>() : nullptr)
					append(std::format("UiText(\"{}\")", CutUiText(text->Text, MaxUiOutlineTextLength)));
				else if (const UiLayoutComponent* layout = name == "UiLayout" ? entity.TryGetComponent<UiLayoutComponent>() : nullptr)
					append(std::format("UiLayout({} {})", GetUiLayoutKindName(layout->Kind), GetUiLayoutDirectionName(layout->Direction)));
				else if (const UiToggleComponent* toggle = name == "UiToggle" ? entity.TryGetComponent<UiToggleComponent>() : nullptr)
					append(std::format("UiToggle({})", toggle->IsOn ? "on" : "off"));
				else if (const UiSliderComponent* slider = name == "UiSlider" ? entity.TryGetComponent<UiSliderComponent>() : nullptr)
					append(std::format("UiSlider({})", FormatUiOutlineNumber(ReadUiSliderValue(*slider))));
				else if (const UiScrollViewComponent* scrollView = name == "UiScrollView" ? entity.TryGetComponent<UiScrollViewComponent>() : nullptr)
				{
					const glm::vec2 position = ReadUiOutlineScrollPosition(*scrollView, table, entity.GetUUID());
					append(std::format("UiScrollView({},{})", FormatUiOutlineNumber(position.x), FormatUiOutlineNumber(position.y)));
				}
				else
					append(name);
			}
			return components;
		}

		static std::string FormatUiOutlineRect(const UiPixelRect& rect)
		{
			return std::format("({},{} {}x{})", FormatUiOutlineNumber(rect.X), FormatUiOutlineNumber(rect.Y), FormatUiOutlineNumber(rect.Width),
				FormatUiOutlineNumber(rect.Height));
		}

		// The scene resolution ui.tree lays out at, and the layout: `requested` when given (both components non-zero), else
		// the play session's current layout for its own scene (laid out afresh at its view's size before the session's first
		// layout, which Activate makes), else the project's window size (the view of every deterministic session).
		struct UiTreeLayout
		{
			UiLayoutTable Computed{};
			const UiLayoutTable* Table = nullptr;
		};

		static void ResolveUiTreeLayout(const AutomationMethodContext& context, const Scene& scene, glm::uvec2 requested, UiTreeLayout& layout)
		{
			if (requested.x != 0 && requested.y != 0)
			{
				layout.Computed = ComputeUiLayout(scene, requested);
				layout.Table = &layout.Computed;
				return;
			}
			const PlaySession* session = context.GetPlaySession();
			if (session != nullptr && &session->GetScene() == &scene)
			{
				const UiLayoutTable& current = session->GetUi().GetLayout();
				if (current.SceneSize.x != 0 && current.SceneSize.y != 0)
				{
					layout.Table = &current;
					return;
				}
				layout.Computed = ComputeUiLayout(scene, glm::max(session->GetViewSize(), glm::uvec2(1)));
				layout.Table = &layout.Computed;
				return;
			}
			const ProjectSettings* settings = context.GetProjectSettings();
			layout.Computed = ComputeUiLayout(scene, GetDeterministicViewSize(settings != nullptr ? settings->Window : WindowSettings{}));
			layout.Table = &layout.Computed;
		}

		// One canvas of the result: its entry or outline line, then each of its listed elements'.
		struct UiTreeWriter
		{
			const Scene* Target = nullptr;
			const AssetManager* Assets = nullptr;
			// The play session's interaction state when Target is its scene (C22.2, decision 31); null for the edit scene.
			const UiInteractionState* Interaction = nullptr;
			uint32_t DepthLimit = 0; // 0: unlimited
			UiTreeResult* Result = nullptr;
			bool Text = false;
		};

		// An element's interaction (UiTreeEntry's members, decision 31).
		struct UiTreeInteraction
		{
			UiWidgetType Widget = UiWidgetType::None;
			bool Interactable = false;
			bool Focused = false;
			bool Hovered = false;
			bool Pressed = false;
		};

		static UiTreeInteraction ReadUiTreeInteraction(const UiTreeWriter& writer, UUID element)
		{
			UiTreeInteraction interaction{ .Widget = GetUiWidgetType(*writer.Target, element),
				.Interactable = IsUiInteractable(*writer.Target, element) };
			if (writer.Interaction != nullptr)
			{
				interaction.Focused = writer.Interaction->Focused == element;
				interaction.Hovered = writer.Interaction->Hovered == element;
				interaction.Pressed = writer.Interaction->Pressed == element;
			}
			return interaction;
		}

		// The outline's interaction markers, one group after "clipped" and "hidden": "disabled" for a widget that is not
		// interactable, then "focused", "hovered" and "pressed", separated by single spaces; empty for none.
		static std::string FormatUiOutlineInteraction(const UiTreeInteraction& interaction)
		{
			std::string markers;
			const auto append = [&markers](std::string_view marker)
			{
				markers += std::format("{}{}", markers.empty() ? "" : " ", marker);
			};
			if (interaction.Widget != UiWidgetType::None && !interaction.Interactable)
				append("disabled");
			if (interaction.Focused)
				append("focused");
			if (interaction.Hovered)
				append("hovered");
			if (interaction.Pressed)
				append("pressed");
			return markers;
		}

		static void WriteUiTreeCanvas(const UiTreeWriter& writer, const UiLayoutTable& table, const UiCanvasLayout& canvas, ConstEntity canvasEntity)
		{
			const Scene& scene = *writer.Target;
			UiTreeResult& result = *writer.Result;
			const UiPixelRect sceneRect{ .X = 0.0f, .Y = 0.0f, .Width = static_cast<float>(table.SceneSize.x), .Height = static_cast<float>(table.SceneSize.y) };
			const std::string canvasId = canvasEntity.GetUUID().ToString();
			if (writer.Text)
			{
				result.Text += std::format("{}  {}  UiCanvas(sort {}, scale {})\n", canvasEntity.GetName(), canvasId.substr(0, 8), canvas.SortOrder,
					FormatUiOutlineNumber(canvas.Scale));
			}
			else
			{
				UiTreeEntry entry;
				entry.Id = canvasId;
				entry.Name = canvasEntity.GetName();
				entry.Path = scene.GetEntityPath(canvasEntity);
				entry.Canvas = canvasId;
				entry.Depth = 0;
				for (const ComponentInfo* info : GetUiComponents(canvasEntity))
					entry.Components.push_back(info->GetName());
				entry.Rect = sceneRect;
				entry.SortOrder = canvas.SortOrder;
				entry.Scale = canvas.Scale;
				entry.Visible = true;
				result.Elements.push_back(std::move(entry));
			}

			// The canvas's elements within the depth limit whose entities the scene still holds, in the table's order.
			const size_t first = std::min<size_t>(canvas.FirstNode, table.Nodes.size());
			const size_t count = std::min<size_t>(canvas.NodeCount, table.Nodes.size() - first);
			std::vector<const UiLayoutNode*> listed;
			for (const UiLayoutNode& node : std::span(table.Nodes).subspan(first, count))
			{
				if ((writer.DepthLimit == 0 || node.Depth <= writer.DepthLimit) && scene.FindEntityByID(node.Entity).IsValid())
					listed.push_back(&node);
			}

			// An element is the last of its parent's listed children when no later listed element has its parent (the table is
			// depth-first, a parent before its children).
			std::vector<bool> lastOfParent(listed.size(), false);
			std::set<UUID> parentsSeen;
			for (size_t index = listed.size(); index > 0; --index)
				lastOfParent[index - 1] = parentsSeen.insert(listed[index - 1]->Parent).second;

			std::vector<bool> lastAtDepth;
			for (size_t index = 0; index < listed.size(); ++index)
			{
				const UiLayoutNode& node = *listed[index];
				const ConstEntity entity = scene.FindEntityByID(node.Entity);
				const UiPixelRect rect = MakeUiPixelRect(node.Min, node.Max);
				const UiTreeInteraction interaction = ReadUiTreeInteraction(writer, node.Entity);
				if (writer.Text)
				{
					const uint32_t depth = std::max<uint32_t>(node.Depth, 1);
					lastAtDepth.resize(std::max<size_t>(lastAtDepth.size(), depth + 1));
					lastAtDepth[depth] = lastOfParent[index];
					std::string line;
					for (uint32_t level = 1; level < depth; ++level)
						line += lastAtDepth[level] ? "   " : "│  ";
					line += std::format("{}{}  {}", lastOfParent[index] ? "└─ " : "├─ ", entity.GetName(), entity.GetUUID().ToString().substr(0, 8));
					for (const std::string& part : { FormatUiOutlineComponents(entity, writer.Assets, table), FormatUiOutlineRect(rect) })
					{
						if (!part.empty())
							line += std::format("  {}", part);
					}
					if (node.Clipped)
						line += "  clipped";
					if (!node.Visible)
						line += "  hidden";
					if (const std::string markers = FormatUiOutlineInteraction(interaction); !markers.empty())
						line += std::format("  {}", markers);
					result.Text += line + "\n";
					continue;
				}

				UiTreeEntry entry;
				entry.Id = entity.GetUUID().ToString();
				entry.Name = entity.GetName();
				entry.Path = scene.GetEntityPath(entity);
				entry.Canvas = canvasId;
				entry.Parent = FormatOptionalUUID(node.Parent);
				entry.Depth = node.Depth;
				for (const ComponentInfo* info : GetUiComponents(entity))
					entry.Components.push_back(info->GetName());
				entry.Rect = rect;
				entry.Clipped = node.Clipped;
				if (node.Clipped)
					entry.Clip = MakeUiPixelRect(node.Clip);
				entry.Visible = node.Visible;
				if (const UiTextComponent* text = entity.TryGetComponent<UiTextComponent>())
					entry.Text = CutUiText(text->Text, MaxUiTreeTextBytes);
				entry.Interactable = interaction.Interactable;
				entry.Focused = interaction.Focused;
				entry.Hovered = interaction.Hovered;
				entry.Pressed = interaction.Pressed;
				entry.Widget = std::string(UiWidgetTypeToString(interaction.Widget));
				result.Elements.push_back(std::move(entry));
			}
		}

	}

	namespace Automation {

		Result<UiTreeResult> UiTree(AutomationMethodContext& context, const UiTreeParams& params)
		{
			if ((params.Width == 0) != (params.Height == 0))
			{
				const std::string_view missing = params.Width == 0 ? "/width" : "/height";
				return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, missing, "width and height are given together or not at all",
					"give both to lay the UI out at that scene resolution, or neither for the scene's own"));
			}
			ENGINE_TRY_ASSIGN(Scene * scene, context.ResolveTargetScene(params.Target, context.HasParam("target"), false));

			std::optional<UUID> onlyCanvas;
			if (!params.Canvas.empty())
			{
				ENGINE_TRY_ASSIGN(const Entity canvas, context.ResolveEntity(*scene, params.Canvas, "/canvas"));
				if (!canvas.HasComponent<UiCanvasComponent>())
				{
					return std::unexpected(Utils::MakeParamError(ErrorCode::InvalidArgument, "/canvas",
						std::format("'{}' has no UiCanvas component", scene->GetEntityPath(canvas)),
						"name the canvas the element belongs to (ui.tree lists each element's canvas), or omit canvas to list every one"));
				}
				onlyCanvas = canvas.GetUUID();
			}

			Utils::UiTreeLayout layout;
			Utils::ResolveUiTreeLayout(context, *scene, glm::uvec2(params.Width, params.Height), layout);
			const UiLayoutTable& table = *layout.Table;

			UiTreeResult result;
			result.Scene = context.MakeSceneSummary(*scene);
			result.Width = table.SceneSize.x;
			result.Height = table.SceneSize.y;
			// The play scene's interaction is its session's (decision 31); the edit scene has none.
			const PlaySession* session = context.GetPlaySession();
			const bool played = session != nullptr && &session->GetScene() == scene;
			const UiInteractionState* interaction = played ? &session->GetUi().GetInteraction() : nullptr;
			const Utils::UiTreeWriter writer{ .Target = scene,
				.Assets = context.GetAssets(),
				.Interaction = interaction,
				.DepthLimit = params.Depth,
				.Result = &result,
				.Text = params.Format == SceneTreeFormat::Text };
			size_t elementCount = 0;
			for (const UiCanvasLayout& canvas : table.Canvases)
			{
				const ConstEntity entity = std::as_const(*scene).FindEntityByID(canvas.Canvas);
				if (!entity.IsValid() || (onlyCanvas.has_value() && *onlyCanvas != canvas.Canvas))
					continue;
				elementCount += canvas.NodeCount;
				Utils::WriteUiTreeCanvas(writer, table, canvas, entity);
			}
			if (writer.Text)
			{
				result.Text = std::format("{}  rev {}  {}x{} scene pixels  {} {}\n{}", Utils::FormatUiOutlineFile(result.Scene), result.Scene.Revision,
					result.Width, result.Height, elementCount, elementCount == 1 ? "element" : "elements", result.Text);
			}
			return result;
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
			.Field("interactable", &UiTreeEntry::Interactable, "Whether the element is a widget whose Interactable is set (it takes the pointer and focus).")
			.Field("focused", &UiTreeEntry::Focused, "Whether the element has keyboard and gamepad focus in the play session; false in the edit scene.")
			.Field("hovered", &UiTreeEntry::Hovered, "Whether the element is the widget under the pointer in the play session; false in the edit scene.")
			.Field("pressed", &UiTreeEntry::Pressed, "Whether the held pointer presses the element in the play session; false in the edit scene.")
			.Field("widget", &UiTreeEntry::Widget, "The element's widget: Button, Toggle, Slider or ScrollView; empty for none.");

		registry.Struct<UiTreeParams>("UiTreeParams", "The params of ui.tree.")
			.Field("canvas", &UiTreeParams::Canvas, "An entity with a UiCanvas whose UI alone is listed; omitted: every canvas.")
			.Field("depth", &UiTreeParams::Depth, "The most levels listed below each canvas; 0: unlimited.")
			.Field("format", &UiTreeParams::Format,
				"Text: a compact outline (widgets as UiButton, UiToggle(on|off), UiSlider(<value>) and UiScrollView(<x>,<y>), marked disabled, "
				"focused, hovered or pressed); Json: a flat list of entries.")
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
							   "components, text and widget, and in the play scene the session's focus, hover and press, as a compact text outline "
							   "or a flat JSON list, optionally for one canvas and to a depth.",
				.ExposeAsTool = true,
				.SupportsSceneTarget = true,
				.AvailableInRuntime = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Outline every canvas.", .Params = treeExample } },
			},
			&Automation::UiTree);
	}

}
