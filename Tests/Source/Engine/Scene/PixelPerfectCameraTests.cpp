#include "TestsPCH.h"

#include "Engine/Scene/PixelPerfectCamera.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PixelPerfectCameraComponent.h"
#include "Engine/Scene/Components/PostProcessComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"
#include "Support/SceneTestFixture.h"

#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <utility>

// The pixel-perfect camera (Docs/Decisions/0032-m21-contract.md decision 6, stream C): its view (CPU), and at scale 3 the
// scene it renders through render extraction, the output stage and picking (GPU).

namespace Engine {

	namespace {

		// The scale-3 case: a 64 x 36 reference at 4 pixels per unit (a view 16 x 9 units) in a 200 x 120 view, Scale 3, the
		// scene image at view pixels [4, 196) x [6, 114).
		constexpr uint32_t ViewWidth = 200;
		constexpr uint32_t ViewHeight = 120;
		constexpr uint32_t ReferenceWidth = 64;
		constexpr uint32_t ReferenceHeight = 36;
		constexpr uint32_t Scale = 3;
		constexpr uint32_t OffsetX = 4;
		constexpr uint32_t OffsetY = 6;
		// The 2 x 2 unit quad of the renderer fixture (its mesh and material handles), at (1, 1): world [0, 2] x [0, 2], which
		// under the camera snapped to the origin is scene pixels [32, 40) x [10, 18).
		const AssetHandle QuadMeshHandle = UUID(8910);
		const AssetHandle QuadMaterialHandle = UUID(8911);
		constexpr uint32_t QuadSceneLeft = 32;
		constexpr uint32_t QuadSceneTop = 10;
		constexpr uint32_t QuadSceneSide = 8;

		glm::u8vec4 GetPixel(const Image& image, uint32_t x, uint32_t y)
		{
			const std::span<const std::byte> texel = image.GetRow(y).subspan(static_cast<size_t>(x) * 4, 4);
			return { std::to_integer<uint8_t>(texel[0]), std::to_integer<uint8_t>(texel[1]), std::to_integer<uint8_t>(texel[2]),
				std::to_integer<uint8_t>(texel[3]) };
		}

		// The linear colour whose sRGB encoding is (r, g, b) / 255.
		glm::vec3 SrgbColor(double r, double g, double b)
		{
			return glm::vec3(static_cast<float>(Test::SrgbToLinear(r / 255.0)), static_cast<float>(Test::SrgbToLinear(g / 255.0)),
				static_cast<float>(Test::SrgbToLinear(b / 255.0)));
		}

		bool IsInQuad(uint32_t sceneX, uint32_t sceneY)
		{
			return sceneX >= QuadSceneLeft && sceneX < QuadSceneLeft + QuadSceneSide && sceneY >= QuadSceneTop && sceneY < QuadSceneTop + QuadSceneSide;
		}

		// A view pixel to pick and whether it shows the quad.
		struct ExpectedPick
		{
			uint32_t X = 0;
			uint32_t Y = 0;
			bool Hit = false;
		};

		// Picks view pixel (x, y) of `snapshot`, the renderer's last submitted render, and returns the result once it is ready.
		std::optional<PickResult> Pick(Test::HeadlessGpuFixture& gpu, SceneRenderer& renderer, const RenderSnapshot& snapshot, uint32_t x, uint32_t y)
		{
			const PickRequest request{ .X = x,
				.Y = y,
				.FrameIndex = snapshot.FrameIndex,
				.SceneRevision = snapshot.SceneRevision,
				.Sequence = 1,
				.ViewGeneration = renderer.GetViewGeneration() };
			Result<PickTicket> ticket = renderer.RequestPick(request);
			REQUIRE_MESSAGE(ticket.has_value(), ticket.error().ToString());
			gpu.GetDevice().WaitForIdle();
			Result<std::optional<PickResult>> ready = renderer.PollPick(*ticket, snapshot.FrameIndex + 2);
			REQUIRE_MESSAGE(ready.has_value(), ready.error().ToString());
			return *ready;
		}

	}

	TEST_SUITE("Scene")
	{
		TEST_CASE("PixelPerfectCamera: the view renders the reference resolution in pixel-art mode at an orthographic size of its height")
		{
			const PixelPerfectCameraComponent camera{ .ReferenceWidth = 256, .ReferenceHeight = 144, .PixelsPerUnit = 8.0f, .LetterboxColor = glm::vec3(0.1f, 0.2f, 0.3f) };
			const PixelPerfectView view = ComputePixelPerfectView(camera, glm::vec3(0.0f, 0.0f, 5.0f));
			CHECK(view.Output.SceneWidth == 256);
			CHECK(view.Output.SceneHeight == 144);
			CHECK(view.Output.PixelArt);
			CHECK(view.Output.LetterboxColor == glm::vec3(0.1f, 0.2f, 0.3f));
			CHECK(view.OrthographicSize == 9.0f);
			CHECK(view.Position == glm::vec3(0.0f, 0.0f, 5.0f));
		}

		TEST_CASE("PixelPerfectCamera: the camera position snaps the view's left and bottom edges to the pixel grid")
		{
			// An odd reference width puts the centre on a half pixel: the left edge, x - 21 / 32 (10.5 pixels of 1/16 unit), is
			// what snaps.
			const PixelPerfectCameraComponent camera{ .ReferenceWidth = 21, .ReferenceHeight = 20, .PixelsPerUnit = 16.0f };
			const PixelPerfectView view = ComputePixelPerfectView(camera, glm::vec3(1.01f, -2.02f, 3.0f));
			const float left = (view.Position.x - 21.0f / 32.0f) * 16.0f;
			const float bottom = (view.Position.y - 20.0f / 32.0f) * 16.0f;
			CHECK(left == std::round(left));
			CHECK(bottom == std::round(bottom));
			CHECK(view.Position.x == doctest::Approx(1.0f + 1.0f / 32.0f));
			CHECK(view.Position.z == 3.0f);

			// An edge half a pixel off the grid rounds away from zero, on either side of the origin; a view on the grid stays.
			const PixelPerfectCameraComponent even{ .ReferenceWidth = 20, .ReferenceHeight = 20, .PixelsPerUnit = 2.0f };
			CHECK(ComputePixelPerfectView(even, glm::vec3(5.25f, -5.25f, 0.0f)).Position == glm::vec3(5.5f, -5.5f, 0.0f));
			CHECK(ComputePixelPerfectView(even, glm::vec3(5.5f, -5.5f, 0.0f)).Position == glm::vec3(5.5f, -5.5f, 0.0f));
		}

		TEST_CASE("PixelPerfectCamera: at scale 3 the scene renders its reference resolution in a letterbox, and picks through it"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture setup(gpu, ViewWidth, ViewHeight);
			Test::SceneTestFixture fixture;
			Scene& scene = fixture.GetScene();

			// A perspective camera off the pixel grid: the PixelPerfectCamera snaps it to the origin and makes it orthographic.
			// Its clear colour is the linear value of the sRGB texel (200, 100, 50), which the post settings below would all
			// change outside the 2D colour path.
			Entity camera = scene.CreateEntity("Camera");
			camera.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.1f, -0.05f, 10.0f);
			});
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true, .Clear = ClearMode::Color, .ClearColor = SrgbColor(200.0, 100.0, 50.0) });
			camera.AddComponent<PixelPerfectCameraComponent>(PixelPerfectCameraComponent{ .ReferenceWidth = ReferenceWidth,
				.ReferenceHeight = ReferenceHeight,
				.PixelsPerUnit = 4.0f,
				.LetterboxColor = SrgbColor(30.0, 60.0, 90.0) });
			Entity post = scene.CreateEntity("Post");
			post.AddComponent<PostProcessComponent>(PostProcessComponent{ .ExposureEV = 2.0f });
			Entity quad = scene.CreateEntity("Quad");
			quad.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(1.0f, 1.0f, 0.0f);
			});
			MeshRendererComponent renderer;
			renderer.Mesh.SetHandle(QuadMeshHandle);
			renderer.Materials.emplace_back(QuadMaterialHandle);
			quad.AddComponent<MeshRendererComponent>(std::move(renderer));
			TransformSystem::Update(scene);

			Result<RenderSnapshot> extracted = ExtractRenderSnapshot(scene, { .Width = ViewWidth, .Height = ViewHeight, .Flags = RenderViewFlags::Picking });
			REQUIRE_MESSAGE(extracted.has_value(), extracted.error().ToString());
			RenderSnapshot snapshot = std::move(*extracted);
			snapshot.FrameIndex = 30;
			snapshot.SceneRevision = 5;
			const Image image = setup.Render(snapshot);
			CHECK(setup.Renderer().GetOutputMapping()
				== RenderOutputMapping{ .ViewWidth = ViewWidth,
					.ViewHeight = ViewHeight,
					.SceneWidth = ReferenceWidth,
					.SceneHeight = ReferenceHeight,
					.Scale = Scale,
					.OffsetX = static_cast<int32_t>(OffsetX),
					.OffsetY = static_cast<int32_t>(OffsetY) });

			// Every scene pixel is a 3 x 3 block, the background keeps its texel (the 2D colour path), the quad covers its
			// pixels exactly, and the letterbox has the camera's LetterboxColor.
			uint32_t mismatches = 0;
			for (uint32_t y = 0; y < ViewHeight; ++y)
			{
				for (uint32_t x = 0; x < ViewWidth; ++x)
				{
					const glm::u8vec4 pixel = GetPixel(image, x, y);
					const bool inScene = x >= OffsetX && x < OffsetX + Scale * ReferenceWidth && y >= OffsetY && y < OffsetY + Scale * ReferenceHeight;
					if (!inScene)
					{
						mismatches += pixel != glm::u8vec4(30, 60, 90, 255) ? 1 : 0;
						continue;
					}
					const uint32_t sceneX = (x - OffsetX) / Scale;
					const uint32_t sceneY = (y - OffsetY) / Scale;
					mismatches += pixel != GetPixel(image, OffsetX + Scale * sceneX, OffsetY + Scale * sceneY) ? 1 : 0;
					mismatches += (pixel == glm::u8vec4(200, 100, 50, 255)) == IsInQuad(sceneX, sceneY) ? 1 : 0;
				}
			}
			CHECK(mismatches == 0);

			// Picking maps a view pixel through the same mapping: the quad's first and last view pixels hit it, the pixels
			// beside them and the letterbox do not.
			const uint32_t quadLeft = OffsetX + Scale * QuadSceneLeft;
			const uint32_t quadTop = OffsetY + Scale * QuadSceneTop;
			const uint32_t quadRight = quadLeft + Scale * QuadSceneSide - 1;
			const uint32_t quadBottom = quadTop + Scale * QuadSceneSide - 1;
			const std::array<ExpectedPick, 5> picks{ { { .X = quadLeft, .Y = quadTop, .Hit = true },
				{ .X = quadRight, .Y = quadBottom, .Hit = true },
				{ .X = quadLeft - 1, .Y = quadTop, .Hit = false },
				{ .X = quadRight, .Y = quadBottom + 1, .Hit = false },
				{ .X = 1, .Y = ViewHeight / 2, .Hit = false } } };
			for (const ExpectedPick& expected : picks)
			{
				CAPTURE(expected.X);
				CAPTURE(expected.Y);
				snapshot.FrameIndex += 3;
				setup.Submit(snapshot);
				const std::optional<PickResult> picked = Pick(gpu, setup.Renderer(), snapshot, expected.X, expected.Y);
				REQUIRE(picked.has_value());
				CHECK(picked->Entity == (expected.Hit ? quad.GetUUID() : UUID()));
			}
		}
	}

}
