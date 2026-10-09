#include "TestsPCH.h"

#include "Engine/Renderer/DepthPyramidPass.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <vector>

namespace Engine {

	namespace {

		static CameraData MakeDepthCamera(RenderProjection kind, uint32_t width, uint32_t height)
		{
			CameraData camera;
			camera.ProjectionKind = kind;
			camera.NearClip = 0.25f;
			camera.FarClip = 100.0f;
			camera.OrthographicSize = 3.0f;
			camera.ViewportWidth = width;
			camera.ViewportHeight = height;
			camera.Projection = ComputeReverseZProjection(kind, camera.VerticalFov, camera.OrthographicSize,
				camera.NearClip, camera.FarClip, width, height);
			return camera;
		}

		// Independent geometric oracle: source and destination cells overlap in normalized image space.
		// Deliberately no ComputeDepthReductionFootprint call and no integer footprint expression from the shader.
		static std::vector<float> ReduceDepthReference(const std::vector<float>& source, uint32_t width, uint32_t height)
		{
			const uint32_t nextWidth = std::max(1U, width / 2);
			const uint32_t nextHeight = std::max(1U, height / 2);
			std::vector<float> result(static_cast<size_t>(nextWidth) * nextHeight, 65504.0f);
			for (uint32_t y = 0; y < nextHeight; ++y)
			{
				for (uint32_t x = 0; x < nextWidth; ++x)
				{
					for (uint32_t sy = 0; sy < height; ++sy)
					{
						if ((sy + 1.0) / height <= static_cast<double>(y) / nextHeight
							|| static_cast<double>(sy) / height >= (y + 1.0) / nextHeight)
							continue;
						for (uint32_t sx = 0; sx < width; ++sx)
						{
							if ((sx + 1.0) / width > static_cast<double>(x) / nextWidth
								&& static_cast<double>(sx) / width < (x + 1.0) / nextWidth)
								result[y * nextWidth + x] = std::min(result[y * nextWidth + x], source[sy * width + sx]);
						}
					}
				}
			}
			return result;
		}

		static void CheckDepthGpu(Test::HeadlessGpuFixture& gpu, DepthPyramidPass& pass, const CameraData& camera,
			const std::vector<float>& reverseDepth)
		{
			GraphicsDevice& device = gpu.GetDevice();
			nvrhi::TextureDesc sourceDesc;
			sourceDesc.width = camera.ViewportWidth;
			sourceDesc.height = camera.ViewportHeight;
			sourceDesc.format = nvrhi::Format::D32;
			sourceDesc.isShaderResource = true;
			sourceDesc.isRenderTarget = true;
			sourceDesc.initialState = nvrhi::ResourceStates::ShaderResource;
			sourceDesc.keepInitialState = true;
			Result<nvrhi::TextureHandle> source = device.CreateTexture(sourceDesc);
			REQUIRE(source.has_value());
			Result<nvrhi::TextureHandle> target = device.CreateTexture(DepthPyramidPass::GetTargetDesc(sourceDesc.width, sourceDesc.height));
			REQUIRE(target.has_value());
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE(list.has_value());
			PassBindingCache bindings;
			RenderStats stats;
			RenderRecordingContext recording(stats, []()
			{
				return 0.0;
			});
			(*list)->open();
			(*list)->writeTexture(*source, 0, 0, reverseDepth.data(), sourceDesc.width * sizeof(float));
			const Status wrongDescriptor = pass.Record(**list, recording, bindings,
				{ .SceneDepth = *target, .ViewDepth = *source, .Camera = camera });
			REQUIRE_FALSE(wrongDescriptor.has_value());
			CHECK(wrongDescriptor.error().GetCode() == ErrorCode::InvalidArgument);
			CameraData invalidCamera = camera;
			invalidCamera.NearClip = 0.0f;
			const Status invalidProjection = pass.Record(**list, recording, bindings,
				{ .SceneDepth = *source, .ViewDepth = *target, .Camera = invalidCamera });
			REQUIRE_FALSE(invalidProjection.has_value());
			CHECK(invalidProjection.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(stats.Passes.empty());
			CHECK(bindings.GetSize() == 0);
			const Status recorded = pass.Record(**list, recording, bindings,
				{ .SceneDepth = *source, .ViewDepth = *target, .Camera = camera });
			(*list)->close();
			REQUIRE_MESSAGE(recorded.has_value(), recorded.error().ToString());
			device.ExecuteCommandList(**list);
			REQUIRE(stats.Passes.size() == 1);
			CHECK(stats.Passes.front().Name == "DepthPyramid");
			CHECK(stats.Passes.front().Dispatches == (*target)->getDesc().mipLevels);

			std::vector<float> expected;
			for (const float depth : reverseDepth)
			{
				const double reference = depth == 0.0f ? 65504.0
													   : (camera.ProjectionKind == RenderProjection::Perspective
																 ? camera.NearClip / static_cast<double>(depth)
																 : (1.0 - depth) * camera.FarClip + depth * static_cast<double>(camera.NearClip));
				expected.push_back(glm::unpackHalf1x16(glm::packHalf1x16(static_cast<float>(std::min(reference, 65504.0)))));
			}
			Readback readback(device);
			uint32_t width = sourceDesc.width;
			uint32_t height = sourceDesc.height;
			for (uint32_t mip = 0; mip < (*target)->getDesc().mipLevels; ++mip)
			{
				CAPTURE(mip);
				Result<Image> image = readback.ReadTexture(**target, mip);
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				REQUIRE(image->Width == width);
				REQUIRE(image->Height == height);
				for (size_t i = 0; i < expected.size(); ++i)
				{
					uint16_t bits = 0;
					std::memcpy(&bits, image->Pixels.data() + i * sizeof(bits), sizeof(bits));
					CHECK(glm::unpackHalf1x16(bits) == doctest::Approx(expected[i]).epsilon(0.001));
				}
				expected = ReduceDepthReference(expected, width, height);
				width = std::max(1U, width / 2);
				height = std::max(1U, height / 2);
			}
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("Projection: orthographic reverse-Z mapping and view-ray reconstruction")
		{
			const CameraData camera = MakeDepthCamera(RenderProjection::Orthographic, 8, 4);
			CHECK(ReconstructLinearViewDepth(camera, 1.0f) == 0.25f);
			CHECK(ReconstructLinearViewDepth(camera, 0.5f) == 50.125f);
			CHECK(ReconstructLinearViewDepth(camera, 0.0f) == 65504.0f);
			for (const glm::vec2 uv : { glm::vec2(0.0f), glm::vec2(0.5f), glm::vec2(1.0f), glm::vec2(0.125f, 0.75f) })
			{
				const glm::vec3 near = ReconstructViewPosition(camera, uv, 0.25f);
				const glm::vec3 far = ReconstructViewPosition(camera, uv, 100.0f);
				CHECK(near.x == far.x);
				CHECK(near.y == far.y);
				CHECK(near.x == doctest::Approx((2.0f * uv.x - 1.0f) * 6.0f));
				CHECK(near.y == doctest::Approx((1.0f - 2.0f * uv.y) * 3.0f));
				const glm::vec4 clip = camera.Projection * glm::vec4(near, 1.0f);
				CHECK(clip.z == doctest::Approx(1.0f));
				CHECK((camera.Projection * glm::vec4(far, 1.0f)).z == doctest::Approx(0.0f).epsilon(1e-6));
			}
		}

		TEST_CASE("Projection: orthographic view reconstruction preserves the frozen projection at different capture aspects")
		{
			CameraData camera = MakeDepthCamera(RenderProjection::Orthographic, 8, 4);
			const glm::mat4 projection = camera.Projection;
			for (const glm::uvec2 extent : { glm::uvec2(8, 4), glm::uvec2(4, 4), glm::uvec2(4, 8) })
			{
				camera.ViewportWidth = extent.x;
				camera.ViewportHeight = extent.y;
				for (const glm::vec2 uv : { glm::vec2(0.0f), glm::vec2(1.0f), glm::vec2(0.75f, 0.25f) })
				{
					for (const float depth : { camera.NearClip, 4.0f, camera.FarClip })
					{
						CAPTURE(extent.x);
						CAPTURE(extent.y);
						CAPTURE(uv.x);
						CAPTURE(uv.y);
						CAPTURE(depth);
						const glm::vec3 position = ReconstructViewPosition(camera, uv, depth);
						// The frozen 2:1 camera has half extents (6,3), regardless of the destination image.
						CHECK(position.x == doctest::Approx((2.0f * uv.x - 1.0f) * 6.0f));
						CHECK(position.y == doctest::Approx((1.0f - 2.0f * uv.y) * 3.0f));
						CHECK(position.z == -depth);
						const glm::vec4 clip = projection * glm::vec4(position, 1.0f);
						CHECK(clip.x / clip.w == doctest::Approx(2.0f * uv.x - 1.0f));
						CHECK(clip.y / clip.w == doctest::Approx(1.0f - 2.0f * uv.y));
					}
				}
			}
		}

		TEST_CASE("DepthPyramid: perspective sky and finite depth do not create false occluders")
		{
			const CameraData camera = MakeDepthCamera(RenderProjection::Perspective, 8, 4);
			CHECK(ReconstructLinearViewDepth(camera, 0.0f) == 65504.0f);
			CHECK(ReconstructLinearViewDepth(camera, 1e-10f) == 65504.0f);
			CHECK(ReconstructLinearViewDepth(camera, 0.5f) == 0.5f);
			const glm::vec3 position = ReconstructViewPosition(camera, { 0.75f, 0.25f }, 4.0f);
			const glm::vec4 clip = camera.Projection * glm::vec4(position, 1.0f);
			CHECK(clip.x / clip.w == doctest::Approx(0.5f));
			CHECK(clip.y / clip.w == doctest::Approx(0.5f));
			CHECK(position.z == -4.0f);
			CHECK(DepthPyramidPass::GetTargetDesc(1, 1).mipLevels == 1);
			CHECK(DepthPyramidPass::GetTargetDesc(5, 3).mipLevels == 3);
			const nvrhi::TextureDesc large = DepthPyramidPass::GetTargetDesc(1920, 1080);
			CHECK(large.mipLevels == 5);
			CHECK(large.format == nvrhi::Format::R16_FLOAT);
			CHECK(large.isUAV);
			CHECK(large.keepInitialState);
		}

		TEST_CASE("DepthPyramid: odd and one-dimensional reductions retain the final row and column")
		{
			for (const glm::uvec2 size : { glm::uvec2(5, 3), glm::uvec2(1, 5), glm::uvec2(5, 1), glm::uvec2(1, 1), glm::uvec2(8, 4) })
			{
				const uint32_t width = std::max(1U, size.x / 2);
				const uint32_t height = std::max(1U, size.y / 2);
				for (uint32_t y = 0; y < height; ++y)
				{
					for (uint32_t x = 0; x < width; ++x)
					{
						const Result<DepthReductionFootprint> footprint = ComputeDepthReductionFootprint(size.x, size.y, x, y);
						REQUIRE(footprint.has_value());
						CHECK(footprint->MinX == static_cast<uint32_t>(std::floor(static_cast<double>(x) / width * size.x)));
						CHECK(footprint->MaxXExclusive == static_cast<uint32_t>(std::ceil((x + 1.0) / width * size.x)));
						CHECK(footprint->MinY == static_cast<uint32_t>(std::floor(static_cast<double>(y) / height * size.y)));
						CHECK(footprint->MaxYExclusive == static_cast<uint32_t>(std::ceil((y + 1.0) / height * size.y)));
					}
				}
			}
			const Result<DepthReductionFootprint> huge = ComputeDepthReductionFootprint(UINT32_MAX, UINT32_MAX, UINT32_MAX / 2 - 1, UINT32_MAX / 2 - 1);
			REQUIRE(huge.has_value());
			CHECK(huge->MaxXExclusive == UINT32_MAX);
			CHECK(huge->MaxYExclusive == UINT32_MAX);
			CHECK_FALSE(ComputeDepthReductionFootprint(0, 1, 0, 0).has_value());
			CHECK_FALSE(ComputeDepthReductionFootprint(5, 3, 2, 0).has_value());
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("DepthPyramid: GPU linear depth matches CPU references for both projections")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Result<Scope<DepthPyramidPass>> pass = DepthPyramidPass::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				for (const RenderProjection kind : { RenderProjection::Perspective, RenderProjection::Orthographic })
				{
					for (const glm::uvec2 size : { glm::uvec2(5, 3), glm::uvec2(8, 4), glm::uvec2(1, 1) })
					{
						std::vector<float> depths(static_cast<size_t>(size.x) * size.y);
						constexpr std::array<float, 6> Values = { 0.0f, 1.0f, 0.5f, 0.125f, 0.001f, 1e-10f };
						for (size_t i = 0; i < depths.size(); ++i)
							depths[i] = Values[i % Values.size()];
						CheckDepthGpu(gpu, **pass, MakeDepthCamera(kind, size.x, size.y), depths);
					}
					CameraData distant = MakeDepthCamera(kind, 5, 3);
					distant.FarClip = 1000000.0f;
					distant.Projection = ComputeReverseZProjection(kind, distant.VerticalFov, distant.OrthographicSize,
						distant.NearClip, distant.FarClip, 5, 3);
					CheckDepthGpu(gpu, **pass, distant, std::vector<float>(15, kind == RenderProjection::Perspective ? 1e-10f : 0.5f));
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("DepthPyramid: odd-edge GPU minima match conservative footprint oracles")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Result<Scope<DepthPyramidPass>> pass = DepthPyramidPass::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				for (const RenderProjection kind : { RenderProjection::Perspective, RenderProjection::Orthographic })
				{
					for (const glm::uvec2 size : { glm::uvec2(5, 3), glm::uvec2(1, 5), glm::uvec2(5, 1), glm::uvec2(1, 1) })
					{
						// Move the sole near sample through every texel, including the overlapping odd column and final edges.
						std::vector<float> depths(static_cast<size_t>(size.x) * size.y, 0.0f);
						for (size_t sample = 0; sample < depths.size(); ++sample)
						{
							depths[sample] = 1.0f;
							CheckDepthGpu(gpu, **pass, MakeDepthCamera(kind, size.x, size.y), depths);
							depths[sample] = 0.0f;
						}
					}
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
