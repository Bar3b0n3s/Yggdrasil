#pragma once

#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"

#include <doctest/doctest.h>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <cstddef>
#include <string_view>

namespace Engine {

	namespace Test {

		// Synthetic snapshots through public APIs, with explicit submission and teardown retirement.
		class SceneRendererIntegrationFixture
		{
		public:
			explicit SceneRendererIntegrationFixture(HeadlessGpuFixture& gpu, uint32_t width = 64, uint32_t height = 64)
				: m_Device(&gpu.GetDevice()), m_Cache(gpu.GetDevice(), m_Assets)
			{
				auto mesh = CreateRef<MeshData>();
				mesh->Vertices = {
					{ .Position = { -1, -1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 0, 1 } },
					{ .Position = { 1, -1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 1, 1 } },
					{ .Position = { 1, 1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 1, 0 } },
					{ .Position = { -1, 1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 0, 0 } },
				};
				mesh->Indices = { 0, 1, 2, 0, 2, 3 };
				mesh->Bounds = { { -1, -1, 0 }, { 1, 1, 0 } };
				mesh->Submeshes = { { .IndexCount = 6, .Bounds = mesh->Bounds } };
				mesh->Slots = { {} };
				m_Assets.Publish(UUID(8910), mesh);
				m_Assets.Publish(UUID(8911), CreateRef<MaterialData>());
				auto pipelines = SceneRendererPipelines::Create(*m_Device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				m_Pipelines = std::move(*pipelines);
				auto renderer = SceneRenderer::Create(*m_Device, *m_Pipelines, m_Cache, m_Assets, { width, height });
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				m_Renderer = std::move(*renderer);
			}

			~SceneRendererIntegrationFixture() { m_Device->WaitForIdle(); }
			SceneRendererIntegrationFixture(const SceneRendererIntegrationFixture&) = delete;
			SceneRendererIntegrationFixture& operator=(const SceneRendererIntegrationFixture&) = delete;

			[[nodiscard]] RenderSnapshot Snapshot(RenderProjection projection = RenderProjection::Perspective) const
			{
				RenderSnapshot snapshot;
				snapshot.HasCamera = true;
				snapshot.Camera.ViewportWidth = m_Renderer->GetWidth();
				snapshot.Camera.ViewportHeight = m_Renderer->GetHeight();
				snapshot.Camera.ProjectionKind = projection;
				snapshot.Camera.OrthographicSize = 2;
				snapshot.Camera.NearClip = 0.1f;
				snapshot.Camera.FarClip = 20;
				snapshot.Camera.Projection = ComputeReverseZProjection(projection, 60, 2, 0.1f, 20, snapshot.Camera.ViewportWidth, snapshot.Camera.ViewportHeight);
				snapshot.Camera.ClearColor = glm::vec3(0);
				snapshot.Camera.ClearToSkybox = false;
				snapshot.Post.BloomEnabled = false;
				snapshot.Post.FxaaEnabled = false;
				snapshot.Post.SsaoEnabled = false;
				snapshot.Post.Tonemap = RenderTonemapper::Linear;
				snapshot.Quality.ShadowMapSize = 256;
				snapshot.FrameIndex = 17;
				snapshot.SceneRevision = 41;
				snapshot.PickTable = { UUID(900), UUID(40), UUID(700) };
				snapshot.Meshes = { { .World = glm::translate(glm::mat4(1), glm::vec3(0, 0, -3)), .Mesh = UUID(8910), .Materials = { UUID(8911) }, .Entity = UUID(900), .PickId = 1 } };
				return snapshot;
			}

			[[nodiscard]] SceneRenderer& Renderer() { return *m_Renderer; }
			[[nodiscard]] InMemoryAssetManager& Assets() { return m_Assets; }
			[[nodiscard]] SceneRendererPipelines& Pipelines() { return *m_Pipelines; }
			[[nodiscard]] GpuResourceCache& Cache() { return m_Cache; }

			void Submit(const RenderSnapshot& snapshot)
			{
				auto list = m_Device->CreateCommandList();
				REQUIRE(list);
				(*list)->open();
				const auto rendered = m_Renderer->Render(**list, snapshot);
				(*list)->close();
				m_Renderer->OnSubmitted(snapshot.FrameIndex, m_Device->ExecuteCommandList(**list));
				REQUIRE_MESSAGE(rendered.has_value(), rendered.error().ToString());
			}

			[[nodiscard]] Image Read()
			{
				Readback readback(*m_Device);
				auto image = readback.ReadTexture(*m_Renderer->GetFinalTexture());
				REQUIRE(image);
				return std::move(*image);
			}

			[[nodiscard]] Image Render(const RenderSnapshot& snapshot)
			{
				Submit(snapshot);
				return Read();
			}

			[[nodiscard]] static int Channel(const Image& image, uint32_t x, uint32_t y, size_t channel = 0)
			{
				return std::to_integer<int>(image.GetRow(y)[4 * static_cast<size_t>(x) + channel]);
			}

			[[nodiscard]] static const RenderPassStats* FindPass(const RenderStats& stats, std::string_view name)
			{
				const auto found = std::find_if(stats.Passes.begin(), stats.Passes.end(), [name](const auto& pass)
				{
					return pass.Name == name;
				});
				return found == stats.Passes.end() ? nullptr : &*found;
			}
		private:
			GraphicsDevice* m_Device = nullptr; // fixture back-reference; device outlives every owned GPU object
			InMemoryAssetManager m_Assets;
			GpuResourceCache m_Cache;
			Scope<SceneRendererPipelines> m_Pipelines;
			Scope<SceneRenderer> m_Renderer;
		};

	}

}
