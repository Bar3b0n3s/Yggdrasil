#include "TestsPCH.h"
#include "Engine/Renderer/EditorOverlay.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/MeshData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/DebugRenderer.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Shared/ViewConstants.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"

#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>

namespace Engine {

	namespace Utils {

		static RenderSnapshot OverlayTestSnapshot(RenderProjection projection)
		{
			RenderSnapshot snapshot;
			snapshot.HasCamera = true;
			snapshot.Camera.Position = { 0.0f, 3.0f, 5.0f };
			snapshot.Camera.View = glm::lookAt(snapshot.Camera.Position, glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			snapshot.Camera.ProjectionKind = projection;
			snapshot.Camera.ViewportWidth = snapshot.Camera.ViewportHeight = 64;
			snapshot.Camera.OrthographicSize = 5.0f;
			snapshot.Camera.FarClip = 100.0f;
			snapshot.Camera.Projection = ComputeReverseZProjection(projection, 60.0f, 5.0f, 0.1f, 100.0f, 64, 64);
			snapshot.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Grid | RenderViewFlags::Icons;
			return snapshot;
		}

		static double OverlayProjectedWidth(const DebugDrawList& list, const CameraData& camera)
		{
			double minimum = 1.0e30, maximum = -1.0e30;
			for (const auto& command : list.GetCommands())
			{
				const auto& line = std::get<DebugLine>(command.Shape);
				for (const glm::vec3 point : { line.From, line.To })
				{
					const glm::vec4 clip = camera.Projection * camera.View * glm::vec4(point, 1.0f);
					const double pixel = clip.x / clip.w * camera.ViewportWidth * 0.5;
					minimum = std::min(minimum, pixel);
					maximum = std::max(maximum, pixel);
				}
			}
			return maximum - minimum;
		}

		static RenderSnapshot WireframeTestSnapshot(Test::InMemoryAssetManager& assets, bool mirrored, bool backwards, bool doubleSided, uint32_t triangles = 1)
		{
			auto mesh = CreateRef<MeshData>();
			mesh->Vertices = { { .Position = { -0.5f, -0.5f, -2.0f } }, { .Position = { 0.5f, -0.5f, -2.0f } }, { .Position = { 0.0f, 0.5f, -2.0f } } };
			for (uint32_t index = 0; index < triangles; ++index)
				mesh->Indices.insert(mesh->Indices.end(), backwards ? std::initializer_list<uint32_t>{ 0, 2, 1 } : std::initializer_list<uint32_t>{ 0, 1, 2 });
			mesh->Bounds = { { -0.5f, -0.5f, -2.0f }, { 0.5f, 0.5f, -2.0f } };
			mesh->Submeshes = { { .IndexCount = triangles * 3, .Bounds = mesh->Bounds } };
			mesh->Slots = { {} };
			assets.Publish(UUID(911), mesh);
			auto material = CreateRef<MaterialData>();
			material->DoubleSided = doubleSided;
			assets.Publish(UUID(912), material);
			auto snapshot = OverlayTestSnapshot(RenderProjection::Perspective);
			snapshot.Camera.Position = glm::vec3(0.0f);
			snapshot.Camera.View = glm::mat4(1.0f);
			snapshot.Flags = RenderViewFlags::Wireframe;
			snapshot.Meshes = { { .World = glm::scale(glm::mat4(1.0f), glm::vec3(mirrored ? -1.0f : 1.0f, 1.0f, 1.0f)), .Mesh = UUID(911), .Materials = { UUID(912) }, .Entity = UUID(42), .PickId = 1 } };
			snapshot.PickTable = { UUID(42) };
			return snapshot;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("EditorOverlay: grid and procedural icons support both camera projections")
		{
			for (const auto projection : { RenderProjection::Perspective, RenderProjection::Orthographic })
			{
				auto snapshot = Utils::OverlayTestSnapshot(projection);
				DebugDrawList grid;
				REQUIRE(AppendEditorOverlay(snapshot, grid).has_value());
				CHECK_FALSE(grid.IsEmpty());
				CHECK(grid.GetDroppedCount() == 0);
				bool axisFound = false, faded = false;
				for (const auto& command : grid.GetCommands())
				{
					const auto& line = std::get<DebugLine>(command.Shape);
					CHECK(line.From.y == 0.0f);
					CHECK(line.To.y == 0.0f);
					CHECK(command.Depth == DebugDepthMode::Tested);
					CHECK(command.Color.a >= 0.0f);
					CHECK(command.Color.a <= 0.8f);
					axisFound |= command.Color.r != command.Color.g;
					faded |= command.Color.a < 0.01f;
				}
				CHECK(axisFound);
				CHECK(faded);
				snapshot.Flags = RenderViewFlags::EditorOverlays | RenderViewFlags::Icons;
				const glm::vec3 forward = -glm::vec3(glm::inverse(snapshot.Camera.View)[2]);
				for (uint32_t kind = 0; kind <= static_cast<uint32_t>(RenderIconKind::AudioListener); ++kind)
				{
					std::array<double, 2> widths{};
					for (size_t distance = 0; distance < widths.size(); ++distance)
					{
						DebugDrawList icons;
						snapshot.Icons = { { .Kind = static_cast<RenderIconKind>(kind), .Position = snapshot.Camera.Position + forward * (distance == 0 ? 2.0f : 20.0f), .Size = 20.0f } };
						REQUIRE(AppendEditorOverlay(snapshot, icons).has_value());
						CHECK_FALSE(icons.IsEmpty());
						widths[distance] = Utils::OverlayProjectedWidth(icons, snapshot.Camera);
					}
					CHECK(widths[0] == doctest::Approx(widths[1]).epsilon(1.0e-4));
					CHECK(widths[0] > 10.0);
					CHECK(widths[0] <= 20.0);
				}
			}
		}

		TEST_CASE("EditorOverlay: no camera or disabled view flags produce no overlay")
		{
			auto snapshot = Utils::OverlayTestSnapshot(RenderProjection::Perspective);
			DebugDrawList output;
			snapshot.HasCamera = false;
			REQUIRE(AppendEditorOverlay(snapshot, output).has_value());
			CHECK(output.IsEmpty());
			snapshot.HasCamera = true;
			snapshot.Flags = RenderViewFlags::Grid | RenderViewFlags::Icons;
			REQUIRE(AppendEditorOverlay(snapshot, output).has_value());
			CHECK(output.IsEmpty());
			snapshot.Flags |= RenderViewFlags::EditorOverlays;
			snapshot.Flags &= ~RenderViewFlags::Grid;
			const glm::vec3 forward = -glm::vec3(glm::inverse(snapshot.Camera.View)[2]);
			snapshot.Icons = { { .Position = snapshot.Camera.Position - forward } };
			REQUIRE(AppendEditorOverlay(snapshot, output).has_value());
			CHECK(output.IsEmpty());
			output.AddLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec4(1.0f));
			const DebugDrawList original = output;
			snapshot.Icons = { { .Position = glm::vec3(0.0f) }, { .Position = glm::vec3(0.0f), .Size = std::numeric_limits<float>::infinity() } };
			CHECK_FALSE(AppendEditorOverlay(snapshot, output).has_value());
			CHECK(output == original);
			snapshot.Icons.clear();
			snapshot.Camera.Projection = glm::mat4(0.0f);
			CHECK_FALSE(AppendEditorOverlay(snapshot, output).has_value());
			CHECK(output == original);
		}

		TEST_CASE("Wireframe: portable triangle edges respect mirrored double-sided and budget rules")
		{
			Test::InMemoryAssetManager assets;
			for (bool mirrored : { false, true })
			{
				for (bool backwards : { false, true })
				{
					for (bool doubleSided : { false, true })
					{
						const auto snapshot = Utils::WireframeTestSnapshot(assets, mirrored, backwards, doubleSided);
						DebugDrawList output;
						REQUIRE(AppendWireframeOverlay(snapshot, assets, output).has_value());
						CHECK(output.GetSize() == (backwards && !doubleSided ? 0 : 3));
						for (const auto& command : output.GetCommands())
						{
							CHECK(command.Color == glm::vec4(1.0f));
							CHECK(command.Depth == DebugDepthMode::Tested);
						}
					}
				}
			}
			auto snapshot = Utils::WireframeTestSnapshot(assets, false, false, false, 22000);
			DebugDrawList budget;
			REQUIRE(AppendWireframeOverlay(snapshot, assets, budget).has_value());
			CHECK(budget.GetSize() == DebugDrawList::MaxCommands);
			CHECK(budget.GetDroppedCount() == 66000 - DebugDrawList::MaxCommands);
			DebugDrawList copy = budget;
			snapshot.Meshes[0].World[0][0] = std::numeric_limits<float>::quiet_NaN();
			CHECK_FALSE(AppendWireframeOverlay(snapshot, assets, budget).has_value());
			CHECK(budget == copy);
			snapshot.DebugView = RenderDebugView::Normals;
			CHECK(AppendWireframeOverlay(snapshot, assets, budget).has_value());
			CHECK(budget == copy);
			snapshot = Utils::WireframeTestSnapshot(assets, false, false, false);
			snapshot.Meshes[0].World = glm::scale(glm::mat4(1.0f), glm::vec3(0.0f));
			DebugDrawList degenerate;
			REQUIRE(AppendWireframeOverlay(snapshot, assets, degenerate).has_value());
			CHECK(degenerate.IsEmpty());
			snapshot.Meshes[0].World = glm::scale(glm::mat4(1.0f), glm::vec3(1.0e-7f));
			DebugDrawList small;
			REQUIRE(AppendWireframeOverlay(snapshot, assets, small).has_value());
			CHECK(small.GetSize() == 3); // Nonzero triangles retain edges regardless of world scale.
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("EditorOverlay: grid and icons render through portable depth-tested lines")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			auto renderer = DebugRenderer::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE(renderer.has_value());
			nvrhi::TextureDesc desc;
			desc.width = desc.height = 64;
			desc.format = nvrhi::Format::RGBA8_UNORM;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			auto color = gpu.GetDevice().CreateTexture(desc);
			desc.format = nvrhi::Format::D32;
			desc.initialState = nvrhi::ResourceStates::DepthWrite;
			auto depth = gpu.GetDevice().CreateTexture(desc);
			REQUIRE(color.has_value());
			REQUIRE(depth.has_value());
			auto framebuffer = gpu.GetDevice().CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(*color).setDepthAttachment(*depth));
			REQUIRE(framebuffer.has_value());
			nvrhi::BufferDesc bufferDesc;
			bufferDesc.byteSize = sizeof(ViewConstants);
			bufferDesc.isConstantBuffer = true;
			bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
			bufferDesc.keepInitialState = true;
			auto buffer = gpu.GetDevice().CreateBuffer(bufferDesc);
			REQUIRE(buffer.has_value());
			PassBindingCache bindings;
			for (auto projection : { RenderProjection::Perspective, RenderProjection::Orthographic })
			{
				const auto snapshot = Utils::OverlayTestSnapshot(projection);
				auto withIcon = snapshot;
				withIcon.Icons = { { .Position = glm::vec3(0.0f), .Size = 20.0f } };
				DebugDrawList list;
				REQUIRE(AppendEditorOverlay(withIcon, list).has_value());
				for (float obstruction : { 0.0f, 1.0f })
				{
					auto commands = gpu.GetDevice().CreateCommandList();
					REQUIRE(commands.has_value());
					(*commands)->open();
					ViewConstants view{};
					view.View = snapshot.Camera.View;
					view.Projection = snapshot.Camera.Projection;
					view.ViewProjection = view.Projection * view.View;
					view.ViewportSize = glm::vec2(64.0f);
					(*commands)->writeBuffer(*buffer, &view, sizeof(view));
					(*commands)->clearTextureFloat(*color, nvrhi::AllSubresources, nvrhi::Color(0.0f));
					(*commands)->clearDepthStencilTexture(*depth, nvrhi::AllSubresources, true, obstruction, false, 0);
					auto recorded = (*renderer)->Record(**commands, bindings, { .DebugDraw = &list, .Framebuffer = *framebuffer, .ViewConstants = *buffer });
					(*commands)->close();
					gpu.GetDevice().ExecuteCommandList(**commands);
					REQUIRE(recorded.has_value());
					CHECK(*recorded > 0);
					Readback readback(gpu.GetDevice());
					auto image = readback.ReadTexture(**color);
					REQUIRE(image.has_value());
					size_t lit = 0;
					for (size_t pixel = 0; pixel < image->Pixels.size(); pixel += 4)
						lit += image->Pixels[pixel] != std::byte{ 0 } || image->Pixels[pixel + 1] != std::byte{ 0 } || image->Pixels[pixel + 2] != std::byte{ 0 };
					if (obstruction == 0.0f)
						CHECK(lit > 40);
					else
						CHECK(lit == 0);
				}
			}
		}

		TEST_CASE("Wireframe: solid picking remains unchanged while mesh interiors become background")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::InMemoryAssetManager assets;
			GpuResourceCache cache(gpu.GetDevice(), assets);
			auto pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE(pipelines.has_value());
			auto renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets, { 64, 64 });
			REQUIRE(renderer.has_value());
			auto snapshot = Utils::WireframeTestSnapshot(assets, false, false, false);
			snapshot.Flags = RenderViewFlags::Picking;
			snapshot.Post.BloomEnabled = false;
			snapshot.Post.FxaaEnabled = false;
			snapshot.Camera.ClearColor = glm::vec3(0.0f);
			snapshot.Camera.ClearToSkybox = false;
			std::array<Image, 2> ids;
			for (size_t frame = 0; frame < 2; ++frame)
			{
				snapshot.FrameIndex = frame + 1;
				if (frame == 1)
					snapshot.Flags |= RenderViewFlags::Wireframe;
				auto commands = gpu.GetDevice().CreateCommandList();
				REQUIRE(commands.has_value());
				(*commands)->open();
				const auto recorded = (*renderer)->Render(**commands, snapshot);
				(*commands)->close();
				(*renderer)->OnSubmitted(snapshot.FrameIndex, gpu.GetDevice().ExecuteCommandList(**commands));
				REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
				REQUIRE((*renderer)->GetEntityIdTexture() != nullptr);
				Readback readback(gpu.GetDevice());
				const auto pixels = readback.ReadTexture(*(*renderer)->GetEntityIdTexture());
				REQUIRE(pixels.has_value());
				ids[frame] = *pixels;
				uint32_t centerId = 0;
				std::memcpy(&centerId, pixels->Pixels.data() + (32 * 64 + 32) * sizeof(uint32_t), sizeof(centerId));
				CHECK(centerId == 1); // Equal all-background images must not satisfy the solid-picking oracle.
				if (frame == 1)
				{
					const auto color = readback.ReadTexture(*(*renderer)->GetFinalTexture());
					REQUIRE(color.has_value());
					const size_t center = (32 * 64 + 32) * 4;
					CHECK(color->Pixels[center] == std::byte{ 0 });
					CHECK(color->Pixels[center + 1] == std::byte{ 0 });
					CHECK(color->Pixels[center + 2] == std::byte{ 0 });
					uint32_t whitePixels = 0;
					for (size_t pixel = 0; pixel < color->Pixels.size(); pixel += 4)
						if (color->Pixels[pixel] == std::byte{ 255 } && color->Pixels[pixel + 1] == std::byte{ 255 } && color->Pixels[pixel + 2] == std::byte{ 255 })
							++whitePixels;
					CHECK(whitePixels > 20);
				}
			}
			CHECK(ids[0].Pixels == ids[1].Pixels);
			gpu.GetDevice().WaitForIdle();
		}
	}

}
