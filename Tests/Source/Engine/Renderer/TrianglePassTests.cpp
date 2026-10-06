#include "TestsPCH.h"

#include "Engine/Renderer/TrianglePass.h"

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

	TEST_SUITE("Renderer")
	{
		TEST_CASE("TrianglePass: the fixed camera is orthographic, reverse-Z and flips Y for Vulkan" * doctest::skip(true))
		{
			const ViewConstants view = TrianglePass::MakeViewConstants(640, 360);
			CHECK(view.ProjectionKind == ProjectionKindOrthographic);
			CHECK(view.OrthoHalfExtents.x == doctest::Approx(640.0 / 360.0));
			CHECK(view.OrthoHalfExtents.y == doctest::Approx(1.0));
			CHECK(view.ViewportSize == glm::vec2(640.0f, 360.0f));
			CHECK(view.AspectRatio == doctest::Approx(640.0 / 360.0));

			// A point on the near plane maps to depth 1, one on the far plane to depth 0 (reverse-Z, §8.3); +Y in the view is
			// -Y in clip space (the Vulkan flip), so a CCW triangle in the view is CW in clip space before the viewport flip.
			const glm::vec4 nearPoint = view.ViewProjection * glm::vec4(0.0f, 0.0f, 5.0f - view.Near, 1.0f);
			const glm::vec4 farPoint = view.ViewProjection * glm::vec4(0.0f, 0.0f, 5.0f - view.Far, 1.0f);
			CHECK(nearPoint.z / nearPoint.w == doctest::Approx(1.0).epsilon(1e-5));
			CHECK(farPoint.z / farPoint.w == doctest::Approx(0.0).epsilon(1e-5));
			const glm::vec4 up = view.ViewProjection * glm::vec4(0.0f, 1.0f, 0.0f, 1.0f);
			CHECK(up.y / up.w < 0.0f);
			// The inverses invert.
			const glm::mat4 identity = view.ViewProjection * view.InverseViewProjection;
			CHECK(Test::ApproxEqual(identity, glm::mat4(1.0f), 1e-5f));
		}

		TEST_CASE("Rasterizer: CCW triangle survives back-face culling" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = 64, .Height = 64 });
			REQUIRE_MESSAGE(target.has_value(), target.error().ToString());

			nvrhi::FramebufferInfo framebuffer;
			framebuffer.addColorFormat(nvrhi::Format::RGBA8_UNORM);
			Result<Scope<TrianglePass>> pass =
				TrianglePass::Create(device, gpu.GetPipelines(), { .Framebuffer = framebuffer, .CullMode = nvrhi::RasterCullMode::Back });
			REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());

			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			(*commandList)->open();
			(*pass)->Render(**commandList, *target->GetFramebuffer());
			(*commandList)->close();
			device.ExecuteCommandList(**commandList);

			Readback readback(device);
			const Result<Image> image = readback.ReadTexture(*target->GetColorTexture());
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			// The centre of the target is inside the triangle: not the clear colour.
			const std::array<uint8_t, 4> centre = GetPixel(*image, 32, 34);
			const std::array<uint8_t, 4> clear = {
				static_cast<uint8_t>(TriangleClearColor[0] * 255.0f + 0.5f),
				static_cast<uint8_t>(TriangleClearColor[1] * 255.0f + 0.5f),
				static_cast<uint8_t>(TriangleClearColor[2] * 255.0f + 0.5f),
				255,
			};
			CHECK(centre != clear);
			// A corner is outside it: the clear colour.
			CHECK(GetPixel(*image, 0, 0) == clear);
		}
	}

}
