#include "TestsPCH.h"

#include "Engine/Renderer/TrianglePass.h"

#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/Readback.h"
#include "Support/HeadlessGpuFixture.h"

#include <glm/glm.hpp>

namespace Engine {

	// The RGBA8 pixel at (x, y) of `image`.
	static std::array<uint8_t, 4> GetPixel(const Image& image, uint32_t x, uint32_t y)
	{
		const std::span<const std::byte> row = image.GetRow(y);
		std::array<uint8_t, 4> pixel{};
		for (size_t channel = 0; channel < 4; ++channel)
			pixel[channel] = static_cast<uint8_t>(row[static_cast<size_t>(x) * 4 + channel]);
		return pixel;
	}

	// TriangleClearColor as the UNORM target stores it.
	static std::array<uint8_t, 4> GetClearPixel()
	{
		std::array<uint8_t, 4> clear{};
		for (size_t channel = 0; channel < clear.size(); ++channel)
			clear[channel] = static_cast<uint8_t>(TriangleClearColor[channel] * 255.0f + 0.5f);
		return clear;
	}

	// Renders the triangle view into a 64x64 RGBA8 target (with a D32 depth attachment when `depth`) and reads it back.
	static Image RenderTriangle(Test::HeadlessGpuFixture& gpu, nvrhi::RasterCullMode cullMode, bool depth)
	{
		GraphicsDevice& device = gpu.GetDevice();
		Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = 64, .Height = 64, .Depth = depth });
		REQUIRE_MESSAGE(target.has_value(), target.error().ToString());

		nvrhi::FramebufferInfo framebuffer;
		framebuffer.addColorFormat(nvrhi::Format::RGBA8_UNORM);
		if (depth)
			framebuffer.setDepthFormat(target->GetDepthTexture()->getDesc().format);
		Result<Scope<TrianglePass>> pass = TrianglePass::Create(device, gpu.GetPipelines(), { .Framebuffer = framebuffer, .CullMode = cullMode });
		REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());

		Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
		REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
		(*commandList)->open();
		(*pass)->Render(**commandList, *target->GetFramebuffer());
		(*commandList)->close();
		device.ExecuteCommandList(**commandList);

		Readback readback(device);
		Result<Image> image = readback.ReadTexture(*target->GetColorTexture());
		REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
		return std::move(*image);
	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("TrianglePass: the fixed camera is orthographic and reverse-Z with clip-space +Y up")
		{
			const ViewConstants view = TrianglePass::MakeViewConstants(640, 360);
			CHECK(view.ProjectionKind == ProjectionKindOrthographic);
			CHECK(view.OrthoHalfExtents.x == doctest::Approx(640.0 / 360.0));
			CHECK(view.OrthoHalfExtents.y == doctest::Approx(1.0));
			CHECK(view.ViewportSize == glm::vec2(640.0f, 360.0f));
			CHECK(Test::ApproxEqual(view.InverseViewportSize, glm::vec2(1.0f / 640.0f, 1.0f / 360.0f)));
			CHECK(view.AspectRatio == doctest::Approx(640.0 / 360.0));
			CHECK(view.Near == doctest::Approx(0.1));
			CHECK(view.Far == doctest::Approx(100.0));
			CHECK(view.CameraPosition == glm::vec3(0.0f, 0.0f, 5.0f));
			CHECK(view.Exposure == 1.0f);

			// A point on the near plane maps to depth 1, one on the far plane to depth 0 (reverse-Z, §8.3). +Y in the view is
			// +Y in clip space: NVRHI's Vulkan viewport has a negative height, which performs the Vulkan Y flip, so a
			// projection that flipped Y too would render upside down with CCW triangles as back faces.
			const glm::vec4 nearPoint = view.ViewProjection * glm::vec4(0.0f, 0.0f, 5.0f - view.Near, 1.0f);
			const glm::vec4 farPoint = view.ViewProjection * glm::vec4(0.0f, 0.0f, 5.0f - view.Far, 1.0f);
			CHECK(nearPoint.z / nearPoint.w == doctest::Approx(1.0).epsilon(1e-5));
			CHECK(farPoint.z / farPoint.w == doctest::Approx(0.0).epsilon(1e-5));
			const glm::vec4 up = view.ViewProjection * glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
			CHECK(up.y / up.w > 0.0f);
			// The view volume's edges land on the clip-space edges.
			const glm::vec4 corner = view.ViewProjection * glm::vec4(view.OrthoHalfExtents.x, view.OrthoHalfExtents.y, 0.0f, 1.0f);
			CHECK(Test::ApproxEqual(glm::vec2(corner) / corner.w, glm::vec2(1.0f, 1.0f)));
			// The inverses invert.
			const glm::mat4 identity = view.ViewProjection * view.InverseViewProjection;
			CHECK(Test::ApproxEqual(identity, glm::mat4(1.0f), 1e-5f));
			CHECK(Test::ApproxEqual(view.Projection * view.InverseProjection, glm::mat4(1.0f), 1e-5f));
			CHECK(Test::ApproxEqual(view.ViewProjection, view.Projection * view.View));
		}

		TEST_CASE("TrianglePass: its layout description passes the reflection check")
		{
			VirtualFileSystem vfs;
			const Result<VfsPath> root = Test::MountCompiledShaders(vfs);
			REQUIRE_MESSAGE(root.has_value(), root.error().ToString());
			ShaderLibrary shaders(nullptr, vfs, *root);

			const PipelineLayoutDescription layout = TrianglePass::GetLayoutDescription();
			CHECK(layout.Program == "Triangle");
			CHECK(layout.Entries == std::vector<std::string>{ "VSMain", "PSMain" });
			const Status valid = ValidatePipelineLayout(layout, shaders);
			CHECK_MESSAGE(valid.has_value(), (valid.has_value() ? std::string() : valid.error().ToString()));
		}

		TEST_CASE("Rasterizer: CCW triangle survives back-face culling" * doctest::test_suite(Test::GpuSuite))
		{
			// §8.3: clip-space +Y is up (NVRHI's viewport performs the Vulkan Y flip) and pipelines set frontCounterClockwise,
			// so the view's counter-clockwise triangle is a front face. Culling back faces keeps it; culling front faces
			// removes it, which proves culling is really on.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			const std::array<uint8_t, 4> clear = GetClearPixel();

			const Image backCulled = RenderTriangle(gpu, nvrhi::RasterCullMode::Back, false);
			// The centre of the target is inside the triangle: not the clear colour.
			CHECK(GetPixel(backCulled, 32, 34) != clear);
			// A corner is outside it: the clear colour.
			CHECK(GetPixel(backCulled, 0, 0) == clear);
			// The image is upright (rows top first): the triangle's wide base is at the bottom, its apex at the top.
			CHECK(GetPixel(backCulled, 12, 48) != clear);
			CHECK(GetPixel(backCulled, 12, 14) == clear);

			const Image frontCulled = RenderTriangle(gpu, nvrhi::RasterCullMode::Front, false);
			CHECK(GetPixel(frontCulled, 32, 34) == clear);
			CHECK(GetPixel(frontCulled, 0, 0) == clear);

			// With a reverse-Z depth attachment (cleared to 0, GreaterOrEqual) the triangle passes the depth test.
			const Image withDepth = RenderTriangle(gpu, nvrhi::RasterCullMode::Back, true);
			CHECK(GetPixel(withDepth, 32, 34) == GetPixel(backCulled, 32, 34));
			CHECK(GetPixel(withDepth, 0, 0) == clear);
		}
	}

}
