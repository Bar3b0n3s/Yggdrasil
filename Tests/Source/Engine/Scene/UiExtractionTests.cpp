#include "TestsPCH.h"

#include "Engine/Scene/UiExtraction.h"

#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/RuntimeComponents.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Components/UiButtonComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Components/UiScrollViewComponent.h"
#include "Engine/Scene/Components/UiSliderComponent.h"
#include "Engine/Scene/Components/UiTextComponent.h"
#include "Engine/Scene/Components/UiTextInputComponent.h"
#include "Engine/Scene/Components/UiToggleComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderAnnotations.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/GlmApprox.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/SceneTestFixture.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <format>
#include <string_view>
#include <utility>
#include <vector>

// The UI's render extraction (M22, C22.1; Docs/Decisions/0033-m22-contract.md decisions 7 and 9): a scene's canvases laid
// out at the snapshot's scene resolution and their visible elements' images and texts in the snapshot's UI draw list, and
// the capture annotations of UI elements. The draw-list cases are stream C's, the annotations' stream E's.

namespace Engine {

	namespace {

		Entity AddElement(Scene& scene, std::string_view name, Entity parent, glm::vec2 position, glm::vec2 size)
		{
			Entity entity = scene.CreateEntity(name, parent);
			entity.AddComponent<UiRectComponent>(
				UiRectComponent{ .AnchorMin = glm::vec2(0.0f), .AnchorMax = glm::vec2(0.0f), .Pivot = glm::vec2(0.0f), .Position = position, .Size = size });
			return entity;
		}

		// A primary camera, and a canvas at two scene pixels per unit holding a clipping panel (an image and a text) with an
		// image child inside it, a hidden child and a child outside the panel's clip.
		struct UiScene
		{
			explicit UiScene(Scene& scene)
			{
				Entity camera = scene.CreateEntity("Camera");
				camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
				Canvas = scene.CreateEntity("Canvas");
				Canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{ .ScaleFactor = 2.0f });
				Panel = AddElement(scene, "Panel", Canvas, glm::vec2(10.0f, 5.0f), glm::vec2(40.0f, 20.0f));
				Panel.GetComponent<UiRectComponent>().ClipChildren = true;
				Panel.AddComponent<UiImageComponent>(UiImageComponent{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(AssetHandle(UUID(0x5E7))),
					.Frame = 3,
					.Color = glm::vec4(0.5f, 0.25f, 1.0f, 0.75f),
					.Type = UiImageType::Sliced,
					.BorderScale = 1.5f,
					.Filter = UiImageFilter::Linear });
				Panel.AddComponent<UiTextComponent>(UiTextComponent{ .Text = "Score",
					.Font = TypedAssetHandle<AssetType::Font>(AssetHandle(UUID(0xF0))),
					.Size = 16.0f,
					.Alignment = TextAlignment::Center,
					.VerticalAlignment = UiTextVerticalAlignment::Middle,
					.Wrap = false,
					.LineSpacing = 1.25f });
				Icon = AddElement(scene, "Icon", Panel, glm::vec2(2.0f), glm::vec2(4.0f));
				Icon.AddComponent<UiImageComponent>(UiImageComponent{});
				Entity hidden = AddElement(scene, "Hidden", Panel, glm::vec2(0.0f), glm::vec2(4.0f));
				hidden.AddComponent<UiImageComponent>(UiImageComponent{});
				hidden.SetActive(false);
				Entity outside = AddElement(scene, "Outside", Panel, glm::vec2(100.0f), glm::vec2(4.0f));
				outside.AddComponent<UiImageComponent>(UiImageComponent{});
				TransformSystem::Update(scene);
			}

			Entity Canvas;
			Entity Panel;
			Entity Icon;
		};

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("RenderExtraction: a scene without a canvas has an empty Ui list")
		{
			Test::SceneTestFixture fixture;
			Entity camera = fixture.GetScene().CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			TransformSystem::Update(fixture.GetScene());
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(fixture.GetScene(), { .Width = 64, .Height = 32 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			CHECK(snapshot->Ui.Items.empty());
			CHECK(snapshot->Ui.Annotations.empty());
		}

		TEST_CASE("UiExtraction: the draw items follow the table's visible elements, an image before its text, at their canvas's scale")
		{
			// AppendUiDrawItems on a table written by hand (the layout's own cases are UiLayoutTests'): the canvases in the
			// table's order, each one's nodes in order, invisible nodes, nodes of entities the scene lacks and empty texts left
			// out, and the canvas's scale applied to the font size and the border scale.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity first = scene.CreateEntity("First");
			Entity second = scene.CreateEntity("Second");
			Entity label = AddElement(scene, "Label", second, glm::vec2(0.0f), glm::vec2(4.0f));
			label.AddComponent<UiImageComponent>(UiImageComponent{ .Sheet = TypedAssetHandle<AssetType::SpriteSheet>(AssetHandle(UUID(0x5E8))),
				.Frame = 7,
				.Color = glm::vec4(0.0f, 0.5f, 1.0f, 1.0f),
				.Type = UiImageType::Simple,
				.BorderScale = 2.0f,
				.Filter = UiImageFilter::Point });
			label.AddComponent<UiTextComponent>(UiTextComponent{ .Text = "Go",
				.Size = 10.0f,
				.Color = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f),
				.Alignment = TextAlignment::Right,
				.VerticalAlignment = UiTextVerticalAlignment::Bottom,
				.Wrap = true,
				.LineSpacing = 2.0f });
			Entity blank = AddElement(scene, "Blank", second, glm::vec2(0.0f), glm::vec2(4.0f));
			blank.AddComponent<UiTextComponent>(UiTextComponent{});
			Entity hidden = AddElement(scene, "Hidden", second, glm::vec2(0.0f), glm::vec2(4.0f));
			hidden.AddComponent<UiImageComponent>(UiImageComponent{});
			Entity panel = AddElement(scene, "Panel", first, glm::vec2(0.0f), glm::vec2(4.0f));
			panel.AddComponent<UiImageComponent>(UiImageComponent{});

			UiLayoutTable table;
			table.SceneSize = glm::uvec2(64, 32);
			const UiClipRect clip{ .MinX = 1, .MinY = 2, .MaxX = 30, .MaxY = 20 };
			table.Nodes = {
				UiLayoutNode{ .Entity = label.GetUUID(), .Canvas = second.GetUUID(), .Parent = second.GetUUID(), .Min = glm::vec2(2.0f, 3.0f), .Max = glm::vec2(10.0f, 9.0f), .Clipped = true, .Clip = clip, .Visible = true },
				UiLayoutNode{ .Entity = blank.GetUUID(), .Canvas = second.GetUUID(), .Parent = second.GetUUID(), .Max = glm::vec2(4.0f), .Visible = true },
				UiLayoutNode{ .Entity = hidden.GetUUID(), .Canvas = second.GetUUID(), .Parent = second.GetUUID(), .Max = glm::vec2(4.0f), .Visible = false },
				UiLayoutNode{ .Entity = UUID(0xDEAD), .Canvas = second.GetUUID(), .Parent = second.GetUUID(), .Max = glm::vec2(4.0f), .Visible = true },
				UiLayoutNode{ .Entity = panel.GetUUID(), .Canvas = first.GetUUID(), .Parent = first.GetUUID(), .Min = glm::vec2(1.0f), .Max = glm::vec2(5.0f), .Visible = true },
			};
			// The second canvas draws first (a lower SortOrder), at 0.5 scene pixels per unit.
			table.Canvases = {
				UiCanvasLayout{ .Canvas = second.GetUUID(), .SortOrder = -1, .Scale = 0.5f, .Size = glm::vec2(128.0f, 64.0f), .FirstNode = 0, .NodeCount = 4 },
				UiCanvasLayout{ .Canvas = first.GetUUID(), .Scale = 1.0f, .Size = glm::vec2(64.0f, 32.0f), .FirstNode = 4, .NodeCount = 1 },
			};
			UiDrawList list;
			AppendUiDrawItems(scene, table, list);
			REQUIRE(list.Items.size() == 3);
			const UiDrawItem& image = list.Items[0];
			CHECK(image.Kind == UiDrawKind::Image);
			CHECK(image.Entity == label.GetUUID());
			CHECK(image.Min == glm::vec2(2.0f, 3.0f));
			CHECK(image.Max == glm::vec2(10.0f, 9.0f));
			CHECK(image.Clipped);
			CHECK(image.Clip == clip);
			CHECK(image.Color == glm::vec4(0.0f, 0.5f, 1.0f, 1.0f));
			CHECK(image.Sheet == AssetHandle(UUID(0x5E8)));
			CHECK(image.Frame == 7);
			CHECK(image.ImageType == UiDrawImageType::Simple);
			CHECK(image.BorderScale == 1.0f);
			CHECK(image.Filter == UiDrawFilter::Point);
			const UiDrawItem& text = list.Items[1];
			CHECK(text.Kind == UiDrawKind::Text);
			CHECK(text.Entity == label.GetUUID());
			CHECK(text.Clipped);
			CHECK(text.Clip == clip);
			CHECK(text.Text == "Go");
			CHECK_FALSE(text.Font.IsValid());
			CHECK(text.FontSize == 5.0f);
			CHECK(text.Color == glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
			CHECK(text.HorizontalAlignment == UiDrawAlignment::End);
			CHECK(text.VerticalAlignment == UiDrawAlignment::End);
			CHECK(text.Wrap);
			CHECK(text.LineSpacing == 2.0f);
			const UiDrawItem& plain = list.Items[2];
			CHECK(plain.Entity == panel.GetUUID());
			CHECK(plain.Min == glm::vec2(1.0f));
			CHECK_FALSE(plain.Clipped);
			CHECK_FALSE(plain.Sheet.IsValid());
			CHECK(plain.Color == glm::vec4(1.0f));
		}

		TEST_CASE("RenderExtraction: a presentation text draws in place of its element's Text in the element's own rectangle")
		{
			// M28 (Docs/Decisions/0043-m28-game-services.md decision 5): the request's presentation texts replace the drawn string
			// only; the element's rectangle is the layout's, which never reads them. An empty one draws no text item.
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const UiScene ui(scene);
			const RenderExtractionRequest authored{ .Width = 128, .Height = 64 };
			const Result<RenderSnapshot> before = ExtractRenderSnapshot(scene, authored);
			REQUIRE_MESSAGE(before.has_value(), before.error().ToString());
			const auto textItem = [&ui](const RenderSnapshot& snapshot) -> const UiDrawItem*
			{
				const auto found = std::ranges::find_if(snapshot.Ui.Items, [&ui](const UiDrawItem& item)
				{
					return item.Kind == UiDrawKind::Text && item.Entity == ui.Panel.GetUUID();
				});
				return found != snapshot.Ui.Items.end() ? &*found : nullptr;
			};
			REQUIRE(textItem(*before) != nullptr);
			CHECK(textItem(*before)->Text == "Score");

			UiPresentationTexts presentation{ { ui.Panel.GetUUID(), "16.7 ms" }, { UUID(0xDEAD), "nobody" } };
			RenderExtractionRequest shown = authored;
			shown.UiPresentation = &presentation;
			const Result<RenderSnapshot> after = ExtractRenderSnapshot(scene, shown);
			REQUIRE_MESSAGE(after.has_value(), after.error().ToString());
			REQUIRE(textItem(*after) != nullptr);
			CHECK(textItem(*after)->Text == "16.7 ms");
			CHECK(textItem(*after)->Min == textItem(*before)->Min);
			CHECK(textItem(*after)->Max == textItem(*before)->Max);
			CHECK(after->Ui.Items.size() == before->Ui.Items.size());
			// The authored text is untouched.
			CHECK(ui.Panel.GetComponent<UiTextComponent>().Text == "Score");

			presentation[ui.Panel.GetUUID()] = "";
			const Result<RenderSnapshot> empty = ExtractRenderSnapshot(scene, shown);
			REQUIRE_MESSAGE(empty.has_value(), empty.error().ToString());
			CHECK(textItem(*empty) == nullptr);
		}

		TEST_CASE("RenderExtraction: a canvas's visible images and texts reach the Ui list in draw order with their scaled lengths")
		{
			Test::SceneTestFixture fixture;
			const UiScene ui(fixture.GetScene());
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(fixture.GetScene(), { .Width = 200, .Height = 100 });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			// The panel's image, then its text, then the icon; the hidden child and the one outside the clip draw nothing.
			REQUIRE(snapshot->Ui.Items.size() == 3);
			const UiDrawItem& image = snapshot->Ui.Items[0];
			CHECK(image.Kind == UiDrawKind::Image);
			CHECK(image.Entity == ui.Panel.GetUUID());
			CHECK(image.Min == glm::vec2(20.0f, 10.0f));
			CHECK(image.Max == glm::vec2(100.0f, 50.0f));
			CHECK_FALSE(image.Clipped);
			CHECK(image.Sheet == AssetHandle(UUID(0x5E7)));
			CHECK(image.Frame == 3);
			CHECK(image.Color == glm::vec4(0.5f, 0.25f, 1.0f, 0.75f));
			CHECK(image.ImageType == UiDrawImageType::Sliced);
			CHECK(image.BorderScale == 3.0f);
			CHECK(image.Filter == UiDrawFilter::Linear);
			const UiDrawItem& text = snapshot->Ui.Items[1];
			CHECK(text.Kind == UiDrawKind::Text);
			CHECK(text.Entity == ui.Panel.GetUUID());
			CHECK(text.Text == "Score");
			CHECK(text.Font == AssetHandle(UUID(0xF0)));
			CHECK(text.FontSize == 32.0f);
			CHECK(text.HorizontalAlignment == UiDrawAlignment::Center);
			CHECK(text.VerticalAlignment == UiDrawAlignment::Center);
			CHECK_FALSE(text.Wrap);
			CHECK(text.LineSpacing == 1.25f);
			const UiDrawItem& icon = snapshot->Ui.Items[2];
			CHECK(icon.Entity == ui.Icon.GetUUID());
			CHECK(icon.Min == glm::vec2(24.0f, 14.0f));
			CHECK(icon.Max == glm::vec2(32.0f, 22.0f));
			CHECK(icon.Clipped);
			CHECK(icon.Clip == UiClipRect{ .MinX = 20, .MinY = 10, .MaxX = 100, .MaxY = 50 });
			CHECK_FALSE(icon.Sheet.IsValid());
		}

		// The scene view draws World canvases (decision 51, "RenderExtraction: World canvases draw in every world view at their
		// render pose, Screen canvases only in game and camera views"); this case's scene holds Screen canvases only.
		TEST_CASE("RenderExtraction: the editor's scene view draws no Screen canvas, and a view without a primary camera draws it")
		{
			Test::SceneTestFixture fixture;
			const UiScene ui(fixture.GetScene());
			const Result<RenderSnapshot> sceneView = ExtractRenderSnapshot(fixture.GetScene(), { .Camera = RenderCameraSource::Explicit, .Width = 200, .Height = 100 });
			REQUIRE_MESSAGE(sceneView.has_value(), sceneView.error().ToString());
			CHECK(sceneView->Ui.Items.empty());

			Test::SceneTestFixture menuFixture;
			Scene& menu = menuFixture.GetScene();
			Entity canvas = menu.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			AddElement(menu, "Title", canvas, glm::vec2(0.0f), glm::vec2(8.0f)).AddComponent<UiImageComponent>(UiImageComponent{});
			TransformSystem::Update(menu);
			const Result<RenderSnapshot> gameView = ExtractRenderSnapshot(menu, { .Width = 64, .Height = 32 });
			REQUIRE_MESSAGE(gameView.has_value(), gameView.error().ToString());
			CHECK_FALSE(gameView->HasCamera);
			CHECK(gameView->Ui.Items.size() == 1);
		}

		TEST_CASE("RenderAnnotations: bounds outline every drawn UI element and labels follow the label mode")
		{
			Test::SceneTestFixture fixture;
			const UiScene ui(fixture.GetScene());
			RenderExtractionRequest request{ .Width = 200, .Height = 100 };
			request.Annotations = RenderAnnotations{ .Labels = RenderAnnotationLabels::Explicit, .LabelEntities = { ui.Icon.GetUUID() }, .Bounds = true };
			Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(fixture.GetScene(), request);
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			AppendUiRenderAnnotations(fixture.GetScene(), *snapshot);
			const auto& annotations = snapshot->Ui.Annotations;
			// One outline per element with a draw item (the panel and the icon), and the icon's label.
			const auto outlines = std::count_if(annotations.begin(), annotations.end(), [](const UiAnnotation& annotation)
			{
				return annotation.Outline;
			});
			CHECK(outlines == 2);
			const auto label = std::find_if(annotations.begin(), annotations.end(), [](const UiAnnotation& annotation)
			{
				return !annotation.Label.empty();
			});
			REQUIRE(label != annotations.end());
			CHECK(label->Entity == ui.Icon.GetUUID());
			CHECK(label->Label == std::format("Icon {}", ui.Icon.GetUUID().ToString().substr(0, 6)));
			CHECK(label->Color == glm::vec4(1.0f));
			CHECK(label->Min == glm::vec2(24.0f, 14.0f));
			for (const UiAnnotation& annotation : annotations)
				if (annotation.Outline)
					CHECK(annotation.Color == glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
		}

		// The annotations read the snapshot's own draw list, whatever filled it, so this case builds the list by hand: an
		// element with an image and a text takes one outline and one label, and a view without a camera still gets them
		// (AppendRenderAnnotations calls AppendUiRenderAnnotations before it returns for a view without one).
		TEST_CASE("RenderAnnotations: UI elements take one outline and one label each, by the label mode, with or without a camera")
		{
			Test::SceneTestFixture fixture;
			Test::InMemoryAssetManager assets;
			Entity panel = fixture.GetScene().CreateEntity("Panel");
			Entity icon = fixture.GetScene().CreateEntity("Icon");
			const UUID gone(0x60e);
			const auto draw = [](UUID entity, UiDrawKind kind, glm::vec2 min, glm::vec2 max)
			{
				return UiDrawItem{ .Kind = kind, .Entity = entity, .Min = min, .Max = max };
			};
			const auto view = [&](RenderAnnotations annotations, std::vector<UUID> selected)
			{
				RenderSnapshot snapshot;
				snapshot.HasCamera = false;
				snapshot.SelectedEntities = std::move(selected);
				snapshot.Annotations = std::move(annotations);
				snapshot.Ui.Items.push_back(draw(panel.GetUUID(), UiDrawKind::Image, glm::vec2(4.0f, 2.0f), glm::vec2(40.0f, 20.0f)));
				snapshot.Ui.Items.push_back(draw(panel.GetUUID(), UiDrawKind::Text, glm::vec2(4.0f, 2.0f), glm::vec2(40.0f, 20.0f)));
				snapshot.Ui.Items.push_back(draw(icon.GetUUID(), UiDrawKind::Image, glm::vec2(8.0f), glm::vec2(16.0f)));
				snapshot.Ui.Items.push_back(draw(gone, UiDrawKind::Image, glm::vec2(0.0f), glm::vec2(2.0f)));
				return snapshot;
			};

			RenderSnapshot all = view(RenderAnnotations{ .Labels = RenderAnnotationLabels::All, .Bounds = true }, {});
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, all));
			CHECK(all.AnnotationDraw.IsEmpty());
			// In the items' order, an outline and then a label per element; an element the scene no longer holds keeps its
			// outline but has no name to label.
			const auto& marks = all.Ui.Annotations;
			REQUIRE(marks.size() == 5);
			CHECK((marks[0].Entity == panel.GetUUID() && marks[0].Outline && marks[0].Label.empty()));
			CHECK(marks[0].Min == glm::vec2(4.0f, 2.0f));
			CHECK(marks[0].Max == glm::vec2(40.0f, 20.0f));
			CHECK((marks[1].Entity == panel.GetUUID() && !marks[1].Outline));
			CHECK(marks[1].Label == std::format("Panel {}", panel.GetUUID().ToString().substr(0, 6)));
			CHECK(marks[1].Color == glm::vec4(1.0f));
			CHECK((marks[2].Entity == icon.GetUUID() && marks[2].Outline));
			CHECK(marks[3].Label == std::format("Icon {}", icon.GetUUID().ToString().substr(0, 6)));
			CHECK((marks[4].Entity == gone && marks[4].Outline));

			RenderSnapshot selected = view(RenderAnnotations{ .Labels = RenderAnnotationLabels::Selected }, { icon.GetUUID() });
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, selected));
			REQUIRE(selected.Ui.Annotations.size() == 1);
			CHECK(selected.Ui.Annotations[0].Entity == icon.GetUUID());
			CHECK_FALSE(selected.Ui.Annotations[0].Outline);

			RenderSnapshot plain = view(RenderAnnotations{}, { icon.GetUUID() });
			REQUIRE(AppendRenderAnnotations(fixture.GetScene(), assets, {}, nullptr, plain));
			CHECK(plain.Ui.Annotations.empty());
		}

		// C22.2 (Docs/Decisions/0033-m22-contract.md decision 25; stream C): each widget's image takes its canvas's tint for
		// its state, from the session's interaction state when there is one; a text without a font takes its canvas's.
		TEST_CASE("UiExtraction: a widget's image takes its canvas's tint for its state, and a text without a font takes its canvas's")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const TypedAssetHandle<AssetType::Font> canvasFont(AssetHandle(UUID(0xF0A7)));
			const TypedAssetHandle<AssetType::Font> ownFont(AssetHandle(UUID(0xF0B7)));
			const UiTheme theme{ .Normal = glm::vec4(1.0f, 1.0f, 1.0f, 1.0f),
				.Hovered = glm::vec4(0.5f, 1.0f, 1.0f, 1.0f),
				.Pressed = glm::vec4(1.0f, 0.5f, 1.0f, 1.0f),
				.Focused = glm::vec4(1.0f, 1.0f, 0.5f, 1.0f),
				.Disabled = glm::vec4(1.0f, 1.0f, 1.0f, 0.5f) };
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{ .Font = canvasFont, .Theme = theme });
			const glm::vec4 color(0.5f, 0.5f, 0.5f, 1.0f);
			const auto addButton = [&scene, &canvas, color](std::string_view name, float y, bool interactable)
			{
				Entity button = AddElement(scene, name, canvas, glm::vec2(0.0f, y), glm::vec2(20.0f, 10.0f));
				button.AddComponent<UiImageComponent>(UiImageComponent{ .Color = color });
				button.AddComponent<UiButtonComponent>(UiButtonComponent{ .Interactable = interactable });
				return button;
			};
			const Entity normal = addButton("Normal", 0.0f, true);
			const Entity hovered = addButton("Hovered", 10.0f, true);
			const Entity pressed = addButton("Pressed", 20.0f, true);
			const Entity focused = addButton("Focused", 30.0f, true);
			const Entity disabled = addButton("Disabled", 40.0f, false);
			Entity label = AddElement(scene, "Label", canvas, glm::vec2(0.0f, 50.0f), glm::vec2(20.0f, 10.0f));
			label.AddComponent<UiImageComponent>(UiImageComponent{ .Color = color });
			label.AddComponent<UiTextComponent>(UiTextComponent{ .Text = "Hi" });
			Entity titled = AddElement(scene, "Titled", canvas, glm::vec2(0.0f, 60.0f), glm::vec2(20.0f, 10.0f));
			titled.AddComponent<UiTextComponent>(UiTextComponent{ .Text = "Title", .Font = ownFont });
			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(100, 100));
			// Each state on a widget of its own (the precedence follows below).
			const UiInteractionState state{ .Focused = focused.GetUUID(), .Hovered = hovered.GetUUID(), .Pressed = pressed.GetUUID() };

			UiDrawList list;
			AppendUiDrawItems(scene, table, list, &state);
			const auto colorOf = [&list](Entity entity, UiDrawKind kind)
			{
				const auto found = std::ranges::find_if(list.Items, [&entity, kind](const UiDrawItem& item)
				{
					return item.Entity == entity.GetUUID() && item.Kind == kind;
				});
				REQUIRE(found != list.Items.end());
				return found->Color;
			};
			CHECK(colorOf(normal, UiDrawKind::Image) == color * theme.Normal);
			CHECK(colorOf(hovered, UiDrawKind::Image) == color * theme.Hovered);
			CHECK(colorOf(pressed, UiDrawKind::Image) == color * theme.Pressed);
			CHECK(colorOf(focused, UiDrawKind::Image) == color * theme.Focused);
			CHECK(colorOf(disabled, UiDrawKind::Image) == color * theme.Disabled);
			// An element that is not a widget keeps its colour.
			CHECK(colorOf(label, UiDrawKind::Image) == color);
			const auto fontOf = [&list](Entity entity)
			{
				const auto found = std::ranges::find_if(list.Items, [&entity](const UiDrawItem& item)
				{
					return item.Entity == entity.GetUUID() && item.Kind == UiDrawKind::Text;
				});
				REQUIRE(found != list.Items.end());
				return found->Font;
			};
			CHECK(fontOf(label) == canvasFont.GetHandle());
			CHECK(fontOf(titled) == ownFont.GetHandle());

			// The first match wins on a widget in several states: Pressed over Focused and Hovered (a pressed button is always
			// under the held pointer), Focused over Hovered, and Disabled over all of them.
			const auto tintWith = [&scene, &table, &colorOf, &list](Entity entity, const UiInteractionState& with)
			{
				UiDrawList states;
				AppendUiDrawItems(scene, table, states, &with);
				list = std::move(states);
				return colorOf(entity, UiDrawKind::Image);
			};
			const UUID normalId = normal.GetUUID();
			CHECK(tintWith(normal, UiInteractionState{ .Focused = normalId, .Hovered = normalId, .Pressed = normalId }) == color * theme.Pressed);
			CHECK(tintWith(normal, UiInteractionState{ .Focused = normalId, .Hovered = normalId }) == color * theme.Focused);
			const UUID disabledId = disabled.GetUUID();
			CHECK(tintWith(disabled, UiInteractionState{ .Focused = disabledId, .Hovered = disabledId, .Pressed = disabledId }) == color * theme.Disabled);

			// Without a session's state every widget is Normal, the disabled one still Disabled.
			UiDrawList edit;
			AppendUiDrawItems(scene, table, edit);
			list = std::move(edit);
			CHECK(colorOf(hovered, UiDrawKind::Image) == color * theme.Normal);
			CHECK(colorOf(disabled, UiDrawKind::Image) == color * theme.Disabled);
		}

		// Decision 25: every widget kind tints its own image, never a child's; a text takes the font of the canvas it belongs
		// to (a nested canvas's below it); and render extraction tints with the request's interaction state.
		TEST_CASE("UiExtraction: every widget kind takes its tint, a text takes its own canvas's font, and extraction reads the request's state")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			const TypedAssetHandle<AssetType::Font> outerFont(AssetHandle(UUID(0xF0C7)));
			const TypedAssetHandle<AssetType::Font> innerFont(AssetHandle(UUID(0xF0D7)));
			const UiTheme theme{ .Hovered = glm::vec4(0.5f, 1.0f, 1.0f, 1.0f), .Disabled = glm::vec4(1.0f, 0.5f, 1.0f, 0.5f) };
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{ .Font = outerFont, .Theme = theme });
			const glm::vec4 color(0.5f, 0.5f, 0.5f, 1.0f);
			const auto addWidget = [&scene, &canvas, color](std::string_view name, float y)
			{
				Entity widget = AddElement(scene, name, canvas, glm::vec2(0.0f, y), glm::vec2(20.0f, 10.0f));
				widget.AddComponent<UiImageComponent>(UiImageComponent{ .Color = color });
				return widget;
			};
			Entity toggle = addWidget("Toggle", 0.0f);
			toggle.AddComponent<UiToggleComponent>(UiToggleComponent{ .Interactable = false });
			Entity slider = addWidget("Slider", 10.0f);
			slider.AddComponent<UiSliderComponent>(UiSliderComponent{ .Interactable = false });
			Entity scrollView = addWidget("ScrollView", 20.0f);
			scrollView.AddComponent<UiScrollViewComponent>(UiScrollViewComponent{ .Interactable = false });
			Entity button = addWidget("Button", 30.0f);
			button.AddComponent<UiButtonComponent>(UiButtonComponent{});
			// The button's label holds an image of its own, which keeps its colour, and a text in the canvas's font.
			Entity label = AddElement(scene, "Label", button, glm::vec2(0.0f), glm::vec2(20.0f, 10.0f));
			label.AddComponent<UiImageComponent>(UiImageComponent{ .Color = color });
			label.AddComponent<UiTextComponent>(UiTextComponent{ .Text = "Go" });
			// A canvas nested below the button's element, whose text takes the nested canvas's font.
			Entity nested = scene.CreateEntity("Nested", button);
			nested.AddComponent<UiCanvasComponent>(UiCanvasComponent{ .SortOrder = 1, .Font = innerFont });
			Entity caption = AddElement(scene, "Caption", nested, glm::vec2(0.0f, 50.0f), glm::vec2(20.0f, 10.0f));
			caption.AddComponent<UiTextComponent>(UiTextComponent{ .Text = "Hi" });
			TransformSystem::Update(scene);

			const UiInteractionState state{ .Hovered = button.GetUUID() };
			const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 100, .Height = 100, .UiInteraction = &state });
			REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
			const auto itemOf = [&snapshot](Entity entity, UiDrawKind kind)
			{
				const auto found = std::ranges::find_if(snapshot->Ui.Items, [&entity, kind](const UiDrawItem& item)
				{
					return item.Entity == entity.GetUUID() && item.Kind == kind;
				});
				REQUIRE(found != snapshot->Ui.Items.end());
				return *found;
			};
			CHECK(itemOf(toggle, UiDrawKind::Image).Color == color * theme.Disabled);
			CHECK(itemOf(slider, UiDrawKind::Image).Color == color * theme.Disabled);
			CHECK(itemOf(scrollView, UiDrawKind::Image).Color == color * theme.Disabled);
			CHECK(itemOf(button, UiDrawKind::Image).Color == color * theme.Hovered);
			CHECK(itemOf(label, UiDrawKind::Image).Color == color);
			CHECK(itemOf(label, UiDrawKind::Text).Font == outerFont.GetHandle());
			CHECK(itemOf(caption, UiDrawKind::Text).Font == innerFont.GetHandle());

			// Without the request's state, the button is Normal.
			const Result<RenderSnapshot> plain = ExtractRenderSnapshot(scene, { .Width = 100, .Height = 100 });
			REQUIRE_MESSAGE(plain.has_value(), plain.error().ToString());
			const auto plainButton = std::ranges::find_if(plain->Ui.Items, [&button](const UiDrawItem& item)
			{
				return item.Entity == button.GetUUID();
			});
			REQUIRE(plainButton != plain->Ui.Items.end());
			CHECK(plainButton->Color == color * theme.Normal);
		}

		// C22.3 (Docs/Decisions/0033-m22-contract.md decision 48; stream T).
		TEST_CASE("UiExtraction: a text input's TextElement draws its Text, with the caret while the input is focused")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity canvas = scene.CreateEntity("Canvas");
			canvas.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			Entity input = AddElement(scene, "Input", canvas, glm::vec2(0.0f), glm::vec2(100.0f, 20.0f));
			Entity text = AddElement(scene, "Text", input, glm::vec2(0.0f), glm::vec2(100.0f, 20.0f));
			text.AddComponent<UiTextComponent>(UiTextComponent{ .Text = "authored", .Wrap = false });
			Entity hint = AddElement(scene, "Hint", input, glm::vec2(0.0f), glm::vec2(100.0f, 20.0f));
			hint.AddComponent<UiTextComponent>(UiTextComponent{ .Text = "Name?" });
			input.AddComponent<UiTextInputComponent>(
				UiTextInputComponent{ .Text = "h\xC3\xA9llo", .Caret = 2, .TextElement = text.GetUUID(), .Placeholder = hint.GetUUID() });
			const UiLayoutTable table = ComputeUiLayout(scene, glm::uvec2(200, 100));

			// Unfocused: the TextElement draws the input's Text, without a caret; the placeholder is hidden (Text is not empty).
			UiDrawList list;
			AppendUiDrawItems(scene, table, list);
			REQUIRE(list.Items.size() == 1);
			CHECK(list.Items[0].Entity == text.GetUUID());
			CHECK(list.Items[0].Text == "h\xC3\xA9llo");
			CHECK_FALSE(list.Items[0].HasCaret);

			// Focused: the caret before codepoint 2, byte 3.
			const UiInteractionState focused{ .Focused = input.GetUUID() };
			list = UiDrawList{};
			AppendUiDrawItems(scene, table, list, &focused);
			REQUIRE(list.Items.size() == 1);
			CHECK(list.Items[0].HasCaret);
			CHECK(list.Items[0].CaretByte == 3u);

			// Empty: the TextElement draws nothing and the placeholder its own text; focused, the TextElement draws only its caret.
			input.GetComponent<UiTextInputComponent>().Text.clear();
			const UiLayoutTable empty = ComputeUiLayout(scene, glm::uvec2(200, 100));
			list = UiDrawList{};
			AppendUiDrawItems(scene, empty, list);
			REQUIRE(list.Items.size() == 1);
			CHECK(list.Items[0].Entity == hint.GetUUID());
			CHECK(list.Items[0].Text == "Name?");
			list = UiDrawList{};
			AppendUiDrawItems(scene, empty, list, &focused);
			REQUIRE(list.Items.size() == 2);
			CHECK(list.Items[0].Entity == text.GetUUID());
			CHECK(list.Items[0].Text.empty());
			CHECK(list.Items[0].HasCaret);
			CHECK(list.Items[0].CaretByte == 0u);
			CHECK(list.Items[1].Entity == hint.GetUUID());
		}

		// C22.3 (decision 51; stream W).
		TEST_CASE("RenderExtraction: World canvases draw in every world view at their render pose, Screen canvases only in game and camera views")
		{
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();
			Entity camera = scene.CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			Entity hud = scene.CreateEntity("Hud");
			hud.AddComponent<UiCanvasComponent>(UiCanvasComponent{});
			AddElement(scene, "Score", hud, glm::vec2(0.0f), glm::vec2(8.0f)).AddComponent<UiImageComponent>(UiImageComponent{});
			Entity board = scene.CreateEntity("Board");
			board.GetComponent<TransformComponent>().Translation = glm::vec3(4.0f, 0.0f, 0.0f);
			board.AddComponent<UiCanvasComponent>(
				UiCanvasComponent{ .RenderMode = UiCanvasRenderMode::World, .WorldWidth = 64, .WorldHeight = 32, .PixelsPerUnit = 32.0f });
			Entity sign = AddElement(scene, "Sign", board, glm::vec2(8.0f), glm::vec2(16.0f));
			sign.AddComponent<UiImageComponent>(UiImageComponent{});
			TransformSystem::Update(scene);

			// The game view: the World canvas's item in its own pixels, naming it; the Screen canvas's in scene pixels.
			const Result<RenderSnapshot> game = ExtractRenderSnapshot(scene, { .Width = 200, .Height = 100 });
			REQUIRE_MESSAGE(game.has_value(), game.error().ToString());
			REQUIRE(game->Ui.WorldCanvases.size() == 1);
			CHECK(game->Ui.WorldCanvases[0].Canvas == board.GetUUID());
			CHECK(game->Ui.WorldCanvases[0].Size == glm::uvec2(64, 32));
			CHECK(glm::vec3(game->Ui.WorldCanvases[0].World * glm::vec4(32.0f, 16.0f, 0.0f, 1.0f)) == glm::vec3(4.0f, 0.0f, 0.0f));
			const auto signItem = std::ranges::find_if(game->Ui.Items, [&sign](const UiDrawItem& item)
			{
				return item.Entity == sign.GetUUID();
			});
			REQUIRE(signItem != game->Ui.Items.end());
			CHECK(signItem->WorldCanvas == 0u);
			CHECK(signItem->Min == glm::vec2(8.0f));
			CHECK(game->Ui.Items.size() == 2);

			// The editor's scene view (an Explicit camera) draws the World canvas only.
			const Result<RenderSnapshot> sceneView = ExtractRenderSnapshot(scene, { .Camera = RenderCameraSource::Explicit, .Width = 200, .Height = 100 });
			REQUIRE_MESSAGE(sceneView.has_value(), sceneView.error().ToString());
			REQUIRE(sceneView->Ui.Items.size() == 1);
			CHECK(sceneView->Ui.Items[0].Entity == sign.GetUUID());
			CHECK(sceneView->Ui.WorldCanvases.size() == 1);
		}

		// The milestone review (decision 51, the lead's decision 1): a World canvas is placed at the extraction request's
		// interpolation alpha, as the camera, the meshes, the sprites and the texts are, never at the scene's own alpha, which
		// the play session sets only inside its RenderExtraction phase (between frames it is 1, while PlaySession::ExtractView of
		// a live session extracts at the last frame's alpha). A canvas moving from x = 0 to x = 4 is drawn at the interpolated
		// pose of its entity at the request's alpha, with the scene's alpha left at 1; at alpha 1 at its latest pose.
		TEST_CASE("RenderExtraction: a World canvas is placed at the request's interpolation alpha, as the meshes are")
		{
			// A runtime scene, as a play session's: only it interpolates.
			Test::SceneTestFixture fixture(1, true);
			Scene& scene = fixture.GetScene();
			Entity camera = scene.CreateEntity("Camera");
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true });
			Entity board = scene.CreateEntity("Board");
			board.AddComponent<UiCanvasComponent>(
				UiCanvasComponent{ .RenderMode = UiCanvasRenderMode::World, .WorldWidth = 64, .WorldHeight = 32, .PixelsPerUnit = 32.0f });
			AddElement(scene, "Sign", board, glm::vec2(8.0f), glm::vec2(16.0f)).AddComponent<UiImageComponent>(UiImageComponent{});
			TransformSystem::Update(scene);
			// The previous tick's pose at the origin, the latest at x = 4, as the session's interpolation snapshot leaves them.
			board.AddComponent<PreviousWorldTransformComponent>(PreviousWorldTransformComponent{ .Matrix = board.GetComponent<WorldTransformComponent>().Matrix });
			board.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(4.0f, 0.0f, 0.0f);
			});
			TransformSystem::Update(scene);
			REQUIRE(scene.GetInterpolationAlpha() == 1.0f);

			for (const float alpha : { 0.25f, 1.0f })
			{
				CAPTURE(alpha);
				const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene, { .Width = 200, .Height = 100, .Alpha = alpha });
				REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
				REQUIRE(snapshot->Ui.WorldCanvases.size() == 1);
				// The canvas's centre stands where the entity's rendered world matrix at the request's alpha puts its origin.
				const glm::vec3 centre(snapshot->Ui.WorldCanvases[0].World * glm::vec4(32.0f, 16.0f, 0.0f, 1.0f));
				const glm::vec3 rendered(ComputeRenderedWorldMatrix(board, alpha)[3]);
				CHECK(Test::ApproxEqual(centre, rendered));
				CHECK(Test::ApproxEqual(centre, glm::vec3(4.0f * alpha, 0.0f, 0.0f)));
			}
		}

		// C22.3 (the review of C22.3, its major on the consumers of World canvases' pixels; stream W, UiExtraction.h): a World
		// canvas's element is outlined and labelled at the scene-pixel rectangle bounding its corners as the camera projects
		// them, and not at all without a camera or when a corner lies behind the camera's plane; a Screen canvas's element
		// keeps its rectangle. The list is built by hand, as the annotations read the snapshot's own draw list.
		TEST_CASE("RenderAnnotations: a World canvas's element is outlined and labelled where the camera projects it")
		{
			Test::SceneTestFixture fixture;
			Entity sign = fixture.GetScene().CreateEntity("Sign");
			Entity hud = fixture.GetScene().CreateEntity("Hud");
			const auto view = [&sign, &hud](bool hasCamera, float z)
			{
				RenderSnapshot snapshot;
				snapshot.HasCamera = hasCamera;
				// The camera at the origin, looking down -Z, through a projection whose clip-space w is the view-space -z: a
				// point at z = -0.5 lands at NDC (2 x, 2 y), in a 200 x 100 scene image.
				snapshot.Camera.Projection = glm::mat4(1.0f);
				snapshot.Camera.Projection[2][3] = -1.0f;
				snapshot.Camera.Projection[3][3] = 0.0f;
				snapshot.Camera.ViewportWidth = 200;
				snapshot.Camera.ViewportHeight = 100;
				snapshot.Annotations = RenderAnnotations{ .Labels = RenderAnnotationLabels::All, .Bounds = true };
				// A 64 x 32 World canvas at 32 canvas pixels per unit, centred on (0, 0, z): the sign's [16, 8] to [48, 24] spans
				// x -0.5 to 0.5 and y 0.25 down to -0.25.
				snapshot.Ui.WorldCanvases.push_back(UiWorldCanvas{ .Canvas = UUID(0xB0A2D),
					.Size = glm::uvec2(64, 32),
					.World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, z)) * glm::scale(glm::mat4(1.0f), glm::vec3(1.0f / 32.0f, -1.0f / 32.0f, 1.0f))
						* glm::translate(glm::mat4(1.0f), glm::vec3(-32.0f, -16.0f, 0.0f)) });
				snapshot.Ui.Items.push_back(UiDrawItem{ .Kind = UiDrawKind::Image,
					.Entity = sign.GetUUID(),
					.Min = glm::vec2(16.0f, 8.0f),
					.Max = glm::vec2(48.0f, 24.0f),
					.WorldCanvas = 0 });
				snapshot.Ui.Items.push_back(
					UiDrawItem{ .Kind = UiDrawKind::Image, .Entity = hud.GetUUID(), .Min = glm::vec2(4.0f, 2.0f), .Max = glm::vec2(40.0f, 20.0f) });
				return snapshot;
			};

			// In front of the camera: the sign's corners at NDC (-1, 0.5) and (1, -0.5), so (0, 25) to (200, 75).
			RenderSnapshot front = view(true, -0.5f);
			AppendUiRenderAnnotations(fixture.GetScene(), front);
			const auto& marks = front.Ui.Annotations;
			REQUIRE(marks.size() == 4);
			CHECK((marks[0].Entity == sign.GetUUID() && marks[0].Outline));
			CHECK(marks[0].Min == glm::vec2(0.0f, 25.0f));
			CHECK(marks[0].Max == glm::vec2(200.0f, 75.0f));
			CHECK((marks[1].Entity == sign.GetUUID() && !marks[1].Outline));
			CHECK(marks[1].Min == glm::vec2(0.0f, 25.0f));
			CHECK((marks[2].Entity == hud.GetUUID() && marks[2].Outline));
			CHECK(marks[2].Min == glm::vec2(4.0f, 2.0f));
			CHECK(marks[2].Max == glm::vec2(40.0f, 20.0f));

			// Behind the camera, or in a view without one: the sign takes none, the HUD's element its own.
			for (RenderSnapshot snapshot : { view(true, 0.5f), view(false, -0.5f) })
			{
				AppendUiRenderAnnotations(fixture.GetScene(), snapshot);
				REQUIRE(snapshot.Ui.Annotations.size() == 2);
				CHECK(snapshot.Ui.Annotations[0].Entity == hud.GetUUID());
				CHECK(snapshot.Ui.Annotations[1].Entity == hud.GetUUID());
			}
		}

		// The milestone review (decision 51, the lead's decision 3): a World canvas's element is annotated only where the
		// camera draws it, within the camera's clip range by view-space depth, the rule decision 50 gives the pointer: beyond
		// the near plane, and before the far plane of an orthographic camera, whose projection clips there (a perspective
		// camera's far plane is at infinity). Clip-space w cannot tell: an orthographic projection's w is always 1, so a canvas
		// behind such a camera or beyond its far plane was outlined over a scene that shows nothing there.
		TEST_CASE("RenderAnnotations: a World canvas's element is annotated only within the camera's clip range")
		{
			Test::SceneTestFixture fixture;
			Entity sign = fixture.GetScene().CreateEntity("Sign");
			const auto annotated = [&fixture, &sign](RenderProjection kind, float z)
			{
				RenderSnapshot snapshot;
				snapshot.HasCamera = true;
				// The camera at the origin, looking down -Z, its clip range 1 to 10 deep. Orthographic: a projection whose w is 1
				// (the view's x and y are NDC). Perspective: one whose w is the view-space -z.
				snapshot.Camera.ProjectionKind = kind;
				snapshot.Camera.NearClip = 1.0f;
				snapshot.Camera.FarClip = 10.0f;
				snapshot.Camera.Projection = glm::mat4(1.0f);
				if (kind == RenderProjection::Perspective)
				{
					snapshot.Camera.Projection[2][3] = -1.0f;
					snapshot.Camera.Projection[3][3] = 0.0f;
				}
				snapshot.Camera.ViewportWidth = 200;
				snapshot.Camera.ViewportHeight = 100;
				snapshot.Annotations = RenderAnnotations{ .Labels = RenderAnnotationLabels::All, .Bounds = true };
				// A 64 x 32 World canvas at 128 canvas pixels per unit centred on (0, 0, z): the sign spans x -0.125 to 0.125.
				snapshot.Ui.WorldCanvases.push_back(UiWorldCanvas{ .Canvas = UUID(0xB0A2D),
					.Size = glm::uvec2(64, 32),
					.World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, z))
						* glm::scale(glm::mat4(1.0f), glm::vec3(1.0f / 128.0f, -1.0f / 128.0f, 1.0f))
						* glm::translate(glm::mat4(1.0f), glm::vec3(-32.0f, -16.0f, 0.0f)) });
				snapshot.Ui.Items.push_back(UiDrawItem{ .Kind = UiDrawKind::Image,
					.Entity = sign.GetUUID(),
					.Min = glm::vec2(16.0f, 8.0f),
					.Max = glm::vec2(48.0f, 24.0f),
					.WorldCanvas = 0 });
				AppendUiRenderAnnotations(fixture.GetScene(), snapshot);
				return snapshot.Ui.Annotations.size() == 2;
			};

			// Orthographic: drawn between the near and the far plane; not behind the camera, before its near plane or beyond
			// its far plane.
			CHECK(annotated(RenderProjection::Orthographic, -5.0f));
			CHECK(annotated(RenderProjection::Orthographic, -10.0f));
			CHECK_FALSE(annotated(RenderProjection::Orthographic, 0.5f));
			CHECK_FALSE(annotated(RenderProjection::Orthographic, -0.5f));
			CHECK_FALSE(annotated(RenderProjection::Orthographic, -20.0f));
			// Perspective: drawn beyond the near plane, however far; not between the camera and its near plane.
			CHECK(annotated(RenderProjection::Perspective, -5.0f));
			CHECK(annotated(RenderProjection::Perspective, -20.0f));
			CHECK_FALSE(annotated(RenderProjection::Perspective, -0.5f));
		}
	}

}
