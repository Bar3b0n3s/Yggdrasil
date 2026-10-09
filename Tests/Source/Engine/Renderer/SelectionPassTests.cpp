#include "TestsPCH.h"
#include "Engine/Renderer/SelectionPass.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstddef>

namespace Engine {

	namespace {

		struct SelectionTestTargets
		{
			nvrhi::TextureHandle Mask{};
			nvrhi::TextureHandle Scratch{};
			nvrhi::TextureHandle Depth{};
			nvrhi::TextureHandle Color{};
			PassBindingCache Bindings{};
		};

	}

	namespace Utils {

		static SelectionTestTargets MakeSelectionTestTargets(GraphicsDevice& device, uint32_t size)
		{
			SelectionTestTargets result;
			auto desc = SelectionPass::GetMaskDesc(size, size);
			auto mask = device.CreateTexture(desc);
			auto scratch = device.CreateTexture(desc);
			desc.format = nvrhi::Format::RGBA8_UNORM;
			auto color = device.CreateTexture(desc);
			desc.format = nvrhi::Format::D32;
			desc.isUAV = false;
			desc.initialState = nvrhi::ResourceStates::DepthWrite;
			auto depth = device.CreateTexture(desc);
			REQUIRE(mask.has_value());
			REQUIRE(scratch.has_value());
			REQUIRE(color.has_value());
			REQUIRE(depth.has_value());
			result.Mask = *mask;
			result.Scratch = *scratch;
			result.Color = *color;
			result.Depth = *depth;
			return result;
		}

		static RenderSnapshot SelectionTestSnapshot(Test::InMemoryAssetManager& assets, RenderProjection projection, bool mirrored, bool backwards, AlphaMode alpha, bool doubleSided, float opacity = 1.0f)
		{
			auto mesh = CreateRef<MeshData>();
			mesh->Vertices = {
				{ .Position = { -0.5f, -0.5f, -2.0f }, .TexCoord = { 0.0f, 0.0f } }, { .Position = { 0.5f, -0.5f, -2.0f }, .TexCoord = { 1.0f, 0.0f } },
				{ .Position = { 0.5f, 0.5f, -2.0f }, .TexCoord = { 1.0f, 1.0f } }, { .Position = { -0.5f, 0.5f, -2.0f }, .TexCoord = { 0.0f, 1.0f } }
			};
			mesh->Indices = backwards ? std::vector<uint32_t>{ 0, 2, 1, 0, 3, 2 } : std::vector<uint32_t>{ 0, 1, 2, 0, 2, 3 };
			mesh->Bounds = { { -0.5f, -0.5f, -2.0f }, { 0.5f, 0.5f, -2.0f } };
			mesh->Submeshes = { { .IndexCount = 6, .Bounds = mesh->Bounds } };
			mesh->Slots = { {} };
			const AssetHandle meshHandle(901), materialHandle(902);
			assets.Publish(meshHandle, mesh);
			auto material = CreateRef<MaterialData>();
			material->AlphaMode = alpha;
			material->DoubleSided = doubleSided;
			material->BaseColor.a = opacity;
			assets.Publish(materialHandle, material);
			RenderSnapshot snapshot;
			snapshot.HasCamera = true;
			snapshot.Camera.ProjectionKind = projection;
			snapshot.Camera.OrthographicSize = 1.0f;
			snapshot.Camera.VerticalFov = 53.130102f;
			snapshot.Camera.ViewportWidth = snapshot.Camera.ViewportHeight = 32;
			snapshot.Camera.Projection = ComputeReverseZProjection(projection, snapshot.Camera.VerticalFov, 1.0f, 0.1f, 100.0f, 32, 32);
			snapshot.Meshes = { { .World = glm::scale(glm::mat4(1.0f), glm::vec3(mirrored ? -1.0f : 1.0f, 1.0f, 1.0f)), .Mesh = meshHandle, .Materials = { materialHandle }, .Entity = UUID(42) } };
			snapshot.SelectedEntities = { UUID(42) };
			return snapshot;
		}

		static Image RenderSelectionTest(GraphicsDevice& device, SelectionPass& pass, GpuResourceCache& cache, AssetManager& assets,
			SelectionTestTargets& targets, const RenderSnapshot& snapshot, float sceneDepth, RenderStats& stats, uint32_t radius = 2)
		{
			auto commands = device.CreateCommandList();
			REQUIRE(commands.has_value());
			(*commands)->open();
			(*commands)->clearDepthStencilTexture(targets.Depth, nvrhi::AllSubresources, true, sceneDepth, false, 0);
			(*commands)->clearTextureFloat(targets.Color, nvrhi::AllSubresources, nvrhi::Color(0.0f));
			Status recorded;
			{
				RenderRecordingContext recording(stats, []
				{
					return 0.0;
				});
				recorded = pass.Record(**commands, recording, targets.Bindings, cache, assets,
					{ .Snapshot = &snapshot, .SceneDepth = targets.Depth, .Mask = targets.Mask, .Scratch = targets.Scratch, .LdrColor = targets.Color, .Radius = radius });
			}
			(*commands)->close();
			device.ExecuteCommandList(**commands);
			REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
			Readback readback(device);
			auto image = readback.ReadTexture(*targets.Color);
			REQUIRE(image.has_value());
			return *image;
		}

		static uint32_t SelectionTestRed(const Image& image, uint32_t x, uint32_t y)
		{
			return std::to_integer<uint32_t>(image.Pixels[(static_cast<size_t>(y) * image.Width + x) * 4]);
		}

	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("Selection: visible edges are solid and occluded edges are dim")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			GpuResourceCache cache(gpu.GetDevice(), assets);
			auto pass = SelectionPass::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
			CHECK(SelectionPass::GetLayoutDescriptions().size() == SelectionPass::PipelineCount);
			auto targets = Utils::MakeSelectionTestTargets(gpu.GetDevice(), 32);
			for (RenderProjection projection : { RenderProjection::Perspective, RenderProjection::Orthographic })
			{
				for (AlphaMode alpha : { AlphaMode::Opaque, AlphaMode::Mask, AlphaMode::Blend })
				{
					for (int winding = 0; winding < 3; ++winding)
					{
						CAPTURE(projection);
						CAPTURE(alpha);
						CAPTURE(winding);
						const auto snapshot = Utils::SelectionTestSnapshot(assets, projection, winding == 1, winding == 2, alpha, winding == 2);
						RenderStats visibleStats, hiddenStats;
						const auto visible = Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, assets, targets, snapshot, 0.0f, visibleStats);
						const auto hidden = Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, assets, targets, snapshot, 1.0f, hiddenStats);
						CHECK(Utils::SelectionTestRed(visible, 6, 16) == 255);
						// Half alpha crosses two R8 conversions and the sRGB OETF; allow one UNORM step.
						CHECK(Utils::SelectionTestRed(hidden, 6, 16) >= 127);
						CHECK(Utils::SelectionTestRed(hidden, 6, 16) <= 128);
						CHECK(Utils::SelectionTestRed(visible, 16, 16) == 0); // Silhouette interior is never filled.
						CHECK(Utils::SelectionTestRed(hidden, 16, 16) == 0);
						CHECK(Utils::SelectionTestRed(visible, 5, 16) == 0); // Exactly radius two.
						REQUIRE(visibleStats.Passes.size() == 4);
						const std::array names{ "SelectionMask", "SelectionDilateHorizontal", "SelectionDilateVertical", "SelectionComposite" };
						for (size_t index = 0; index < names.size(); ++index)
						{
							CHECK(visibleStats.Passes[index].Name == names[index]);
							CHECK(visibleStats.Passes[index].Dispatches == (index == 0 ? 0 : 1));
						}
						CHECK(visibleStats.Passes[0].DrawCalls == 1);
						CHECK(visibleStats.Passes[0].Triangles == 2);
					}
				}
			}
			// Mask alpha is sampled, but selected Blend uses geometric coverage even at alpha zero.
			for (AlphaMode alpha : { AlphaMode::Mask, AlphaMode::Blend })
			{
				const auto snapshot = Utils::SelectionTestSnapshot(assets, RenderProjection::Orthographic, false, false, alpha, false, 0.0f);
				RenderStats stats;
				const auto image = Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, assets, targets, snapshot, 0.0f, stats);
				CHECK(Utils::SelectionTestRed(image, 6, 16) == (alpha == AlphaMode::Mask ? 0 : 255));
			}
		}

		TEST_CASE("Selection: texture alpha UV transforms and radius control the silhouette")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			GpuResourceCache cache(gpu.GetDevice(), assets);
			auto pass = SelectionPass::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE(pass.has_value());
			auto targets = Utils::MakeSelectionTestTargets(gpu.GetDevice(), 32);
			const auto snapshot = Utils::SelectionTestSnapshot(assets, RenderProjection::Orthographic, false, false, AlphaMode::Mask, false);
			auto texture = CreateRef<TextureData>();
			texture->Format = TextureFormat::RGBA8Srgb;
			texture->Width = 2;
			texture->Height = 1;
			texture->Mips = { { .Width = 2, .Height = 1, .Size = 8 } };
			texture->Pixels.assign(8, std::byte{ 255 });
			texture->Pixels[3] = std::byte{ 0 };
			assets.Publish(UUID(903), texture);
			for (bool shifted : { false, true })
			{
				auto material = CreateRef<MaterialData>();
				material->AlphaMode = AlphaMode::Mask;
				material->BaseColorMap = TypedAssetHandle<AssetType::Texture>(UUID(903));
				material->UVOffset.x = shifted ? 0.5f : 0.0f;
				assets.Publish(UUID(902), material);
				RenderStats stats;
				const auto image = Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, assets, targets, snapshot, 0.0f, stats);
				CHECK(Utils::SelectionTestRed(image, 6, 16) == (shifted ? 255 : 0));
				CHECK(Utils::SelectionTestRed(image, 25, 16) == (shifted ? 0 : 255));
			}
			assets.Publish(UUID(902), CreateRef<MaterialData>());
			for (uint32_t radius : { 1u, 8u })
			{
				RenderStats stats;
				const auto image = Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, assets, targets, snapshot, 0.0f, stats, radius);
				CHECK(Utils::SelectionTestRed(image, 8 - radius, 16) == 255);
				CHECK(Utils::SelectionTestRed(image, 23 + radius, 16) == 255);
				CHECK(Utils::SelectionTestRed(image, 16, 16) == 0);
				if (radius == 1)
					CHECK(Utils::SelectionTestRed(image, 6, 16) == 0);
			}
			auto commands = gpu.GetDevice().CreateCommandList();
			REQUIRE(commands.has_value());
			(*commands)->open();
			RenderStats stats;
			{
				RenderRecordingContext recording(stats, []
				{
					return 0.0;
				});
				for (int invalid = 0; invalid < 4; ++invalid)
				{
					SelectionRenderInputs inputs{ .Snapshot = &snapshot, .SceneDepth = targets.Depth, .Mask = targets.Mask, .Scratch = targets.Scratch, .LdrColor = targets.Color };
					if (invalid == 0)
						inputs.Radius = 0;
					if (invalid == 1)
						inputs.Radius = 9;
					if (invalid == 2)
						inputs.Scratch = inputs.Mask;
					if (invalid == 3)
						inputs.SceneDepth = inputs.Mask;
					const auto refused = (*pass)->Record(**commands, recording, targets.Bindings, cache, assets, inputs);
					CHECK_FALSE(refused.has_value());
					if (!refused)
						CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
				}
			}
			(*commands)->close();
			gpu.GetDevice().ExecuteCommandList(**commands);
			CHECK(stats.Passes.empty());
		}

		TEST_CASE("Selection: deselection clears stale mask and resize releases view targets")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			GpuResourceCache cache(gpu.GetDevice(), assets);
			auto pass = SelectionPass::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
			auto first = Utils::MakeSelectionTestTargets(gpu.GetDevice(), 32);
			auto second = Utils::MakeSelectionTestTargets(gpu.GetDevice(), 16);
			auto snapshot = Utils::SelectionTestSnapshot(assets, RenderProjection::Orthographic, false, false, AlphaMode::Opaque, false);
			RenderStats firstStats, secondStats;
			Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, assets, first, snapshot, 0.0f, firstStats);
			snapshot.SelectedEntities = { UUID(999) }; // Deleted or stale selection does not resolve through another index.
			const auto empty = Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, assets, second, snapshot, 0.0f, secondStats);
			CHECK(std::ranges::all_of(empty.Pixels, [](std::byte pixel)
			{
				return pixel == std::byte{ 0 };
			}));
			snapshot.SelectedEntities.clear();
			RenderStats clearedStats;
			Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, assets, first, snapshot, 0.0f, clearedStats);
			REQUIRE(clearedStats.Passes.size() == 1);
			CHECK(clearedStats.Passes[0].DrawCalls == 0);
			Readback readback(gpu.GetDevice());
			const auto mask = readback.ReadTexture(*first.Mask);
			REQUIRE(mask.has_value());
			CHECK(std::ranges::all_of(mask->Pixels, [](std::byte pixel)
			{
				return pixel == std::byte{ 0 };
			}));
			first.Bindings.Clear();
			first = Utils::MakeSelectionTestTargets(gpu.GetDevice(), 48);
			gpu.GetDevice().RunGarbageCollection();
			snapshot.SelectedEntities = { UUID(42) };
			RenderStats resizedStats;
			const auto resized = Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, assets, first, snapshot, 0.0f, resizedStats);
			CHECK(Utils::SelectionTestRed(resized, 10, 24) == 255);
			// Fixture checks all targets, binding sets and pipelines are released after submission retirement.
		}
	}

}
