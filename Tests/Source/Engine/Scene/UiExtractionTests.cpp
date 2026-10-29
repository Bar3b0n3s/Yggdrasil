#include "TestsPCH.h"

#include "Engine/Scene/UiExtraction.h"

#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/UiCanvasComponent.h"
#include "Engine/Scene/Components/UiImageComponent.h"
#include "Engine/Scene/Components/UiRectComponent.h"
#include "Engine/Scene/Components/UiTextComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/SceneTestFixture.h"

#include <glm/glm.hpp>

#include <algorithm>
#include <format>
#include <string_view>

// The UI's render extraction (M22, C22.1; Docs/Decisions/0033-m22-contract.md decisions 7 and 9): a scene's canvases laid
// out at the snapshot's scene resolution and their visible elements' images and texts in the snapshot's UI draw list, and
// the capture annotations of UI elements. The draw-list cases are stream C's, the annotations' stream E's; skipped by the
// contract but for the scene without a canvas, which holds on the contract tree.

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

		TEST_CASE("RenderExtraction: a canvas's visible images and texts reach the Ui list in draw order with their scaled lengths" * doctest::skip(true))
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

		TEST_CASE("RenderExtraction: the editor's scene view draws no game UI, and a view without a primary camera draws it" * doctest::skip(true))
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

		TEST_CASE("RenderAnnotations: bounds outline every drawn UI element and labels follow the label mode" * doctest::skip(true))
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
	}

}
