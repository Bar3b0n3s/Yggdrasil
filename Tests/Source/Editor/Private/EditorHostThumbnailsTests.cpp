#include "TestsPCH.h"

#include "Editor/Private/EditorHostThumbnails.h"
#include "EditorCore/EditorContext.h"
#include "EditorCore/Project/ProjectManager.h"
#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/AssetPipeline/EditorAssetManager.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Json/JsonReader.h"
#include "Engine/Core/Json/JsonWriter.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/ImGui/ImGuiRenderer.h"
#include "Support/EditorTestFixture.h"
#include "Support/HeadlessGpuFixture.h"

#include <nlohmann/json.hpp>

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorHostThumbnails: a regenerated preview replaces its registration and project reset retires it" * doctest::test_suite(Test::GpuSuite))
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
			image->Pixels[1] = std::byte{ 7 };
			REQUIRE(WritePng(file, *image).has_value());
			REQUIRE(editor.GetAssets().Refresh().has_value());
			REQUIRE(editor.GetAssets().GetVersion(*asset) != request.Version);
			CHECK(thumbnails.FindTexture(request) == 0); // a request for the old version gets nothing
			const ThumbnailRequest replacement{ 1, *asset, editor.GetAssets().GetVersion(*asset), 32 };
			REQUIRE(thumbnails.Pump().has_value()); // the previous image stays registered until its replacement exists
			CHECK(thumbnails.FindTexture(replacement) == firstTexture);
			CHECK((*renderer)->GetTextureCount() == 1);

			REQUIRE(thumbnails.GetCache().Request(replacement).has_value());
			REQUIRE(thumbnails.Pump().has_value());
			const uint64_t secondTexture = thumbnails.FindTexture(replacement);
			CHECK(secondTexture != 0);
			CHECK(secondTexture != firstTexture);
			device.WaitForIdle();
			(*renderer)->BeginFrame(0);
			CHECK((*renderer)->GetTextureCount() == 1);
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

		TEST_CASE("EditorHostThumbnails: previews render with the open project's rendering quality")
		{
			Test::EditorTestFixture fixture("HostThumbnailQuality");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			// Rendering settings other than the defaults, read from the project file when it opens.
			const auto projectFile = editor.GetProject().GetProjectFile();
			const auto text = FileSystem::ReadText(projectFile);
			REQUIRE(text.has_value());
			auto document = JsonReader::Parse(*text);
			REQUIRE(document.has_value());
			(*document)["Rendering"]["ShadowMapSize"] = 256;
			(*document)["Rendering"]["SsaoHalfResolution"] = true;
			const auto written = JsonWriter::Write(*document);
			REQUIRE(written.has_value());
			REQUIRE(FileSystem::WriteFileAtomic(projectFile, AsBytes(*written)).has_value());
			REQUIRE(editor.CloseProject().has_value());
			auto loaded = ProjectManager::OpenProject(projectFile, {}, editor.GetTypeRegistry());
			REQUIRE(loaded.has_value());
			REQUIRE(editor.OpenProject(std::move(*loaded)).has_value());
			REQUIRE(editor.GetProject().GetSettings().Rendering.ShadowMapSize == 256);
			REQUIRE(editor.GetAssets().Load(BuiltinAssetHandles::DefaultMaterial).has_value());
			std::optional<RenderQualitySettings> quality;
			EditorHostThumbnails thumbnails(editor, [&quality](const RenderSnapshot& snapshot, uint32_t size) -> Result<Image>
			{
				quality = snapshot.Quality;
				return CreateImage(size, size, nvrhi::Format::RGBA8_UNORM);
			});
			REQUIRE(thumbnails.GetCache().BindProject(1).has_value());
			const ThumbnailRequest request{ 1, BuiltinAssetHandles::DefaultMaterial, editor.GetAssets().GetVersion(BuiltinAssetHandles::DefaultMaterial), 32 };
			REQUIRE(thumbnails.GetCache().Request(request).has_value());
			REQUIRE(quality.has_value());
			CHECK(quality->ShadowMapSize == 256);
			CHECK(quality->SsaoHalfResolution);
			thumbnails.Reset();
		}
	}

}
