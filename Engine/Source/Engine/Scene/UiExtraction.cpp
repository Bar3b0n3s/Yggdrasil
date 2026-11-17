#include "EnginePCH.h"
#include "Engine/Scene/UiExtraction.h"

#include "Engine/Scene/Components/TextComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiTextComponent.h"
#include "Engine/Scene/Components/UiTextInputComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Private/UiWorldCanvas.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/UiTextInput.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <format>
#include <optional>
#include <set>
#include <utility>
#include <vector>

// The UI's render extraction (UiExtraction.h; Docs/Decisions/0033-m22-contract.md decisions 7, 9, 25 and 51): the draw items of
// a layout table's visible elements, with their canvas's theme tints and font, the World canvases they name, and the capture
// annotations of a snapshot's UI draw list.

namespace Engine {

	namespace Utils {

		static UiDrawAlignment ToUiDrawAlignment(TextAlignment alignment)
		{
			switch (alignment)
			{
				case TextAlignment::Left:   return UiDrawAlignment::Start;
				case TextAlignment::Center: return UiDrawAlignment::Center;
				case TextAlignment::Right:  return UiDrawAlignment::End;
				case TextAlignment::Start:  return UiDrawAlignment::ParagraphStart;
				case TextAlignment::End:    return UiDrawAlignment::ParagraphEnd;
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

		// The text inputs of one canvas's nodes ([first, last) of `table`) whose TextElement is an element below the input in
		// the canvas (the table's parent chain, which never leaves it), as (TextElement, input) pairs sorted by TextElement;
		// when two inputs name one element, the later in canonical order wins, as the layout's part rules do. Empty in a
		// canvas without a text input.
		static std::vector<std::pair<UUID, UUID>> FindUiTextInputElements(const Scene& scene, const UiLayoutTable& table, size_t first, size_t last)
		{
			std::vector<std::pair<UUID, UUID>> elements;
			for (size_t index = first; index < last; ++index)
			{
				const ConstEntity entity = scene.FindEntityByID(table.Nodes[index].Entity);
				const UiTextInputComponent* input = entity.IsValid() ? entity.TryGetComponent<UiTextInputComponent>() : nullptr;
				if (input == nullptr || !input->TextElement.IsValid())
					continue;
				const UiLayoutNode* part = FindUiLayoutNode(table, input->TextElement);
				bool below = false;
				for (const UiLayoutNode* parent = part != nullptr ? FindUiLayoutNode(table, part->Parent) : nullptr; parent != nullptr && !below;
					parent = FindUiLayoutNode(table, parent->Parent))
				{
					below = parent->Entity == entity.GetUUID();
				}
				if (!below)
					continue;
				std::erase_if(elements, [&input](const std::pair<UUID, UUID>& element)
				{
					return element.first == input->TextElement;
				});
				elements.emplace_back(input->TextElement, entity.GetUUID());
			}
			std::ranges::sort(elements);
			return elements;
		}

		// The colours of the UI's capture annotations (UiExtraction.h): outlines yellow, labels white, as the entities' bounds
		// and labels are drawn (RenderAnnotations.cpp).
		constexpr glm::vec4 UiOutlineColor = glm::vec4(1.0f, 1.0f, 0.0f, 1.0f);
		constexpr glm::vec4 UiLabelColor = glm::vec4(1.0f);

		// C22.3 (decision 51): a World canvas's matrix at its entity's render pose, the interpolated world matrix meshes draw
		// with (ComputeRenderedWorldMatrix at the extraction's interpolation alpha, the milestone review: never the scene's,
		// which is the frame's only inside the play session's RenderExtraction phase), times the canvas's own placement
		// (Private/UiWorldCanvas.h); the layout's simulated matrix for a canvas the scene no longer holds.
		static glm::mat4 GetUiWorldCanvasRenderMatrix(const UiCanvasLayout& canvas, ConstEntity entity, const UiCanvasComponent* component, float alpha)
		{
			if (!entity.IsValid() || component == nullptr)
				return canvas.World;
			return ComputeRenderedWorldMatrix(entity, std::clamp(alpha, 0.0f, 1.0f)) * MakeUiWorldCanvasLocalMatrix(canvas.SceneSize, component->PixelsPerUnit);
		}

		// The scene-pixel rectangle bounding a World canvas element's rectangle [min, max] (its canvas's pixels) as `snapshot`'s
		// camera projects it through `world` (UiExtraction.h's annotation rule); nullopt without a camera, or when a corner lies
		// outside the camera's clip range by view-space depth (the milestone review: decision 50's rule for the pointer, since
		// an orthographic projection's clip-space w is always 1) or its clip-space w is not positive.
		static std::optional<std::pair<glm::vec2, glm::vec2>> ProjectUiWorldRect(const RenderSnapshot& snapshot, const glm::mat4& world, glm::vec2 min,
			glm::vec2 max)
		{
			if (!snapshot.HasCamera)
				return std::nullopt;
			const CameraData& camera = snapshot.Camera;
			const glm::mat4 toView = camera.View * world;
			const glm::mat4 toClip = camera.Projection * toView;
			const glm::vec2 viewport(static_cast<float>(camera.ViewportWidth), static_cast<float>(camera.ViewportHeight));
			const bool orthographic = camera.ProjectionKind == RenderProjection::Orthographic;
			glm::vec2 lowest(0.0f);
			glm::vec2 highest(0.0f);
			const std::array<glm::vec2, 4> corners = { min, glm::vec2(max.x, min.y), max, glm::vec2(min.x, max.y) };
			for (size_t index = 0; index < corners.size(); ++index)
			{
				const glm::vec4 corner(corners[index], 0.0f, 1.0f);
				// Drawn from the near plane on, and up to the far plane for an orthographic camera (a perspective one's is at
				// infinity, Architecture §8.3), inclusive, as the pointer's ray takes the clip range.
				const float depth = -(toView * corner).z;
				if (!(depth >= camera.NearClip) || (orthographic && !(depth <= camera.FarClip)))
					return std::nullopt;
				const glm::vec4 clip = toClip * corner;
				if (!(clip.w > 0.0f))
					return std::nullopt;
				const glm::vec2 pixel((clip.x / clip.w + 1.0f) * 0.5f * viewport.x, (1.0f - clip.y / clip.w) * 0.5f * viewport.y);
				lowest = index == 0 ? pixel : glm::min(lowest, pixel);
				highest = index == 0 ? pixel : glm::max(highest, pixel);
			}
			return std::pair{ lowest, highest };
		}

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

	void AppendUiDrawItems(const Scene& scene, const UiLayoutTable& table, UiDrawList& list, const UiInteractionState* interaction, bool worldOnly,
		float alpha, const UiPresentationTexts* presentation)
	{
		for (const UiCanvasLayout& canvas : table.Canvases)
		{
			// C22.3 (decision 51): the editor's scene view draws World canvases only.
			const bool world = canvas.RenderMode == UiCanvasRenderMode::World;
			if (worldOnly && !world)
				continue;
			// The canvas's theme and font (C22.2): its widgets' tints, and the font of its texts whose own Font is null. A
			// canvas the scene no longer holds tints with the default theme and names no font.
			const ConstEntity canvasEntity = scene.FindEntityByID(canvas.Canvas);
			const UiCanvasComponent* canvasComponent = canvasEntity.IsValid() ? canvasEntity.TryGetComponent<UiCanvasComponent>() : nullptr;
			// A World canvas's items are in its canvas pixels and name it (C22.3, decision 51), placed at its render pose.
			const size_t firstItem = list.Items.size();
			uint32_t worldCanvas = UiScreenCanvas;
			if (world)
			{
				worldCanvas = static_cast<uint32_t>(list.WorldCanvases.size());
				list.WorldCanvases.push_back(UiWorldCanvas{ .Canvas = canvas.Canvas,
					.Size = canvas.SceneSize,
					.World = Utils::GetUiWorldCanvasRenderMatrix(canvas, canvasEntity, canvasComponent, alpha) });
			}
			const UiTheme theme = canvasComponent != nullptr ? canvasComponent->Theme : UiTheme{};
			const AssetHandle canvasFont = canvasComponent != nullptr ? canvasComponent->Font.GetHandle() : AssetHandle();
			// A table of this scene's layout holds its canvases' node ranges; a range past the nodes is cut to them.
			const size_t first = std::min<size_t>(canvas.FirstNode, table.Nodes.size());
			const size_t last = std::min<size_t>(first + canvas.NodeCount, table.Nodes.size());
			// C22.3 (decision 48): the elements whose UiText draws a text input's Text in place of its own.
			const std::vector<std::pair<UUID, UUID>> textInputElements = Utils::FindUiTextInputElements(scene, table, first, last);
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
				const UiTextComponent* text = entity.TryGetComponent<UiTextComponent>();
				if (text == nullptr)
					continue;
				// A text input's TextElement draws the input's Text, with the caret while the input has focus (an empty Text
				// then draws only the caret); an unfocused empty input draws nothing here (its Placeholder shows).
				const std::string* drawn = &text->Text;
				// M28 (decision 5): a presentation text the session's scripts set draws in place of the authored Text.
				if (presentation != nullptr)
				{
					if (const auto shown = presentation->find(node.Entity); shown != presentation->end())
						drawn = &shown->second;
				}
				bool caret = false;
				uint32_t caretByte = 0;
				if (const auto found = std::ranges::lower_bound(textInputElements, node.Entity, {}, &std::pair<UUID, UUID>::first);
					found != textInputElements.end() && found->first == node.Entity)
				{
					const ConstEntity inputEntity = scene.FindEntityByID(found->second);
					if (const UiTextInputComponent* input = inputEntity.IsValid() ? inputEntity.TryGetComponent<UiTextInputComponent>() : nullptr)
					{
						drawn = &input->Text;
						caret = interaction != nullptr && interaction->Focused == found->second;
						caretByte = caret ? GetUiTextInputCaretByte(*input) : 0;
					}
				}
				if (!drawn->empty() || caret)
				{
					list.Items.push_back(UiDrawItem{
						.Kind = UiDrawKind::Text,
						.Entity = node.Entity,
						.Min = node.Min,
						.Max = node.Max,
						.Color = text->Color,
						.Clipped = node.Clipped,
						.Clip = node.Clip,
						.Text = *drawn,
						.Font = text->Font.IsValid() ? text->Font.GetHandle() : canvasFont,
						.FontSize = text->Size * canvas.Scale,
						.HorizontalAlignment = Utils::ToUiDrawAlignment(text->Alignment),
						.VerticalAlignment = Utils::ToUiDrawAlignment(text->VerticalAlignment),
						.Wrap = text->Wrap,
						.LineSpacing = text->LineSpacing,
						.HasCaret = caret,
						.CaretByte = caretByte,
					});
				}
			}
			for (size_t item = firstItem; item < list.Items.size(); ++item)
				list.Items[item].WorldCanvas = worldCanvas;
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
			// C22.3: a World canvas's element at the scene-pixel rectangle bounding its corners as the camera projects them, and
			// none when the camera does not see all four (or there is no camera).
			glm::vec2 min = item.Min;
			glm::vec2 max = item.Max;
			if (item.WorldCanvas != UiScreenCanvas)
			{
				const std::optional<std::pair<glm::vec2, glm::vec2>> projected = item.WorldCanvas < ui.WorldCanvases.size()
					? Utils::ProjectUiWorldRect(snapshot, ui.WorldCanvases[item.WorldCanvas].World, item.Min, item.Max)
					: std::nullopt;
				if (!projected.has_value())
					continue;
				min = projected->first;
				max = projected->second;
			}
			if (options.Bounds)
				ui.Annotations.push_back(UiAnnotation{ .Entity = item.Entity, .Min = min, .Max = max, .Color = Utils::UiOutlineColor, .Outline = true });
			if (!Utils::IsUiElementLabelled(snapshot, item.Entity))
				continue;
			const ConstEntity entity = scene.FindEntityByID(item.Entity);
			if (!entity.IsValid())
				continue;
			ui.Annotations.push_back(UiAnnotation{ .Entity = item.Entity,
				.Min = min,
				.Max = max,
				.Color = Utils::UiLabelColor,
				.Outline = false,
				.Label = std::format("{} {}", entity.GetName(), item.Entity.ToString().substr(0, 6)) });
		}
	}

}
