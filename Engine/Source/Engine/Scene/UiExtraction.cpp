#include "EnginePCH.h"
#include "Engine/Scene/UiExtraction.h"

#include "Engine/Scene/Components/TextComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiTextComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <cstddef>
#include <format>
#include <set>

// The UI's render extraction (UiExtraction.h; Docs/Decisions/0033-m22-contract.md decisions 7, 9 and 25): the draw items of a
// layout table's visible elements, with their canvas's theme tints and font, and the capture annotations of a snapshot's UI
// draw list.

namespace Engine {

	namespace Utils {

		static UiDrawAlignment ToUiDrawAlignment(TextAlignment alignment)
		{
			switch (alignment)
			{
				case TextAlignment::Left:   return UiDrawAlignment::Start;
				case TextAlignment::Center: return UiDrawAlignment::Center;
				case TextAlignment::Right:  return UiDrawAlignment::End;
			}
			return UiDrawAlignment::Start;
		}

		static UiDrawAlignment ToUiDrawAlignment(UiTextVerticalAlignment alignment)
		{
			switch (alignment)
			{
				case UiTextVerticalAlignment::Top:    return UiDrawAlignment::Start;
				case UiTextVerticalAlignment::Middle: return UiDrawAlignment::Center;
				case UiTextVerticalAlignment::Bottom: return UiDrawAlignment::End;
			}
			return UiDrawAlignment::Start;
		}

		static UiDrawImageType ToUiDrawImageType(UiImageType type)
		{
			return type == UiImageType::Sliced ? UiDrawImageType::Sliced : UiDrawImageType::Simple;
		}

		static UiDrawFilter ToUiDrawFilter(UiImageFilter filter)
		{
			return filter == UiImageFilter::Linear ? UiDrawFilter::Linear : UiDrawFilter::Point;
		}

		// The theme's tint of a widget's state, first match (UiTheme): Disabled when not interactable, then, from the session's
		// `interaction` (null: none), Pressed, Focused and Hovered, else Normal.
		static glm::vec4 GetUiThemeTint(const UiTheme& theme, UUID widget, bool interactable, const UiInteractionState* interaction)
		{
			if (!interactable)
				return theme.Disabled;
			if (interaction == nullptr)
				return theme.Normal;
			if (interaction->Pressed == widget)
				return theme.Pressed;
			if (interaction->Focused == widget)
				return theme.Focused;
			if (interaction->Hovered == widget)
				return theme.Hovered;
			return theme.Normal;
		}

		// The colours of the UI's capture annotations (UiExtraction.h): outlines yellow, labels white, as the entities' bounds
		// and labels are drawn (RenderAnnotations.cpp).
		constexpr glm::vec4 UiOutlineColor = glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);
		constexpr glm::vec4 UiLabelColor = glm::vec4(1.0f);

		// Whether the label mode of `snapshot` labels `entity`.
		static bool IsUiElementLabelled(const RenderSnapshot& snapshot, UUID entity)
		{
			switch (snapshot.Annotations.Labels)
			{
				case RenderAnnotationLabels::None:
					return false;
				case RenderAnnotationLabels::All:
					return true;
				case RenderAnnotationLabels::Selected:
					return std::ranges::find(snapshot.SelectedEntities, entity) != snapshot.SelectedEntities.end();
				case RenderAnnotationLabels::Explicit:
					return std::ranges::find(snapshot.Annotations.LabelEntities, entity) != snapshot.Annotations.LabelEntities.end();
			}
			return false;
		}

	}

	// C22.3: no caller passes worldOnly before stream W, which makes the editor's scene view pass it and reads it here.
	void AppendUiDrawItems(const Scene& scene, const UiLayoutTable& table, UiDrawList& list, const UiInteractionState* interaction,
		bool /*worldOnly*/)
	{
		for (const UiCanvasLayout& canvas : table.Canvases)
		{
			// The canvas's theme and font (C22.2): its widgets' tints, and the font of its texts whose own Font is null. A
			// canvas the scene no longer holds tints with the default theme and names no font.
			const ConstEntity canvasEntity = scene.FindEntityByID(canvas.Canvas);
			const UiCanvasComponent* canvasComponent = canvasEntity.IsValid() ? canvasEntity.TryGetComponent<UiCanvasComponent>() : nullptr;
			const UiTheme theme = canvasComponent != nullptr ? canvasComponent->Theme : UiTheme{};
			const AssetHandle canvasFont = canvasComponent != nullptr ? canvasComponent->Font.GetHandle() : AssetHandle();
			// A table of this scene's layout holds its canvases' node ranges; a range past the nodes is cut to them.
			const size_t first = std::min<size_t>(canvas.FirstNode, table.Nodes.size());
			const size_t last = std::min<size_t>(first + canvas.NodeCount, table.Nodes.size());
			for (size_t index = first; index < last; ++index)
			{
				const UiLayoutNode& node = table.Nodes[index];
				if (!node.Visible)
					continue;
				const ConstEntity entity = scene.FindEntityByID(node.Entity);
				if (!entity.IsValid())
					continue;
				if (const UiImageComponent* image = entity.TryGetComponent<UiImageComponent>())
				{
					// A widget's own image takes its state's tint; any other image keeps its colour.
					glm::vec4 color = image->Color;
					if (GetUiWidgetType(scene, node.Entity) != UiWidgetType::None)
						color *= Utils::GetUiThemeTint(theme, node.Entity, IsUiInteractable(scene, node.Entity), interaction);
					list.Items.push_back(UiDrawItem{
						.Kind = UiDrawKind::Image,
						.Entity = node.Entity,
						.Min = node.Min,
						.Max = node.Max,
						.Color = color,
						.Clipped = node.Clipped,
						.Clip = node.Clip,
						.Sheet = image->Sheet.GetHandle(),
						.Frame = image->Frame,
						.ImageType = Utils::ToUiDrawImageType(image->Type),
						.BorderScale = image->BorderScale * canvas.Scale,
						.Filter = Utils::ToUiDrawFilter(image->Filter),
					});
				}
				if (const UiTextComponent* text = entity.TryGetComponent<UiTextComponent>(); text != nullptr && !text->Text.empty())
				{
					list.Items.push_back(UiDrawItem{
						.Kind = UiDrawKind::Text,
						.Entity = node.Entity,
						.Min = node.Min,
						.Max = node.Max,
						.Color = text->Color,
						.Clipped = node.Clipped,
						.Clip = node.Clip,
						.Text = text->Text,
						.Font = text->Font.IsValid() ? text->Font.GetHandle() : canvasFont,
						.FontSize = text->Size * canvas.Scale,
						.HorizontalAlignment = Utils::ToUiDrawAlignment(text->Alignment),
						.VerticalAlignment = Utils::ToUiDrawAlignment(text->VerticalAlignment),
						.Wrap = text->Wrap,
						.LineSpacing = text->LineSpacing,
					});
				}
			}
		}
	}

	void AppendUiRenderAnnotations(const Scene& scene, RenderSnapshot& snapshot)
	{
		const RenderAnnotations& options = snapshot.Annotations;
		if (!options.Bounds && options.Labels == RenderAnnotationLabels::None)
			return;
		// An element draws an image, a text or both (adjacent items); it takes one outline and one label, at its first item.
		std::set<UUID> annotated;
		UiDrawList& ui = snapshot.Ui;
		for (const UiDrawItem& item : ui.Items)
		{
			if (!annotated.insert(item.Entity).second)
				continue;
			if (options.Bounds)
				ui.Annotations.push_back(UiAnnotation{ .Entity = item.Entity, .Min = item.Min, .Max = item.Max, .Color = Utils::UiOutlineColor, .Outline = true });
			if (!Utils::IsUiElementLabelled(snapshot, item.Entity))
				continue;
			const ConstEntity entity = scene.FindEntityByID(item.Entity);
			if (!entity.IsValid())
				continue;
			ui.Annotations.push_back(UiAnnotation{ .Entity = item.Entity,
				.Min = item.Min,
				.Max = item.Max,
				.Color = Utils::UiLabelColor,
				.Outline = false,
				.Label = std::format("{} {}", entity.GetName(), item.Entity.ToString().substr(0, 6)) });
		}
	}

}
