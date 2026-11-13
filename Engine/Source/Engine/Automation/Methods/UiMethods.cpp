#include "EnginePCH.h"
#include "Engine/Automation/Methods/UiMethods.h"

#include "Engine/Asset/AssetManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/FontData.h"
#include "Engine/Automation/Methods/AutomationMethodContext.h"
#include "Engine/Automation/Methods/SharedMethodSupport.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Core/UUIDGenerator.h"
#include "Engine/Project/ProjectSettings.h"
#include "Engine/Reflection/ComponentInfo.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Renderer/TextMeasurement.h"
#include "Engine/Renderer/UiDrawList.h"
#include "Engine/Renderer/UiPass.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiLayoutComponent.h"
#include "Engine/Scene/Components/UiScrollViewComponent.h"
#include "Engine/Scene/Components/UiSliderComponent.h"
#include "Engine/Scene/Components/UiTextComponent.h"
#include "Engine/Scene/Components/UiTextInputComponent.h"
#include "Engine/Scene/Components/UiToggleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/LoadReport.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scene/UiExtraction.h"
#include "Engine/Scene/UiInteraction.h"
#include "Engine/Scene/UiLayout.h"
#include "Engine/Scene/UiSystem.h"
#include "Engine/Scene/UiWidgets.h"
#include "Engine/Session/PlaySession.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
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
// scene, the session's interaction state (UiSystem::GetInteraction). M25's friction F02: each element's text box, measured from
// the table's draw items (AppendUiDrawItems) as the Ui pass lays them out (MeasureUiTreeText). M25's friction F10: with
// includeInactive, a serializer copy of the scene in which every entity is active is laid out, read and measured in the scene's
// place (CopyUiTreeSceneActive), and each entry's Active is read from the scene itself.

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
		// "UiScrollView(<x>,<y>)" (its position as read in `table`), C22.3's "UiTextInput(\"<text>\")", and the name of any
		// other but UiRect, which every element holds (UiButton, UiLayoutElement).
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
				else if (const UiTextInputComponent* textInput = name == "UiTextInput" ? entity.TryGetComponent<UiTextInputComponent>() : nullptr)
					append(std::format("UiTextInput(\"{}\")", CutUiText(textInput->Text, MaxUiOutlineTextLength)));
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

		// M25's F02 (Docs/Decisions/0036-m25-tetris-pixel-art.md): a text's box in its element's pixels, and whether it reaches
		// beyond the element's rectangle by more than UiTextOverflowTolerance.
		struct UiTreeTextBox
		{
			UiPixelRect Bounds{};
			bool Overflows = false;
		};

		// The horizontal alignment a text draw item lays its lines out with (the Ui pass's rule: Start left, Center centred, End
		// right).
		static RenderTextAlignment ToUiTreeTextAlignment(UiDrawAlignment alignment)
		{
			switch (alignment)
			{
				case UiDrawAlignment::Start:  return RenderTextAlignment::Left;
				case UiDrawAlignment::Center: return RenderTextAlignment::Center;
				case UiDrawAlignment::End:    return RenderTextAlignment::Right;
			}
			return RenderTextAlignment::Left;
		}

		// The share of its rectangle's height a text block leaves above it (the Ui pass's rule: Start none, Center half, End all).
		static float GetUiTreeVerticalShare(UiDrawAlignment alignment)
		{
			switch (alignment)
			{
				case UiDrawAlignment::Start:  return 0.0f;
				case UiDrawAlignment::Center: return 0.5f;
				case UiDrawAlignment::End:    return 1.0f;
			}
			return 0.0f;
		}

		// The font `handle` names when it loads as a font; null otherwise.
		static AssetRef<FontData> LoadUiTreeFont(AssetManager& assets, AssetHandle handle)
		{
			const Result<AssetRef<Asset>> loaded = assets.Load(handle);
			return loaded.has_value() ? AssetCast<FontData>(*loaded) : AssetRef<FontData>{};
		}

		// The fonts a text of the font `handle` lays out with, as the Ui pass resolves its chain (BuildUiFontChain,
		// Renderer/UiPass.h), without reporting anything, since ui.tree only reads: the font itself (null, missing or failed:
		// the Default font, which the font placeholder is), then its fallbacks depth first in their order, each once, at most
		// MaxUiFontChainLength fonts, a fallback that does not load left out. Empty when not even the Default font loads.
		static std::vector<AssetRef<FontData>> ResolveUiTreeFontChain(AssetManager& assets, AssetHandle handle)
		{
			std::vector<AssetRef<FontData>> fonts;
			AssetRef<FontData> own = handle.IsValid() ? LoadUiTreeFont(assets, handle) : AssetRef<FontData>{};
			AssetHandle ownHandle = handle;
			if (own == nullptr)
			{
				own = LoadUiTreeFont(assets, BuiltinAssetHandles::DefaultFont);
				ownHandle = BuiltinAssetHandles::DefaultFont;
			}
			if (own == nullptr)
				return fonts;
			std::vector<AssetHandle> handles = { ownHandle };
			fonts.push_back(own);
			// `pending` holds the fonts whose fallbacks are being walked, innermost last, each with the index of its next one.
			std::vector<std::pair<AssetRef<FontData>, size_t>> pending = { { own, 0 } };
			while (!pending.empty() && fonts.size() < MaxUiFontChainLength)
			{
				auto& [current, next] = pending.back();
				if (next >= current->Fallbacks.size())
				{
					pending.pop_back();
					continue;
				}
				const AssetHandle fallback = current->Fallbacks[next++];
				if (!fallback.IsValid() || std::ranges::find(handles, fallback) != handles.end())
					continue;
				AssetRef<FontData> fallbackFont = LoadUiTreeFont(assets, fallback);
				if (fallbackFont == nullptr)
					continue;
				handles.push_back(fallback);
				fonts.push_back(fallbackFont);
				pending.emplace_back(std::move(fallbackFont), 0);
			}
			return fonts;
		}

		// The box of the text draw item `item` where the Ui pass lays it out (PlanUiPass, Renderer/UiPass.h): measured with its
		// font's chain (ResolveUiTreeFontChain) at its FontSize and LineSpacing, wrapped at and aligned within its rectangle's width,
		// the block aligned vertically in its rectangle; a focused text input's block as tall as a line at least and moved the
		// least that keeps its caret inside the rectangle when it does not wrap (C22.3's caret rule). The box runs from the
		// lines' leftmost pen start to their rightmost advance end, and from the block's top to its bottom. nullopt for an item
		// that lays out no line (an empty text, or no font).
		static std::optional<UiTreeTextBox> MeasureUiTreeText(const UiDrawItem& item, AssetManager& assets)
		{
			if (item.Text.empty() || !(item.FontSize > 0.0f) || !(item.LineSpacing > 0.0f) || !std::isfinite(item.FontSize) || !std::isfinite(item.LineSpacing))
				return std::nullopt;
			const std::vector<AssetRef<FontData>> chain = ResolveUiTreeFontChain(assets, item.Font);
			if (chain.empty())
				return std::nullopt;
			std::vector<const FontData*> fonts;
			fonts.reserve(chain.size());
			for (const AssetRef<FontData>& font : chain)
				fonts.push_back(font.get());
			const float width = item.Max.x - item.Min.x;
			const float height = item.Max.y - item.Min.y;
			const TextMeasureOptions options{ .Size = item.FontSize,
				.LineSpacing = item.LineSpacing,
				.Wrap = item.Wrap ? TextWrap::Word : TextWrap::None,
				.MaxWidth = width,
				.Alignment = ToUiTreeTextAlignment(item.HorizontalAlignment) };
			const TextMeasurement measured = MeasureText(fonts, item.Text, options);
			if (measured.Lines.empty())
				return std::nullopt;
			const float blockHeight = item.HasCaret ? std::max(measured.Size.y, measured.LineHeight) : measured.Size.y;
			glm::vec2 topLeft(item.Min.x, item.Min.y + (height - blockHeight) * GetUiTreeVerticalShare(item.VerticalAlignment));
			if (item.HasCaret && !item.Wrap)
			{
				const TextCaret caret = FindTextCaret(measured, item.CaretByte);
				const float caretWidth = std::max(1.0f, std::floor(item.FontSize / 16.0f + 0.5f));
				const float caretLeft = topLeft.x + caret.Top.x;
				if (caretWidth > width || caretLeft < item.Min.x)
					topLeft.x += item.Min.x - caretLeft;
				else if (caretLeft + caretWidth > item.Max.x)
					topLeft.x += item.Max.x - (caretLeft + caretWidth);
			}
			float left = measured.Lines.front().Left;
			float right = measured.Lines.front().Left + measured.Lines.front().Width;
			for (const TextMeasuredLine& line : measured.Lines)
			{
				left = std::min(left, line.Left);
				right = std::max(right, line.Left + line.Width);
			}
			const glm::vec2 min(topLeft.x + left, topLeft.y);
			const glm::vec2 max(topLeft.x + right, topLeft.y + measured.Size.y);
			const bool overflows = min.x < item.Min.x - UiTextOverflowTolerance || min.y < item.Min.y - UiTextOverflowTolerance
				|| max.x > item.Max.x + UiTextOverflowTolerance || max.y > item.Max.y + UiTextOverflowTolerance;
			return UiTreeTextBox{ .Bounds = MakeUiPixelRect(min, max), .Overflows = overflows };
		}

		// The text boxes of the elements of `table` (a layout of `scene`) that draw text, by entity: the draw items
		// AppendUiDrawItems makes of the layout, with the play session's `interaction` (null for the edit scene), which places a
		// focused text input's caret, each text item measured by MeasureUiTreeText; none without an asset manager.
		static std::map<UUID, UiTreeTextBox> MeasureUiTreeTexts(const Scene& scene, const UiLayoutTable& table, const UiInteractionState* interaction,
			AssetManager* assets)
		{
			std::map<UUID, UiTreeTextBox> boxes;
			if (assets == nullptr)
				return boxes;
			UiDrawList list;
			AppendUiDrawItems(scene, table, list, interaction);
			for (const UiDrawItem& item : list.Items)
			{
				if (item.Kind != UiDrawKind::Text)
					continue;
				if (const std::optional<UiTreeTextBox> box = MeasureUiTreeText(item, *assets))
					boxes.insert_or_assign(item.Entity, *box);
			}
			return boxes;
		}

		// M25's friction F10 (ui.tree {includeInactive}): a copy of the target scene in which every entity is active, laid out,
		// read and measured in the scene's place. It is made through the serializer, the one scene-copy path (SceneSerializer.h),
		// with a scratch id generator of its own, so reading never advances the host's ids (as scene.diff's copy does), and it is
		// never written back.
		struct UiTreeActiveCopy
		{
			UUIDGenerator Ids = UUIDGenerator::CreateDeterministic(0); // the copy's back-reference, so it outlives the copy
			Scope<Scene> Copy{};
		};

		static Status CopyUiTreeSceneActive(const Scene& scene, UiTreeActiveCopy& active)
		{
			constexpr std::string_view CopyContext = "copying the scene to lay out its inactive UI (includeInactive)";
			Result<Json> document = SceneSerializer::ToJson(scene);
			if (!document)
				return std::unexpected(std::move(document).error().WithContext(std::string(CopyContext)));
			active.Copy = Scene::Create(
				{ .Name = scene.GetName(), .Seed = scene.GetSeed(), .Registry = &scene.GetTypeRegistry(), .IdGenerator = &active.Ids, .Runtime = false });
			LoadReport report;
			if (Status loaded = SceneSerializer::FromJson(*active.Copy, *document, LoadOptions{}, report); !loaded)
				return std::unexpected(std::move(loaded).error().WithContext(std::string(CopyContext)));
			// Activating an entity changes no order; the order is copied all the same, so nothing reads the scene's cache while
			// its entities change.
			const std::span<const UUID> canonical = active.Copy->GetCanonicalOrder();
			const std::vector<UUID> order(canonical.begin(), canonical.end());
			for (const UUID id : order)
			{
				const Entity entity = active.Copy->FindEntityByID(id);
				if (entity.IsValid() && !entity.IsActiveSelf())
					entity.SetActive(true);
			}
			return {};
		}

		// The scene resolution ui.tree lays out at, and the layout of `laidOut` there: `scene` itself, or with includeInactive
		// its copy in which every entity is active (M25's F10). The resolution is `requested` when given (both components
		// non-zero), else the play session's for its own scene: its current layout, which `scene` itself reads as it is
		// (laid out afresh at its view's scene resolution before the session's first layout, which Activate makes), else the
		// scene resolution of the game view at the project's window size (the view of every deterministic session). A view's
		// scene resolution is `scene`'s game view output mapping's (ComputeGameViewOutputMapping: its size, or under a
		// PixelPerfectCamera the reference resolution; M22's landing).
		struct UiTreeLayout
		{
			UiLayoutTable Computed{};
			const UiLayoutTable* Table = nullptr;
		};

		static void ResolveUiTreeLayout(const AutomationMethodContext& context, const Scene& scene, const Scene& laidOut, glm::uvec2 requested,
			UiTreeLayout& layout)
		{
			const auto layOut = [&layout, &laidOut](glm::uvec2 sceneSize)
			{
				layout.Computed = ComputeUiLayout(laidOut, sceneSize);
				layout.Table = &layout.Computed;
			};
			if (requested.x != 0 && requested.y != 0)
			{
				layOut(requested);
				return;
			}
			const PlaySession* session = context.GetPlaySession();
			if (session != nullptr && &session->GetScene() == &scene)
			{
				const UiLayoutTable& current = session->GetUi().GetLayout();
				if (current.SceneSize.x != 0 && current.SceneSize.y != 0)
				{
					if (&laidOut == &scene)
						layout.Table = &current;
					else
						layOut(current.SceneSize);
					return;
				}
				const RenderOutputMapping view = ComputeGameViewOutputMapping(scene, glm::max(session->GetViewSize(), glm::uvec2(1)));
				layOut(glm::uvec2(view.SceneWidth, view.SceneHeight));
				return;
			}
			const ProjectSettings* settings = context.GetProjectSettings();
			const RenderOutputMapping view = ComputeGameViewOutputMapping(scene, GetDeterministicViewSize(settings != nullptr ? settings->Window : WindowSettings{}));
			layOut(glm::uvec2(view.SceneWidth, view.SceneHeight));
		}

		// One canvas of the result: its entry or outline line, then each of its listed elements'.
		struct UiTreeWriter
		{
			const Scene* Target = nullptr; // the scene laid out: the target scene, or its active copy (M25's F10)
			const Scene* Source = nullptr; // the target scene, which each entry's Active is read from
			const AssetManager* Assets = nullptr;
			// The play session's interaction state when Target is its scene (C22.2, decision 31); null for the edit scene.
			const UiInteractionState* Interaction = nullptr;
			uint32_t DepthLimit = 0; // 0: unlimited
			UiTreeResult* Result = nullptr;
			bool Text = false;
			// M25's F02: the box of each element that draws text, by its entity (MeasureUiTreeTexts).
			const std::map<UUID, UiTreeTextBox>* TextBoxes = nullptr;
		};

		// The text box of the element `element` (M25's F02), or null when it draws no text.
		static const UiTreeTextBox* FindUiTreeTextBox(const UiTreeWriter& writer, UUID element)
		{
			if (writer.TextBoxes == nullptr)
				return nullptr;
			const auto found = writer.TextBoxes->find(element);
			return found != writer.TextBoxes->end() ? &found->second : nullptr;
		}

		// Whether the entity `id` is effectively active in the target scene (M25's F10), which every laid-out entity comes from.
		static bool IsUiTreeEntityActive(const UiTreeWriter& writer, UUID id)
		{
			const ConstEntity entity = writer.Source->FindEntityByID(id);
			return entity.IsValid() && entity.IsActive();
		}

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
			// C22.3 (decision 53): a World canvas covers its own canvas pixels, in which its elements' rectangles and clips are.
			const bool world = canvas.RenderMode == UiCanvasRenderMode::World;
			const glm::uvec2 canvasSize = world ? canvas.SceneSize : table.SceneSize;
			const UiPixelRect sceneRect{ .X = 0.0f, .Y = 0.0f, .Width = static_cast<float>(canvasSize.x), .Height = static_cast<float>(canvasSize.y) };
			const std::string canvasId = canvasEntity.GetUUID().ToString();
			const bool canvasActive = IsUiTreeEntityActive(writer, canvasEntity.GetUUID());
			if (writer.Text)
			{
				const std::string worldSize = world ? std::format(", world {}x{}", canvasSize.x, canvasSize.y) : std::string();
				result.Text += std::format("{}  {}  UiCanvas(sort {}, scale {}{}){}\n", canvasEntity.GetName(), canvasId.substr(0, 8), canvas.SortOrder,
					FormatUiOutlineNumber(canvas.Scale), worldSize, canvasActive ? "" : "  inactive");
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
				entry.WorldCanvas = world;
				entry.Active = canvasActive;
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
				const UiTreeTextBox* textBox = FindUiTreeTextBox(writer, node.Entity);
				const bool active = IsUiTreeEntityActive(writer, node.Entity);
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
					if (!active)
						line += "  inactive";
					if (textBox != nullptr && textBox->Overflows)
						line += "  overflows";
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
				// M25's F11: an element reports its canvas's scale, which turns its pixel measurements into its canvas units.
				entry.Scale = canvas.Scale;
				entry.Clipped = node.Clipped;
				if (node.Clipped)
					entry.Clip = MakeUiPixelRect(node.Clip);
				entry.Visible = node.Visible;
				// A text input's element reports its input's Text (C22.3, decision 53), any other its UiText's.
				if (const UiTextInputComponent* textInput = entity.TryGetComponent<UiTextInputComponent>())
					entry.Text = CutUiText(textInput->Text, MaxUiTreeTextBytes);
				else if (const UiTextComponent* text = entity.TryGetComponent<UiTextComponent>())
					entry.Text = CutUiText(text->Text, MaxUiTreeTextBytes);
				entry.Interactable = interaction.Interactable;
				entry.Focused = interaction.Focused;
				entry.Hovered = interaction.Hovered;
				entry.Pressed = interaction.Pressed;
				entry.Widget = std::string(UiWidgetTypeToString(interaction.Widget));
				entry.WorldCanvas = world;
				if (textBox != nullptr)
				{
					entry.TextBounds = textBox->Bounds;
					entry.TextOverflows = textBox->Overflows;
				}
				entry.Active = active;
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

			// M25's F10: with includeInactive, a copy of the scene in which every entity is active is laid out in its place.
			Utils::UiTreeActiveCopy activeCopy;
			if (params.IncludeInactive)
			{
				ENGINE_TRY(Utils::CopyUiTreeSceneActive(*scene, activeCopy));
			}
			const Scene& laidOut = activeCopy.Copy != nullptr ? *activeCopy.Copy : *scene;
			Utils::UiTreeLayout layout;
			Utils::ResolveUiTreeLayout(context, *scene, laidOut, glm::uvec2(params.Width, params.Height), layout);
			const UiLayoutTable& table = *layout.Table;

			UiTreeResult result;
			result.Scene = context.MakeSceneSummary(*scene);
			result.Width = table.SceneSize.x;
			result.Height = table.SceneSize.y;
			// The play scene's interaction is its session's (decision 31); the edit scene has none.
			const PlaySession* session = context.GetPlaySession();
			const bool played = session != nullptr && &session->GetScene() == scene;
			const UiInteractionState* interaction = played ? &session->GetUi().GetInteraction() : nullptr;
			const std::map<UUID, Utils::UiTreeTextBox> textBoxes = Utils::MeasureUiTreeTexts(laidOut, table, interaction, context.GetAssets());
			const Utils::UiTreeWriter writer{ .Target = &laidOut,
				.Source = scene,
				.Assets = context.GetAssets(),
				.Interaction = interaction,
				.DepthLimit = params.Depth,
				.Result = &result,
				.Text = params.Format == SceneTreeFormat::Text,
				.TextBoxes = &textBoxes };
			size_t elementCount = 0;
			for (const UiCanvasLayout& canvas : table.Canvases)
			{
				const ConstEntity entity = laidOut.FindEntityByID(canvas.Canvas);
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
			.Field("rect", &UiTreeEntry::Rect,
				"The laid-out rectangle in scene pixels, a canvas's the whole scene image; a World canvas's and its elements' in the canvas's own "
				"pixels (worldCanvas).")
			.Field("sortOrder", &UiTreeEntry::SortOrder, "A canvas's UiCanvas SortOrder (canvases draw in ascending order); 0 for an element.")
			.Field("scale", &UiTreeEntry::Scale,
				"The scale of the entry's canvas (a canvas's own, an element's its canvas's): the scene pixels per canvas unit at the "
				"resolution laid out at, which turns a pixel measurement of the entry into UiRect's canvas units.")
			.Field("clipped", &UiTreeEntry::Clipped, "Whether a clipping ancestor (UiRect.ClipChildren) clips the element to clip.")
			.Field("clip", &UiTreeEntry::Clip, "The clip rectangle in whole scene pixels, when clipped.")
			.Field("visible", &UiTreeEntry::Visible, "Whether the element covers a pixel of the scene image once clipped.")
			.Field("text", &UiTreeEntry::Text, "Its UiTextInput's text, else its UiText's, cut to 256 bytes; empty without either.")
			.Field("interactable", &UiTreeEntry::Interactable, "Whether the element is a widget whose Interactable is set (it takes the pointer and focus).")
			.Field("focused", &UiTreeEntry::Focused, "Whether the element has keyboard and gamepad focus in the play session; false in the edit scene.")
			.Field("hovered", &UiTreeEntry::Hovered, "Whether the element is the widget under the pointer in the play session; false in the edit scene.")
			.Field("pressed", &UiTreeEntry::Pressed, "Whether the held pointer presses the element in the play session; false in the edit scene.")
			.Field("widget", &UiTreeEntry::Widget, "The element's widget: Button, Toggle, Slider, ScrollView or TextInput; empty for none.")
			.Field("worldCanvas", &UiTreeEntry::WorldCanvas, "True for a World canvas and its elements, whose rect and clip are in the canvas's own pixels.")
			.Field("textBounds", &UiTreeEntry::TextBounds,
				"The box of the text the element draws, where it is laid out in rect: from its lines' leftmost pen start to their rightmost "
				"advance end, and from the block's top to its bottom, in rect's pixels; all 0 when it draws none (no UiText, an empty text, or "
				"hidden).")
			.Field("textOverflows", &UiTreeEntry::TextOverflows, "Whether textBounds reaches beyond rect on a side (by more than 0.01 pixel).")
			.Field("active", &UiTreeEntry::Active,
				"Whether the entity is active now, it and every ancestor; false for an inactive canvas or element that includeInactive "
				"lists, laid out and measured as if it were active.");

		registry.Struct<UiTreeParams>("UiTreeParams", "The params of ui.tree.")
			.Field("canvas", &UiTreeParams::Canvas, "An entity with a UiCanvas whose UI alone is listed; omitted: every canvas.")
			.Field("depth", &UiTreeParams::Depth, "The most levels listed below each canvas; 0: unlimited.")
			.Field("format", &UiTreeParams::Format,
				"Text: a compact outline (widgets as UiButton, UiToggle(on|off), UiSlider(<value>), UiScrollView(<x>,<y>) and "
				"UiTextInput(\"<text>\"), an inactive canvas or element that includeInactive lists marked inactive, a text that reaches "
				"beyond its rectangle marked overflows, widgets marked disabled, focused, hovered or pressed); Json: a flat list of entries.")
			.Field("width", &UiTreeParams::Width,
				"The scene resolution's width to lay out at, with height; omitted: the play session's own, or for the edit scene its game "
				"view's at the project's window size (a PixelPerfectCamera's reference resolution under one).",
				{ .Min = 0.0, .Max = 16384.0, .Unit = "px" })
			.Field("height", &UiTreeParams::Height, "The scene resolution's height to lay out at, with width.", { .Min = 0.0, .Max = 16384.0, .Unit = "px" })
			.Field("target", &UiTreeParams::Target, "The scene to read; omitted: the play scene while playing, else the edit scene.")
			.Field("includeInactive", &UiTreeParams::IncludeInactive,
				"Also list the inactive canvases and elements (a hidden panel and its texts), each laid out, and its text measured, as if "
				"it were active, and marked inactive (active false); a copy of the scene as it is now is laid out at the scene "
				"resolution, the play scene's too, and the scene itself is only read. Omitted: only the active ones.");

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
		Json inactiveExample = Json::object();
		inactiveExample["format"] = "Json";
		inactiveExample["includeInactive"] = true;
		methods.Add(
			{
				.Name = "ui.tree",
				.Description = "Shows the scene's UI laid out in scene pixels: every active canvas and element (with includeInactive the inactive "
							   "ones too, laid out as if active and marked inactive) with its rectangle, its canvas's scale, clip, visibility, "
							   "components, text and widget, the box each text is laid out in and whether it overflows its rectangle, and in the play "
							   "scene the session's focus, hover and press, as a compact text outline or a flat JSON list, optionally for one canvas "
							   "and to a depth.",
				.ExposeAsTool = true,
				.SupportsSceneTarget = true,
				.AvailableInRuntime = true,
				.AllowedInBatch = true,
				.Examples = { { .Description = "Outline every canvas.", .Params = treeExample },
					{ .Description = "List every canvas and element as JSON, the inactive ones (a hidden panel and its texts) included.",
						.Params = inactiveExample } },
			},
			&Automation::UiTree);
	}

}
