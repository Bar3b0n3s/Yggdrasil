#include "TestsPCH.h"

#include "Engine/Graphics/HostImageUpload.h"

#include "Engine/Core/Log.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/Readback.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TestOptions.h"

#include <algorithm>
#include <array>
#include <limits>
#include <vector>

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

	// Reports a host-copy case the device cannot run (§8.1: "it reports a skip with the reason"): a doctest MESSAGE, and a
	// Warn log line that Scripts/Test.py collects (HOST_COPY_SKIP_PATTERN), because doctest's JUnit reporter drops
	// messages. Keep the text in step with that pattern.
	static void ReportHostCopySkipped(HostImageUpload& upload, const nvrhi::TextureDesc& desc)
	{
		std::string reason = "the device's image format limits do not cover the texture";
		if (!upload.IsHostImageCopyAvailable())
			reason = "hostImageCopy is not enabled (API 1.3 or no device support)";
		else if (!upload.SupportsHostImageCopy(desc.format))
			reason = "the format lacks the host-transfer, sampled or transfer-source feature";
		const std::string what = std::format("'{}' ({})", desc.debugName, nvrhi::getFormatInfo(desc.format).name);
		MESSAGE("host image copy skipped for " << what << ": " << reason);
		ENGINE_CORE_WARN("Host image copy skipped for {}: {}", what, reason);
	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("Texture upload: host-copy and staging paths read back identically"
			* doctest::test_suite(Test::GpuSuite))
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
				if (!upload.SupportsHostImageCopy(desc))
				{
					ReportHostCopySkipped(upload, desc);
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
			* doctest::test_suite(Test::GpuSuite))
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

			// Twice the same subresource, and one that does not exist.
			const std::array<TextureSubresourceData, 2> duplicated = { { { .Data = texels }, { .Data = texels } } };
			nvrhi::TextureDesc pair = single;
			pair.dimension = nvrhi::TextureDimension::Texture2DArray;
			pair.arraySize = 2;
			const Result<TextureUpload> twice = upload.CreateTexture(pair, duplicated);
			REQUIRE_FALSE(twice.has_value());
			CHECK(twice.error().GetCode() == ErrorCode::InvalidArgument);
			const std::array<TextureSubresourceData, 1> outOfRange = { { { .MipLevel = 1, .Data = texels } } };
			const Result<TextureUpload> missingMip = upload.CreateTexture(single, outOfRange);
			REQUIRE_FALSE(missingMip.has_value());
			CHECK(missingMip.error().GetCode() == ErrorCode::InvalidArgument);

			// A row pitch below the row size.
			const std::array<TextureSubresourceData, 1> narrowRows = { { { .Data = texels, .RowPitch = 8 } } };
			const Result<TextureUpload> narrow = upload.CreateTexture(single, narrowRows);
			REQUIRE_FALSE(narrow.has_value());
			CHECK(narrow.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("HostImageUpload: pitches that overflow, absurd array sizes and tall 1D textures are InvalidArgument"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			HostImageUpload& upload = gpu.GetDevice().GetHostImageUpload();
			nvrhi::TextureDesc desc;
			desc.width = 4;
			desc.height = 4;
			desc.format = nvrhi::Format::RGBA8_UNORM;
			desc.debugName = "Overflow";

			// Pitches whose products wrap around to small numbers must not let an empty span pass, on either path.
			for (const TextureUploadPath path : { TextureUploadPath::Staging, TextureUploadPath::HostImageCopy })
			{
				CAPTURE(std::string(TextureUploadPathToString(path)));
				const std::array<TextureSubresourceData, 1> wrappingRows = { { { .RowPitch = size_t{ 1 } << 62 } } };
				const Result<TextureUpload> rows = upload.CreateTexture(desc, wrappingRows, path);
				REQUIRE_FALSE(rows.has_value());
				CHECK(rows.error().GetCode() == ErrorCode::InvalidArgument);

				nvrhi::TextureDesc volume = desc;
				volume.dimension = nvrhi::TextureDimension::Texture3D;
				volume.depth = 4;
				const std::array<TextureSubresourceData, 1> wrappingSlices = { { { .DepthPitch = std::numeric_limits<size_t>::max() / 2 + 1 } } };
				const Result<TextureUpload> slices = upload.CreateTexture(volume, wrappingSlices, path);
				REQUIRE_FALSE(slices.has_value());
				CHECK(slices.error().GetCode() == ErrorCode::InvalidArgument);

				// A pitch that is in range but whose row length in texels does not fit the host copy's 32 bits.
				const std::array<TextureSubresourceData, 1> wideRows = { { { .RowPitch = size_t{ 1 } << 40 } } };
				const Result<TextureUpload> wide = upload.CreateTexture(desc, wideRows, path);
				REQUIRE_FALSE(wide.has_value());
				CHECK(wide.error().GetCode() == ErrorCode::InvalidArgument);
			}

			// An array size no device has is rejected by the subresource count, before anything is sized from it.
			const std::vector<std::byte> texels = MakeTexels(4, 4, 4);
			nvrhi::TextureDesc absurd;
			absurd.width = 1u << 31;
			absurd.height = 1;
			absurd.mipLevels = 32;
			absurd.arraySize = std::numeric_limits<uint32_t>::max();
			absurd.dimension = nvrhi::TextureDimension::Texture2DArray;
			absurd.format = nvrhi::Format::RGBA8_UNORM;
			absurd.debugName = "Absurd";
			const std::array<TextureSubresourceData, 1> one = { { { .Data = texels } } };
			const Result<TextureUpload> huge = upload.CreateTexture(absurd, one);
			REQUIRE_FALSE(huge.has_value());
			CHECK(huge.error().GetCode() == ErrorCode::InvalidArgument);

			// A 1D texture has height 1 (VUID-VkImageCreateInfo-imageType-00956).
			nvrhi::TextureDesc line = desc;
			line.dimension = nvrhi::TextureDimension::Texture1D;
			line.height = 2;
			const Result<TextureUpload> tall = upload.CreateTexture(line, one);
			REQUIRE_FALSE(tall.has_value());
			CHECK(tall.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("HostImageUpload: mip chains, cube faces and padded rows read back identically on both paths"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			HostImageUpload& upload = device.GetHostImageUpload();
			Readback readback(device);

			// A 2D mip chain whose rows carry 3 bytes of padding (no whole texel, so the host copy repacks them), and a cube
			// with two mips whose rows carry one padding texel (which the host copy expresses as a row length). The cube's
			// image is created CUBE_COMPATIBLE, which the device's image format limits must cover (SupportsHostImageCopy).
			struct UploadCase
			{
				nvrhi::TextureDesc Desc{};
				size_t RowPadding = 0;
			};
			std::vector<UploadCase> cases(2);
			cases[0].Desc.width = 8;
			cases[0].Desc.height = 4;
			cases[0].Desc.mipLevels = 3;
			cases[0].Desc.format = nvrhi::Format::RGBA8_UNORM;
			cases[0].Desc.debugName = "MipChain";
			cases[0].RowPadding = 3;
			cases[1].Desc.width = 4;
			cases[1].Desc.height = 4;
			cases[1].Desc.mipLevels = 2;
			cases[1].Desc.arraySize = 6;
			cases[1].Desc.dimension = nvrhi::TextureDimension::TextureCube;
			cases[1].Desc.format = nvrhi::Format::RGBA8_UNORM;
			cases[1].Desc.debugName = "Cube";
			cases[1].RowPadding = 4;

			for (const UploadCase& uploadCase : cases)
			{
				CAPTURE(uploadCase.Desc.debugName);
				// The padded data of every subresource, and what a readback of it must return (the rows without padding).
				std::vector<std::vector<std::byte>> padded;
				std::vector<std::vector<std::byte>> expected;
				std::vector<TextureSubresourceData> subresources;
				const size_t subresourceCount = static_cast<size_t>(uploadCase.Desc.arraySize) * uploadCase.Desc.mipLevels;
				padded.reserve(subresourceCount); // the spans below point into these vectors
				expected.reserve(subresourceCount);
				for (uint32_t slice = 0; slice < uploadCase.Desc.arraySize; ++slice)
				{
					for (uint32_t mip = 0; mip < uploadCase.Desc.mipLevels; ++mip)
					{
						const uint32_t width = std::max(uploadCase.Desc.width >> mip, 1u);
						const uint32_t height = std::max(uploadCase.Desc.height >> mip, 1u);
						const size_t rowSize = static_cast<size_t>(width) * 4;
						const size_t rowPitch = rowSize + uploadCase.RowPadding;
						std::vector<std::byte> tight = MakeTexels(width, height, 4);
						for (std::byte& texel : tight)
							texel ^= static_cast<std::byte>(slice * 16 + mip); // every subresource differs
						std::vector<std::byte> rows(rowPitch * height, std::byte{ 0xEE });
						for (uint32_t row = 0; row < height; ++row)
						{
							std::copy_n(tight.begin() + static_cast<std::ptrdiff_t>(row * rowSize), rowSize,
								rows.begin() + static_cast<std::ptrdiff_t>(row * rowPitch));
						}
						expected.push_back(std::move(tight));
						padded.push_back(std::move(rows));
						subresources.push_back({ .MipLevel = mip, .ArraySlice = slice, .Data = padded.back(), .RowPitch = rowPitch });
					}
				}

				for (const TextureUploadPath path : { TextureUploadPath::Staging, TextureUploadPath::HostImageCopy })
				{
					CAPTURE(std::string(TextureUploadPathToString(path)));
					if (path == TextureUploadPath::HostImageCopy && !upload.SupportsHostImageCopy(uploadCase.Desc))
					{
						ReportHostCopySkipped(upload, uploadCase.Desc);
						continue;
					}
					Result<TextureUpload> uploaded = upload.CreateTexture(uploadCase.Desc, subresources, path);
					REQUIRE_MESSAGE(uploaded.has_value(), uploaded.error().ToString());
					CHECK(uploaded->Path == path);
					for (size_t index = 0; index < subresources.size(); ++index)
					{
						const TextureSubresourceData& subresource = subresources[index];
						CAPTURE(subresource.MipLevel);
						CAPTURE(subresource.ArraySlice);
						Result<Image> image = readback.ReadTexture(*uploaded->Texture, subresource.MipLevel, subresource.ArraySlice);
						REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
						CHECK(image->Pixels == expected[index]);
					}
				}
			}

			device.WaitForIdle();
			device.RunGarbageCollection();
			device.WaitForIdle();
			device.RunGarbageCollection();
			CHECK(upload.GetHostImageCount() == 0);
		}

		TEST_CASE("HostImageUpload: a 3D texture takes the host-copy path only where the device's limits allow it"
			* doctest::test_suite(Test::GpuSuite))
		{
			// Readback cannot read a 3D texture (NVRHI copies one depth slice only into 3D textures), so this checks that
			// both paths create it without a validation message, which fails the fixture: the host copy creates a 3D image
			// only after vkGetPhysicalDeviceImageFormatProperties2 accepted it.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			HostImageUpload& upload = device.GetHostImageUpload();
			nvrhi::TextureDesc desc;
			desc.dimension = nvrhi::TextureDimension::Texture3D;
			desc.width = 8;
			desc.height = 4;
			desc.depth = 4;
			desc.mipLevels = 2;
			desc.format = nvrhi::Format::RGBA8_UNORM;
			desc.debugName = "Volume";
			std::vector<std::vector<std::byte>> data;
			std::vector<TextureSubresourceData> subresources;
			data.reserve(desc.mipLevels); // the spans below point into these vectors
			for (uint32_t mip = 0; mip < desc.mipLevels; ++mip)
			{
				const uint32_t width = std::max(desc.width >> mip, 1u);
				const uint32_t height = std::max(desc.height >> mip, 1u);
				const uint32_t depth = std::max(desc.depth >> mip, 1u);
				data.push_back(MakeTexels(width, height * depth, 4));
				subresources.push_back({ .MipLevel = mip, .Data = data.back() });
			}

			for (const TextureUploadPath path : { TextureUploadPath::Staging, TextureUploadPath::HostImageCopy })
			{
				CAPTURE(std::string(TextureUploadPathToString(path)));
				if (path == TextureUploadPath::HostImageCopy && !upload.SupportsHostImageCopy(desc))
				{
					ReportHostCopySkipped(upload, desc);
					continue;
				}
				Result<TextureUpload> uploaded = upload.CreateTexture(desc, subresources, path);
				REQUIRE_MESSAGE(uploaded.has_value(), uploaded.error().ToString());
				CHECK(uploaded->Path == path);
			}

			// A 3D texture beyond the device's 3D extent limit never takes the host-copy path.
			nvrhi::TextureDesc oversized = desc;
			oversized.width = 1u << 20;
			oversized.mipLevels = 1;
			CHECK_FALSE(upload.SupportsHostImageCopy(oversized));
			device.WaitForIdle();
			device.RunGarbageCollection();
			device.WaitForIdle();
			device.RunGarbageCollection();
			CHECK(upload.GetHostImageCount() == 0);
		}

		TEST_CASE("HostImageUpload: the path names are their enumerator names")
		{
			CHECK(TextureUploadPathToString(TextureUploadPath::HostImageCopy) == "HostImageCopy");
			CHECK(TextureUploadPathToString(TextureUploadPath::Staging) == "Staging");
		}
	}

}
