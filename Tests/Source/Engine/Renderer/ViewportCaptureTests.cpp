#include "TestsPCH.h"

#include "Engine/Renderer/ViewportCapture.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Image.h"
#include "Engine/Renderer/TrianglePass.h"
#include "Support/HeadlessGpuFixture.h"

#include <cstdlib>

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("ViewportCapture: captures the clear-and-triangle view at the requested size"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Result<Scope<ViewportCapture>> capture = ViewportCapture::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());

			// The default request is the golden-image size; two captures of one request are identical (§8.3: no temporal
			// effects), and the triangle covers the centre while the corners show the clear colour.
			const Result<Image> first = (*capture)->Capture({});
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			CHECK(first->Width == DefaultViewportScreenshotWidth);
			CHECK(first->Height == DefaultViewportScreenshotHeight);
			CHECK(first->Format == nvrhi::Format::RGBA8_UNORM);
			REQUIRE(first->IsValid());
			const Result<Image> second = (*capture)->Capture({});
			REQUIRE_MESSAGE(second.has_value(), second.error().ToString());
			CHECK(first->Pixels == second->Pixels);
			const std::span<const std::byte> centreRow = first->GetRow(first->Height / 2);
			const std::span<const std::byte> topRow = first->GetRow(0);
			const auto centre = static_cast<std::ptrdiff_t>(static_cast<size_t>(first->Width / 2) * 4);
			CHECK_FALSE(std::equal(centreRow.begin() + centre, centreRow.begin() + centre + 3, topRow.begin()));
			// The top-left corner holds TriangleClearColor as stored in the UNORM target (within the GPU's conversion rounding).
			for (size_t channel = 0; channel < TriangleClearColor.size(); ++channel)
			{
				CAPTURE(channel);
				const int expected = static_cast<int>(TriangleClearColor[channel] * 255.0f + 0.5f);
				CHECK(std::abs(std::to_integer<int>(topRow[channel]) - expected) <= 1);
			}

			// Another size needs no new pipeline: the capture renders any size with the pipeline it created at startup.
			const Result<Image> small = (*capture)->Capture({ .Width = 320, .Height = 200 });
			REQUIRE_MESSAGE(small.has_value(), small.error().ToString());
			CHECK(small->Width == 320);
			CHECK(small->Height == 200);
		}

		TEST_CASE("ViewportCapture: MaxDimension scales the image down and keeps the aspect ratio"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Result<Scope<ViewportCapture>> capture = ViewportCapture::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());
			const Result<Image> image = (*capture)->Capture({ .MaxDimension = 160 });
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			CHECK(image->Width == 160);
			CHECK(image->Height == 90);
		}

		TEST_CASE("ViewportCapture: a zero or oversized request is InvalidArgument"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			Result<Scope<ViewportCapture>> capture = ViewportCapture::Create(gpu.GetDevice(), gpu.GetPipelines());
			REQUIRE_MESSAGE(capture.has_value(), capture.error().ToString());
			const std::array<ViewportScreenshotRequest, 4> invalid = { {
				{ .Width = 0, .Height = 360 },
				{ .Width = 640, .Height = 0 },
				{ .Width = MaxViewportScreenshotDimension + 1, .Height = 360 },
				{ .Width = 640, .Height = MaxViewportScreenshotDimension + 1 },
			} };
			for (const ViewportScreenshotRequest& request : invalid)
			{
				CAPTURE(request.Width);
				CAPTURE(request.Height);
				const Result<Image> image = (*capture)->Capture(request);
				REQUIRE_FALSE(image.has_value());
				CHECK(image.error().GetCode() == ErrorCode::InvalidArgument);
			}
		}
	}

}
