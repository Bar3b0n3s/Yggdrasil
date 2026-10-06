#include "TestsPCH.h"

#include "Engine/Graphics/HostImageUpload.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"

#include <array>

namespace Engine {

	namespace {

		// The texture formats the engine uploads as immutable textures (asset textures, environment cubes, LUTs), each
		// with a deterministic texel pattern of its size.
		struct UploadFormat
		{
			nvrhi::Format Format = nvrhi::Format::UNKNOWN;
			uint32_t BytesPerTexel = 0;
		};

		constexpr std::array<UploadFormat, 6> UploadFormats = { {
			{ nvrhi::Format::RGBA8_UNORM, 4 },
			{ nvrhi::Format::SRGBA8_UNORM, 4 },
			{ nvrhi::Format::R8_UNORM, 1 },
			{ nvrhi::Format::RG16_FLOAT, 4 },
			{ nvrhi::Format::RGBA16_FLOAT, 8 },
			{ nvrhi::Format::RGBA32_FLOAT, 16 },
		} };

	}

	// Deterministic bytes: every byte differs from its neighbours, so a misplaced row or swizzle shows.
	static std::vector<std::byte> MakeTexels(uint32_t width, uint32_t height, uint32_t bytesPerTexel)
	{
		std::vector<std::byte> texels(static_cast<size_t>(width) * height * bytesPerTexel);
		for (size_t index = 0; index < texels.size(); ++index)
			texels[index] = static_cast<std::byte>((index * 37 + 11) & 0xFF);
		return texels;
	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("Texture upload: host-copy and staging paths read back identically"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			HostImageUpload& upload = device.GetHostImageUpload();
			Readback readback(device);

			// The comparison is byte-wise: uploads, copies and readbacks move bits without conversion, so even float formats
			// filled with arbitrary bytes must come back unchanged.
			for (const UploadFormat& format : UploadFormats)
			{
				CAPTURE(std::string(nvrhi::getFormatInfo(format.Format).name));
				const uint32_t width = 16;
				const uint32_t height = 8;
				const std::vector<std::byte> texels = MakeTexels(width, height, format.BytesPerTexel);
				nvrhi::TextureDesc desc;
				desc.width = width;
				desc.height = height;
				desc.format = format.Format;
				desc.debugName = "UploadTest";
				const std::array<TextureSubresourceData, 1> subresources = { { { .Data = texels } } };

				// The staging path runs under both API caps.
				Result<TextureUpload> staged = upload.CreateTexture(desc, subresources, TextureUploadPath::Staging);
				REQUIRE_MESSAGE(staged.has_value(), staged.error().ToString());
				CHECK(staged->Path == TextureUploadPath::Staging);
				Result<Image> stagedImage = readback.ReadTexture(*staged->Texture);
				REQUIRE_MESSAGE(stagedImage.has_value(), stagedImage.error().ToString());
				CHECK(stagedImage->Pixels == texels);

				// The host-copy path: a 1.4 path, skipped with the reason when the device or the cap lacks it (§8.1).
				if (!upload.SupportsHostImageCopy(format.Format))
				{
					MESSAGE("host image copy skipped for this format: ",
						upload.IsHostImageCopyAvailable() ? "the format lacks VK_FORMAT_FEATURE_2_HOST_IMAGE_TRANSFER_BIT"
														  : "hostImageCopy is not enabled (API 1.3 or no device support)");
					continue;
				}
				Result<TextureUpload> copied = upload.CreateTexture(desc, subresources, TextureUploadPath::HostImageCopy);
				REQUIRE_MESSAGE(copied.has_value(), copied.error().ToString());
				CHECK(copied->Path == TextureUploadPath::HostImageCopy);
				CHECK(upload.GetHostImageCount() >= 1);
				Result<Image> copiedImage = readback.ReadTexture(*copied->Texture);
				REQUIRE_MESSAGE(copiedImage.has_value(), copiedImage.error().ToString());
				CHECK(copiedImage->Pixels == stagedImage->Pixels);
			}

			// Dropped host images go through the deferred-release queue and are gone once their submission completed.
			device.WaitForIdle();
			device.RunGarbageCollection();
			device.WaitForIdle();
			device.RunGarbageCollection();
			CHECK(upload.GetHostImageCount() == 0);
			CHECK(upload.GetPendingReleaseCount() == 0);
			CHECK(device.GetResourceTracker().GetLiveCount(GpuResourceType::HostImage) == 0);
		}

		TEST_CASE("HostImageUpload: render targets and incomplete subresources are InvalidArgument"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			HostImageUpload& upload = gpu.GetDevice().GetHostImageUpload();
			const std::vector<std::byte> texels = MakeTexels(4, 4, 4);
			const std::array<TextureSubresourceData, 1> subresources = { { { .Data = texels } } };

			nvrhi::TextureDesc target;
			target.width = 4;
			target.height = 4;
			target.format = nvrhi::Format::RGBA8_UNORM;
			target.isRenderTarget = true;
			const Result<TextureUpload> rejectedTarget = upload.CreateTexture(target, subresources);
			REQUIRE_FALSE(rejectedTarget.has_value());
			CHECK(rejectedTarget.error().GetCode() == ErrorCode::InvalidArgument);

			nvrhi::TextureDesc mipped = target;
			mipped.isRenderTarget = false;
			mipped.mipLevels = 3; // only mip 0 is given
			const Result<TextureUpload> incomplete = upload.CreateTexture(mipped, subresources);
			REQUIRE_FALSE(incomplete.has_value());
			CHECK(incomplete.error().GetCode() == ErrorCode::InvalidArgument);

			const std::vector<std::byte> tooSmall(8);
			const std::array<TextureSubresourceData, 1> shortData = { { { .Data = tooSmall } } };
			nvrhi::TextureDesc single = mipped;
			single.mipLevels = 1;
			const Result<TextureUpload> truncated = upload.CreateTexture(single, shortData);
			REQUIRE_FALSE(truncated.has_value());
			CHECK(truncated.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("HostImageUpload: the path names are their enumerator names")
		{
			CHECK(TextureUploadPathToString(TextureUploadPath::HostImageCopy) == "HostImageCopy");
			CHECK(TextureUploadPathToString(TextureUploadPath::Staging) == "Staging");
		}
	}

}
