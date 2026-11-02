#include "EnginePCH.h"
#include "Engine/Scene/UiExtraction.h"

#include "Engine/Scene/Components/TextComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiTextComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"

#include <algorithm>
#include <cstddef>
#include <format>
#include <set>

// The UI's render extraction (UiExtraction.h; Docs/Decisions/0033-m22-contract.md decisions 7 and 9): the draw items of a
// layout table's visible elements, and the capture annotations of a snapshot's UI draw list.

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

	void AppendUiDrawItems(const Scene& scene, const UiLayoutTable& table, UiDrawList& list)
	{
		for (const UiCanvasLayout& canvas : table.Canvases)
		{
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
					list.Items.push_back(UiDrawItem{
						.Kind = UiDrawKind::Image,
						.Entity = node.Entity,
						.Min = node.Min,
						.Max = node.Max,
						.Color = image->Color,
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
						.Font = text->Font.GetHandle(),
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
