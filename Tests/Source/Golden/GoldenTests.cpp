#include "TestsPCH.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/TrianglePass.h"
#include "Engine/Renderer/ViewportCapture.h"
#include "Engine/Scene/Components/CameraComponent.h"
#include "Engine/Scene/Components/DirectionalLightComponent.h"
#include "Engine/Scene/Components/MeshRendererComponent.h"
#include "Engine/Scene/Components/PostProcessComponent.h"
#include "Engine/Scene/Components/TransformComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/RenderExtraction.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Testing/ImageCompare.h"
#include "Support/AssetTestFixture.h"
#include "Support/GoldenImage.h"
#include "Support/GoldenScene.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/SceneTestFixture.h"

#include <string>
#include <utility>
#include <vector>

// The golden images (Architecture §15.4): "Triangle", the clear-and-triangle view of the Graphics foundation (Roadmap M5)
// drawn by TrianglePass into an OffscreenTarget at 640x360; "LitScene", a simple lit scene (Roadmap M7: a camera, a sun, a
// cube, a sphere and a ground plane) extracted from a scene and rendered by the scene renderer through the viewport
// capture (viewport.screenshot's path); and the M8 goldens of Roadmap M8 (MaterialGrid, IblOnly, Tonemappers, Bloom,
// AlphaModes, DebugDraw, Text, GltfFixture), each a scene of Projects/FeatureTest/Assets/Scenes/Golden rendered through
// Test::RenderGoldenScene (§15.4; Docs/Decisions/0013-m8-decisions.md decision 14). They run in the golden stage (Release) on
// this machine's device class; Test.py --update-golden writes candidates for review.

namespace Engine {

	namespace {

		// The scene of "LitScene": a primary camera above and in front of the origin looking slightly down at it, a sun from
		// the upper left behind the camera, a white cube turned 30 degrees, a magenta sphere (the Error material) and a ground
		// plane 8 m wide; the light stays below 1 everywhere, so no channel saturates and the faces tell apart.
		void BuildLitScene(Scene& scene)
		{
			Entity camera = scene.CreateEntity("Camera");
			camera.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Translation = glm::vec3(0.0f, 1.6f, 5.5f);
				transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(-12.0f, 0.0f, 0.0f));
			});
			camera.AddComponent<CameraComponent>(CameraComponent{ .Primary = true, .Clear = ClearMode::Color, .ClearColor = glm::vec3(0.1f, 0.12f, 0.16f) });

			Entity sun = scene.CreateEntity("Sun");
			sun.Patch<TransformComponent>([](TransformComponent& transform)
			{
				transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(glm::vec3(-50.0f, -30.0f, 0.0f));
			});
			sun.AddComponent<DirectionalLightComponent>(DirectionalLightComponent{ .Intensity = 1.0f, .CastShadows = false });

			const auto addMesh = [&scene](std::string_view name, AssetHandle mesh, const glm::vec3& translation, const glm::vec3& euler,
									 const glm::vec3& scale, AssetHandle material)
			{
				Entity entity = scene.CreateEntity(name);
				entity.Patch<TransformComponent>([&translation, &euler, &scale](TransformComponent& transform)
				{
					transform.Translation = translation;
					transform.Rotation = TransformSystem::QuaternionFromEulerDegrees(euler);
					transform.Scale = scale;
				});
				MeshRendererComponent& renderer = entity.AddComponent<MeshRendererComponent>();
				renderer.Mesh = TypedAssetHandle<AssetType::Mesh>(mesh);
				if (material.IsValid())
					renderer.Materials = { TypedAssetHandle<AssetType::Material>(material) };
			};
			addMesh("Ground", BuiltinAssetHandles::PlaneMesh, glm::vec3(0.0f, -0.5f, 0.0f), glm::vec3(0.0f), glm::vec3(8.0f, 1.0f, 8.0f), AssetHandle());
			addMesh("Cube", BuiltinAssetHandles::CubeMesh, glm::vec3(-0.9f, 0.0f, 0.0f), glm::vec3(0.0f, 30.0f, 0.0f), glm::vec3(1.0f), AssetHandle());
			addMesh("Sphere", BuiltinAssetHandles::SphereMesh, glm::vec3(1.0f, 0.0f, 0.5f), glm::vec3(0.0f), glm::vec3(1.2f),
				BuiltinAssetHandles::ErrorMaterial);
			// M8 golden inputs disable shadow casting and SSAO (Architecture §15.4).
			scene.CreateEntity("PostProcess").AddComponent<PostProcessComponent>(PostProcessComponent{ .SsaoEnabled = false });
			TransformSystem::Update(scene);
		}

	}

	TEST_SUITE(Test::GoldenSuite)
	{
		TEST_CASE("Golden: Triangle")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				// The triangle view drawn by its pass into an RGBA8 target without depth, with back-face culling.
				TrianglePassSpecification specification;
				specification.Framebuffer.addColorFormat(nvrhi::Format::RGBA8_UNORM);
				specification.CullMode = nvrhi::RasterCullMode::Back;
				Result<Scope<TrianglePass>> pass = TrianglePass::Create(device, gpu.GetPipelines(), specification);
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = 640, .Height = 360, .DebugName = "GoldenTriangle" });
				REQUIRE_MESSAGE(target.has_value(), target.error().ToString());

				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
				(*commandList)->open();
				(*pass)->Render(**commandList, *target->GetFramebuffer());
				(*commandList)->close();
				device.ExecuteCommandList(**commandList);

				Readback readback(device);
				const Result<Image> image = readback.ReadTexture(*target->GetColorTexture());
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				ENGINE_CHECK_GOLDEN("Triangle", *image, device.GetInfo().DeviceClass);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("Golden: LitScene")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Test::AssetTestFixture assets;
			Test::SceneTestFixture scene;
			BuildLitScene(scene.GetScene());
			{
				GpuResourceCache cache(device, assets.GetManager());
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<ViewportCapture>> capture = ViewportCapture::CreateForScenes(device, **pipelines, cache, assets.GetManager());
				REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());

				// The game view at the golden-image size, as viewport.screenshot {view: "game"} renders an edit scene.
				const Result<RenderSnapshot> snapshot = ExtractRenderSnapshot(scene.GetScene(),
					{ .Width = DefaultViewportScreenshotWidth, .Height = DefaultViewportScreenshotHeight });
				REQUIRE_MESSAGE(snapshot.has_value(), snapshot.error().ToString());
				REQUIRE(snapshot->HasCamera);
				REQUIRE(snapshot->Meshes.size() == 3);
				const Result<Image> image = (*capture)->Capture({}, *snapshot);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				ENGINE_CHECK_GOLDEN("LitScene", *image, device.GetInfo().DeviceClass);
			}
			device.RunGarbageCollection();
		}

		// M8 (Roadmap M8, §15.4): the scenes Projects/FeatureTest/Scaffold/Golden.jsonl wrote into
		// Projects/FeatureTest/Assets/Scenes/Golden, rendered by Test::RenderGoldenScene. Their images are committed for the
		// device class nvidia-61x (Docs/Decisions/0013-m8-decisions.md decision 24, with LitScene's regenerated for the PBR
		// renderer, AgX, bloom and FXAA); every other device class checks them in smoke mode and the golden stage warns
		// "goldens missing" (GoldenImage.h).

		// Renders golden scene `name` with `options` and compares it with the golden of the same name.
		static void CheckGoldenScene(std::string_view name, const Test::GoldenSceneOptions& options = {})
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const Result<Image> image = Test::RenderGoldenScene(gpu, name, options);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			CHECK(image->Width == options.Width);
			CHECK(image->Height == options.Height);
			ENGINE_CHECK_GOLDEN(std::string(name), *image, gpu.GetDevice().GetInfo().DeviceClass);
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("Golden: MaterialGrid")
		{
			// §15.4: a PBR roughness x metallic sphere grid under the Studio HDRI.
			CheckGoldenScene("MaterialGrid");
		}

		TEST_CASE("Golden: IblOnly")
		{
			// Image-based lighting alone (no light components): the Sky HDRI's SH9 diffuse and prefiltered specular on a few
			// shapes, with the skybox visible.
			CheckGoldenScene("IblOnly");
		}

		TEST_CASE("Golden: Tonemappers")
		{
			// Each tonemapper (AgX, ACES, PbrNeutral, Linear) on one HDR scene, as a 2 x 2 grid of 320 x 180 renders.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			std::vector<Image> images;
			for (const RenderTonemapper tonemapper : { RenderTonemapper::AgX, RenderTonemapper::Aces, RenderTonemapper::PbrNeutral, RenderTonemapper::Linear })
			{
				const Test::GoldenSceneOptions options{ .Width = 320, .Height = 180, .EditSnapshot = [tonemapper](RenderSnapshot& snapshot)
				{
					snapshot.Post.Tonemap = tonemapper;
				} };
				Result<Image> image = Test::RenderGoldenScene(gpu, "Tonemappers", options);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				images.push_back(std::move(*image));
			}
			const Result<Image> grid = Test::ComposeImageGrid(images, 2);
			REQUIRE_MESSAGE(grid.has_value(), grid.error().ToString());
			ENGINE_CHECK_GOLDEN("Tonemappers", *grid, gpu.GetDevice().GetInfo().DeviceClass);
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("Golden: Bloom")
		{
			// Bright emissive shapes against a dark background with BloomEnabled and a high BloomIntensity.
			CheckGoldenScene("Bloom");
		}

		TEST_CASE("Golden: AlphaModes")
		{
			// Opaque; Mask (the cut-out pattern of Tests/Data's Rgba.png alpha) on a quad seen from the front, a double-sided
			// one seen from the back and a single-sided one seen from the back (culled); Blend (overlapping translucent quads
			// sorted back to front, and a translucent double-sided sphere); lit by a point light and a spot light.
			CheckGoldenScene("AlphaModes");
		}

		TEST_CASE("Golden: DebugDraw")
		{
			// Every primitive of the DebugDrawList, depth-tested and on top, over a simple scene (scripts emit them from M13; the
			// golden appends them to the extracted snapshot).
			CheckGoldenScene("DebugDraw", { .EditSnapshot = [](RenderSnapshot& snapshot)
			{
				DebugDrawList& list = snapshot.DebugDraw;
				list.AddLine(glm::vec3(-2.0f, 0.0f, 0.0f), glm::vec3(2.0f, 0.0f, 0.0f), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f));
				list.AddRay(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), 1.5f, glm::vec4(0.0f, 1.0f, 0.0f, 1.0f));
				list.AddBox(glm::vec3(-1.5f, 0.5f, 0.0f), glm::vec3(0.5f), glm::quat(glm::vec3(0.0f, 0.5f, 0.0f)), glm::vec4(1.0f, 1.0f, 0.0f, 1.0f));
				list.AddSphere(glm::vec3(1.5f, 0.5f, 0.0f), 0.5f, glm::vec4(0.0f, 1.0f, 1.0f, 1.0f));
				list.AddCapsule(glm::vec3(0.0f, 0.5f, -1.5f), glm::vec3(0.0f, 1.5f, -1.5f), 0.3f, glm::vec4(1.0f, 0.0f, 1.0f, 1.0f));
				list.AddArrow(glm::vec3(-1.0f, 2.0f, 0.0f), glm::vec3(1.0f, 2.0f, 0.0f), 0.2f, glm::vec4(1.0f));
				list.AddFrustum({ glm::vec3(-0.5f, -0.5f, 1.0f), glm::vec3(0.5f, -0.5f, 1.0f), glm::vec3(0.5f, 0.5f, 1.0f), glm::vec3(-0.5f, 0.5f, 1.0f),
									glm::vec3(-1.0f, -1.0f, 2.5f), glm::vec3(1.0f, -1.0f, 2.5f), glm::vec3(1.0f, 1.0f, 2.5f), glm::vec3(-1.0f, 1.0f, 2.5f) },
					glm::vec4(0.5f, 0.5f, 1.0f, 1.0f));
				list.AddLine(glm::vec3(-2.0f, 0.25f, -3.0f), glm::vec3(2.0f, 0.25f, 3.0f), glm::vec4(1.0f, 0.5f, 0.0f, 1.0f), 0.0f, DebugDepthMode::OnTop);
				list.AddText(glm::vec3(0.0f, 2.5f, 0.0f), "Debug", 32.0f, glm::vec4(1.0f));
			} });
		}

		TEST_CASE("Golden: Text")
		{
			// Screen texts at several anchors, pivots, sizes, colours and alignments (multi-line), and world texts, one of them
			// billboarded, in the Default font.
			CheckGoldenScene("Text");
		}

		TEST_CASE("Golden: GltfFixture")
		{
			// The imported glTF fixtures of Tests/Data/Generate/MakeGltfFixtures.py, the textured two-material cube
			// (Textured.gltf, with its external buffer and image) and the normal-mapped quad (NormalMapped.gltf), copied into
			// Projects/FeatureTest/Assets/Models/Golden by the scaffold and instantiated as prefabs, under the Studio HDRI
			// with a blurred skybox (SkyboxBlur) and a sun.
			CheckGoldenScene("GltfFixture");
		}
	}

}
