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
#include "Support/DeviceLossTest.h"
#include "Support/EditorTestFixture.h"
#include "Support/HeadlessGpuFixture.h"

#include <nlohmann/json.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

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

		TEST_CASE("EditorHostThumbnails: a device loss uploads the registered thumbnail again" * doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decisions 32 and 33; it recovers through the shared loss test and
			// the ImGuiRenderer owner, which holds the thumbnail's texture). The host's release removes the thumbnail's ImGui
			// key, which held the texture's last reference, and forgets which image it showed; on the lost device a pump
			// uploads nothing, and the first pump after the recovery uploads the image again.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			struct UiContext
			{
				ImGuiContext* Context = ImGui::CreateContext();
				~UiContext() { ImGui::DestroyContext(Context); }
			} ui;
			ImGui::GetIO().IniFilename = nullptr;
			Test::EditorTestFixture fixture("HostThumbnailDeviceLoss");
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
			CHECK(thumbnails.FindTexture(request) == 0); // the tile asks for the thumbnail, as its panel's Draw does

			// A safe point's pump, then the frame slot coming round, as the editor's frames retire the renderer's textures:
			// whether the tile has a texture, and how many textures the renderer holds.
			Test::DeviceLossTest test;
			test.Owners = { "EditorHostThumbnails" };
			test.Use = [&device, &renderer, &thumbnails, &request]() -> Result<std::vector<std::byte>>
			{
				ENGINE_TRY(thumbnails.Pump());
				device.WaitForIdle();
				(*renderer)->BeginFrame(0);
				const uint64_t key = thumbnails.FindTexture(request);
				return std::vector<std::byte>{ std::byte{ key != 0 ? uint8_t{ 1 } : uint8_t{ 0 } }, static_cast<std::byte>((*renderer)->GetTextureCount()) };
			};
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), (report.has_value() ? std::string() : report.error().ToString()));
			CHECK(report->Before == std::vector<std::byte>{ std::byte{ 1 }, std::byte{ 1 } });
			thumbnails.Reset();
			device.WaitForIdle();
			(*renderer)->BeginFrame(0);
			(*renderer)->DestroyTextures();
		}

		TEST_CASE("EditorHostThumbnails: a capture that failed on the lost device is rendered again after the recovery"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 33): the host's release marks the cache's results stale
			// (ThumbnailCache::Revalidate), so a mesh preview whose capture failed with the loss is not remembered as a failure,
			// a vector icon for the rest of the session, but captured again once the device is back.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			struct UiContext
			{
				ImGuiContext* Context = ImGui::CreateContext();
				~UiContext() { ImGui::DestroyContext(Context); }
			} ui;
			ImGui::GetIO().IniFilename = nullptr;
			Test::EditorTestFixture fixture("HostThumbnailCaptureLoss");
			fixture.CreateAndOpenProject();
			auto& editor = fixture.GetEditor();
			auto& device = gpu.GetDevice();
			auto renderer = ImGuiRenderer::Create(device, gpu.GetPipelines(), {});
			REQUIRE(renderer.has_value());
			// The capture an editor's offscreen view would make: on the lost device it fails with the loss, as every capture
			// does (decision 28), and otherwise it returns an image of the requested size.
			GraphicsDevice* captureDevice = &device;
			uint32_t captures = 0;
			EditorHostThumbnails thumbnails(editor, [captureDevice, &captures](const RenderSnapshot& /*snapshot*/, uint32_t size) -> Result<Image>
			{
				++captures;
				if (captureDevice->IsLost())
					return MakeError(ErrorCode::Gpu, "cannot capture the thumbnail: the device is lost");
				return CreateImage(size, size, nvrhi::Format::RGBA8_UNORM);
			});
			struct WaitForThumbnailGpu
			{
				GraphicsDevice& Device;
				~WaitForThumbnailGpu() { Device.WaitForIdle(); }
			} idle{ device };
			thumbnails.SetGraphics(device, **renderer);
			REQUIRE(thumbnails.GetCache().BindProject(1).has_value());
			const AssetHandle mesh = BuiltinAssetHandles::CubeMesh;
			REQUIRE(editor.GetAssets().Load(mesh).has_value());
			const ThumbnailRequest request{ 1, mesh, editor.GetAssets().GetVersion(mesh), 32 };

			device.GetDiagnostics().SetDeviceLost(true);
			device.ReportDeviceLost("EditorHostThumbnailsTests");
			const Result<ThumbnailResult> failed = thumbnails.GetCache().Request(request);
			REQUIRE_FALSE(failed.has_value());
			CHECK(failed.error().GetCode() == ErrorCode::Gpu);
			CHECK(captures == 1);
			// Remembered while its inputs are unchanged: asking again captures nothing.
			CHECK_FALSE(thumbnails.GetCache().Request(request).has_value());
			CHECK(captures == 1);

			const Status recovered = device.Recover(nullptr);
			REQUIRE_MESSAGE(recovered.has_value(), (recovered.has_value() ? std::string() : recovered.error().ToString()));
			// No longer a remembered failure: the tile queues it again (Find answers nullopt), and the request captures it.
			const Result<std::optional<ThumbnailResult>> found = thumbnails.GetCache().Find(request);
			REQUIRE(found.has_value());
			CHECK_FALSE(found->has_value());
			const Result<ThumbnailResult> rendered = thumbnails.GetCache().Request(request);
			REQUIRE_MESSAGE(rendered.has_value(), (rendered.has_value() ? std::string() : rendered.error().ToString()));
			CHECK(captures == 2);
			CHECK_FALSE(rendered->TypeIcon);
			thumbnails.Reset();
			device.WaitForIdle();
			(*renderer)->BeginFrame(0);
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
