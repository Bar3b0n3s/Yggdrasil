#include "TestsPCH.h"

#include "Engine/Graphics/Readback.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Graphics")
	{
		TEST_CASE("Readback: clear colour is exact" * doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// §15.3: a clear to values that are exact in 8 and 32 bits reads back bit for bit, with the row pitch removed.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Readback readback(device);
			struct Case
			{
				nvrhi::Format Format = nvrhi::Format::UNKNOWN;
				std::vector<std::byte> ExpectedPixel{};
			};
			const std::array<float, 4> clear = { 0.0f, 0.2f, 0.6f, 1.0f }; // 0, 51, 153, 255 in 8 bits
			const auto bytesOf = [](std::initializer_list<uint8_t> values)
			{
				std::vector<std::byte> bytes;
				for (const uint8_t value : values)
					bytes.push_back(static_cast<std::byte>(value));
				return bytes;
			};
			std::vector<std::byte> floats(16);
			std::memcpy(floats.data(), clear.data(), sizeof(clear));
			const std::vector<Case> cases = {
				{ nvrhi::Format::RGBA8_UNORM, bytesOf({ 0, 51, 153, 255 }) },
				{ nvrhi::Format::BGRA8_UNORM, bytesOf({ 153, 51, 0, 255 }) },
				{ nvrhi::Format::RGBA32_FLOAT, floats },
			};
			for (const Case& testCase : cases)
			{
				CAPTURE(std::string(nvrhi::getFormatInfo(testCase.Format).name));
				// An odd width, so the staging texture's row pitch differs from the tight one.
				const OffscreenTargetSpecification specification = {
					.Width = 13,
					.Height = 7,
					.ColorFormat = testCase.Format,
					.ClearColor = nvrhi::Color(clear[0], clear[1], clear[2], clear[3]),
				};
				Result<OffscreenTarget> target = OffscreenTarget::Create(device, specification);
				REQUIRE_MESSAGE(target.has_value(), target.error().ToString());
				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
				(*commandList)->open();
				target->Clear(**commandList);
				(*commandList)->close();
				device.ExecuteCommandList(**commandList);

				const Result<Image> image = readback.ReadTexture(*target->GetColorTexture());
				REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
				CHECK(image->Width == 13);
				CHECK(image->Height == 7);
				CHECK(image->Format == testCase.Format);
				REQUIRE(image->IsValid());
				const size_t pixelSize = testCase.ExpectedPixel.size();
				bool allExact = true;
				for (size_t offset = 0; offset < image->Pixels.size(); offset += pixelSize)
					allExact = allExact && std::equal(testCase.ExpectedPixel.begin(), testCase.ExpectedPixel.end(), image->Pixels.begin() + static_cast<std::ptrdiff_t>(offset));
				CHECK(allExact);
			}
		}

		TEST_CASE("Readback: depth formats and out-of-range subresources are InvalidArgument"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Readback readback(device);
			Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = 8, .Height = 8, .Depth = true });
			REQUIRE_MESSAGE(target.has_value(), target.error().ToString());
			const Result<Image> depth = readback.ReadTexture(*target->GetDepthTexture());
			REQUIRE_FALSE(depth.has_value());
			CHECK(depth.error().GetCode() == ErrorCode::InvalidArgument);
			const Result<Image> mip = readback.ReadTexture(*target->GetColorTexture(), 3);
			REQUIRE_FALSE(mip.has_value());
			CHECK(mip.error().GetCode() == ErrorCode::InvalidArgument);
			const Result<Image> slice = readback.ReadTexture(*target->GetColorTexture(), 0, 1);
			REQUIRE_FALSE(slice.has_value());
			CHECK(slice.error().GetCode() == ErrorCode::InvalidArgument);
		}
	}

}
