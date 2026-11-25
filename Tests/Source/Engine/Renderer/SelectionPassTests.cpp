#include "TestsPCH.h"
#include "Engine/Renderer/SelectionPass.h"

#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SkinningPass.h"
#include "Shared/InstanceRecord.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		// The most instance records a selection test writes.
		constexpr size_t SelectionTestMaxInstances = 16;

		// M46: the meshes and materials of the levels and wind case, on the integration fixture.
		constexpr AssetHandle SelectionLodQuad{ 0x5e46000000000001ULL };     // level 1, below the screen size 0.9: the lower-right triangle
		constexpr AssetHandle SelectionLodTriangle{ 0x5e46000000000002ULL }; // the same quad's vertices drawing that triangle as level 0
		constexpr AssetHandle SelectionSweptGrid{ 0x5e46000000000003ULL };   // a dense vertical grid with a _WIND stream
		constexpr AssetHandle SelectionRedMaterial{ 0x5e46000000000010ULL };
		constexpr AssetHandle SelectionStormMaterial{ 0x5e46000000000011ULL }; // white, with wind: every term

		struct SelectionTestTargets
		{
			nvrhi::TextureHandle Mask{};
			nvrhi::TextureHandle Scratch{};
			nvrhi::TextureHandle Depth{};
			nvrhi::TextureHandle Color{};
			nvrhi::BufferHandle Instances{}; // the view's Instances buffer (SceneRenderer::State::InstanceBuffer's desc)
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
			nvrhi::BufferDesc instancesDesc;
			instancesDesc.byteSize = SelectionTestMaxInstances * sizeof(InstanceRecord);
			instancesDesc.structStride = sizeof(InstanceRecord);
			instancesDesc.initialState = nvrhi::ResourceStates::ShaderResource;
			instancesDesc.keepInitialState = true;
			auto instances = device.CreateBuffer(instancesDesc);
			REQUIRE(instances.has_value());
			REQUIRE(mask.has_value());
			REQUIRE(scratch.has_value());
			REQUIRE(color.has_value());
			REQUIRE(depth.has_value());
			result.Mask = *mask;
			result.Scratch = *scratch;
			result.Color = *color;
			result.Depth = *depth;
			result.Instances = *instances;
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

		// Prepares the snapshot's selection draws (their instance records written into the view's Instances buffer, as
		// RecordViewSetup writes them) and records the pass; `instancing` false is RenderQualitySettings::Instancing's
		// reference path.
		static Image RenderSelectionTest(GraphicsDevice& device, SelectionPass& pass, GpuResourceCache& cache, SelectionTestTargets& targets,
			const RenderSnapshot& snapshot, float sceneDepth, RenderStats& stats, uint32_t radius = 2, bool instancing = true)
		{
			std::vector<InstanceRecord> instances;
			const SelectionDrawSet draws = SelectionPass::PrepareDraws(cache, snapshot, instancing, instances);
			REQUIRE(instances.size() <= SelectionTestMaxInstances);
			auto commands = device.CreateCommandList();
			REQUIRE(commands.has_value());
			(*commands)->open();
			if (!instances.empty())
				(*commands)->writeBuffer(targets.Instances, instances.data(), instances.size() * sizeof(InstanceRecord));
			(*commands)->clearDepthStencilTexture(targets.Depth, nvrhi::AllSubresources, true, sceneDepth, false, 0);
			(*commands)->clearTextureFloat(targets.Color, nvrhi::AllSubresources, nvrhi::Color(0.0f));
			Status recorded;
			{
				RenderRecordingContext recording(stats, []
				{
					return 0.0;
				});
				recorded = pass.Record(**commands, recording, targets.Bindings,
					{ .Snapshot = &snapshot,
						.Draws = &draws,
						.Instances = targets.Instances,
						.SceneDepth = targets.Depth,
						.Mask = targets.Mask,
						.Scratch = targets.Scratch,
						.LdrColor = targets.Color,
						.Radius = radius });
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

		// M46: the integration fixture's quad (positions (+-1, +-1, 0), facing +Z) drawing `indices`, with a further level of
		// the index-only triangles `level` below the screen size 0.9 when `level` is not empty.
		static Ref<MeshData> MakeSelectionLodQuad(std::vector<uint32_t> indices, std::vector<uint32_t> level)
		{
			auto mesh = CreateRef<MeshData>();
			mesh->Vertices = {
				{ .Position = { -1, -1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 0, 1 } },
				{ .Position = { 1, -1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 1, 1 } },
				{ .Position = { 1, 1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 1, 0 } },
				{ .Position = { -1, 1, 0 }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { 0, 0 } },
			};
			mesh->Indices = std::move(indices);
			mesh->Bounds = { { -1, -1, 0 }, { 1, 1, 0 } };
			Aabb bounds;
			for (const uint32_t index : mesh->Indices)
				bounds.Extend(mesh->Vertices[index].Position);
			mesh->Submeshes = { { .IndexCount = static_cast<uint32_t>(mesh->Indices.size()), .Bounds = bounds } };
			mesh->Slots = { {} };
			if (!level.empty())
			{
				const auto count = static_cast<uint32_t>(level.size());
				mesh->LodIndices = std::move(level);
				mesh->Lods = { { .ScreenSize = 0.9f, .Submeshes = { { .IndexOffset = 0, .IndexCount = count } } } };
			}
			const Status valid = ValidateMeshData(*mesh);
			REQUIRE_MESSAGE(valid.has_value(), valid.error().ToString());
			return mesh;
		}

		// M46: a vertical grid of 8 x 32 quads in its entity's XY plane, x in [-0.5, 0.5] and y in [0, 2], facing +Z, with a
		// _WIND stream whose branch weight rises across the grid, whose phases vary per vertex, whose flutter is 200 / 255 and
		// whose bottom row's bend mask holds it still (SceneRendererLodWindTests' swept grid).
		static Ref<MeshData> MakeSelectionSweptGrid()
		{
			constexpr uint32_t Columns = 8;
			constexpr uint32_t Rows = 32;
			auto mesh = CreateRef<MeshData>();
			for (uint32_t row = 0; row <= Rows; ++row)
			{
				for (uint32_t column = 0; column <= Columns; ++column)
				{
					const float u = static_cast<float>(column) / static_cast<float>(Columns);
					const float v = static_cast<float>(row) / static_cast<float>(Rows);
					mesh->Vertices.push_back({ .Position = { u - 0.5f, 2.0f * v, 0.0f }, .Normal = { 0, 0, 1 }, .Tangent = { 1, 0, 0, 1 }, .TexCoord = { u, 1.0f - v } });
					mesh->Wind.push_back({ .Branch = static_cast<uint8_t>(255U * column / Columns),
						.Phase = static_cast<uint8_t>((37U * (row * (Columns + 1) + column)) % 256U),
						.Flutter = 200,
						.BendMask = static_cast<uint8_t>(row == 0 ? 255 : 0) });
				}
			}
			for (uint32_t row = 0; row < Rows; ++row)
			{
				for (uint32_t column = 0; column < Columns; ++column)
				{
					const uint32_t a = row * (Columns + 1) + column;
					const uint32_t d = a + Columns + 1;
					mesh->Indices.insert(mesh->Indices.end(), { a, a + 1, d + 1, a, d + 1, d });
				}
			}
			mesh->Bounds = { { -0.5f, 0.0f, 0.0f }, { 0.5f, 2.0f, 0.0f } };
			mesh->Submeshes = { { .IndexCount = static_cast<uint32_t>(mesh->Indices.size()), .Bounds = mesh->Bounds } };
			mesh->Slots = { {} };
			const Status valid = ValidateMeshData(*mesh);
			REQUIRE_MESSAGE(valid.has_value(), valid.error().ToString());
			return mesh;
		}

		static void PublishSelectionLodWindAssets(Test::SceneRendererIntegrationFixture& fixture)
		{
			Test::InMemoryAssetManager& assets = fixture.Assets();
			assets.Publish(SelectionLodQuad, MakeSelectionLodQuad({ 0, 1, 2, 0, 2, 3 }, { 0, 1, 2 }));
			assets.Publish(SelectionLodTriangle, MakeSelectionLodQuad({ 0, 1, 2 }, {}));
			assets.Publish(SelectionSweptGrid, MakeSelectionSweptGrid());
			auto red = CreateRef<MaterialData>();
			red->BaseColor = glm::vec4(1.0f, 0.0f, 0.0f, 1.0f);
			assets.Publish(SelectionRedMaterial, red);
			auto storm = CreateRef<MaterialData>();
			storm->Wind = { .Enabled = true, .Bend = 0.25f, .Branch = 0.4f, .Flutter = 0.2f, .Stiffness = 0.1f };
			assets.Publish(SelectionStormMaterial, storm);
		}

		// Whether pixel (x, y) of a rendered view is the outline's solid colour: the default selection colour (1, 0.55, 0.1)
		// encoded, (255, 196, 89), which SelectionFilter.slang's composite writes at alpha 1 whatever lies behind it.
		static bool IsSelectionSolidOutline(const Image& image, uint32_t x, uint32_t y)
		{
			constexpr std::array<int, 3> Orange = { 255, 196, 89 };
			for (size_t channel = 0; channel < Orange.size(); ++channel)
			{
				if (std::abs(Test::SceneRendererIntegrationFixture::Channel(image, x, y, channel) - Orange[channel]) > 1)
					return false;
			}
			return true;
		}

		static bool SelectionPixelsDiffer(const Image& image, const Image& other, uint32_t x, uint32_t y)
		{
			for (size_t channel = 0; channel < 3; ++channel)
			{
				if (Test::SceneRendererIntegrationFixture::Channel(image, x, y, channel) != Test::SceneRendererIntegrationFixture::Channel(other, x, y, channel))
					return true;
			}
			return false;
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
						const auto visible = Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, targets, snapshot, 0.0f, visibleStats);
						const auto hidden = Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, targets, snapshot, 1.0f, hiddenStats);
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
				const auto image = Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, targets, snapshot, 0.0f, stats);
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
				const auto image = Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, targets, snapshot, 0.0f, stats);
				CHECK(Utils::SelectionTestRed(image, 6, 16) == (shifted ? 255 : 0));
				CHECK(Utils::SelectionTestRed(image, 25, 16) == (shifted ? 0 : 255));
			}
			assets.Publish(UUID(902), CreateRef<MaterialData>());
			for (uint32_t radius : { 1u, 8u })
			{
				RenderStats stats;
				const auto image = Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, targets, snapshot, 0.0f, stats, radius);
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
				// M23 (decision 2): the snapshot has a camera and a selection, so its prepared draws and the view's instance
				// buffer are required too (cases 4 and 5, each with valid targets and radius).
				REQUIRE(snapshot.HasCamera);
				REQUIRE_FALSE(snapshot.SelectedEntities.empty());
				std::vector<InstanceRecord> records;
				const SelectionDrawSet draws = SelectionPass::PrepareDraws(cache, snapshot, true, records);
				for (int invalid = 0; invalid < 6; ++invalid)
				{
					SelectionRenderInputs inputs{ .Snapshot = &snapshot, .Draws = &draws, .Instances = targets.Instances, .SceneDepth = targets.Depth, .Mask = targets.Mask, .Scratch = targets.Scratch, .LdrColor = targets.Color };
					if (invalid == 0)
						inputs.Radius = 0;
					if (invalid == 1)
						inputs.Radius = 9;
					if (invalid == 2)
						inputs.Scratch = inputs.Mask;
					if (invalid == 3)
						inputs.SceneDepth = inputs.Mask;
					if (invalid == 4)
						inputs.Draws = nullptr;
					if (invalid == 5)
						inputs.Instances = nullptr;
					CAPTURE(invalid);
					const auto refused = (*pass)->Record(**commands, recording, targets.Bindings, inputs);
					CHECK_FALSE(refused.has_value());
					if (!refused)
						CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
				}
			}
			(*commands)->close();
			gpu.GetDevice().ExecuteCommandList(**commands);
			CHECK(stats.Passes.empty());
		}

		// M23 (C-MESH2; Docs/Decisions/0034-m23-contract.md decisions 9 and 10): the mask's alpha test samples the base colour
		// map with the base colour's UV set and multiplies the vertex colour's alpha, as the forward passes do. The quad's
		// TexCoord1 mirrors its TexCoord, so the texture of the UV transform case (its left texel transparent) keeps the right
		// half on UV0 and the left half on UV1, and a vertex alpha below the cutoff keeps neither.
		TEST_CASE("Selection: a Mask material's silhouette reads the base colour's UV set and multiplies the vertex alpha")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			GpuResourceCache cache(gpu.GetDevice(), assets);
			auto pass = SelectionPass::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
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
			const auto publishQuad = [&assets](float alpha)
			{
				auto mesh = CreateRef<MeshData>();
				for (const glm::vec2 corner : { glm::vec2(0.0f, 0.0f), glm::vec2(1.0f, 0.0f), glm::vec2(1.0f, 1.0f), glm::vec2(0.0f, 1.0f) })
				{
					mesh->Vertices.push_back({ .Position = { corner.x - 0.5f, corner.y - 0.5f, -2.0f }, .TexCoord = corner });
					mesh->Attributes.push_back({ .TexCoord1 = { 1.0f - corner.x, corner.y }, .Color = { 1.0f, 1.0f, 1.0f, alpha } });
				}
				mesh->Indices = { 0, 1, 2, 0, 2, 3 };
				mesh->Bounds = { { -0.5f, -0.5f, -2.0f }, { 0.5f, 0.5f, -2.0f } };
				mesh->Submeshes = { { .IndexCount = 6, .Bounds = mesh->Bounds } };
				mesh->Slots = { {} };
				REQUIRE_MESSAGE(ValidateMeshData(*mesh).has_value(), ValidateMeshData(*mesh).error().ToString());
				assets.Publish(UUID(901), mesh);
			};
			struct Case
			{
				TextureCoordinateSet Set = TextureCoordinateSet::UV0;
				float Alpha = 1.0f;
				uint32_t LeftOutline = 0;  // red two pixels outside the quad's left edge
				uint32_t RightOutline = 0; // and outside its right edge
			};
			for (const Case& probe : { Case{ TextureCoordinateSet::UV0, 1.0f, 0, 255 }, Case{ TextureCoordinateSet::UV1, 1.0f, 255, 0 },
					 Case{ TextureCoordinateSet::UV1, 0.25f, 0, 0 } })
			{
				INFO("UV set ", static_cast<int>(probe.Set), ", vertex alpha ", probe.Alpha);
				publishQuad(probe.Alpha);
				auto material = CreateRef<MaterialData>();
				material->AlphaMode = AlphaMode::Mask;
				material->BaseColorMap = TypedAssetHandle<AssetType::Texture>(UUID(903));
				material->BaseColorUVSet = probe.Set;
				assets.Publish(UUID(902), material);
				RenderStats stats;
				const auto image = Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, targets, snapshot, 0.0f, stats);
				CHECK(Utils::SelectionTestRed(image, 6, 16) == probe.LeftOutline);
				CHECK(Utils::SelectionTestRed(image, 25, 16) == probe.RightOutline);
			}
		}

		// M23 (R-INST; Docs/Decisions/0034-m23-contract.md decision 2): two selected items of one mesh and material are one
		// instanced mask draw, and the outline equals the per-draw reference path's.
		TEST_CASE("Selection: selected items of one mesh draw one instanced mask draw with the per-draw outline")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			GpuResourceCache cache(gpu.GetDevice(), assets);
			auto pass = SelectionPass::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
			auto targets = Utils::MakeSelectionTestTargets(gpu.GetDevice(), 32);
			RenderSnapshot snapshot = Utils::SelectionTestSnapshot(assets, RenderProjection::Perspective, false, false, AlphaMode::Opaque, false);
			MeshDrawItem left = snapshot.Meshes[0];
			left.World = glm::translate(glm::mat4(1.0f), glm::vec3(-0.5f, 0.0f, 0.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(0.5f, 0.5f, 1.0f));
			MeshDrawItem right = left;
			right.World[3].x = 0.5f;
			right.Entity = UUID(43);
			snapshot.Meshes = { left, right };
			snapshot.SelectedEntities = { UUID(42), UUID(43) };
			RenderStats instancedStats, perDrawStats;
			const Image instanced = Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, targets, snapshot, 0.0f, instancedStats);
			const Image perDraw = Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, targets, snapshot, 0.0f, perDrawStats, 2, false);
			REQUIRE_FALSE(instancedStats.Passes.empty());
			REQUIRE_FALSE(perDrawStats.Passes.empty());
			CHECK(instancedStats.Passes[0].DrawCalls == 1);
			CHECK(instancedStats.Passes[0].Triangles == 4);
			CHECK(perDrawStats.Passes[0].DrawCalls == 2);
			CHECK(instanced.Pixels == perDraw.Pixels);
			// Both quads are outlined: each one's outer edge, two pixels outside it.
			CHECK(Utils::SelectionTestRed(instanced, 2, 16) == 255);
			CHECK(Utils::SelectionTestRed(instanced, 29, 16) == 255);
		}

		// M23 (C23.3, C-SKEL, C-MORPH; Docs/Decisions/0034-m23-contract.md decision 27, stream C): a selected deformed item's mask
		// draws bind the deformed vertices its source names and key its VertexStream, so two deformed items of one mesh are two
		// mask draws where the same items undeformed are one instanced draw; a skipped item draws no mask.
		TEST_CASE("Selection: a deformed item's mask draws its own vertices in a draw of its own")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			{
				GpuResourceCache cache(gpu.GetDevice(), assets);
				RenderSnapshot snapshot = Utils::SelectionTestSnapshot(assets, RenderProjection::Perspective, false, false, AlphaMode::Opaque, false);
				MeshDrawItem second = snapshot.Meshes[0];
				second.Entity = UUID(43);
				MeshDrawItem third = snapshot.Meshes[0];
				third.Entity = UUID(44);
				snapshot.Meshes = { snapshot.Meshes[0], second, third };
				snapshot.SelectedEntities = { UUID(42), UUID(43), UUID(44) };
				std::vector<InstanceRecord> instances;
				const SelectionDrawSet rigid = SelectionPass::PrepareDraws(cache, snapshot, true, instances);
				CHECK(rigid.Draws.size() == 3);
				CHECK(rigid.Batches.size() == 1);

				Result<nvrhi::BufferHandle> first = gpu.GetDevice().CreateBuffer(SkinningPass::GetVerticesDesc(4));
				Result<nvrhi::BufferHandle> other = gpu.GetDevice().CreateBuffer(SkinningPass::GetVerticesDesc(4));
				REQUIRE(first.has_value());
				REQUIRE(other.has_value());
				const std::array<DeformedVertexSource, 3> sources = { DeformedVertexSource{ .Vertices = *first, .VertexStream = 1 },
					DeformedVertexSource{ .Vertices = *other, .VertexStream = 2 }, DeformedVertexSource{ .Skipped = true } };
				instances.clear();
				const SelectionDrawSet deformed = SelectionPass::PrepareDraws(cache, snapshot, true, instances, sources);
				REQUIRE(deformed.Draws.size() == 2);
				CHECK(deformed.Batches.size() == 2);
				CHECK(instances.size() == 2);
				CHECK(deformed.Draws[0].Vertices == *first);
				CHECK(deformed.Draws[1].Vertices == *other);
				CHECK(deformed.Draws[0].Attributes == cache.GetMesh(snapshot.Meshes[0].Mesh).AttributeBuffer);
			}
			gpu.GetDevice().RunGarbageCollection();
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
			Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, first, snapshot, 0.0f, firstStats);
			snapshot.SelectedEntities = { UUID(999) }; // Deleted or stale selection does not resolve through another index.
			const auto empty = Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, second, snapshot, 0.0f, secondStats);
			CHECK(std::ranges::all_of(empty.Pixels, [](std::byte pixel)
			{
				return pixel == std::byte{ 0 };
			}));
			snapshot.SelectedEntities.clear();
			RenderStats clearedStats;
			Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, first, snapshot, 0.0f, clearedStats);
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
			const auto resized = Utils::RenderSelectionTest(gpu.GetDevice(), **pass, cache, first, snapshot, 0.0f, resizedStats);
			CHECK(Utils::SelectionTestRed(resized, 10, 24) == 255);
			// Fixture checks all targets, binding sets and pipelines are released after submission retirement.
		}

		// M46 (Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decisions 3E and 3G; the W46.2 review, finding 5): the
		// mask draws what the view draws, through the scene renderer.
		//   - The quad at 3 m (screen size 0.816) draws its level 1, its lower-right triangle (screen pixels with x + y > 64),
		//     below the level's switch size 0.9: its mask is that one triangle, so its outline equals the outline of a mesh whose
		//     own level is the triangle, and pixel (30, 30), inside the quad but outside the triangle, is outline; LodBias 2
		//     (1.63) draws level 0, whose mask is the whole quad.
		//   - At 5.1 m beyond a CullDistance of 4 m it draws no mask and no outline; within 10 m it does.
		//   - A selected grid swept by a storm masks the surface the prepass drew, bit for bit: every pixel of the mask is
		//     visible, so every outline pixel is solid (none dim, as an outline of occluded coverage is), and none lies on the
		//     grid's own pixels in the image without the selection.
		TEST_CASE("Selection: a selected item's mask draws its view's level, nothing beyond its CullDistance, and a wind-swept item where the prepass drew it")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu);
			Utils::PublishSelectionLodWindAssets(fixture);
			// The last render's SelectionMask row.
			const auto maskPass = [&fixture]()
			{
				const RenderStats stats = fixture.Renderer().GetRenderStats();
				const RenderPassStats* mask = Test::SceneRendererIntegrationFixture::FindPass(stats, "SelectionMask");
				REQUIRE(mask != nullptr);
				return *mask;
			};

			// The view's level.
			RenderSnapshot snapshot = fixture.Snapshot();
			snapshot.DebugView = RenderDebugView::Albedo;
			snapshot.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Selection;
			snapshot.SelectedEntities = { UUID(900) };
			snapshot.Meshes[0].Mesh = SelectionLodQuad;
			const Image coarse = fixture.Render(snapshot);
			CHECK(fixture.Renderer().GetRenderStats().MeshLodDraws[1] == 1);
			RenderPassStats mask = maskPass();
			CHECK(mask.DrawCalls == 1);
			CHECK(mask.Triangles == 1);
			CHECK(Utils::IsSelectionSolidOutline(coarse, 30, 30));
			snapshot.Meshes[0].Mesh = SelectionLodTriangle;
			++snapshot.FrameIndex;
			CHECK(fixture.Render(snapshot).Pixels == coarse.Pixels);
			snapshot.Meshes[0].Mesh = SelectionLodQuad;
			snapshot.Meshes[0].Distance.LodBias = 2.0f;
			++snapshot.FrameIndex;
			const Image fine = fixture.Render(snapshot);
			CHECK(fixture.Renderer().GetRenderStats().MeshLodDraws[0] == 1);
			mask = maskPass();
			CHECK(mask.DrawCalls == 1);
			CHECK(mask.Triangles == 2);
			CHECK_FALSE(Utils::IsSelectionSolidOutline(fine, 30, 30));

			// Nothing beyond the CullDistance: the SelectionMask row draws nothing and no outline is composited, so the image
			// is the one without a selection.
			snapshot.Meshes[0].World = glm::translate(glm::mat4(1.0f), glm::vec3(-1.0f, 0.0f, -5.0f));
			snapshot.Meshes[0].Distance = { .CullDistance = 4.0f };
			++snapshot.FrameIndex;
			const Image culled = fixture.Render(snapshot);
			RenderStats stats = fixture.Renderer().GetRenderStats();
			CHECK(stats.DistanceCulledMeshes == 1);
			CHECK(maskPass().DrawCalls == 0);
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(stats, "SelectionComposite") == nullptr);
			RenderSnapshot unselected = snapshot;
			unselected.SelectedEntities.clear();
			++unselected.FrameIndex;
			CHECK(fixture.Render(unselected).Pixels == culled.Pixels);
			snapshot.Meshes[0].Distance.CullDistance = 10.0f;
			snapshot.FrameIndex = unselected.FrameIndex + 1;
			CHECK(fixture.Render(snapshot).Pixels != culled.Pixels);
			stats = fixture.Renderer().GetRenderStats();
			CHECK(stats.DistanceCulledMeshes == 0);
			CHECK(maskPass().DrawCalls == 1);
			CHECK(Test::SceneRendererIntegrationFixture::FindPass(stats, "SelectionComposite") != nullptr);

			// A wind-swept item: the grid at 3 m in front of a red wall of 12 x 12 m at z = -8 that fills the view.
			RenderSnapshot swept = fixture.Snapshot();
			swept.DebugView = RenderDebugView::Albedo;
			swept.FrameIndex = snapshot.FrameIndex + 1;
			MeshDrawItem wall = swept.Meshes.front();
			wall.World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, 0.0f, -8.0f)) * glm::scale(glm::mat4(1.0f), glm::vec3(6.0f));
			wall.Materials = { SelectionRedMaterial };
			MeshDrawItem grid = wall;
			grid.World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, -1.2f, -3.0f));
			grid.Mesh = SelectionSweptGrid;
			grid.Materials = { SelectionStormMaterial };
			grid.Entity = UUID(40);
			grid.PickId = 2;
			swept.Meshes = { wall, grid };
			swept.SelectedEntities = { UUID(40) };
			const Image still = fixture.Render(swept);
			for (const double time : { 12.345, 40.5 })
			{
				CAPTURE(time);
				// A storm: gusts, turbulence and a direction off the X axis.
				swept.Wind = { .Enabled = true, .Direction = glm::vec3(1.0f, 0.3f, 0.5f), .Strength = 18.0f, .GustStrength = 9.0f, .GustFrequency = 0.7f, .Turbulence = 1.0f, .Time = time };
				swept.Flags = RenderViewFlags::None;
				++swept.FrameIndex;
				const Image plain = fixture.Render(swept);
				CHECK(plain.Pixels != still.Pixels);
				swept.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Selection;
				++swept.FrameIndex;
				const Image outlined = fixture.Render(swept);
				mask = maskPass();
				CHECK(mask.DrawCalls == 1);
				CHECK(mask.Triangles == 512);
				size_t outline = 0;
				size_t notSolid = 0;
				size_t onGrid = 0;
				for (uint32_t y = 0; y < outlined.Height; ++y)
				{
					for (uint32_t x = 0; x < outlined.Width; ++x)
					{
						if (!Utils::SelectionPixelsDiffer(outlined, plain, x, y))
							continue;
						++outline;
						notSolid += Utils::IsSelectionSolidOutline(outlined, x, y) ? 0U : 1U;
						onGrid += Test::SceneRendererIntegrationFixture::Channel(plain, x, y, 1) > 128 ? 1U : 0U;
					}
				}
				CAPTURE(outline);
				CHECK(outline > 100);
				CHECK(notSolid == 0);
				CHECK(onGrid == 0);
			}
		}
	}

}
