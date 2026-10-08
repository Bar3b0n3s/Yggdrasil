#include "TestsPCH.h"

#include "Engine/Renderer/DebugRenderer.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/ViewConstants.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/matrix_transform.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <vector>

// The debug lines (Architecture §8.10, §8.3 pass 13): the CPU tessellation of every primitive and the two GPU pipelines.
// Skeletons of the M8 contract (Docs/Decisions/0013-m8-decisions.md decision 10); stream D implements the renderer and
// removes the skips.

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("DebugRenderer: each primitive tessellates into its documented number of segments" * doctest::skip(true))
		{
			const glm::vec4 white(1.0f);
			const std::array<std::pair<DebugShape, size_t>, 8> cases = { {
				{ DebugLine{}, 1 },
				{ DebugRay{}, 1 },
				{ DebugBox{}, 12 },
				{ DebugSphere{}, 3 * DebugCircleSegments },
				{ DebugCapsule{}, 2 * DebugCircleSegments + 4 + 2 * DebugCircleSegments },
				{ DebugArrow{}, 5 },
				{ DebugFrustum{}, 12 },
				{ DebugText{ .Position = glm::vec3(0.0f), .Text = "Label", .Size = 16.0f }, 0 },
			} };
			for (const auto& [shape, segments] : cases)
			{
				CAPTURE(shape.index());
				DebugDrawList list;
				list.Add({ .Shape = shape, .Color = white, .Duration = 0.0f, .Depth = DebugDepthMode::Tested, .Remaining = 0.0f });
				const DebugLineVertices vertices = BuildDebugLineVertices(list);
				CHECK(vertices.Tested.size() == 2 * segments);
				CHECK(vertices.OnTop.empty());
				CHECK(vertices.SkippedCommands == 0);
			}
		}

		TEST_CASE("DebugRenderer: depth modes split the vertices, colours pass through and non-finite commands are skipped" * doctest::skip(true))
		{
			DebugDrawList list;
			list.AddLine(glm::vec3(0.0f), glm::vec3(1.0f, 2.0f, 3.0f), glm::vec4(1.0f, 0.5f, 0.25f, 0.75f));
			list.AddLine(glm::vec3(0.0f), glm::vec3(0.0f, 1.0f, 0.0f), glm::vec4(1.0f), 0.0f, DebugDepthMode::OnTop);
			list.AddSphere(glm::vec3(std::numeric_limits<float>::quiet_NaN()), 1.0f, glm::vec4(1.0f));
			list.AddRay(glm::vec3(0.0f), glm::vec3(0.0f), 1.0f, glm::vec4(1.0f)); // a zero direction draws nothing
			const DebugLineVertices vertices = BuildDebugLineVertices(list);
			REQUIRE(vertices.Tested.size() == 2);
			CHECK(vertices.Tested[0] == DebugLineVertex{ .Position = glm::vec3(0.0f), .Color = glm::vec4(1.0f, 0.5f, 0.25f, 0.75f) });
			CHECK(vertices.Tested[1].Position == glm::vec3(1.0f, 2.0f, 3.0f));
			CHECK(vertices.OnTop.size() == 2);
			CHECK(vertices.SkippedCommands == 2);
		}

		TEST_CASE("DebugRenderer: tessellation stops at the vertex budget and counts the commands left out" * doctest::skip(true))
		{
			// MaxDebugLineVertices bounds what a runaway producer costs (the list bounds only its commands): a sphere is
			// 3 * DebugCircleSegments segments, so the budget holds this many whole spheres, the next one is left out, and a
			// later line still fits in what remains.
			constexpr uint32_t SphereVertices = 2 * 3 * DebugCircleSegments;
			constexpr uint32_t Fitting = MaxDebugLineVertices / SphereVertices;
			static_assert(MaxDebugLineVertices - Fitting * SphereVertices >= 2, "a line must fit after the whole spheres");
			DebugDrawList list;
			for (uint32_t index = 0; index <= Fitting; ++index)
				list.AddSphere(glm::vec3(0.0f), 1.0f, glm::vec4(1.0f));
			list.AddLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec4(1.0f), 0.0f, DebugDepthMode::OnTop);
			REQUIRE(list.GetDroppedCount() == 0);
			const DebugLineVertices vertices = BuildDebugLineVertices(list);
			CHECK(vertices.Tested.size() == static_cast<size_t>(Fitting) * SphereVertices);
			CHECK(vertices.OnTop.size() == 2);
			CHECK(vertices.Tested.size() + vertices.OnTop.size() <= MaxDebugLineVertices);
			CHECK(vertices.BudgetSkippedCommands == 1);
			CHECK(vertices.SkippedCommands == 0);
		}

		TEST_CASE("DebugRenderer: a ray ends Length metres along its normalized direction" * doctest::skip(true))
		{
			DebugDrawList list;
			list.AddRay(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -10.0f), 2.0f, glm::vec4(1.0f));
			const DebugLineVertices vertices = BuildDebugLineVertices(list);
			REQUIRE(vertices.Tested.size() == 2);
			CHECK(vertices.Tested[1].Position == glm::vec3(1.0f, 0.0f, -2.0f));
		}

		TEST_CASE("DebugRenderer: draws depth-tested lines behind geometry hidden and on-top lines visible"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// A cleared LDR target and a SceneDepth holding 0.5 everywhere (a wall at that depth): a red line farther than the
			// wall (depth below 0.5) is hidden when Tested and visible when OnTop.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<Scope<DebugRenderer>> renderer = DebugRenderer::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				CHECK((*renderer)->GetPipelineCount() == DebugRenderer::PipelineCount);
				constexpr uint32_t Size = 32;
				nvrhi::TextureDesc colorDesc;
				colorDesc.width = Size;
				colorDesc.height = Size;
				colorDesc.format = LdrColorFormat;
				colorDesc.isRenderTarget = true;
				colorDesc.isShaderResource = true;
				colorDesc.initialState = nvrhi::ResourceStates::RenderTarget;
				colorDesc.keepInitialState = true;
				nvrhi::TextureDesc depthDesc = colorDesc;
				depthDesc.format = SceneDepthFormat;
				depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
				Result<nvrhi::TextureHandle> color = device.CreateTexture(colorDesc);
				Result<nvrhi::TextureHandle> depth = device.CreateTexture(depthDesc);
				REQUIRE(color.has_value());
				REQUIRE(depth.has_value());
				Result<nvrhi::FramebufferHandle> framebuffer =
					device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(*color).setDepthAttachment(*depth));
				REQUIRE(framebuffer.has_value());

				// A perspective camera at the origin looking down -Z; the line crosses the view at z = -50 (far behind a wall at
				// depth 0.5, which is z = -0.2 for a near plane of 0.1).
				ViewConstants view{};
				view.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 90.0f, 10.0f, 0.1f, 100.0f, Size, Size);
				view.View = glm::mat4(1.0f);
				view.ViewProjection = view.Projection;
				view.ViewportSize = glm::vec2(static_cast<float>(Size));
				view.InverseViewportSize = glm::vec2(1.0f / static_cast<float>(Size));
				nvrhi::BufferDesc bufferDesc;
				bufferDesc.byteSize = sizeof(ViewConstants);
				bufferDesc.isConstantBuffer = true;
				bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
				bufferDesc.keepInitialState = true;
				Result<nvrhi::BufferHandle> viewBuffer = device.CreateBuffer(bufferDesc);
				REQUIRE(viewBuffer.has_value());

				const auto centreRed = [&](DebugDepthMode mode)
				{
					DebugDrawList list;
					list.AddLine(glm::vec3(-100.0f, 0.0f, -50.0f), glm::vec3(100.0f, 0.0f, -50.0f), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), 0.0f, mode);
					Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
					REQUIRE(commandList.has_value());
					(*commandList)->open();
					(*commandList)->writeBuffer(*viewBuffer, &view, sizeof(view));
					(*commandList)->clearTextureFloat(*color, nvrhi::AllSubresources, nvrhi::Color(0.0f));
					(*commandList)->clearDepthStencilTexture(*depth, nvrhi::AllSubresources, true, 0.5f, false, 0);
					PassBindingCache bindings;
					const Result<uint32_t> drawn = (*renderer)->Record(**commandList, bindings, { .DebugDraw = &list, .Framebuffer = *framebuffer, .ViewConstants = *viewBuffer });
					(*commandList)->close();
					REQUIRE_MESSAGE(drawn.has_value(), drawn.error().ToString());
					CHECK(*drawn == 2);
					device.ExecuteCommandList(**commandList);
					Readback readback(device);
					const Result<Image> image = readback.ReadTexture(**color);
					REQUIRE(image.has_value());
					int brightest = 0;
					for (uint32_t row = Size / 2 - 1; row <= Size / 2; ++row)
						brightest = std::max(brightest, std::to_integer<int>(image->GetRow(row)[static_cast<size_t>(Size / 2) * 4]));
					return brightest;
				};
				CHECK(centreRed(DebugDepthMode::Tested) == 0);
				CHECK(centreRed(DebugDepthMode::OnTop) == 255);
			}
			device.RunGarbageCollection();
		}
	}

}
