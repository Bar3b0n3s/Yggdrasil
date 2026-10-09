#include "TestsPCH.h"

#include "Engine/Renderer/GtaoPass.h"

#include "Engine/Asset/BuiltinAssets.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Asset/TextureData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Renderer/DepthPyramidPass.h"
#include "Engine/Renderer/GpuResourceCache.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Shared/GtaoConstants.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/InMemoryAssetManager.h"

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/packing.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <numeric>
#include <span>
#include <utility>
#include <vector>

namespace Engine {

	namespace {

		static CameraData MakeGtaoCamera(RenderProjection kind, uint32_t width = 65, uint32_t height = 49)
		{
			CameraData camera;
			camera.ViewportWidth = width;
			camera.ViewportHeight = height;
			camera.ProjectionKind = kind;
			camera.OrthographicSize = 4.0f;
			camera.NearClip = 0.1f;
			camera.FarClip = 100.0f;
			camera.Projection = ComputeReverseZProjection(kind, camera.VerticalFov, camera.OrthographicSize,
				camera.NearClip, camera.FarClip, width, height);
			return camera;
		}

		static nvrhi::TextureHandle UploadGtaoTexture(GraphicsDevice& device, const nvrhi::TextureDesc& desc, std::span<const std::byte> bytes)
		{
			Result<nvrhi::TextureHandle> texture = device.CreateTexture(desc);
			REQUIRE(texture.has_value());
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE(list.has_value());
			(*list)->open();
			(*list)->writeTexture(*texture, 0, 0, bytes.data(), bytes.size() / desc.height);
			(*list)->close();
			device.ExecuteCommandList(**list);
			return *texture;
		}

		struct GtaoTestTargets
		{
			nvrhi::TextureHandle Depth{};
			nvrhi::TextureHandle Normals{};
			nvrhi::TextureHandle Occlusion{};
			nvrhi::TextureHandle Scratch{};
		};

		static GtaoTestTargets MakeGtaoTargets(Test::HeadlessGpuFixture& gpu, const CameraData& camera,
			const std::vector<float>& depths, const std::vector<glm::vec2>& normals, bool half)
		{
			GraphicsDevice& device = gpu.GetDevice();
			GtaoTestTargets targets;
			nvrhi::TextureDesc depthDesc = DepthPyramidPass::GetTargetDesc(camera.ViewportWidth, camera.ViewportHeight);
			std::vector<std::vector<uint16_t>> levels;
			std::vector<float> previous = depths;
			uint32_t width = camera.ViewportWidth;
			uint32_t height = camera.ViewportHeight;
			for (uint32_t mip = 0; mip < depthDesc.mipLevels; ++mip)
			{
				std::vector<uint16_t> halfDepth;
				for (const float depth : previous)
					halfDepth.push_back(glm::packHalf1x16(depth));
				levels.push_back(std::move(halfDepth));
				const uint32_t nextWidth = std::max(1U, width / 2);
				const uint32_t nextHeight = std::max(1U, height / 2);
				std::vector<float> next(static_cast<size_t>(nextWidth) * nextHeight, 65504.0f);
				for (uint32_t y = 0; y < nextHeight; ++y)
				{
					for (uint32_t x = 0; x < nextWidth; ++x)
					{
						const uint32_t xEnd = static_cast<uint32_t>(std::ceil((x + 1.0) * width / nextWidth));
						const uint32_t yEnd = static_cast<uint32_t>(std::ceil((y + 1.0) * height / nextHeight));
						for (uint32_t sy = y * height / nextHeight; sy < yEnd; ++sy)
						{
							for (uint32_t sx = x * width / nextWidth; sx < xEnd; ++sx)
								next[y * nextWidth + x] = std::min(next[y * nextWidth + x], previous[sy * width + sx]);
						}
					}
				}
				previous = std::move(next);
				width = nextWidth;
				height = nextHeight;
			}
			Result<nvrhi::TextureHandle> depthTexture = device.CreateTexture(depthDesc);
			REQUIRE(depthTexture.has_value());
			targets.Depth = *depthTexture;
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE(list.has_value());
			(*list)->open();
			for (uint32_t mip = 0; mip < levels.size(); ++mip)
				(*list)->writeTexture(targets.Depth, 0, mip, levels[mip].data(), std::max(1U, camera.ViewportWidth >> mip) * sizeof(uint16_t));
			(*list)->close();
			device.ExecuteCommandList(**list);

			nvrhi::TextureDesc normalDesc = GtaoPass::GetTargetDesc(camera.ViewportWidth, camera.ViewportHeight, false);
			normalDesc.format = nvrhi::Format::RG16_FLOAT;
			std::vector<std::array<uint16_t, 2>> packedNormals;
			for (const glm::vec2 normal : normals)
				packedNormals.push_back({ glm::packHalf1x16(normal.x), glm::packHalf1x16(normal.y) });
			targets.Normals = UploadGtaoTexture(device, normalDesc, std::as_bytes(std::span(packedNormals)));
			Result<nvrhi::TextureHandle> output = device.CreateTexture(GtaoPass::GetTargetDesc(camera.ViewportWidth, camera.ViewportHeight, half));
			Result<nvrhi::TextureHandle> scratch = device.CreateTexture(GtaoPass::GetTargetDesc(camera.ViewportWidth, camera.ViewportHeight, half));
			REQUIRE(output.has_value());
			REQUIRE(scratch.has_value());
			targets.Occlusion = *output;
			targets.Scratch = *scratch;
			return targets;
		}

		static Image ReadGtao(GraphicsDevice& device, nvrhi::ITexture& texture)
		{
			Readback readback(device);
			Result<Image> result = readback.ReadTexture(texture);
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			return std::move(*result);
		}

		static Image RunGtao(Test::HeadlessGpuFixture& gpu, GtaoPass& pass, const CameraData& camera,
			GtaoTestTargets& targets, const PostProcessSettings& post, bool half = false)
		{
			Result<nvrhi::CommandListHandle> list = gpu.GetDevice().CreateCommandList();
			REQUIRE(list.has_value());
			PassBindingCache bindings;
			RenderStats stats;
			RenderRecordingContext recording(stats, []()
			{
				return 0.0;
			});
			(*list)->open();
			(*list)->clearTextureFloat(targets.Occlusion, nvrhi::AllSubresources, nvrhi::Color(0.0f));
			(*list)->setTextureState(targets.Occlusion, nvrhi::AllSubresources, nvrhi::ResourceStates::ShaderResource);
			(*list)->commitBarriers();
			const Result<nvrhi::ITexture*> result = pass.Record(**list, recording, bindings,
				{ .Camera = camera, .Post = post, .HalfResolution = half, .ViewDepth = targets.Depth, .SceneNormals = targets.Normals, .Occlusion = targets.Occlusion, .Scratch = targets.Scratch });
			(*list)->close();
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK(*result == targets.Occlusion.Get());
			REQUIRE(stats.Passes.size() == (post.SsaoEnabled ? 3U : 0U));
			for (const RenderPassStats& row : stats.Passes)
				CHECK(row.Dispatches == 1);
			gpu.GetDevice().ExecuteCommandList(**list);
			return ReadGtao(gpu.GetDevice(), *targets.Occlusion);
		}

		// Exercise a public shader layout directly: controlled inputs isolate the horizon integration and each filter.
		static void DispatchGtaoKernel(Test::HeadlessGpuFixture& gpu, std::string_view entry, const GtaoConstants& constants,
			GtaoTestTargets& targets, nvrhi::ITexture* source, nvrhi::ITexture* output)
		{
			PipelineLayoutDescription layout = GtaoPass::GetLayoutDescriptions()[entry == "CSMain" ? 0 : 1];
			layout.Entries = { std::string(entry) };
			Result<ComputePipeline> pipeline = gpu.GetPipelines().CreateComputePipeline({ .Layout = std::move(layout) });
			REQUIRE_MESSAGE(pipeline.has_value(), pipeline.error().ToString());
			nvrhi::BindingSetDesc desc;
			desc.bindings = { nvrhi::BindingSetItem::PushConstants(0, sizeof(constants)),
				nvrhi::BindingSetItem::Texture_SRV(0, targets.Depth), nvrhi::BindingSetItem::Texture_SRV(1, targets.Normals),
				nvrhi::BindingSetItem::Texture_UAV(0, output, nvrhi::Format::R8_UNORM) };
			if (source != nullptr)
				desc.bindings.push_back(nvrhi::BindingSetItem::Texture_SRV(2, source));
			Result<nvrhi::BindingSetHandle> set = gpu.GetDevice().CreateBindingSet(desc, *pipeline->BindingLayouts.front());
			REQUIRE_MESSAGE(set.has_value(), set.error().ToString());
			Result<nvrhi::CommandListHandle> list = gpu.GetDevice().CreateCommandList();
			REQUIRE(list.has_value());
			(*list)->open();
			nvrhi::ComputeState compute;
			compute.pipeline = pipeline->Pipeline;
			compute.bindings = { *set };
			(*list)->setComputeState(compute);
			(*list)->setPushConstants(&constants, sizeof(constants));
			(*list)->dispatch((output->getDesc().width + 7) / 8, (output->getDesc().height + 7) / 8);
			(*list)->close();
			gpu.GetDevice().ExecuteCommandList(**list);
		}

		static uint32_t GtaoByte(const Image& image, uint32_t x, uint32_t y)
		{
			return std::to_integer<uint32_t>(image.Pixels[static_cast<size_t>(y) * image.Width + x]);
		}

		// Generic homogeneous unprojection, independent of the pass's PositionScale construction.
		static glm::dvec3 GtaoCaptureOraclePosition(const glm::dmat4& projection, const glm::dmat4& inverse,
			const glm::dvec2& pixel, const glm::uvec2& size, double depth)
		{
			const glm::dvec2 uv = (pixel + 0.5) / glm::dvec2(size);
			const glm::dvec4 projected = projection * glm::dvec4(0.0, 0.0, -depth, 1.0);
			const glm::dvec4 position = inverse * glm::dvec4(2.0 * uv.x - 1.0, 1.0 - 2.0 * uv.y, projected.z / projected.w, 1.0);
			return glm::dvec3(position) / position.w;
		}

		static double GtaoCaptureContactOracle(const CameraData& camera, const std::vector<float>& depths, float radius)
		{
			const glm::uvec2 size(camera.ViewportWidth, camera.ViewportHeight);
			const glm::uvec2 center = size / 2U;
			const glm::dmat4 projection(camera.Projection);
			const glm::dmat4 inverse = glm::inverse(projection);
			const auto position = [&](const glm::uvec2& pixel)
			{
				return GtaoCaptureOraclePosition(projection, inverse, pixel, size, depths[pixel.y * size.x + pixel.x]);
			};
			const glm::dvec3 receiver = position(center);
			const double verticalSpan = glm::length(GtaoCaptureOraclePosition(projection, inverse, glm::dvec2(0, size.y), size, 4.0)
				- GtaoCaptureOraclePosition(projection, inverse, glm::dvec2(0), size, 4.0));
			const float pixelRadius = radius * static_cast<float>(size.y / verticalSpan);
			REQUIRE(pixelRadius < 8.0f); // all six rays sample mip zero; no pyramid approximation enters the oracle
			const auto fraction = [](float value)
			{
				return value - std::floor(value);
			};
			// Only the documented fixed sample locations and thickness relaxation are shared with the algorithm.
			const float rotation = fraction(52.9829189f * fraction(center.x * 0.06711056f + center.y * 0.00583715f));
			constexpr double Pi = 3.14159265358979323846;
			double visibility = 0.0;
			for (uint32_t slice = 0; slice < 3; ++slice)
			{
				const float angle = static_cast<float>(Pi) * (static_cast<float>(slice) + rotation) / 3.0f;
				const glm::vec2 direction(std::cos(angle), std::sin(angle));
				for (const float side : { -1.0f, 1.0f })
				{
					double horizon = -1.0;
					for (uint32_t step = 1; step <= 3; ++step)
					{
						const float offset = std::max(1.0f, pixelRadius * static_cast<float>(step * step) / 9.0f);
						const glm::uvec2 tap(glm::floor(glm::vec2(center) + direction * (side * offset)));
						const glm::dvec3 delta = position(tap) - receiver;
						const double distance = glm::length(delta);
						if (distance <= 1e-6 || delta.z <= 0.0)
							continue;
						const double attenuation = std::clamp((1.0 - distance / radius) * 5.0, 0.0, 1.0);
						const double candidate = -1.0 + (1.0 + delta.z / distance) * attenuation;
						horizon = candidate >= horizon ? candidate : 0.9 * horizon + 0.1 * candidate;
					}
					// Integrate the visible hemisphere numerically instead of using the shader's arc antiderivative.
					const double limit = std::min(std::acos(std::clamp(horizon, -1.0, 1.0)), Pi / 2.0);
					constexpr uint32_t Steps = 4096;
					for (uint32_t sample = 0; sample < Steps; ++sample)
					{
						const double theta = (sample + 0.5) * limit / Steps;
						visibility += std::cos(theta) * std::sin(theta) * limit / Steps / 3.0;
					}
				}
			}
			return visibility;
		}

		static RenderSnapshot MakeGtaoScene()
		{
			RenderSnapshot snapshot;
			snapshot.HasCamera = true;
			snapshot.Camera = MakeGtaoCamera(RenderProjection::Orthographic);
			snapshot.Post = { .Tonemap = RenderTonemapper::Linear, .SsaoRadius = 1.5f, .SsaoIntensity = 2.0f, .SsaoQuality = RenderSsaoQuality::High, .BloomEnabled = false, .FxaaEnabled = false };
			snapshot.Environment.FallbackColor = glm::vec3(0.2f);
			snapshot.Environment.ShowSkybox = false;
			for (const glm::vec3 position : { glm::vec3(0, 0, -4.0f), glm::vec3(0, 0, -3.7f) })
			{
				MeshDrawItem item;
				item.Mesh = BuiltinAssetHandles::CubeMesh;
				item.CastShadows = false;
				item.World = glm::translate(glm::mat4(1.0f), position);
				if (position.z == -4.0f)
					item.World = glm::scale(item.World, glm::vec3(12.0f, 10.0f, 0.1f));
				snapshot.Meshes.push_back(item);
			}
			return snapshot;
		}

		static Image RenderGtaoScene(GraphicsDevice& device, SceneRenderer& renderer, RenderSnapshot& snapshot)
		{
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE(list.has_value());
			(*list)->open();
			const Status result = renderer.Render(**list, snapshot);
			(*list)->close();
			const uint64_t submission = device.ExecuteCommandList(**list);
			renderer.OnSubmitted(snapshot.FrameIndex++, submission);
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			return ReadGtao(device, *renderer.GetFinalTexture());
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("GTAO: screen radius for perspective and orthographic")
		{
			CameraData camera = MakeGtaoCamera(RenderProjection::Perspective, 1920, 1080);
			const Result<float> near = ComputeGtaoScreenRadius(camera, 0.5f, 2.0f);
			const Result<float> far = ComputeGtaoScreenRadius(camera, 0.5f, 4.0f);
			REQUIRE(near.has_value());
			REQUIRE(far.has_value());
			CHECK(*near == doctest::Approx(0.5 * 540.0 * std::sqrt(3.0) / 2.0));
			CHECK(*near == *far * 2.0f);
			camera = MakeGtaoCamera(RenderProjection::Orthographic, 1920, 1080);
			CHECK(*ComputeGtaoScreenRadius(camera, 0.5f, 2.0f) == 67.5f);
			CHECK(*ComputeGtaoScreenRadius(camera, 0.5f, 20.0f) == 67.5f);
			CHECK_FALSE(ComputeGtaoScreenRadius(camera, -1.0f, 1.0f).has_value());
			CHECK_FALSE(ComputeGtaoScreenRadius(camera, 1.0f, 0.0f).has_value());
			CHECK_FALSE(ComputeGtaoScreenRadius(camera, std::numeric_limits<float>::infinity(), 1.0f).has_value());
			camera.OrthographicSize = 0.0f;
			CHECK_FALSE(ComputeGtaoScreenRadius(camera, 1.0f, 1.0f).has_value());
		}

		TEST_CASE("GTAO: quality and half-resolution settings choose the documented sample count")
		{
			CHECK(GetGtaoSliceCount(RenderSsaoQuality::Low) == 1);
			CHECK(GetGtaoSliceCount(RenderSsaoQuality::Medium) == 2);
			CHECK(GetGtaoSliceCount(RenderSsaoQuality::High) == 3);
			const nvrhi::TextureDesc odd = GtaoPass::GetTargetDesc(5, 3, true);
			CHECK(odd.width == 3);
			CHECK(odd.height == 2);
			CHECK(odd.format == nvrhi::Format::R8_UNORM);
			CHECK(odd.mipLevels == 1);
			CHECK(odd.isUAV);
			CHECK(GtaoPass::GetTargetDesc(1, 1, true).width == 1);
			CHECK(GtaoPass::GetTargetDesc(1, 5, true).height == 3);
			CHECK(GtaoPass::GetTargetDesc(5, 3, false).width == 5);
			CHECK(GtaoPass::GetLayoutDescriptions().size() == GtaoPass::PipelineCount);
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		TEST_CASE("GTAO: disabled and background are white and repeated projections are deterministic")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Result<Scope<GtaoPass>> pass = GtaoPass::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				for (const RenderProjection kind : { RenderProjection::Perspective, RenderProjection::Orthographic })
				{
					for (const RenderSsaoQuality quality : { RenderSsaoQuality::Low, RenderSsaoQuality::Medium, RenderSsaoQuality::High })
					{
						const CameraData camera = MakeGtaoCamera(kind, 17, 13);
						const size_t count = static_cast<size_t>(camera.ViewportWidth) * camera.ViewportHeight;
						std::vector<float> depths(count, 3.0f);
						std::fill(depths.begin() + count / 2, depths.end(), 65504.0f);
						GtaoTestTargets targets = MakeGtaoTargets(gpu, camera, depths, std::vector<glm::vec2>(count, glm::vec2(0.0f)), quality == RenderSsaoQuality::Low);
						PostProcessSettings post;
						post.SsaoQuality = quality;
						const Image first = RunGtao(gpu, **pass, camera, targets, post);
						const Image second = RunGtao(gpu, **pass, camera, targets, post);
						CHECK(first.Pixels == second.Pixels);
						for (const std::byte pixel : first.Pixels)
							CHECK(pixel == std::byte{ 255 });
						post.SsaoEnabled = false;
						const Image disabled = RunGtao(gpu, **pass, camera, targets, post);
						for (const std::byte pixel : disabled.Pixels)
							CHECK(pixel == std::byte{ 255 });
					}
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("GTAO: one-dimensional explicit half targets remain white and invalid inputs record nothing")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Result<Scope<GtaoPass>> pass = GtaoPass::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE(pass.has_value());
				for (const glm::uvec2 size : { glm::uvec2(1, 1), glm::uvec2(1, 5), glm::uvec2(5, 1) })
				{
					const CameraData camera = MakeGtaoCamera(RenderProjection::Orthographic, size.x, size.y);
					GtaoTestTargets targets = MakeGtaoTargets(gpu, camera, std::vector<float>(size.x * size.y, 3.0f),
						std::vector<glm::vec2>(size.x * size.y, glm::vec2(0.0f)), true);
					PostProcessSettings post;
					post.SsaoQuality = RenderSsaoQuality::High;
					const Image image = RunGtao(gpu, **pass, camera, targets, post, true);
					for (const std::byte pixel : image.Pixels)
						CHECK(pixel == std::byte{ 255 });
					GtaoInputs inputs{ .Camera = camera, .Post = post, .HalfResolution = true, .ViewDepth = targets.Depth, .SceneNormals = targets.Normals, .Occlusion = targets.Occlusion, .Scratch = targets.Scratch };
					Result<nvrhi::CommandListHandle> list = gpu.GetDevice().CreateCommandList();
					REQUIRE(list.has_value());
					PassBindingCache bindings;
					RenderStats stats;
					RenderRecordingContext recording(stats, []()
					{
						return 0.0;
					});
					(*list)->open();
					for (uint32_t invalid = 0; invalid < 8; ++invalid)
					{
						GtaoInputs bad = inputs;
						switch (invalid)
						{
							case 0: bad.Camera.NearClip = 0.0f; break;
							case 1: bad.Post.SsaoRadius = -1.0f; break;
							case 2: bad.Post.SsaoIntensity = std::numeric_limits<float>::quiet_NaN(); break;
							case 3: bad.Scratch = bad.Occlusion; break;
							case 4: bad.SceneNormals = nullptr; break;
							case 5: bad.Post.SsaoQuality = static_cast<RenderSsaoQuality>(255); break;
							case 6: bad.ViewDepth = targets.Occlusion; break;
							case 7: bad.Camera.ViewportWidth += 1; break;
						}
						const Result<nvrhi::ITexture*> rejected = (*pass)->Record(**list, recording, bindings, bad);
						REQUIRE_FALSE(rejected.has_value());
						CHECK(rejected.error().GetCode() == ErrorCode::InvalidArgument);
					}
					(*list)->close();
					gpu.GetDevice().ExecuteCommandList(**list);
					CHECK(stats.Passes.empty());
					CHECK(bindings.GetSize() == 0);
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		static AssetHandle PublishZeroGtaoMaterial(Test::InMemoryAssetManager & assets)
		{
			constexpr AssetHandle TextureHandle{ 0x9A01 };
			constexpr AssetHandle MaterialHandle{ 0x9A02 };
			TextureData texture;
			texture.Format = TextureFormat::R8Unorm;
			texture.Width = 1;
			texture.Height = 1;
			texture.Mips = { { .Width = 1, .Height = 1, .Size = 1 } };
			texture.Pixels = { std::byte{ 0 } };
			assets.Publish(TextureHandle, CreateRef<TextureData>(std::move(texture)));
			MaterialData material;
			material.BaseColor = glm::vec4(0.4f, 0.6f, 0.8f, 1.0f);
			material.OcclusionMap = TypedAssetHandle<AssetType::Texture>(TextureHandle);
			assets.Publish(MaterialHandle, CreateRef<MaterialData>(std::move(material)));
			return MaterialHandle;
		}

		TEST_CASE("GTAO: conical horizon matches an independent solid-angle oracle")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				const CameraData camera = MakeGtaoCamera(RenderProjection::Orthographic, 65, 65);
				std::vector<float> depths(65 * 65);
				for (uint32_t y = 0; y < 65; ++y)
				{
					for (uint32_t x = 0; x < 65; ++x)
					{
						const double dx = (static_cast<double>(x) - 32.0) * 8.0 / 65.0;
						const double dy = (static_cast<double>(y) - 32.0) * 8.0 / 65.0;
						depths[y * 65 + x] = static_cast<float>(std::max(0.1, 8.0 - std::hypot(dx, dy)));
					}
				}
				GtaoTestTargets targets = MakeGtaoTargets(gpu, camera, depths, std::vector<glm::vec2>(depths.size(), glm::vec2(0.0f)), false);
				GtaoConstants constants;
				constants.FullSize = glm::uvec2(65);
				constants.OutputSize = constants.FullSize;
				constants.PositionScale = glm::vec2(4.0f);
				constants.ProjectionKind = 1;
				constants.Radius = 1.5f;
				constants.RadiusScale = 65.0f / 8.0f;
				// Every tap uses mip zero here. The conical surface has a 45-degree horizon regardless of rotation.
				// The last tap lies outside the radius; the thickness heuristic relaxes cos(h) 10% toward -1.
				const double horizonCosine = 0.9 / std::sqrt(2.0) - 0.1;
				// Independent numerical solid-angle integral: integral cos(theta)*sin(theta) over the visible cap.
				constexpr uint32_t Steps = 10000;
				const double limit = std::acos(horizonCosine);
				double expected = 0.0;
				for (uint32_t i = 0; i < Steps; ++i)
				{
					const double theta = (i + 0.5) * limit / Steps;
					expected += 2.0 * std::cos(theta) * std::sin(theta) * limit / Steps;
				}
				for (uint32_t slices = 1; slices <= 3; ++slices)
				{
					constants.SliceCount = slices;
					DispatchGtaoKernel(gpu, "CSMain", constants, targets, nullptr, targets.Occlusion);
					const Image image = ReadGtao(gpu.GetDevice(), *targets.Occlusion);
					CHECK(GtaoByte(image, 32, 32) / 255.0 == doctest::Approx(expected).epsilon(0.02));
				}
				Result<Scope<GtaoPass>> pass = GtaoPass::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE(pass.has_value());
				for (const bool half : { false, true })
				{
					GtaoTestTargets repeatedTargets = MakeGtaoTargets(gpu, camera, depths,
						std::vector<glm::vec2>(depths.size(), glm::vec2(0.0f)), half);
					PostProcessSettings post;
					post.SsaoRadius = 1.5f;
					post.SsaoQuality = RenderSsaoQuality::High;
					const Image first = RunGtao(gpu, **pass, camera, repeatedTargets, post, half);
					const Image second = RunGtao(gpu, **pass, camera, repeatedTargets, post, half);
					CHECK(first.Pixels == second.Pixels);
					CHECK(GtaoByte(first, first.Width / 2, first.Height / 2) < 240);
					post.SsaoIntensity = 0.0f;
					const Image neutral = RunGtao(gpu, **pass, camera, repeatedTargets, post, half);
					for (const std::byte pixel : neutral.Pixels)
						CHECK(pixel == std::byte{ 255 });
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("GTAO: frozen orthographic projection reconstructs contact occlusion at a different capture aspect")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				const auto pass = GtaoPass::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE(pass);
				for (const uint32_t projectionWidth : { 16U, 64U, 256U })
				{
					INFO("frozen projection aspect ", projectionWidth, ":64; capture aspect 65:65");
					CameraData camera = MakeGtaoCamera(RenderProjection::Orthographic, 65, 65);
					camera.Projection = ComputeReverseZProjection(camera.ProjectionKind, camera.VerticalFov, camera.OrthographicSize,
						camera.NearClip, camera.FarClip, projectionWidth, 64);
					const glm::dmat4 projection(camera.Projection);
					const glm::dmat4 inverse = glm::inverse(projection);
					const glm::uvec2 size(camera.ViewportWidth, camera.ViewportHeight);
					std::vector<float> depths(size.x * size.y);
					std::vector<glm::vec2> normals(depths.size(), glm::vec2(0.0f));
					for (uint32_t y = 0; y < size.y; ++y)
					{
						for (uint32_t x = 0; x < size.x; ++x)
						{
							const glm::dvec3 point = GtaoCaptureOraclePosition(projection, inverse, glm::dvec2(x, y), size, 4.0);
							const double radial = std::hypot(point.x, point.y);
							// A +Z receiver touches a 45-degree conical occluder. Build geometry in frozen view space,
							// then store exactly the R16 depths consumed by the GPU, rather than a screen-space cone.
							depths[y * size.x + x] = glm::unpackHalf1x16(glm::packHalf1x16(static_cast<float>(std::max(0.1, 4.0 - radial))));
							if (radial > 1e-6)
							{
								const glm::dvec3 normal(-point.x / radial, -point.y / radial, 1.0);
								normals[y * size.x + x] = glm::vec2(normal) / static_cast<float>(std::abs(normal.x) + std::abs(normal.y) + 1.0);
							}
						}
					}
					PostProcessSettings post;
					post.SsaoRadius = 0.95f;
					post.SsaoIntensity = 1.0f;
					post.SsaoQuality = RenderSsaoQuality::High;
					const double expected = GtaoCaptureContactOracle(camera, depths, post.SsaoRadius);
					CHECK(expected > 0.45);
					CHECK(expected < 0.9); // require real contact occlusion, not a flat or empty white result
					for (const bool half : { false, true })
					{
						INFO("half resolution ", half);
						GtaoTestTargets targets = MakeGtaoTargets(gpu, camera, depths, normals, half);
						const Image image = RunGtao(gpu, **pass, camera, targets, post, half);
						const double actual = GtaoByte(image, image.Width / 2, image.Height / 2) / 255.0;
						// R8 quantization plus the two bilateral filters' small conical-normal contributions.
						INFO("actual visibility ", actual, "; oracle visibility ", expected);
						CHECK(std::abs(actual - expected) <= 2.0 / 255.0);
						CHECK(RunGtao(gpu, **pass, camera, targets, post, half).Pixels == image.Pixels);
					}
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("GTAO: denoise preserves depth edges and half resolution upsamples without halos")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			for (const bool normalEdge : { false, true })
			{
				const CameraData camera = MakeGtaoCamera(RenderProjection::Orthographic, 9, 5);
				std::vector<float> depths(45, 2.0f);
				std::vector<glm::vec2> normals(45, glm::vec2(0.0f));
				for (uint32_t y = 0; y < 5; ++y)
				{
					for (uint32_t x = 4; x < 9; ++x)
					{
						if (normalEdge)
							normals[y * 9 + x] = glm::vec2(1.0f, 0.0f); // +X, perpendicular to the left's +Z
						else
							depths[y * 9 + x] = 6.0f;
					}
				}
				GtaoTestTargets targets = MakeGtaoTargets(gpu, camera, depths, normals, true);
				std::vector<std::byte> field(15);
				for (uint32_t y = 0; y < 3; ++y)
				{
					for (uint32_t x = 0; x < 5; ++x)
						field[y * 5 + x] = x < 2 ? std::byte{ 51 } : std::byte{ 255 };
				}
				targets.Occlusion = UploadGtaoTexture(gpu.GetDevice(), GtaoPass::GetTargetDesc(9, 5, true), field);
				GtaoConstants constants;
				constants.FullSize = glm::uvec2(9, 5);
				constants.OutputSize = glm::uvec2(5, 3);
				DispatchGtaoKernel(gpu, "CSDenoise", constants, targets, targets.Occlusion, targets.Scratch);
				constants.Axis = 1;
				DispatchGtaoKernel(gpu, "CSDenoise", constants, targets, targets.Scratch, targets.Occlusion);
				CHECK(ReadGtao(gpu.GetDevice(), *targets.Occlusion).Pixels == field);
				Result<nvrhi::TextureHandle> full = gpu.GetDevice().CreateTexture(GtaoPass::GetTargetDesc(9, 5, false));
				REQUIRE(full.has_value());
				DispatchGtaoKernel(gpu, "CSTestUpsample", constants, targets, targets.Occlusion, *full);
				const Image upsampled = ReadGtao(gpu.GetDevice(), **full);
				for (uint32_t y = 0; y < 5; ++y)
				{
					for (uint32_t x = 0; x < 9; ++x)
						CHECK(GtaoByte(upsampled, x, y) == (x < 4 ? 51 : 255));
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("GTAO: both denoise axes match the separable spatial-filter oracle")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				const CameraData camera = MakeGtaoCamera(RenderProjection::Orthographic, 9, 9);
				GtaoTestTargets targets = MakeGtaoTargets(gpu, camera, std::vector<float>(81, 2.0f),
					std::vector<glm::vec2>(81, glm::vec2(0.0f)), false);
				std::vector<std::byte> field(81, std::byte{ 0 });
				field[40] = std::byte{ 255 };
				targets.Occlusion = UploadGtaoTexture(gpu.GetDevice(), GtaoPass::GetTargetDesc(9, 9, false), field);
				GtaoConstants constants;
				constants.FullSize = glm::uvec2(9);
				constants.OutputSize = constants.FullSize;
				DispatchGtaoKernel(gpu, "CSDenoise", constants, targets, targets.Occlusion, targets.Scratch);
				constants.Axis = 1;
				DispatchGtaoKernel(gpu, "CSDenoise", constants, targets, targets.Scratch, targets.Occlusion);
				const Image image = ReadGtao(gpu.GetDevice(), *targets.Occlusion);
				constexpr std::array<double, 9> Weights = { 0, 0, 1, 4, 6, 4, 1, 0, 0 };
				for (uint32_t y = 0; y < 9; ++y)
				{
					for (uint32_t x = 0; x < 9; ++x)
					{
						const double expected = std::round(std::round(255.0 * Weights[x] / 16.0) * Weights[y] / 16.0);
						CHECK(std::abs(static_cast<double>(GtaoByte(image, x, y)) - expected) <= 1.0);
					}
				}
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("GTAO: AO changes indirect light only and preserves direct lighting")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::InMemoryAssetManager assets;
				GpuResourceCache cache(gpu.GetDevice(), assets);
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets, { .Width = 65, .Height = 49 });
				REQUIRE(renderer.has_value());
				RenderSnapshot snapshot = MakeGtaoScene();
				snapshot.Post.SsaoEnabled = false;
				const Image ambientOff = RenderGtaoScene(gpu.GetDevice(), **renderer, snapshot);
				snapshot.Post.SsaoEnabled = true;
				const Image ambientOn = RenderGtaoScene(gpu.GetDevice(), **renderer, snapshot);
				uint64_t offSum = 0;
				uint64_t onSum = 0;
				for (size_t i = 0; i < ambientOn.Pixels.size(); i += 4)
				{
					offSum += std::to_integer<uint32_t>(ambientOff.Pixels[i]);
					onSum += std::to_integer<uint32_t>(ambientOn.Pixels[i]);
				}
				CHECK(onSum + 100 < offSum);
				// min(GTAO, materialAO=0) removes indirect light in both settings, including the multi-bounce term.
				const AssetHandle material = PublishZeroGtaoMaterial(assets);
				for (MeshDrawItem& mesh : snapshot.Meshes)
					mesh.Materials = { material };
				for (const bool enabled : { false, true })
				{
					snapshot.Post.SsaoEnabled = enabled;
					const Image zeroAo = RenderGtaoScene(gpu.GetDevice(), **renderer, snapshot);
					for (size_t i = 0; i < zeroAo.Pixels.size(); i += 4)
						CHECK(zeroAo.Pixels[i] == std::byte{ 0 });
				}
				snapshot.Environment.Intensity = 0.0f;
				snapshot.Lights.push_back({ .Type = RenderLightType::Directional, .Intensity = 0.25f, .Direction = glm::vec3(0, 0, -1), .CastShadows = false });
				const Image directOn = RenderGtaoScene(gpu.GetDevice(), **renderer, snapshot);
				snapshot.Post.SsaoEnabled = false;
				const Image directOff = RenderGtaoScene(gpu.GetDevice(), **renderer, snapshot);
				CHECK(directOn.Pixels == directOff.Pixels);
			}
			gpu.GetDevice().RunGarbageCollection();
		}

		TEST_CASE("DebugViews: AO displays final upsampled occlusion without material AO or post effects")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				Test::InMemoryAssetManager assets;
				GpuResourceCache cache(gpu.GetDevice(), assets);
				Result<Scope<SceneRendererPipelines>> pipelines = SceneRendererPipelines::Create(gpu.GetDevice(), gpu.GetPipelines());
				REQUIRE_MESSAGE(pipelines.has_value(), pipelines.error().ToString());
				Result<Scope<SceneRenderer>> renderer = SceneRenderer::Create(gpu.GetDevice(), **pipelines, cache, assets, { .Width = 65, .Height = 49 });
				REQUIRE(renderer.has_value());
				RenderSnapshot snapshot = MakeGtaoScene();
				snapshot.Quality.SsaoHalfResolution = true;
				snapshot.DebugView = RenderDebugView::AO;
				const Image initial = RenderGtaoScene(gpu.GetDevice(), **renderer, snapshot);
				CHECK(std::ranges::any_of(initial.Pixels, [](std::byte value)
				{
					return value != std::byte{ 255 };
				}));
				snapshot.Post.ExposureEV = 8.0f;
				snapshot.Post.BloomEnabled = true;
				snapshot.Post.FxaaEnabled = true;
				snapshot.Environment.Intensity = 20.0f;
				const AssetHandle material = PublishZeroGtaoMaterial(assets);
				for (MeshDrawItem& mesh : snapshot.Meshes)
					mesh.Materials = { material };
				snapshot.Lights.push_back({ .Type = RenderLightType::Directional, .Intensity = 12.0f, .Direction = glm::vec3(0, 0, -1), .CastShadows = false });
				const Image changed = RenderGtaoScene(gpu.GetDevice(), **renderer, snapshot);
				CHECK(initial.Pixels == changed.Pixels);
				snapshot.Post.SsaoEnabled = false;
				const Image disabled = RenderGtaoScene(gpu.GetDevice(), **renderer, snapshot);
				for (const std::byte value : disabled.Pixels)
					CHECK(value == std::byte{ 255 });
			}
			gpu.GetDevice().RunGarbageCollection();
		}
	}

}
