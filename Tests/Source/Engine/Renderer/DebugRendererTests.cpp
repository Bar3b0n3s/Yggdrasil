#include "TestsPCH.h"

#include "Engine/Renderer/DebugRenderer.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/ViewConstants.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <span>
#include <utility>
#include <vector>

// The debug lines (Architecture §8.10, §8.3 pass 13; Docs/Decisions/0013-m8-decisions.md decision 10): the CPU tessellation
// of every primitive and the two GPU pipelines.

namespace Engine {

	namespace {

		constexpr uint32_t TargetSize = 32;

		// An overlay framebuffer (LDR + SceneDepth) of TargetSize x TargetSize and a ViewConstants buffer of a perspective
		// camera at the origin looking down -Z (90 degrees, near plane 0.1); Render records lists into one command list, as
		// views rendering one after another do, and reads the LDR target back.
		class LineSetup
		{
		public:
			explicit LineSetup(GraphicsDevice& device)
				: m_Device(device)
			{
				nvrhi::TextureDesc colorDesc;
				colorDesc.width = TargetSize;
				colorDesc.height = TargetSize;
				colorDesc.format = LdrColorFormat;
				colorDesc.isRenderTarget = true;
				colorDesc.isShaderResource = true;
				colorDesc.initialState = nvrhi::ResourceStates::RenderTarget;
				colorDesc.keepInitialState = true;
				colorDesc.debugName = "DebugRendererTests.LdrColor";
				nvrhi::TextureDesc depthDesc = colorDesc;
				depthDesc.format = SceneDepthFormat;
				depthDesc.initialState = nvrhi::ResourceStates::DepthWrite;
				depthDesc.debugName = "DebugRendererTests.SceneDepth";
				Result<nvrhi::TextureHandle> color = m_Device.CreateTexture(colorDesc);
				Result<nvrhi::TextureHandle> depth = m_Device.CreateTexture(depthDesc);
				REQUIRE(color.has_value());
				REQUIRE(depth.has_value());
				m_Color = *color;
				m_Depth = *depth;
				Result<nvrhi::FramebufferHandle> framebuffer =
					m_Device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(m_Color).setDepthAttachment(m_Depth));
				REQUIRE(framebuffer.has_value());
				m_Framebuffer = *framebuffer;

				m_View.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 90.0f, 10.0f, 0.1f, 100.0f, TargetSize, TargetSize);
				m_View.View = glm::mat4(1.0f);
				m_View.ViewProjection = m_View.Projection;
				m_View.ViewportSize = glm::vec2(static_cast<float>(TargetSize));
				m_View.InverseViewportSize = glm::vec2(1.0f / static_cast<float>(TargetSize));
				nvrhi::BufferDesc bufferDesc;
				bufferDesc.byteSize = sizeof(ViewConstants);
				bufferDesc.isConstantBuffer = true;
				bufferDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
				bufferDesc.keepInitialState = true;
				bufferDesc.debugName = "DebugRendererTests.ViewConstants";
				Result<nvrhi::BufferHandle> viewBuffer = m_Device.CreateBuffer(bufferDesc);
				REQUIRE(viewBuffer.has_value());
				m_ViewBuffer = *viewBuffer;
			}

			// Clears the LDR target to black and SceneDepth to `depth`, records each of `lists` with a binding cache of its own
			// (one view each), and returns the image; `drawn` receives each Record's vertex count.
			Image Render(DebugRenderer& renderer, std::span<const DebugDrawList* const> lists, float depth, std::vector<uint32_t>& drawn)
			{
				Result<nvrhi::CommandListHandle> commandList = m_Device.CreateCommandList();
				REQUIRE(commandList.has_value());
				(*commandList)->open();
				(*commandList)->writeBuffer(m_ViewBuffer, &m_View, sizeof(m_View));
				(*commandList)->clearTextureFloat(m_Color, nvrhi::AllSubresources, nvrhi::Color(0.0f));
				(*commandList)->clearDepthStencilTexture(m_Depth, nvrhi::AllSubresources, true, depth, false, 0);
				std::vector<PassBindingCache> bindings(lists.size());
				drawn.clear();
				for (size_t index = 0; index < lists.size(); ++index)
				{
					const Result<uint32_t> recorded =
						renderer.Record(**commandList, bindings[index], { .DebugDraw = lists[index], .Framebuffer = m_Framebuffer, .ViewConstants = m_ViewBuffer });
					REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
					drawn.push_back(*recorded);
				}
				(*commandList)->close();
				m_Device.ExecuteCommandList(**commandList);
				Readback readback(m_Device);
				Result<Image> image = readback.ReadTexture(*m_Color);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				return std::move(*image);
			}
		private:
			GraphicsDevice& m_Device;
			nvrhi::TextureHandle m_Color;
			nvrhi::TextureHandle m_Depth;
			nvrhi::FramebufferHandle m_Framebuffer;
			nvrhi::BufferHandle m_ViewBuffer;
			ViewConstants m_View{};
		};

		// The largest red value of the pixels in columns [x0, x1) and rows [y0, y1).
		int BrightestRed(const Image& image, uint32_t x0, uint32_t y0, uint32_t x1, uint32_t y1)
		{
			int brightest = 0;
			for (uint32_t row = y0; row < y1; ++row)
			{
				for (uint32_t column = x0; column < x1; ++column)
					brightest = std::max(brightest, std::to_integer<int>(image.GetRow(row)[static_cast<size_t>(column) * 4]));
			}
			return brightest;
		}

		// The distance of `point` from the segment between `start` and `end` (which differ).
		float DistanceToSegment(const glm::vec3& point, const glm::vec3& start, const glm::vec3& end)
		{
			const glm::vec3 segment = end - start;
			const float t = std::clamp(glm::dot(point - start, segment) / glm::dot(segment, segment), 0.0f, 1.0f);
			return glm::length(point - (start + t * segment));
		}

		// The depth-tested vertices of a list holding `shape` alone, checking that each carries the command's colour.
		std::vector<DebugLineVertex> Tessellate(const DebugShape& shape)
		{
			const glm::vec4 color(0.25f, 0.5f, 0.75f, 1.0f);
			DebugDrawList list;
			list.Add({ .Shape = shape, .Color = color, .Duration = 0.0f, .Depth = DebugDepthMode::Tested, .Remaining = 0.0f });
			DebugLineVertices vertices = BuildDebugLineVertices(list);
			for (const DebugLineVertex& vertex : vertices.Tested)
				CHECK(vertex.Color == color);
			return std::move(vertices.Tested);
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("DebugRenderer: each primitive tessellates into its documented number of segments")
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

		TEST_CASE("DebugRenderer: depth modes split the vertices, colours pass through and non-finite commands are skipped")
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

		TEST_CASE("DebugRenderer: finite commands that tessellate beyond float's range are skipped whole")
		{
			// Every value is finite, but the ray's end (3e38 + 3e38) and the sphere's circles (centre 3e38 plus radius 3e38)
			// overflow: no vertex of either reaches the GPU, and a sound command after them still draws.
			constexpr float Huge = 3e38f;
			DebugDrawList list;
			list.AddRay(glm::vec3(Huge, 0.0f, 0.0f), glm::vec3(1.0f, 0.0f, 0.0f), Huge, glm::vec4(1.0f));
			list.AddSphere(glm::vec3(Huge, 0.0f, 0.0f), Huge, glm::vec4(1.0f), 0.0f, DebugDepthMode::OnTop);
			list.AddLine(glm::vec3(0.0f), glm::vec3(1.0f), glm::vec4(1.0f));
			const DebugLineVertices vertices = BuildDebugLineVertices(list);
			CHECK(vertices.SkippedCommands == 2);
			CHECK(vertices.BudgetSkippedCommands == 0);
			REQUIRE(vertices.Tested.size() == 2);
			CHECK(vertices.Tested[1].Position == glm::vec3(1.0f));
			CHECK(vertices.OnTop.empty());
			for (const DebugLineVertex& vertex : vertices.Tested)
			{
				CHECK(std::isfinite(vertex.Position.x));
				CHECK(std::isfinite(vertex.Position.y));
				CHECK(std::isfinite(vertex.Position.z));
			}
		}

		TEST_CASE("DebugRenderer: tessellation stops at the vertex budget and counts the commands left out")
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

		TEST_CASE("DebugRenderer: a ray ends Length metres along its normalized direction")
		{
			DebugDrawList list;
			list.AddRay(glm::vec3(1.0f, 0.0f, 0.0f), glm::vec3(0.0f, 0.0f, -10.0f), 2.0f, glm::vec4(1.0f));
			const DebugLineVertices vertices = BuildDebugLineVertices(list);
			REQUIRE(vertices.Tested.size() == 2);
			CHECK(vertices.Tested[1].Position == glm::vec3(1.0f, 0.0f, -2.0f));
		}

		TEST_CASE("DebugRenderer: a box's vertices are its rotated corners joined by its edges")
		{
			// Turned 90 degrees about +Y: every vertex is a corner (centre + rotated (+-0.5, +-1, +-2)), each corner ends three
			// edges, and every edge is as long as one of the box's sides.
			const glm::vec3 centre(1.0f, 2.0f, 3.0f);
			const glm::quat turn = glm::angleAxis(glm::radians(90.0f), glm::vec3(0.0f, 1.0f, 0.0f));
			const std::vector<DebugLineVertex> box = Tessellate(DebugBox{ .Center = centre, .HalfExtents = glm::vec3(0.5f, 1.0f, 2.0f), .Rotation = turn });
			REQUIRE(box.size() == 24);
			for (size_t index = 0; index < box.size(); index += 2)
			{
				for (const DebugLineVertex& end : { box[index], box[index + 1] })
				{
					const glm::vec3 local = glm::inverse(turn) * (end.Position - centre);
					CHECK(std::abs(local.x) == doctest::Approx(0.5f));
					CHECK(std::abs(local.y) == doctest::Approx(1.0f));
					CHECK(std::abs(local.z) == doctest::Approx(2.0f));
				}
				const float edge = glm::length(box[index + 1].Position - box[index].Position);
				const bool isSide = std::abs(edge - 1.0f) < 1e-5f || std::abs(edge - 2.0f) < 1e-5f || std::abs(edge - 4.0f) < 1e-5f;
				CHECK(isSide);
			}
		}

		TEST_CASE("DebugRenderer: sphere and capsule vertices lie at their radius")
		{
			// A sphere: every vertex at the radius from the centre, and each great circle closes.
			const glm::vec3 centre(-1.0f, 0.5f, 2.0f);
			const std::vector<DebugLineVertex> sphere = Tessellate(DebugSphere{ .Center = centre, .Radius = 0.75f });
			REQUIRE(sphere.size() == 2 * 3 * DebugCircleSegments);
			for (const DebugLineVertex& vertex : sphere)
				CHECK(glm::length(vertex.Position - centre) == doctest::Approx(0.75f));
			for (size_t circle = 0; circle < 3; ++circle)
			{
				const size_t first = circle * 2 * DebugCircleSegments;
				CHECK(sphere[first].Position == sphere[first + 2 * DebugCircleSegments - 1].Position);
			}

			// A capsule along a slanted axis: every vertex at the radius from the axis segment, reaching the radius beyond both
			// hemisphere centres along the axis.
			const glm::vec3 start(0.0f, 0.0f, 0.0f);
			const glm::vec3 end(1.0f, 2.0f, -2.0f); // 3 m long
			const std::vector<DebugLineVertex> capsule = Tessellate(DebugCapsule{ .Start = start, .End = end, .Radius = 0.5f });
			REQUIRE(capsule.size() == 2 * (4 * DebugCircleSegments + 4));
			const glm::vec3 axis = (end - start) / 3.0f;
			float highest = 0.0f;
			float lowest = 0.0f;
			for (const DebugLineVertex& vertex : capsule)
			{
				CHECK(DistanceToSegment(vertex.Position, start, end) == doctest::Approx(0.5f));
				highest = std::max(highest, glm::dot(vertex.Position - start, axis));
				lowest = std::min(lowest, glm::dot(vertex.Position - start, axis));
			}
			CHECK(highest == doctest::Approx(3.5f));
			CHECK(lowest == doctest::Approx(-0.5f));

			// Equal end points draw a sphere of the radius.
			for (const DebugLineVertex& vertex : Tessellate(DebugCapsule{ .Start = centre, .End = centre, .Radius = 0.25f }))
				CHECK(glm::length(vertex.Position - centre) == doctest::Approx(0.25f));
		}

		TEST_CASE("DebugRenderer: an arrow has four barbs of its head size and a frustum joins its eight corners")
		{
			// The shaft, then four barbs of HeadSize from the tip, 30 degrees off the reversed shaft.
			const glm::vec3 tip(0.0f, 0.0f, -2.0f);
			const std::vector<DebugLineVertex> arrow = Tessellate(DebugArrow{ .From = glm::vec3(0.0f), .To = tip, .HeadSize = 0.4f });
			REQUIRE(arrow.size() == 10);
			CHECK(arrow[0].Position == glm::vec3(0.0f));
			CHECK(arrow[1].Position == tip);
			for (size_t index = 2; index < arrow.size(); index += 2)
			{
				CHECK(arrow[index].Position == tip);
				const glm::vec3 barb = arrow[index + 1].Position - tip;
				CHECK(glm::length(barb) == doctest::Approx(0.4f));
				CHECK(glm::dot(glm::normalize(barb), glm::vec3(0.0f, 0.0f, 1.0f)) == doctest::Approx(std::cos(glm::radians(30.0f))));
			}
			// A zero-length arrow keeps its segment count, every segment collapsed onto the tip.
			const std::vector<DebugLineVertex> point = Tessellate(DebugArrow{ .From = glm::vec3(1.0f), .To = glm::vec3(1.0f), .HeadSize = 0.4f });
			REQUIRE(point.size() == 10);
			for (const DebugLineVertex& vertex : point)
				CHECK(vertex.Position == glm::vec3(1.0f));

			// A frustum: the near rectangle, the far rectangle and the four edges between them, from the given corners only
			// (corner i at x = i), each corner ending three edges.
			DebugFrustum frustum;
			for (size_t index = 0; index < frustum.Corners.size(); ++index)
				frustum.Corners[index] = glm::vec3(static_cast<float>(index), 0.0f, 0.0f);
			const std::vector<DebugLineVertex> edges = Tessellate(frustum);
			REQUIRE(edges.size() == 24);
			std::array<int, 8> ends{};
			for (const DebugLineVertex& vertex : edges)
			{
				const float corner = vertex.Position.x;
				REQUIRE(corner >= 0.0f);
				REQUIRE(corner <= 7.0f);
				++ends[static_cast<size_t>(corner)];
			}
			for (const int count : ends)
				CHECK(count == 3);
		}

		TEST_CASE("DebugRenderer: draws depth-tested lines behind geometry hidden and on-top lines visible"
			* doctest::test_suite(Test::GpuSuite))
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
				LineSetup setup(device);

				// The camera is at the origin looking down -Z; the line crosses the view at z = -50 (far behind a wall at depth
				// 0.5, which is z = -0.2 for a near plane of 0.1).
				const auto centreRed = [&setup, &renderer](DebugDepthMode mode)
				{
					DebugDrawList list;
					list.AddLine(glm::vec3(-100.0f, 0.0f, -50.0f), glm::vec3(100.0f, 0.0f, -50.0f), glm::vec4(1.0f, 0.0f, 0.0f, 1.0f), 0.0f, mode);
					const std::array<const DebugDrawList*, 1> lists = { &list };
					std::vector<uint32_t> drawn;
					const Image image = setup.Render(**renderer, lists, 0.5f, drawn);
					REQUIRE(drawn.size() == 1);
					CHECK(drawn[0] == 2);
					return BrightestRed(image, TargetSize / 2, TargetSize / 2 - 1, TargetSize / 2 + 1, TargetSize / 2 + 1);
				};
				// Each render runs outside its CHECK: the helper REQUIREs a successful record and readback.
				const int tested = centreRed(DebugDepthMode::Tested);
				const int onTop = centreRed(DebugDepthMode::OnTop);
				CHECK(tested == 0);
				CHECK(onTop == 255);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("DebugRenderer: the vertex buffer grows, views record one after another and the budget warning is logged once"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<Scope<DebugRenderer>> renderer = DebugRenderer::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(renderer.has_value(), renderer.error().ToString());
				LineSetup setup(device);
				std::vector<uint32_t> drawn;
				const glm::vec4 red(1.0f, 0.0f, 0.0f, 1.0f);

				// Nothing to draw: an empty list, and a list holding only a label (the TextRenderer draws labels).
				DebugDrawList empty;
				DebugDrawList labelOnly;
				labelOnly.AddText(glm::vec3(0.0f, 0.0f, -5.0f), "Label", 16.0f, red);
				const std::array<const DebugDrawList*, 2> nothing = { &empty, &labelOnly };
				const Image blank = setup.Render(**renderer, nothing, 0.0f, drawn);
				CHECK(drawn == std::vector<uint32_t>{ 0, 0 });
				CHECK(BrightestRed(blank, 0, 0, TargetSize, TargetSize) == 0);

				// A small list, then one larger than the buffer's first capacity, in one command list: the second record grows
				// the buffer after the first one's draw read it.
				DebugDrawList small;
				small.AddLine(glm::vec3(-1.0f, -1.0f, -5.0f), glm::vec3(1.0f, 1.0f, -5.0f), red);
				DebugDrawList spheres;
				for (uint32_t index = 0; index < 64; ++index)
					spheres.AddSphere(glm::vec3(0.0f, 0.0f, -5.0f), 1.0f + 0.01f * static_cast<float>(index), red);
				const std::array<const DebugDrawList*, 2> growing = { &small, &spheres };
				const Image grown = setup.Render(**renderer, growing, 0.0f, drawn);
				CHECK(drawn == std::vector<uint32_t>{ 2, 64 * 2 * 3 * DebugCircleSegments });
				CHECK(BrightestRed(grown, 0, 0, TargetSize, TargetSize) == 255);

				// More on-top spheres than the vertex budget holds, recorded by two views: each draws the whole spheres that fit,
				// and the warning is logged once per renderer.
				constexpr uint32_t SphereVertices = 2 * 3 * DebugCircleSegments;
				constexpr uint32_t Fitting = MaxDebugLineVertices / SphereVertices;
				DebugDrawList many;
				for (uint32_t index = 0; index <= Fitting; ++index)
					many.AddSphere(glm::vec3(0.0f, 0.0f, -5.0f), 1.0f, red, 0.0f, DebugDepthMode::OnTop);
				Test::ExpectLog warning(LogLevel::Warn, "whose lines exceed the budget");
				const std::array<const DebugDrawList*, 2> twice = { &many, &many };
				const Image full = setup.Render(**renderer, twice, 0.0f, drawn);
				CHECK(drawn == std::vector<uint32_t>{ Fitting * SphereVertices, Fitting * SphereVertices });
				CHECK(BrightestRed(full, 0, 0, TargetSize, TargetSize) == 255);
				const Image again = setup.Render(**renderer, twice, 0.0f, drawn);
				CHECK(BrightestRed(again, 0, 0, TargetSize, TargetSize) == 255);
				CHECK(warning.GetMatchCount() == 1);
			}
			device.RunGarbageCollection();
		}
	}

}
