#include "TestsPCH.h"

#include "Engine/Core/Buffer.h"
#include "Engine/Core/FileSystem.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/ShaderLibrary.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"
#include "Support/TempDirectory.h"

#include <filesystem>
#include <system_error>
#include <utility>

// The editor's shader hot reload, the renderer half of shaders.reload (M18, C18.1, R-SHR; Docs/Decisions/0029-m18-contract.md
// decision 3): SceneRendererPipelines::ReloadShaders over a copy of this build's compiled shaders.

namespace Engine {

	namespace {

		// Renders a camera view of an empty scene (the clear colour through the whole post chain) and reads it back.
		Image RenderView(GraphicsDevice& device, SceneRenderer& renderer, uint64_t frameIndex)
		{
			RenderSnapshot snapshot;
			snapshot.HasCamera = true;
			snapshot.Camera.ViewportWidth = renderer.GetWidth();
			snapshot.Camera.ViewportHeight = renderer.GetHeight();
			snapshot.Camera.Projection =
				ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 1.0f, 0.1f, 100.0f, renderer.GetWidth(), renderer.GetHeight());
			snapshot.Camera.ClearColor = glm::vec3(0.25f, 0.5f, 0.75f);
			snapshot.Camera.ClearToSkybox = false;
			snapshot.Quality.ShadowMapSize = 256;
			snapshot.FrameIndex = frameIndex;
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE_MESSAGE(list.has_value(), list.error().ToString());
			(*list)->open();
			const Status rendered = renderer.Render(**list, snapshot);
			(*list)->close();
			renderer.OnSubmitted(frameIndex, device.ExecuteCommandList(**list));
			REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
			Readback readback(device);
			Result<Image> image = readback.ReadTexture(*renderer.GetFinalTexture());
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			return std::move(*image);
		}

	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("ShaderReload: a broken shader keeps the old pipelines, and a fixed one swaps in without a restart")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			// A copy of this build's compiled shaders, which the case breaks and fixes as a rebuild of the Shaders project would.
			Test::TempDirectory directory("ShaderReload");
			const std::filesystem::path shaders = directory.GetPath() / "Shaders";
			std::error_code error;
			std::filesystem::copy(std::filesystem::path(ENGINE_SHADER_DIRECTORY), shaders, std::filesystem::copy_options::recursive, error);
			REQUIRE_MESSAGE(!error, error.message());
			VirtualFileSystem vfs;
			Result<Scope<NativeDirectoryMount>> mount = NativeDirectoryMount::Create(shaders, MountAccess::ReadOnly);
			REQUIRE_MESSAGE(mount.has_value(), mount.error().ToString());
			REQUIRE(vfs.Mount(ShaderScheme, std::move(*mount)).has_value());
			Result<VfsPath> root = VfsPath::Create(ShaderScheme, "");
			REQUIRE(root.has_value());
			ShaderLibrary library(&device, vfs, *root);
			PipelineFactory factory(device, library);
			Test::InMemoryAssetManager assets;
			{
				GpuResourceCache cache(device, assets);
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(device, factory);
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(device, **pipelines, cache, assets, { .Width = 32, .Height = 32 });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				const Image before = RenderView(device, **renderer, 1);
				const uint64_t generation = (*pipelines)->GetGeneration();

				// A broken build output: the tonemap pass's reflection no longer parses. The reload fails, nothing is swapped.
				const std::filesystem::path reflection = shaders / "Tonemap" / "CSMain.refl.json";
				Result<Buffer> original = FileSystem::ReadFile(reflection);
				REQUIRE_MESSAGE(original.has_value(), original.error().ToString());
				REQUIRE(FileSystem::WriteFileAtomic(reflection, AsBytes("{ \"entryPoints\": ")).has_value());
				const Status broken = (*pipelines)->ReloadShaders();
				REQUIRE_FALSE(broken.has_value());
				CHECK(broken.error().GetCode() == ErrorCode::Parse);
				CHECK((*pipelines)->GetGeneration() == generation);
				CHECK((*pipelines)->GetPipelineCount() == SceneRendererPipelines::StartupPipelineCount);
				CHECK(RenderView(device, **renderer, 2).Pixels == before.Pixels);

				// Fixed: every pipeline is created again from the build output and swapped in, without a restart.
				REQUIRE(FileSystem::WriteFileAtomic(reflection, *original).has_value());
				const Status fixed = (*pipelines)->ReloadShaders();
				REQUIRE_MESSAGE(fixed.has_value(), fixed.error().ToString());
				CHECK((*pipelines)->GetGeneration() == generation + 1);
				CHECK((*pipelines)->GetPipelineCount() == SceneRendererPipelines::StartupPipelineCount);
				CHECK(RenderView(device, **renderer, 3).Pixels == before.Pixels);
				device.WaitForIdle();
			}
			device.RunGarbageCollection();
		}
	}

}
