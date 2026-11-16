#include "TestsPCH.h"

#include "Engine/Renderer/SsrPass.h"

#include "Engine/Asset/EnvironmentData.h"
#include "Engine/Asset/MaterialData.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Graphics/Readback.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/DepthPyramidPass.h"
#include "Engine/Renderer/Private/SceneRendererIntegrationFixture.h"
#include "Engine/Renderer/RenderPassDeclaration.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/SceneRenderer.h"
#include "Engine/Renderer/SceneTargetFormats.h"
#include "Shared/SsrConstants.h"
#include "Support/DeviceLossTest.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/RenderReference.h"

#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <optional>
#include <utility>
#include <vector>

// The SSR pass (M26, C26.1, R-SSR-IN, R-SSR; Docs/Decisions/0037-m26-contract.md decision 18; stream X-D; Renderer/SsrPass.h):
// glossy screen-space reflections composited over the forward passes' probe specular, driven by synthetic snapshots of a
// mirror floor in front of an emissive wall inside a uniform reflection probe. A white metal's specular term is the reflected
// radiance itself (its multi-scatter specular albedo is 1, and nothing occludes it), so each floor pixel shows what its
// reflection reaches: the wall where the trace hits it, the probe where the ray misses. The renderer cases need the prepass's
// SceneRoughness target (decision 14, stream X-C).

namespace Engine {

	namespace {

		constexpr AssetHandle FloorMaterialHandle{ 0x9c01 };
		constexpr AssetHandle WallMaterialHandle{ 0x9c02 };
		constexpr AssetHandle ProbeEnvironmentHandle{ 0x9c03 };
		constexpr uint32_t ViewSize = 64;
		// The wall: the fixture's quad facing +Z, 3 m wide and 2 m high, standing on the floor (y = 0) at z = -1.
		constexpr float WallHalfWidth = 1.5f;
		constexpr float WallHeight = 2.0f;
		constexpr float WallZ = -1.0f;
		constexpr float FloorHalfExtent = 6.0f;
		// The probe's uniform radiance, which a floor pixel whose reflection misses shows.
		constexpr glm::dvec3 ProbeRadiance(0.0, 0.5, 0.0);

		// An environment whose prefiltered specular cube holds `radiance` at every texel of its four levels (binary16), with a
		// black 1² skybox and no irradiance: a uniform reflection probe's data.
		AssetRef<Asset> MakeUniformEnvironment(const glm::dvec3& radiance)
		{
			constexpr uint32_t FaceSize = 8;
			constexpr uint32_t MipCount = 4;
			EnvironmentData environment;
			environment.Skybox = { .FaceSize = 1, .MipCount = 1, .Texels = Buffer(ComputeCubeMapByteSize(1, 1), std::byte{ 0 }) };
			CubeMapData specular{ .FaceSize = FaceSize, .MipCount = MipCount, .Texels = Buffer(ComputeCubeMapByteSize(FaceSize, MipCount)) };
			const std::array<uint16_t, 4> texel = { Test::DoubleToHalf(radiance.x), Test::DoubleToHalf(radiance.y), Test::DoubleToHalf(radiance.z),
				Test::DoubleToHalf(1.0) };
			for (size_t offset = 0; offset < specular.Texels.size(); offset += sizeof(texel))
				std::memcpy(specular.Texels.data() + offset, texel.data(), sizeof(texel));
			environment.Specular = std::move(specular);
			return CreateRef<EnvironmentData>(std::move(environment));
		}

		AssetRef<Asset> MakeMaterial(const glm::vec3& baseColor, float metallic, float roughness, const glm::vec3& emissive)
		{
			MaterialData material;
			material.BaseColor = glm::vec4(baseColor, 1.0f);
			material.Metallic = metallic;
			material.Roughness = roughness;
			material.Emissive = emissive;
			return CreateRef<MaterialData>(std::move(material));
		}

		// The fixture of the mirror cases: a white metal floor of perceptual roughness 0 (0.045, its clamp) and a black, rough,
		// red-emitting wall, with the probe's environment.
		void PublishMirrorAssets(Test::SceneRendererIntegrationFixture& fixture)
		{
			fixture.Assets().Publish(FloorMaterialHandle, MakeMaterial(glm::vec3(1.0f), 1.0f, 0.0f, glm::vec3(0.0f)));
			fixture.Assets().Publish(WallMaterialHandle, MakeMaterial(glm::vec3(0.0f), 0.0f, 1.0f, glm::vec3(0.6f, 0.2f, 0.05f)));
			fixture.Assets().Publish(ProbeEnvironmentHandle, MakeUniformEnvironment(ProbeRadiance));
		}

		// The direction the camera looks along: down and towards -Z, 34 degrees below the horizon.
		glm::vec3 GetViewDirection()
		{
			return glm::normalize(glm::vec3(0.0f, -1.0f, -1.5f));
		}

		// The fixture's snapshot of the floor and the wall seen by a `projection` camera 10 m from (0, 0.8, -0.5) along the view
		// direction (an orthographic view 6 m high, a perspective one of 35 degrees), inside a 40 m reflection probe of the
		// uniform environment, with no light, no Environment map, a black ambient, the Linear tonemapper and SSR on with
		// PostProcess's defaults.
		RenderSnapshot MakeMirrorSnapshot(Test::SceneRendererIntegrationFixture& fixture, RenderProjection projection)
		{
			RenderSnapshot snapshot = fixture.Snapshot(projection);
			const glm::vec3 target(0.0f, 0.8f, -0.5f);
			snapshot.Camera.Position = target - GetViewDirection() * 10.0f;
			snapshot.Camera.View = glm::lookAt(snapshot.Camera.Position, target, glm::vec3(0.0f, 1.0f, 0.0f));
			snapshot.Camera.OrthographicSize = 3.0f;
			snapshot.Camera.NearClip = 0.1f;
			snapshot.Camera.FarClip = 30.0f;
			snapshot.Camera.Projection = ComputeReverseZProjection(projection, 35.0f, 3.0f, 0.1f, 30.0f, snapshot.Camera.ViewportWidth, snapshot.Camera.ViewportHeight);
			MeshDrawItem floor = snapshot.Meshes.front();
			floor.World = glm::mat4(glm::vec4(FloorHalfExtent, 0.0f, 0.0f, 0.0f), glm::vec4(0.0f, 0.0f, -FloorHalfExtent, 0.0f), glm::vec4(0.0f, FloorHalfExtent, 0.0f, 0.0f),
				glm::vec4(0.0f, 0.0f, 0.0f, 1.0f));
			floor.Materials = { FloorMaterialHandle };
			floor.CastShadows = false;
			MeshDrawItem wall = floor;
			wall.World = glm::translate(glm::mat4(1.0f), glm::vec3(0.0f, WallHeight / 2.0f, WallZ)) * glm::scale(glm::mat4(1.0f), glm::vec3(WallHalfWidth, WallHeight / 2.0f, 1.0f));
			wall.Materials = { WallMaterialHandle };
			wall.Entity = UUID(40);
			wall.PickId = 2;
			snapshot.Meshes = { floor, wall };
			snapshot.Environment = RenderEnvironment{ .Environment = {}, .Intensity = 1.0f, .ShowSkybox = false, .FallbackColor = glm::vec3(0.0f) };
			snapshot.ReflectionProbes.push_back(ReflectionProbeItem{ .Size = glm::vec3(40.0f), .Data = ProbeEnvironmentHandle, .BoxProjection = false, .BlendDistance = 0.0f, .Entity = UUID(41) });
			snapshot.Post.ExposureEV = 0.0f;
			snapshot.Post.SsrEnabled = true;
			return snapshot;
		}

		// What a pixel of the mirror snapshot shows, by its camera ray against the scene's planes: the wall, the floor reflecting
		// the wall (the point of the wall the reflection reaches in `hit`) or the probe, or a pixel left out (Skip): one near an
		// edge of the wall, the floor or the wall's reflection, near the wall's foot (where the ray's start is lifted off the
		// floor) or past the floor.
		enum class MirrorPixel : uint8_t
		{
			Wall,
			ReflectsWall,
			ReflectsProbe,
			Skip
		};

		MirrorPixel ClassifyMirrorPixel(const CameraData& camera, uint32_t x, uint32_t y)
		{
			constexpr double Margin = 0.15;
			const Result<RenderRay> ray = ComputeViewPixelRay(camera, x, y);
			REQUIRE_MESSAGE(ray.has_value(), ray.error().ToString());
			const glm::dvec3 origin(ray->Origin);
			const glm::dvec3 direction(ray->Direction);
			const double wallDistance = (WallZ - origin.z) / direction.z;
			const glm::dvec3 onWall = origin + direction * wallDistance;
			if (std::abs(onWall.x) <= WallHalfWidth + Margin && onWall.y >= -Margin && onWall.y <= WallHeight + Margin)
				return std::abs(onWall.x) <= WallHalfWidth - Margin && onWall.y >= Margin && onWall.y <= WallHeight - Margin ? MirrorPixel::Wall : MirrorPixel::Skip;
			const glm::dvec3 floor = origin + direction * (-origin.y / direction.y);
			if (std::max(std::abs(floor.x), std::abs(floor.z)) > FloorHalfExtent - 0.5)
				return MirrorPixel::Skip;
			const glm::dvec3 reflection(direction.x, -direction.y, direction.z);
			if (floor.z <= WallZ || reflection.z >= 0.0)
				return MirrorPixel::ReflectsProbe;
			const glm::dvec3 hit = floor + reflection * ((WallZ - floor.z) / reflection.z);
			if (hit.y < 0.3 || std::abs(std::abs(hit.x) - WallHalfWidth) < Margin || std::abs(hit.y - WallHeight) < Margin)
				return MirrorPixel::Skip;
			return std::abs(hit.x) < WallHalfWidth && hit.y < WallHeight ? MirrorPixel::ReflectsWall : MirrorPixel::ReflectsProbe;
		}

		glm::ivec3 GetPixel(const Image& image, uint32_t x, uint32_t y)
		{
			return { Test::SceneRendererIntegrationFixture::Channel(image, x, y, 0), Test::SceneRendererIntegrationFixture::Channel(image, x, y, 1),
				Test::SceneRendererIntegrationFixture::Channel(image, x, y, 2) };
		}

		// Whether every channel of `actual` is within `tolerance` of `expected`.
		bool IsNear(const glm::ivec3& actual, const glm::ivec3& expected, int tolerance)
		{
			return std::abs(actual.x - expected.x) <= tolerance && std::abs(actual.y - expected.y) <= tolerance && std::abs(actual.z - expected.z) <= tolerance;
		}

		// The 8-bit sRGB value the Linear tonemap writes for the linear `value`.
		int ToSrgb8(double value)
		{
			return static_cast<int>(std::lround(255.0 * Test::LinearToSrgb(std::clamp(value, 0.0, 1.0))));
		}

		// Whether the last render's stats list the row `name`.
		bool HasPass(SceneRenderer& renderer, std::string_view name)
		{
			return Test::SceneRendererIntegrationFixture::FindPass(renderer.GetRenderStats(), name) != nullptr;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("SsrPass: its declaration holds its three pipelines of the program Ssr, and SsrColor its traced reflections")
		{
			const auto ssr = std::find_if(ScenePassDeclarations.begin(), ScenePassDeclarations.end(), [](const RenderPassDeclaration& declaration)
			{
				return declaration.Name == "Ssr";
			});
			REQUIRE(ssr != ScenePassDeclarations.end());
			CHECK(ssr->Stage == RenderStage::Forward);
			CHECK(ssr->PipelineCount == SsrPass::PipelineCount);
			CHECK(SsrPass::PipelineCount == 3);
			REQUIRE(ssr->Programs.size() == 1);
			CHECK(ssr->Programs[0] == "Ssr");
			REQUIRE(ssr->StatsRows.size() == 1);
			CHECK(ssr->StatsRows[0] == "SSR");
			const std::vector<PipelineLayoutDescription> layouts = SsrPass::GetLayoutDescriptions();
			REQUIRE(layouts.size() == SsrPass::PipelineCount);
			CHECK(layouts[0].Entries == std::vector<std::string>{ "CSClassify" });
			CHECK(layouts[1].Entries == std::vector<std::string>{ "CSTrace" });
			CHECK(layouts[2].Entries == std::vector<std::string>{ "VSComposite", "PSComposite" });
			const nvrhi::TextureDesc target = SsrPass::GetTargetDesc(40, 20);
			CHECK(target.width == 40);
			CHECK(target.height == 20);
			CHECK(target.format == SsrColorFormat);
			CHECK(target.isUAV);
			CHECK(target.isShaderResource);
			// M32 (Docs/Decisions/0047-m32-fog-ssr.md decisions 10 and 11): the trace image is the scene image or its half rounded
			// up, cut into 8 x 8 tiles; the tile list holds one word a tile, the arguments their layout.
			CHECK(SsrPass::GetTraceSize(40, 21, false) == glm::uvec2(40, 21));
			CHECK(SsrPass::GetTraceSize(40, 21, true) == glm::uvec2(20, 11));
			CHECK(SsrPass::GetTileCount(glm::uvec2(40, 21)) == 5 * 3);
			CHECK(SsrPass::GetTileCount(glm::uvec2(20, 11)) == 3 * 2);
			const nvrhi::BufferDesc tiles = SsrPass::GetTileBufferDesc(15);
			CHECK(tiles.byteSize == 15 * sizeof(uint32_t));
			CHECK(tiles.structStride == sizeof(uint32_t));
			CHECK(tiles.canHaveUAVs);
			CHECK(SsrPass::GetTileBufferDesc(0).byteSize == sizeof(uint32_t));
			const nvrhi::BufferDesc arguments = SsrPass::GetArgumentsBufferDesc();
			CHECK(arguments.byteSize == SsrArgumentsLayout::ByteSize);
			CHECK(arguments.isDrawIndirectArgs);
			CHECK(arguments.canHaveRawViews);
			CHECK(SsrPass::GetAreaReadbackDesc().cpuAccess == nvrhi::CpuAccessMode::Read);
			const nvrhi::FramebufferInfo composite = SsrPass::GetCompositeFramebufferInfo();
			REQUIRE(composite.colorFormats.size() == 1);
			CHECK(composite.colorFormats[0] == SceneColorFormat);
			CHECK(composite.depthFormat == nvrhi::Format::UNKNOWN);
		}

		// M32 (decision 13): the traced area is read from the arguments buffer's words: the tile count, the listed tiles (the
		// trace's group count) and the traced pixels.
		TEST_CASE("SsrPass: the traced area is read from the arguments' tile count, listed tiles and traced pixels")
		{
			std::array<uint32_t, SsrArgumentsLayout::ByteSize / sizeof(uint32_t)> words = { 7, 1, 1, 6, 7, 0, 0, 300, 40 };
			const SsrArea area = ReadSsrArea(std::as_bytes(std::span(words)));
			CHECK(area == SsrArea{ .Tiles = 40, .TracedTiles = 7, .TracedPixels = 300 });
			CHECK(SsrArgumentsLayout::CompositeInstancesOffset == SsrArgumentsLayout::CompositeDrawOffset + sizeof(uint32_t));
		}

		// The push constants carry the image's size, the pyramid's mips and PostProcess's settings made usable.
		TEST_CASE("SsrPass: the push constants carry the image, the pyramid and the usable settings")
		{
			PostProcessSettings post;
			post.SsrIntensity = 0.75f;
			post.SsrMaxDistance = 20.0f;
			post.SsrThickness = 0.5f;
			post.SsrMaxRoughness = 0.4f;
			const SsrConstants plain = MakeSsrConstants(post, 40, 20, 5);
			CHECK(plain.ImageSize == glm::uvec2(40, 20));
			CHECK(plain.InverseImageSize == glm::vec2(1.0f / 40.0f, 1.0f / 20.0f));
			CHECK(plain.MaxDistance == 20.0f);
			CHECK(plain.Thickness == 0.5f);
			CHECK(plain.MaxRoughness == 0.4f);
			CHECK(plain.Intensity == 0.75f);
			CHECK(plain.PyramidLevels == 5);
			CHECK(plain.HalfResolution == 0);
			CHECK(MakeSsrConstants(post, 40, 20, 5, true).HalfResolution == 1);

			// Values past the registered ranges are clamped to them, and a pyramid of no mips counts one.
			post.SsrIntensity = 2.0f;
			post.SsrMaxDistance = 5000.0f;
			post.SsrThickness = 0.0f;
			post.SsrMaxRoughness = -1.0f;
			const SsrConstants clamped = MakeSsrConstants(post, 40, 20, 0);
			CHECK(clamped.Intensity == 1.0f);
			CHECK(clamped.MaxDistance == 1000.0f);
			CHECK(clamped.Thickness == 0.001f);
			CHECK(clamped.MaxRoughness == 0.0f);
			CHECK(clamped.PyramidLevels == 1);

			// Non-finite distances take the defaults; a non-finite intensity or roughness limit reflects nothing.
			post.SsrIntensity = std::nanf("");
			post.SsrMaxDistance = std::numeric_limits<float>::infinity();
			post.SsrThickness = std::nanf("");
			post.SsrMaxRoughness = std::numeric_limits<float>::infinity();
			const SsrConstants sanitized = MakeSsrConstants(post, 40, 20, 3);
			CHECK(sanitized.Intensity == 0.0f);
			CHECK(sanitized.MaxDistance == PostProcessSettings{}.SsrMaxDistance);
			CHECK(sanitized.Thickness == PostProcessSettings{}.SsrThickness);
			CHECK(sanitized.MaxRoughness == 0.0f);
		}

		TEST_CASE("SsrPass: Record refuses a missing input, a target of another size and a framebuffer other than SceneColor alone"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<Scope<SsrPass>> pass = SsrPass::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				CHECK((*pass)->GetPipelineCount() == SsrPass::PipelineCount);
				const auto texture = [&device](uint32_t width, uint32_t height, nvrhi::Format format, bool renderTarget)
				{
					nvrhi::TextureDesc desc;
					desc.width = width;
					desc.height = height;
					desc.format = format;
					desc.isShaderResource = true;
					desc.isRenderTarget = renderTarget;
					desc.initialState = format == SceneDepthFormat ? nvrhi::ResourceStates::DepthWrite
						: renderTarget                             ? nvrhi::ResourceStates::RenderTarget
																   : nvrhi::ResourceStates::ShaderResource;
					desc.keepInitialState = true;
					Result<nvrhi::TextureHandle> created = device.CreateTexture(desc);
					REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
					return *created;
				};
				const auto buffer = [&device](size_t size)
				{
					nvrhi::BufferDesc desc;
					desc.byteSize = size;
					desc.isConstantBuffer = true;
					desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
					desc.keepInitialState = true;
					Result<nvrhi::BufferHandle> created = device.CreateBuffer(desc);
					REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
					return *created;
				};
				const nvrhi::TextureHandle sceneColor = texture(8, 8, SceneColorFormat, true);
				const nvrhi::TextureHandle sceneDepth = texture(8, 8, SceneDepthFormat, true);
				const nvrhi::TextureHandle sceneNormals = texture(8, 8, SceneNormalsFormat, true);
				const nvrhi::TextureHandle sceneRoughness = texture(8, 8, SceneRoughnessFormat, true);
				Result<nvrhi::TextureHandle> viewDepth = device.CreateTexture(DepthPyramidPass::GetTargetDesc(8, 8));
				Result<nvrhi::TextureHandle> ssrColor = device.CreateTexture(SsrPass::GetTargetDesc(8, 8));
				Result<nvrhi::TextureHandle> smallSsrColor = device.CreateTexture(SsrPass::GetTargetDesc(4, 8));
				Result<nvrhi::BufferHandle> tiles = device.CreateBuffer(SsrPass::GetTileBufferDesc(1));
				Result<nvrhi::BufferHandle> arguments = device.CreateBuffer(SsrPass::GetArgumentsBufferDesc());
				Result<nvrhi::FramebufferHandle> framebuffer = device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(sceneColor));
				Result<nvrhi::FramebufferHandle> withDepth =
					device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(sceneColor).setDepthAttachment(sceneDepth));
				const nvrhi::TextureHandle otherSceneColor = texture(8, 8, SceneColorFormat, true);
				Result<nvrhi::FramebufferHandle> otherColor = device.CreateFramebuffer(nvrhi::FramebufferDesc().addColorAttachment(otherSceneColor));
				REQUIRE(viewDepth.has_value());
				REQUIRE(ssrColor.has_value());
				REQUIRE(smallSsrColor.has_value());
				REQUIRE((tiles.has_value() && arguments.has_value()));
				REQUIRE(framebuffer.has_value());
				REQUIRE(withDepth.has_value());
				REQUIRE(otherColor.has_value());
				const nvrhi::BufferHandle constants = buffer(4096);
				SsrPassInputs valid{
					.ViewConstants = constants,
					.EnvironmentConstants = constants,
					.ProbeConstants = constants,
					.FogConstants = constants,
					.SceneDepth = sceneDepth,
					.SceneNormals = sceneNormals,
					.SceneRoughness = sceneRoughness,
					.ViewDepth = *viewDepth,
					.SceneColor = sceneColor,
					.Framebuffer = *framebuffer,
					.SsrColor = *ssrColor,
					.Tiles = *tiles,
					.Arguments = *arguments,
					.AmbientOcclusion = sceneNormals,
					.LinearViewDepth = sceneNormals,
					.EnvironmentSpecular = sceneNormals,
					.ReflectionProbeCubes = sceneNormals,
				};
				std::vector<SsrPassInputs> refused(9, valid);
				refused[0] = SsrPassInputs{};
				refused[1].SceneRoughness = nullptr; // the prepass target X-C adds (decision 14)
				refused[2].SsrColor = *smallSsrColor;
				refused[3].Framebuffer = *withDepth;
				refused[4].Framebuffer = *otherColor;
				refused[5].FogConstants = nullptr; // M32's fog (Docs/Decisions/0047-m32-fog-ssr.md decision 6)
				refused[6].Arguments = nullptr;    // M32's classification (decision 10)
				refused[7].HalfResolution = true;  // SsrColor of the scene's size, not the half trace image's (decision 11)
				refused[8].Arguments = buffer(16); // smaller than the arguments' layout
				Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
				REQUIRE(list.has_value());
				(*list)->open();
				PassBindingCache bindings;
				for (size_t index = 0; index < refused.size(); ++index)
				{
					CAPTURE(index);
					RenderPassCounters counters;
					const Status recorded = (*pass)->Record(**list, bindings, refused[index], counters);
					REQUIRE_FALSE(recorded.has_value());
					CHECK(recorded.error().GetCode() == ErrorCode::InvalidArgument);
					CHECK(counters.Dispatches == 0);
					CHECK(counters.DrawCalls == 0);
				}
				(*list)->close();
				device.ExecuteCommandList(**list);
				device.WaitForIdle();
				CHECK(bindings.GetSize() == 0);
			}
			device.RunGarbageCollection();
		}

		// The acceptance (decision 18): SSR off records nothing, so the image is byte-identical to the render without the pass;
		// a render with SSR on between two renders with it off leaves nothing behind.
		TEST_CASE("SceneRenderer: a render with SSR off records no SSR pass and equals the render before it" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, ViewSize, ViewSize);
			PublishMirrorAssets(fixture);
			RenderSnapshot snapshot = MakeMirrorSnapshot(fixture, RenderProjection::Perspective);
			snapshot.Post.SsrEnabled = false;
			const Image before = fixture.Render(snapshot);
			// SSR off records neither its pass nor the depth pyramid it traces over (GTAO is off).
			CHECK_FALSE(HasPass(fixture.Renderer(), "SSR"));
			CHECK_FALSE(HasPass(fixture.Renderer(), "DepthPyramid"));

			snapshot.Post.SsrEnabled = true;
			const Image reflected = fixture.Render(snapshot);
			CHECK(HasPass(fixture.Renderer(), "SSR"));
			CHECK(HasPass(fixture.Renderer(), "DepthPyramid"));
			CHECK(reflected.Pixels != before.Pixels);

			snapshot.Post.SsrEnabled = false;
			const Image after = fixture.Render(snapshot);
			CHECK_FALSE(HasPass(fixture.Renderer(), "SSR"));
			CHECK(after.Pixels == before.Pixels);
		}

		// Decision 32 (c) (W26.2's review): SSR runs only in a Lit view outside the wireframe, so a material debug view and the
		// wireframe record no SSR pass with SsrEnabled and show what they show with it off.
		TEST_CASE("SceneRenderer: SSR is off in the debug views and the wireframe" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, ViewSize, ViewSize);
			PublishMirrorAssets(fixture);
			RenderSnapshot lit = MakeMirrorSnapshot(fixture, RenderProjection::Perspective);
			RenderSnapshot albedo = lit;
			albedo.DebugView = RenderDebugView::Albedo;
			RenderSnapshot wireframe = lit;
			wireframe.Flags = RenderViewFlags::Wireframe;
			for (RenderSnapshot* snapshot : { &albedo, &wireframe })
			{
				CAPTURE(snapshot == &albedo);
				snapshot->Post.SsrEnabled = false;
				const Image off = fixture.Render(*snapshot);
				snapshot->Post.SsrEnabled = true;
				const Image on = fixture.Render(*snapshot);
				CHECK_FALSE(HasPass(fixture.Renderer(), "SSR"));
				CHECK(on.Pixels == off.Pixels);
			}
			// The Lit view of the same snapshot traces.
			static_cast<void>(fixture.Render(lit));
			CHECK(HasPass(fixture.Renderer(), "SSR"));
		}

		// The acceptance (decision 18): each floor pixel of a mirror shows what its reflection reaches, the wall's own pixels
		// where the trace hits the wall and the probe's radiance where the ray misses (it leaves the screen), in an orthographic
		// and a perspective view; the SSR row records one trace and one composite.
		TEST_CASE("SsrPass: a mirror floor reflects what the trace hits and the probe where it misses" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, ViewSize, ViewSize);
			PublishMirrorAssets(fixture);
			const glm::ivec3 probe(ToSrgb8(ProbeRadiance.x), ToSrgb8(ProbeRadiance.y), ToSrgb8(ProbeRadiance.z));
			for (const RenderProjection projection : { RenderProjection::Orthographic, RenderProjection::Perspective })
			{
				const bool orthographic = projection == RenderProjection::Orthographic;
				CAPTURE(orthographic);
				const RenderSnapshot snapshot = MakeMirrorSnapshot(fixture, projection);
				const Image image = fixture.Render(snapshot);
				const RenderStats stats = fixture.Renderer().GetRenderStats();
				const RenderPassStats* ssr = Test::SceneRendererIntegrationFixture::FindPass(stats, "SSR");
				REQUIRE(ssr != nullptr);
				CHECK(ssr->Dispatches == 2);
				CHECK(ssr->DrawCalls == 1);

				// The wall is one colour (its emission and the probe's faint reflection at roughness 1, which SSR leaves alone).
				std::optional<glm::ivec3> wall;
				uint32_t hits = 0;
				uint32_t misses = 0;
				for (uint32_t y = 0; y < ViewSize; ++y)
				{
					for (uint32_t x = 0; x < ViewSize; ++x)
					{
						if (ClassifyMirrorPixel(snapshot.Camera, x, y) != MirrorPixel::Wall)
							continue;
						CAPTURE(x);
						CAPTURE(y);
						if (!wall)
							wall = GetPixel(image, x, y);
						CHECK(IsNear(GetPixel(image, x, y), *wall, 1));
					}
				}
				REQUIRE(wall.has_value());
				CHECK(wall->x > 150);
				for (uint32_t y = 0; y < ViewSize; ++y)
				{
					for (uint32_t x = 0; x < ViewSize; ++x)
					{
						const MirrorPixel kind = ClassifyMirrorPixel(snapshot.Camera, x, y);
						if (kind != MirrorPixel::ReflectsWall && kind != MirrorPixel::ReflectsProbe)
							continue;
						CAPTURE(x);
						CAPTURE(y);
						const bool hit = kind == MirrorPixel::ReflectsWall;
						CHECK(IsNear(GetPixel(image, x, y), hit ? *wall : probe, 2));
						hits += hit ? 1 : 0;
						misses += hit ? 0 : 1;
					}
				}
				CHECK(hits > 100);
				CHECK(misses > 100);
			}
		}

		// M32 (Docs/Decisions/0047-m32-fog-ssr.md decision 6): in a fogged view the composite's addition passes through the fog in
		// front of the floor, so the floor and its reflection fog as one surface. A fog dense enough that every pixel of the
		// view has its MaxOpacity m: with SSR off a floor pixel is P (1 - m) + F m (P the probe's radiance), the wall W (1 - m)
		// + F m, and with SSR on a floor pixel that reflects the wall adds what it traced, the fogged wall, less P, times 1 - m;
		// with m = 1 the reflection adds nothing at all.
		TEST_CASE("SsrPass: in a fogged view the reflection is fogged with the surface it lies on" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, ViewSize, ViewSize);
			PublishMirrorAssets(fixture);
			const auto readSceneColor = [&gpu, &fixture]()
			{
				Readback readback(gpu.GetDevice());
				Result<Image> image = readback.ReadTexture(*fixture.Renderer().GetSceneColorTexture());
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				return std::move(*image);
			};
			const auto radiance = [](const Image& image, uint32_t x, uint32_t y)
			{
				std::array<uint16_t, 4> halves{};
				std::memcpy(halves.data(), image.GetRow(y).data() + static_cast<size_t>(x) * sizeof(halves), sizeof(halves));
				return glm::dvec3(Test::HalfToDouble(halves[0]), Test::HalfToDouble(halves[1]), Test::HalfToDouble(halves[2]));
			};
			RenderSnapshot snapshot = MakeMirrorSnapshot(fixture, RenderProjection::Perspective);
			snapshot.Post.SsrEnabled = false;
			fixture.Submit(snapshot);
			const Image clear = readSceneColor();
			constexpr double Opacity = 0.6;
			const glm::dvec3 fogColor(0.4, 0.4, 0.5);
			snapshot.Fog = RenderFog{ .Enabled = true, .Color = glm::vec3(fogColor), .Density = 5.0f, .MaxOpacity = static_cast<float>(Opacity), .SunGlow = 0.0f };
			fixture.Submit(snapshot);
			const Image fogged = readSceneColor();
			snapshot.Post.SsrEnabled = true;
			fixture.Submit(snapshot);
			const Image reflected = readSceneColor();

			uint32_t compared = 0;
			for (uint32_t y = 0; y < ViewSize; ++y)
			{
				for (uint32_t x = 0; x < ViewSize; ++x)
				{
					const MirrorPixel kind = ClassifyMirrorPixel(snapshot.Camera, x, y);
					CAPTURE(x);
					CAPTURE(y);
					if (kind == MirrorPixel::Wall)
					{
						const glm::dvec3 expected = radiance(clear, x, y) * (1.0 - Opacity) + fogColor * Opacity;
						CHECK(glm::length(radiance(fogged, x, y) - expected) < 5e-3);
					}
					if (kind != MirrorPixel::ReflectsWall)
						continue;
					// The fogged wall the trace reads: the wall's own radiance at a pixel of the clear render's wall.
					const glm::dvec3 probe = radiance(clear, x, y);
					CHECK(glm::length(probe - ProbeRadiance) < 5e-3);
					const glm::dvec3 floorOff = probe * (1.0 - Opacity) + fogColor * Opacity;
					CHECK(glm::length(radiance(fogged, x, y) - floorOff) < 5e-3);
					++compared;
				}
			}
			CHECK(compared > 100);
			// The reflection's addition: what the floor gains with SSR on is (the fogged wall - P) (1 - m), so the floor's two
			// fogged images differ by 1 - m times what the trace reads less the probe.
			std::optional<glm::dvec3> wall;
			for (uint32_t y = 0; y < ViewSize && !wall; ++y)
			{
				for (uint32_t x = 0; x < ViewSize && !wall; ++x)
				{
					if (ClassifyMirrorPixel(snapshot.Camera, x, y) == MirrorPixel::Wall)
						wall = radiance(fogged, x, y);
				}
			}
			REQUIRE(wall.has_value());
			uint32_t near = 0;
			for (uint32_t y = 0; y < ViewSize; ++y)
			{
				for (uint32_t x = 0; x < ViewSize; ++x)
				{
					if (ClassifyMirrorPixel(snapshot.Camera, x, y) != MirrorPixel::ReflectsWall)
						continue;
					const glm::dvec3 gained = radiance(reflected, x, y) - radiance(fogged, x, y);
					const glm::dvec3 expected = (*wall - ProbeRadiance) * (1.0 - Opacity);
					near += glm::length(gained - expected) < 2e-2 ? 1U : 0U;
				}
			}
			CHECK(near > 100);

			// A fog that hides everything leaves nothing for the reflection to add.
			snapshot.Fog.MaxOpacity = 1.0f;
			snapshot.Post.SsrEnabled = false;
			fixture.Submit(snapshot);
			const Image hiddenOff = readSceneColor();
			snapshot.Post.SsrEnabled = true;
			fixture.Submit(snapshot);
			CHECK(HasPass(fixture.Renderer(), "SSR"));
			CHECK(readSceneColor().Pixels == hiddenOff.Pixels);
		}

		// M32 (Docs/Decisions/0047-m32-fog-ssr.md decisions 10 and 13): the classification lists exactly the tiles that hold a
		// tracing pixel, and the renderer reports them for a completed frame. The mirror snapshot's floor traces (its roughness is
		// below SsrMaxRoughness), its rough wall and the sky do not; SceneRoughness read back gives each pixel's test, so the
		// expected tiles and pixels follow from it, at full and at half resolution (where a trace pixel tests the scene pixel at
		// its footprint's centre), and the stats of a render after the frame completed report them exactly.
		TEST_CASE("SsrPass: the trace covers only the tiles that hold a tracing pixel, which the stats report" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, ViewSize, ViewSize);
			PublishMirrorAssets(fixture);
			// The frame indices keep rising across both renders' frames, so the history never takes the second for an older one.
			uint64_t frameIndex = 100;
			for (const bool half : { false, true })
			{
				CAPTURE(half);
				RenderSnapshot snapshot = MakeMirrorSnapshot(fixture, RenderProjection::Perspective);
				snapshot.Quality.SsrHalfResolution = half;
				snapshot.FrameIndex = ++frameIndex;
				fixture.Submit(snapshot);
				Readback readback(gpu.GetDevice());
				const Result<Image> roughness = readback.ReadTexture(*fixture.Renderer().GetSceneRoughnessTexture());
				REQUIRE_MESSAGE(roughness.has_value(), roughness.error().ToString());
				const uint32_t traceSize = half ? (ViewSize + 1) / 2 : ViewSize;
				const auto traces = [&roughness, &snapshot, half, traceSize](uint32_t x, uint32_t y)
				{
					const uint32_t sceneX = half ? std::min((2 * x + 1) * ViewSize / (2 * traceSize), ViewSize - 1) : x;
					const uint32_t sceneY = half ? std::min((2 * y + 1) * ViewSize / (2 * traceSize), ViewSize - 1) : y;
					uint32_t word = 0;
					std::memcpy(&word, roughness->GetRow(sceneY).data() + static_cast<size_t>(sceneX) * 8, sizeof(word));
					const float value = static_cast<float>(word >> 24) / 255.0f;
					return (word & 0x00FFFFFFu) != 0 && !(value > snapshot.Post.SsrMaxRoughness);
				};
				uint32_t expectedTiles = 0;
				uint32_t expectedPixels = 0;
				for (uint32_t tileY = 0; tileY < traceSize; tileY += SsrPass::TileSize)
				{
					for (uint32_t tileX = 0; tileX < traceSize; tileX += SsrPass::TileSize)
					{
						uint32_t traced = 0;
						for (uint32_t y = tileY; y < std::min(tileY + SsrPass::TileSize, traceSize); ++y)
						{
							for (uint32_t x = tileX; x < std::min(tileX + SsrPass::TileSize, traceSize); ++x)
								traced += traces(x, y) ? 1U : 0U;
						}
						expectedTiles += traced > 0 ? 1U : 0U;
						expectedPixels += traced;
					}
				}
				CHECK(expectedTiles > 0);
				CHECK(expectedTiles < SsrPass::GetTileCount(glm::uvec2(traceSize)));
				// Render until a render collects a completed frame of this snapshot that recorded SSR: its stats carry its area.
				const uint64_t first = snapshot.FrameIndex;
				std::optional<RenderStats> reported;
				for (uint32_t frame = 0; frame < 3 * gpu.GetDevice().GetFramesInFlight() + 2 && !reported; ++frame)
				{
					snapshot.FrameIndex = ++frameIndex;
					fixture.Submit(snapshot);
					gpu.GetDevice().WaitForIdle();
					const RenderStats stats = fixture.Renderer().GetRenderStats();
					if (stats.SsrAreaAvailable && stats.SsrAreaFrameIndex >= first)
						reported = stats;
				}
				REQUIRE(reported.has_value());
				CHECK(reported->SsrHalfResolution == half);
				CHECK(reported->SsrTiles == SsrPass::GetTileCount(glm::uvec2(traceSize)));
				CHECK(reported->SsrTracedTiles == expectedTiles);
				CHECK(reported->SsrTracedPixels == expectedPixels);
			}
		}

		// The probe fallback is exact where nothing is traced: a floor rougher than SsrMaxRoughness traces no ray, and an
		// SsrIntensity of 0 replaces nothing, so either image equals the render with SSR off, while the pass still records.
		TEST_CASE("SsrPass: a surface rougher than SsrMaxRoughness, or an SsrIntensity of 0, keeps the probes' reflection exactly"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, ViewSize, ViewSize);
			PublishMirrorAssets(fixture);
			RenderSnapshot snapshot = MakeMirrorSnapshot(fixture, RenderProjection::Orthographic);
			snapshot.Post.SsrEnabled = false;
			const Image off = fixture.Render(snapshot);
			snapshot.Post.SsrEnabled = true;
			snapshot.Post.SsrMaxRoughness = 0.02f; // below the floor's 0.045
			const Image rough = fixture.Render(snapshot);
			CHECK(HasPass(fixture.Renderer(), "SSR"));
			CHECK(rough.Pixels == off.Pixels);
			// M32 (decision 10): no tile is listed, so the trace and the composite draw nothing; the stats of a later render report
			// the completed frame's empty area.
			const uint64_t first = snapshot.FrameIndex;
			std::optional<RenderStats> reported;
			for (uint32_t frame = 0; frame < 3 * gpu.GetDevice().GetFramesInFlight() + 2 && !reported; ++frame)
			{
				snapshot.FrameIndex += 1;
				CHECK(fixture.Render(snapshot).Pixels == off.Pixels);
				const RenderStats stats = fixture.Renderer().GetRenderStats();
				if (stats.SsrAreaAvailable && stats.SsrAreaFrameIndex >= first)
					reported = stats;
			}
			REQUIRE(reported.has_value());
			CHECK(reported->SsrTiles > 0);
			CHECK(reported->SsrTracedTiles == 0);
			CHECK(reported->SsrTracedPixels == 0);
			snapshot.Post.SsrMaxRoughness = PostProcessSettings{}.SsrMaxRoughness;
			snapshot.Post.SsrIntensity = 0.0f;
			const Image faded = fixture.Render(snapshot);
			CHECK(HasPass(fixture.Renderer(), "SSR"));
			CHECK(faded.Pixels == off.Pixels);
			// With the intensity back, the floor reflects the wall.
			snapshot.Post.SsrIntensity = 1.0f;
			CHECK(fixture.Render(snapshot).Pixels != off.Pixels);
		}

		// M32 (decision 11): the half-resolution trace reflects what the full one does: on the mirror floor nine pixels in ten
		// within six levels of the full-resolution image, the rest along the reflection's edges, where the upsample's taps mix
		// the wall and the probe; it differs from the full-resolution image, and two renders of it are equal.
		TEST_CASE("SsrPass: a half-resolution trace reflects what the full one does" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, ViewSize, ViewSize);
			PublishMirrorAssets(fixture);
			for (const RenderProjection projection : { RenderProjection::Orthographic, RenderProjection::Perspective })
			{
				CAPTURE(static_cast<int>(projection));
				RenderSnapshot snapshot = MakeMirrorSnapshot(fixture, projection);
				const Image full = fixture.Render(snapshot);
				snapshot.Quality.SsrHalfResolution = true;
				const Image half = fixture.Render(snapshot);
				CHECK(half.Pixels != full.Pixels);
				CHECK(fixture.Render(snapshot).Pixels == half.Pixels);
				uint32_t compared = 0;
				uint32_t near = 0;
				for (uint32_t y = 0; y < ViewSize; ++y)
				{
					for (uint32_t x = 0; x < ViewSize; ++x)
					{
						const MirrorPixel kind = ClassifyMirrorPixel(snapshot.Camera, x, y);
						if (kind != MirrorPixel::ReflectsWall && kind != MirrorPixel::ReflectsProbe)
							continue;
						++compared;
						near += IsNear(GetPixel(half, x, y), GetPixel(full, x, y), 6) ? 1U : 0U;
					}
				}
				CAPTURE(compared);
				CAPTURE(near);
				CHECK(compared > 200);
				CHECK(near * 10 >= compared * 9);
			}
		}

		TEST_CASE("SsrPass: a renderer with SSR recovers from a device loss with the same image" * doctest::test_suite(Test::GpuSuite))
		{
			// The SSR pass's pipelines are the set's (place 1), SsrColor, its framebuffer and the depth pyramid the view's targets
			// (place 2): a loss drops them, and the first render after the recovery creates them again.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Test::SceneRendererIntegrationFixture fixture(gpu, ViewSize, ViewSize);
			PublishMirrorAssets(fixture);
			RenderSnapshot snapshot = MakeMirrorSnapshot(fixture, RenderProjection::Perspective);
			GraphicsDevice& device = gpu.GetDevice();
			uint64_t frame = 90;
			Test::DeviceLossTest test;
			test.Owners = { "GpuResourceCache", "SceneRendererPipelines", "SceneRenderer" };
			test.Use = [&device, &fixture, &snapshot, &frame]() -> Result<std::vector<std::byte>>
			{
				std::vector<std::byte> pixels;
				for (uint32_t slot = 0; slot < device.GetFramesInFlight(); ++slot)
				{
					snapshot.FrameIndex = ++frame;
					ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, device.CreateCommandList());
					list->open();
					const Status rendered = fixture.Renderer().Render(*list, snapshot);
					list->close();
					fixture.Renderer().OnSubmitted(snapshot.FrameIndex, device.ExecuteCommandList(*list));
					ENGINE_TRY(rendered);
					nvrhi::ITexture* image = fixture.Renderer().GetFinalTexture();
					if (image == nullptr)
						return MakeError(ErrorCode::InvalidState, "the renderer has no final texture");
					Readback readback(device);
					ENGINE_TRY_ASSIGN(Image read, readback.ReadTexture(*image));
					pixels = std::move(read.Pixels);
				}
				return pixels;
			};
			// The view's targets reach their steady state in the pool first, as the other renderer loss cases do.
			for (uint32_t warmUp = 0; warmUp <= RenderTargetPool::DefaultKeepFrames; ++warmUp)
			{
				const Result<std::vector<std::byte>> steady = test.Use();
				REQUIRE_MESSAGE(steady.has_value(), steady.error().ToString());
			}
			const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
			REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
			CHECK(HasPass(fixture.Renderer(), "SSR"));
		}
	}

}
