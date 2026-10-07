#include "TestsPCH.h"

#include "Engine/Graphics/Readback.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Graphics")
	{
		TEST_CASE("Readback: clear colour is exact" * doctest::test_suite(Test::GpuSuite))
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
				{
					const auto pixel = image->Pixels.begin() + static_cast<std::ptrdiff_t>(offset);
					allExact = allExact && std::equal(testCase.ExpectedPixel.begin(), testCase.ExpectedPixel.end(), pixel);
				}
				CHECK(allExact);
			}
		}

		TEST_CASE("Readback: reads the requested mip level and array slice" * doctest::test_suite(Test::GpuSuite))
		{
			// A 2-slice, 2-mip render target whose four subresources are cleared to four values (exact in 8 bits).
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			nvrhi::TextureDesc desc;
			desc.width = 16;
			desc.height = 8;
			desc.mipLevels = 2;
			desc.arraySize = 2;
			desc.dimension = nvrhi::TextureDimension::Texture2DArray;
			desc.format = nvrhi::Format::RGBA8_UNORM;
			desc.isRenderTarget = true;
			desc.debugName = "Subresources";
			desc.enableAutomaticStateTracking(nvrhi::ResourceStates::RenderTarget);
			Result<nvrhi::TextureHandle> texture = device.CreateTexture(desc);
			REQUIRE_MESSAGE(texture.has_value(), texture.error().ToString());

			const auto valueOf = [](uint32_t mip, uint32_t slice)
			{
				return static_cast<uint8_t>(51 * (1 + mip + 2 * slice)); // 51, 102, 153, 204
			};
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			(*commandList)->open();
			for (uint32_t slice = 0; slice < 2; ++slice)
			{
				for (uint32_t mip = 0; mip < 2; ++mip)
				{
					const float value = static_cast<float>(valueOf(mip, slice)) / 255.0f;
					(*commandList)->clearTextureFloat(*texture, nvrhi::TextureSubresourceSet(mip, 1, slice, 1), nvrhi::Color(value, value, value, 1.0f));
				}
			}
			(*commandList)->close();
			device.ExecuteCommandList(**commandList);

			Readback readback(device);
			for (uint32_t slice = 0; slice < 2; ++slice)
			{
				for (uint32_t mip = 0; mip < 2; ++mip)
				{
					CAPTURE(mip);
					CAPTURE(slice);
					const Result<Image> image = readback.ReadTexture(**texture, mip, slice);
					REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
					CHECK(image->Width == (16u >> mip));
					CHECK(image->Height == (8u >> mip));
					REQUIRE(image->IsValid());
					const std::byte expected{ valueOf(mip, slice) };
					CHECK(image->Pixels[0] == expected);
					CHECK(image->Pixels[image->Pixels.size() - 2] == expected); // blue of the last pixel
				}
			}
		}

		TEST_CASE("Readback: depth formats and out-of-range subresources are InvalidArgument"
			* doctest::test_suite(Test::GpuSuite))
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
