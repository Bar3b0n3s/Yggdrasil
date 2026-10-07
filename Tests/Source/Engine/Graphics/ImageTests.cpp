#include "TestsPCH.h"

#include "Engine/Graphics/Image.h"

#include "Engine/Core/Random.h"
#include "Support/TempDirectory.h"

namespace Engine {

	// The error code of a failed result; nullopt for a success.
	template<typename T>
	static std::optional<ErrorCode> ErrorCodeOf(const Result<T>& result)
	{
		return result.has_value() ? std::nullopt : std::optional<ErrorCode>(result.error().GetCode());
	}

	// A `width` x `height` RGBA8 image whose pixels encode their coordinates.
	static Image MakeGradient(uint32_t width, uint32_t height)
	{
		Image image{ .Width = width, .Height = height, .Format = nvrhi::Format::RGBA8_UNORM };
		image.Pixels.resize(static_cast<size_t>(width) * height * 4);
		for (uint32_t y = 0; y < height; ++y)
		{
			for (uint32_t x = 0; x < width; ++x)
			{
				const size_t offset = (static_cast<size_t>(y) * width + x) * 4;
				image.Pixels[offset + 0] = static_cast<std::byte>(x * 255 / std::max(width - 1, 1u));
				image.Pixels[offset + 1] = static_cast<std::byte>(y * 255 / std::max(height - 1, 1u));
				image.Pixels[offset + 2] = static_cast<std::byte>((x + y) & 0xFF);
				image.Pixels[offset + 3] = static_cast<std::byte>(255 - (x & 0x7F));
			}
		}
		return image;
	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("Image: row pitch, rows and validity follow the format")
		{
			Image image{ .Width = 3, .Height = 2, .Format = nvrhi::Format::RGBA8_UNORM };
			CHECK(image.GetBytesPerPixel() == 4);
			CHECK(image.GetRowPitch() == 12);
			CHECK_FALSE(image.IsValid());
			image.Pixels.resize(24);
			CHECK(image.IsValid());
			image.Pixels[12] = std::byte{ 0x7F };
			CHECK(image.GetRow(1).size() == 12);
			CHECK(image.GetRow(1)[0] == std::byte{ 0x7F });

			const Image floats{ .Width = 2, .Height = 1, .Format = nvrhi::Format::RGBA32_FLOAT, .Pixels = std::vector<std::byte>(32) };
			CHECK(floats.GetBytesPerPixel() == 16);
			CHECK(floats.IsValid());
			const Image compressed{ .Width = 4, .Height = 4, .Format = nvrhi::Format::BC1_UNORM, .Pixels = std::vector<std::byte>(8) };
			CHECK(compressed.GetBytesPerPixel() == 0);
			CHECK_FALSE(compressed.IsValid());
			const Image depth{ .Width = 1, .Height = 1, .Format = nvrhi::Format::D32, .Pixels = std::vector<std::byte>(4) };
			CHECK_FALSE(depth.IsValid());

			// An empty image is no image: a comparison of two sizes has no difference image (ImageCompareResult), and PNG
			// encoding refuses it instead of writing a file with a zero-size header.
			CHECK_FALSE(Image{}.IsValid());
			const Image wide{ .Width = 4, .Height = 0, .Format = nvrhi::Format::RGBA8_UNORM };
			CHECK_FALSE(wide.IsValid());
			const Result<Buffer> encoded = EncodePng(Image{});
			REQUIRE_FALSE(encoded.has_value());
			CHECK(encoded.error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("Image: CreateImage zero-fills and rejects sizes and formats it cannot hold")
		{
			const Result<Image> image = CreateImage(5, 3, nvrhi::Format::RG16_FLOAT);
			REQUIRE_MESSAGE(image.has_value(), image.error().ToString());
			CHECK(image->IsValid());
			CHECK(image->Pixels.size() == 60);
			CHECK(std::ranges::all_of(image->Pixels, [](std::byte value)
			{
				return value == std::byte{ 0 };
			}));
			CHECK(ErrorCodeOf(CreateImage(0, 3, nvrhi::Format::RGBA8_UNORM)) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(CreateImage(4, 4, nvrhi::Format::BC7_UNORM)) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(CreateImage(4, 4, nvrhi::Format::D32)) == ErrorCode::InvalidArgument);
		}

		TEST_CASE("Image: PNG encoding round-trips RGBA8 exactly and is deterministic")
		{
			const Image original = MakeGradient(37, 21);
			const Result<Buffer> first = EncodePng(original);
			REQUIRE_MESSAGE(first.has_value(), first.error().ToString());
			const Result<Buffer> second = EncodePng(original);
			REQUIRE(second.has_value());
			CHECK(*first == *second);
			const Result<Image> decoded = DecodePng(*first);
			REQUIRE_MESSAGE(decoded.has_value(), decoded.error().ToString());
			CHECK(decoded->Width == 37);
			CHECK(decoded->Height == 21);
			CHECK(decoded->Format == nvrhi::Format::RGBA8_UNORM);
			CHECK(decoded->Pixels == original.Pixels);

			// Files go through FileSystem.
			Test::TempDirectory directory("Png");
			REQUIRE(WritePng(directory / "Gradient.png", original).has_value());
			const Result<Image> read = ReadPng(directory / "Gradient.png");
			REQUIRE_MESSAGE(read.has_value(), read.error().ToString());
			CHECK(read->Pixels == original.Pixels);
			CHECK(ErrorCodeOf(ReadPng(directory / "Missing.png")) == ErrorCode::NotFound);
		}

		TEST_CASE("Image: BGRA8 is swizzled and sRGB formats are taken as encoded bytes")
		{
			Image bgra{ .Width = 1, .Height = 1, .Format = nvrhi::Format::BGRA8_UNORM };
			bgra.Pixels = { std::byte{ 10 }, std::byte{ 20 }, std::byte{ 30 }, std::byte{ 40 } };
			const Result<Image> rgba = ConvertToRgba8(bgra);
			REQUIRE_MESSAGE(rgba.has_value(), rgba.error().ToString());
			const std::vector<std::byte> expected = { std::byte{ 30 }, std::byte{ 20 }, std::byte{ 10 }, std::byte{ 40 } };
			CHECK(rgba->Pixels == expected);
			CHECK(rgba->Format == nvrhi::Format::RGBA8_UNORM);

			Image srgb = bgra;
			srgb.Format = nvrhi::Format::SRGBA8_UNORM;
			const Result<Image> fromSrgb = ConvertToRgba8(srgb);
			REQUIRE(fromSrgb.has_value());
			CHECK(fromSrgb->Pixels == bgra.Pixels);

			const Image floats{ .Width = 1, .Height = 1, .Format = nvrhi::Format::RGBA32_FLOAT, .Pixels = std::vector<std::byte>(16) };
			CHECK(ErrorCodeOf(ConvertToRgba8(floats)) == ErrorCode::InvalidArgument);
			CHECK(ErrorCodeOf(EncodePng(floats)) == ErrorCode::InvalidArgument);
		}

		TEST_CASE("Image: malformed and mutated PNG data is a Parse error, never a crash")
		{
			CHECK(ErrorCodeOf(DecodePng({})) == ErrorCode::Parse);
			const std::array<std::byte, 8> notPng = {};
			CHECK(ErrorCodeOf(DecodePng(notPng)) == ErrorCode::Parse);

			const Result<Buffer> valid = EncodePng(MakeGradient(16, 16));
			REQUIRE(valid.has_value());
			Random random(0xC0FFEEu);
			for (int iteration = 0; iteration < 2000; ++iteration)
			{
				Buffer mutated = *valid;
				if (iteration % 3 == 0)
					mutated.resize(random.NextU32() % mutated.size());
				else
					mutated[random.NextU32() % mutated.size()] = static_cast<std::byte>(random.NextU32() & 0xFF);
				const Result<Image> decoded = DecodePng(mutated);
				if (decoded.has_value())
					CHECK(decoded->IsValid());
				else
					CHECK(decoded.error().GetCode() == ErrorCode::Parse);
			}
		}

		TEST_CASE("Image: DecodePng accepts PNG only and rejects a header declaring more pixels than the file holds")
		{
			// stb_image decodes other formats too; a BMP is not a PNG.
			std::array<std::byte, 64> bitmap{};
			bitmap[0] = std::byte{ 'B' };
			bitmap[1] = std::byte{ 'M' };
			const Result<Image> notPng = DecodePng(bitmap);
			REQUIRE_FALSE(notPng.has_value());
			CHECK(notPng.error().GetCode() == ErrorCode::Parse);
			CHECK(notPng.error().GetMessageText().contains("not a PNG"));

			// The IHDR chunk's width and height (big-endian, after the 8-byte signature and the chunk's length and type) set
			// to 4096 x 4096: a size stb_image accepts, but 16 Mi pixels cannot come from a file of a few hundred bytes.
			Result<Buffer> encoded = EncodePng(MakeGradient(16, 16));
			REQUIRE(encoded.has_value());
			Buffer& bytes = *encoded;
			REQUIRE(bytes.size() > 24);
			for (const size_t offset : { size_t{ 16 }, size_t{ 20 } })
			{
				bytes[offset + 0] = std::byte{ 0x00 };
				bytes[offset + 1] = std::byte{ 0x00 };
				bytes[offset + 2] = std::byte{ 0x10 };
				bytes[offset + 3] = std::byte{ 0x00 };
			}
			const Result<Image> oversized = DecodePng(bytes);
			REQUIRE_FALSE(oversized.has_value());
			CHECK(oversized.error().GetCode() == ErrorCode::Parse);
			CHECK(oversized.error().GetMessageText().contains("4096x4096"));
		}

		TEST_CASE("Image: DownscaleImage averages the pixels each output pixel covers")
		{
			// 4 x 2 -> 2 x 1: each output pixel is the rounded mean of a 2 x 2 block.
			Image source{ .Width = 4, .Height = 2, .Format = nvrhi::Format::RGBA8_UNORM, .Pixels = std::vector<std::byte>(32, std::byte{ 255 }) };
			const std::array<uint8_t, 4> leftBlock = { 10, 20, 30, 43 };
			for (size_t index = 0; index < leftBlock.size(); ++index)
			{
				const size_t pixel = (index / 2) * 4 + (index % 2); // (0, 0), (1, 0), (0, 1), (1, 1)
				source.Pixels[pixel * 4] = static_cast<std::byte>(leftBlock[index]);
			}
			const Result<Image> result = DownscaleImage(source, 2);
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			REQUIRE(result->Width == 2);
			REQUIRE(result->Height == 1);
			CHECK(result->Pixels[0] == std::byte{ 26 }); // 103 / 4 = 25.75, rounded to the nearest value
			CHECK(result->Pixels[1] == std::byte{ 255 });
			CHECK(result->Pixels[4] == std::byte{ 255 });

			// BGRA input comes out as RGBA8.
			Image bgra = MakeGradient(8, 8);
			bgra.Format = nvrhi::Format::BGRA8_UNORM;
			const Result<Image> converted = DownscaleImage(bgra, 4);
			REQUIRE(converted.has_value());
			CHECK(converted->Format == nvrhi::Format::RGBA8_UNORM);
		}

		TEST_CASE("Image: DownscaleImage fits the larger side and keeps the aspect ratio")
		{
			const Image wide = MakeGradient(640, 360);
			const Result<Image> small = DownscaleImage(wide, 320);
			REQUIRE_MESSAGE(small.has_value(), small.error().ToString());
			CHECK(small->Width == 320);
			CHECK(small->Height == 180);
			CHECK(small->IsValid());

			const Result<Image> unchanged = DownscaleImage(wide, 1024);
			REQUIRE(unchanged.has_value());
			CHECK(unchanged->Width == 640);
			CHECK(unchanged->Pixels == wide.Pixels);

			const Result<Image> tall = DownscaleImage(MakeGradient(10, 1000), 100);
			REQUIRE(tall.has_value());
			CHECK(tall->Width == 1);
			CHECK(tall->Height == 100);
			CHECK(ErrorCodeOf(DownscaleImage(wide, 0)) == ErrorCode::InvalidArgument);

			// A uniform image stays uniform under the box filter.
			Image grey{ .Width = 8, .Height = 8, .Format = nvrhi::Format::RGBA8_UNORM, .Pixels = std::vector<std::byte>(256, std::byte{ 128 }) };
			const Result<Image> shrunk = DownscaleImage(grey, 2);
			REQUIRE(shrunk.has_value());
			CHECK(std::ranges::all_of(shrunk->Pixels, [](std::byte value)
			{
				return value == std::byte{ 128 };
			}));
		}
	}

}
