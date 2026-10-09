#include "TestsPCH.h"

#include "Editor/Private/EditorHostThumbnails.h"
#include "EditorCore/EditorContext.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/ImGui/ImGuiRenderer.h"
#include "Support/EditorTestFixture.h"
#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorHostThumbnails: uploaded previews retire after reload and project reset" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			struct UiContext
			{
				ImGuiContext* Context = ImGui::CreateContext();
				~UiContext() { ImGui::DestroyContext(Context); }
			} ui;
			ImGui::GetIO().IniFilename = nullptr;
			Test::EditorTestFixture fixture("HostThumbnailUpload");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			auto& device = gpu.GetDevice();
			auto renderer = ImGuiRenderer::Create(device, gpu.GetPipelines(), {});
			REQUIRE(renderer.has_value());
			EditorHostThumbnails thumbnails(editor, {});
			struct WaitForThumbnailGpu
			{
				GraphicsDevice& Device;
				~WaitForThumbnailGpu() { Device.WaitForIdle(); }
			} idle{ device };
			thumbnails.SetGraphics(device, **renderer);
			REQUIRE(thumbnails.GetCache().BindProject(1).has_value());
			const auto file = editor.GetProject().GetRoot() / "Assets/Preview.png";
			auto image = CreateImage(16, 16, nvrhi::Format::RGBA8_UNORM);
			REQUIRE(image.has_value());
			REQUIRE(WritePng(file, *image).has_value());
			REQUIRE(editor.GetAssets().Refresh().has_value());
			const auto asset = editor.GetAssets().Resolve("Assets/Preview.png");
			REQUIRE(asset.has_value());
			REQUIRE(editor.GetAssets().Load(*asset).has_value());
			const ThumbnailRequest request{ 1, *asset, editor.GetAssets().GetVersion(*asset), 32 };
			REQUIRE(thumbnails.GetCache().Request(request).has_value());
			CHECK(thumbnails.FindTexture(request) == 0);
			REQUIRE(thumbnails.Pump().has_value());
			const uint64_t firstTexture = thumbnails.FindTexture(request);
			CHECK(firstTexture != 0);
			CHECK((*renderer)->GetTextureCount() == 1);

			image->Pixels[0] = std::byte{ 255 };
			REQUIRE(WritePng(file, *image).has_value());
			REQUIRE(editor.GetAssets().Refresh().has_value());
			REQUIRE(editor.GetAssets().GetVersion(*asset) != request.Version);
			CHECK(thumbnails.FindTexture(request) == 0);
			REQUIRE(thumbnails.Pump().has_value()); // stale entries retire without a logged error
			device.WaitForIdle();
			(*renderer)->BeginFrame(0);
			CHECK((*renderer)->GetTextureCount() == 0);

			const ThumbnailRequest replacement{ 1, *asset, editor.GetAssets().GetVersion(*asset), 32 };
			REQUIRE(thumbnails.GetCache().Request(replacement).has_value());
			CHECK(thumbnails.FindTexture(replacement) == 0);
			REQUIRE(thumbnails.Pump().has_value());
			const uint64_t secondTexture = thumbnails.FindTexture(replacement);
			CHECK(secondTexture != 0);
			CHECK(secondTexture != firstTexture);
			thumbnails.Reset();
			CHECK(thumbnails.FindTexture(replacement) == 0);
			device.WaitForIdle();
			(*renderer)->BeginFrame(0);
			CHECK((*renderer)->GetTextureCount() == 0);
			(*renderer)->DestroyTextures();
		}

		TEST_CASE("EditorHostThumbnails: decoded texture pixels produce a real square preview without a GPU")
		{
			Test::EditorTestFixture fixture("HostThumbnail");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			REQUIRE(editor.GetAssets().Load(BuiltinAssetHandles::CheckerTexture).has_value());
			EditorHostThumbnails thumbnails(editor, {});
			REQUIRE(thumbnails.GetCache().BindProject(1).has_value());
			const ThumbnailRequest request{ 1, BuiltinAssetHandles::CheckerTexture, editor.GetAssets().GetVersion(BuiltinAssetHandles::CheckerTexture), 32 };
			const auto preview = thumbnails.GetCache().Request(request);
			REQUIRE(preview.has_value());
			CHECK_FALSE(preview->TypeIcon);
			const auto image = ReadPng(FileSystem::PathFromUtf8(preview->Path));
			REQUIRE(image.has_value());
			CHECK(image->Width == 32);
			CHECK(image->Height == 32);
			CHECK(image->Pixels[0] == std::byte(192));
			CHECK(image->Pixels[4 * 4] == std::byte(128));
			CHECK(image->Pixels[4 * 32 * 4] == std::byte(128));
			CHECK(image->Pixels[3] == std::byte(255));
			CHECK(thumbnails.FindTexture(request) == 0);
			thumbnails.Reset();
			CHECK_FALSE(thumbnails.GetCache().Find(request).has_value());
		}
		TEST_CASE("EditorHostThumbnails: material previews capture the referenced material on a fitted sphere")
		{
			Test::EditorTestFixture fixture("HostMaterialThumbnail");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			REQUIRE(editor.GetAssets().Load(BuiltinAssetHandles::DefaultMaterial).has_value());
			uint32_t calls = 0;
			EditorHostThumbnails thumbnails(editor, [&calls](const RenderSnapshot& snapshot, uint32_t size) -> Result<Image>
			{
				++calls;
				CHECK(snapshot.HasCamera);
				REQUIRE(snapshot.Meshes.size() == 1);
				CHECK(snapshot.Meshes[0].Mesh == BuiltinAssetHandles::SphereMesh);
				REQUIRE(snapshot.Meshes[0].Materials.size() == 1);
				CHECK(snapshot.Meshes[0].Materials[0] == BuiltinAssetHandles::DefaultMaterial);
				CHECK(snapshot.Camera.ViewportWidth == size);
				REQUIRE(snapshot.Lights.size() == 1);
				CHECK(snapshot.Lights.front().Type == RenderLightType::Directional);
				CHECK_FALSE(snapshot.Lights.front().CastShadows);
				return CreateImage(size, size, nvrhi::Format::RGBA8_UNORM);
			});
			REQUIRE(thumbnails.GetCache().BindProject(1).has_value());
			const ThumbnailRequest request{ 1, BuiltinAssetHandles::DefaultMaterial, editor.GetAssets().GetVersion(BuiltinAssetHandles::DefaultMaterial), 32 };
			CHECK(thumbnails.GetCache().Request(request).has_value());
			CHECK(calls == 1);
			CHECK(thumbnails.GetCache().Request(request).has_value());
			CHECK(calls == 1);
			thumbnails.Reset();
			REQUIRE(thumbnails.GetCache().BindProject(2).has_value());
			CHECK_FALSE(thumbnails.GetCache().Request(request).has_value());
			CHECK(calls == 1);
		}
	}

}
