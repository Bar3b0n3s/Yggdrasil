#include "EnginePCH.h"
#include "Engine/Graphics/Image.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FileSystem.h"

#include <stb_image.h>
#include <stb_image_write.h>

#include <climits>
#include <optional>

namespace Engine {

	namespace Utils {

		constexpr uint32_t Rgba8BytesPerPixel = 4;

		// The eight bytes every PNG file starts with (PNG specification, section 5.2). stb_image decodes other formats too;
		// the codec accepts PNG only.
		constexpr std::array<uint8_t, 8> PngSignature = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };

		// A deflate stream expands at most 1032-fold, and the smallest PNG row is one filter byte plus one bit per pixel, so a
		// PNG of N bytes holds at most 8 * 1032 * N pixels. stb_image allocates the declared image before it finds that the
		// data is too short, so DecodePng rejects a header declaring more pixels than the file can hold: a corrupt or hostile
		// header can never make it allocate gigabytes.
		constexpr uint64_t MaxPixelsPerPngByte = 8 * 1032;

		[[nodiscard]] static std::string_view GetFormatName(nvrhi::Format format)
		{
			return nvrhi::getFormatInfo(format).name;
		}

		// Whether `format` can be held by an Image: uncompressed, with no depth or stencil, and not UNKNOWN.
		[[nodiscard]] static bool IsImageFormat(nvrhi::Format format)
		{
			const nvrhi::FormatInfo& info = nvrhi::getFormatInfo(format);
			return format != nvrhi::Format::UNKNOWN && info.blockSize == 1 && info.bytesPerBlock != 0 && !info.hasDepth && !info.hasStencil;
		}

		[[nodiscard]] static std::string DescribeImage(const Image& image)
		{
			return std::format("{}x{} {} image with {} bytes", image.Width, image.Height, GetFormatName(image.Format), image.Pixels.size());
		}

		// stb_image_write's output callback: appends each chunk of the encoded file to the Buffer `context` points to.
		static void AppendToBuffer(void* context, void* data, int size)
		{
			Buffer& buffer = *static_cast<Buffer*>(context);
			const auto* bytes = static_cast<const std::byte*>(data);
			buffer.insert(buffer.end(), bytes, bytes + size);
		}

		// stb_image's reason for its last failure on this thread (its failure reason is thread-local).
		[[nodiscard]] static std::string_view GetStbFailureReason()
		{
			const char* reason = stbi_failure_reason();
			return reason != nullptr ? std::string_view(reason) : std::string_view("unknown reason");
		}

		// The rounded mean of the `count` values summed in `sum`.
		[[nodiscard]] static uint8_t RoundedMean(uint64_t sum, uint64_t count)
		{
			return static_cast<uint8_t>((sum + count / 2) / count);
		}

		// `image` itself when it is a valid RGBA8 image (UNORM or sRGB: the same bytes), so a screenshot of up to 8192x8192
		// pixels is read in place; otherwise its ConvertToRgba8 copy, held in `storage`. Errors: those of ConvertToRgba8.
		[[nodiscard]] static Result<const Image*> ViewAsRgba8(const Image& image, std::optional<Image>& storage)
		{
			const bool isRgba = image.Format == nvrhi::Format::RGBA8_UNORM || image.Format == nvrhi::Format::SRGBA8_UNORM;
			if (isRgba && image.IsValid())
				return &image;
			ENGINE_TRY_ASSIGN(storage, ConvertToRgba8(image));
			return &*storage;
		}

	}

	uint32_t Image::GetBytesPerPixel() const
	{
		const nvrhi::FormatInfo& info = nvrhi::getFormatInfo(Format);
		return info.blockSize == 1 ? info.bytesPerBlock : 0;
	}

	size_t Image::GetRowPitch() const
	{
		return static_cast<size_t>(Width) * GetBytesPerPixel();
	}

	std::span<const std::byte> Image::GetRow(uint32_t y) const
	{
		ENGINE_CORE_ASSERT(y < Height, "Image::GetRow({}) of an image {} rows high", y, Height);
		const size_t rowPitch = GetRowPitch();
		return std::span<const std::byte>(Pixels).subspan(static_cast<size_t>(y) * rowPitch, rowPitch);
	}

	bool Image::IsValid() const
	{
		const nvrhi::FormatInfo& info = nvrhi::getFormatInfo(Format);
		return Width > 0 && Height > 0 && GetBytesPerPixel() != 0 && !info.hasDepth && !info.hasStencil
			&& Pixels.size() == GetRowPitch() * Height;
	}

	Result<Image> CreateImage(uint32_t width, uint32_t height, nvrhi::Format format)
	{
		if (width == 0 || height == 0)
			return MakeError(ErrorCode::InvalidArgument, "an image needs a size above zero, got {}x{}", width, height);
		if (!Utils::IsImageFormat(format))
		{
			return MakeError(ErrorCode::InvalidArgument, "an image cannot hold format {}: it must be uncompressed and have no depth or stencil",
				Utils::GetFormatName(format));
		}

		const uint64_t bytesPerPixel = nvrhi::getFormatInfo(format).bytesPerBlock;
		const uint64_t pixelCount = static_cast<uint64_t>(width) * height; // two 32-bit factors never overflow 64 bits
		constexpr uint64_t MaxImageBytes = static_cast<uint64_t>(std::numeric_limits<size_t>::max());
		if (pixelCount > MaxImageBytes / bytesPerPixel)
		{
			return MakeError(ErrorCode::InvalidArgument, "a {}x{} {} image does not fit in memory", width, height,
				Utils::GetFormatName(format));
		}

		Image image{ .Width = width, .Height = height, .Format = format };
		image.Pixels.resize(static_cast<size_t>(pixelCount * bytesPerPixel));
		return image;
	}

	Result<Image> ConvertToRgba8(const Image& image)
	{
		if (!image.IsValid())
			return MakeError(ErrorCode::InvalidArgument, "cannot convert an invalid image ({}) to RGBA8", Utils::DescribeImage(image));

		// The sRGB variants store the same display-encoded bytes; only the GPU's view of them differs.
		const bool isRgba = image.Format == nvrhi::Format::RGBA8_UNORM || image.Format == nvrhi::Format::SRGBA8_UNORM;
		const bool isBgra = image.Format == nvrhi::Format::BGRA8_UNORM || image.Format == nvrhi::Format::SBGRA8_UNORM;
		if (!isRgba && !isBgra)
		{
			return MakeError(ErrorCode::InvalidArgument, "cannot convert a {} image to RGBA8: only RGBA8 and BGRA8 (UNORM or sRGB) can",
				Utils::GetFormatName(image.Format));
		}

		Image converted = image;
		converted.Format = nvrhi::Format::RGBA8_UNORM;
		if (isBgra)
		{
			for (size_t offset = 0; offset < converted.Pixels.size(); offset += Utils::Rgba8BytesPerPixel)
				std::swap(converted.Pixels[offset], converted.Pixels[offset + 2]);
		}
		return converted;
	}

	Result<Buffer> EncodePng(const Image& image)
	{
		std::optional<Image> converted;
		ENGINE_TRY_ASSIGN(const Image* rgbaImage, Utils::ViewAsRgba8(image, converted));
		const Image& rgba = *rgbaImage;
		// stb_image_write takes the size and the row stride as int.
		constexpr uint32_t MaxEncodableWidth = static_cast<uint32_t>(INT_MAX) / Utils::Rgba8BytesPerPixel;
		if (rgba.Width > MaxEncodableWidth || rgba.Height > static_cast<uint32_t>(INT_MAX))
			return MakeError(ErrorCode::InvalidArgument, "a {}x{} image is too large to encode as PNG", rgba.Width, rgba.Height);

		Buffer encoded;
		const int written = stbi_write_png_to_func(&Utils::AppendToBuffer, &encoded, static_cast<int>(rgba.Width), static_cast<int>(rgba.Height),
			static_cast<int>(Utils::Rgba8BytesPerPixel), rgba.Pixels.data(), static_cast<int>(rgba.GetRowPitch()));
		if (written == 0 || encoded.empty())
			return MakeError(ErrorCode::Io, "the PNG encoder failed for a {}x{} image", rgba.Width, rgba.Height);
		return encoded;
	}

	Result<Image> DecodePng(std::span<const std::byte> bytes)
	{
		if (bytes.size() < Utils::PngSignature.size()
			|| !std::equal(Utils::PngSignature.begin(), Utils::PngSignature.end(), bytes.begin(), [](uint8_t expected, std::byte actual)
		{
			return static_cast<std::byte>(expected) == actual;
		}))
		{
			return MakeError(ErrorCode::Parse, "not a PNG file ({} bytes without the PNG signature)", bytes.size());
		}
		if (bytes.size() > static_cast<size_t>(INT_MAX))
			return MakeError(ErrorCode::Parse, "a PNG file of {} bytes is too large to decode", bytes.size());

		const auto* data = reinterpret_cast<const stbi_uc*>(bytes.data());
		const int length = static_cast<int>(bytes.size());
		int width = 0;
		int height = 0;
		int channels = 0;
		if (stbi_info_from_memory(data, length, &width, &height, &channels) == 0)
			return MakeError(ErrorCode::Parse, "corrupt PNG header: {}", Utils::GetStbFailureReason());
		const uint64_t declaredPixels = static_cast<uint64_t>(width) * static_cast<uint64_t>(height);
		if (width <= 0 || height <= 0 || declaredPixels > Utils::MaxPixelsPerPngByte * bytes.size())
			return MakeError(ErrorCode::Parse, "corrupt PNG: it declares {}x{} pixels, more than its {} bytes can hold", width, height, bytes.size());

		stbi_uc* pixels = stbi_load_from_memory(data, length, &width, &height, &channels, static_cast<int>(Utils::Rgba8BytesPerPixel));
		if (pixels == nullptr)
			return MakeError(ErrorCode::Parse, "corrupt PNG: {}", Utils::GetStbFailureReason());

		Image image{ .Width = static_cast<uint32_t>(width), .Height = static_cast<uint32_t>(height), .Format = nvrhi::Format::RGBA8_UNORM };
		const auto* decoded = reinterpret_cast<const std::byte*>(pixels);
		image.Pixels.assign(decoded, decoded + image.GetRowPitch() * image.Height);
		stbi_image_free(pixels);
		return image;
	}

	Status WritePng(const std::filesystem::path& path, const Image& image)
	{
		ENGINE_TRY_ASSIGN(const Buffer encoded, EncodePng(image));
		// A screenshot or golden image replaces its previous version without leaving a .bak file beside it.
		return FileSystem::WriteFileAtomic(path, encoded, AtomicWriteOptions{ .KeepBackup = false });
	}

	Result<Image> ReadPng(const std::filesystem::path& path)
	{
		ENGINE_TRY_ASSIGN(const Buffer bytes, FileSystem::ReadFile(path));
		return DecodePng(bytes);
	}

	Result<Image> DownscaleImage(const Image& image, uint32_t maxDimension)
	{
		if (maxDimension == 0)
			return MakeError(ErrorCode::InvalidArgument, "DownscaleImage needs a maximum dimension above zero");
		std::optional<Image> converted;
		ENGINE_TRY_ASSIGN(const Image* sourceImage, Utils::ViewAsRgba8(image, converted));
		const Image& source = *sourceImage;
		const uint32_t largest = std::max(source.Width, source.Height);
		if (largest <= maxDimension)
		{
			if (converted.has_value())
				return std::move(*converted);
			return source;
		}

		// The larger side becomes maxDimension; the other keeps the aspect ratio, rounded, and at least one pixel.
		const auto scaleSide = [largest, maxDimension](uint32_t side)
		{
			const uint64_t scaled = (static_cast<uint64_t>(side) * maxDimension + largest / 2) / largest;
			return static_cast<uint32_t>(std::max<uint64_t>(scaled, 1));
		};
		const uint32_t width = scaleSide(source.Width);
		const uint32_t height = scaleSide(source.Height);
		ENGINE_TRY_ASSIGN(Image result, CreateImage(width, height, nvrhi::Format::RGBA8_UNORM));

		// A box filter: each destination pixel is the rounded mean of the source pixels its footprint covers. Integer
		// arithmetic only, so the result is the same on every platform.
		const size_t sourcePitch = source.GetRowPitch();
		for (uint32_t y = 0; y < height; ++y)
		{
			const uint64_t top = static_cast<uint64_t>(y) * source.Height / height;
			const uint64_t bottom = std::max(top + 1, static_cast<uint64_t>(y + 1) * source.Height / height);
			for (uint32_t x = 0; x < width; ++x)
			{
				const uint64_t left = static_cast<uint64_t>(x) * source.Width / width;
				const uint64_t right = std::max(left + 1, static_cast<uint64_t>(x + 1) * source.Width / width);
				std::array<uint64_t, Utils::Rgba8BytesPerPixel> sums{};
				for (uint64_t sourceY = top; sourceY < bottom; ++sourceY)
				{
					const size_t rowOffset = static_cast<size_t>(sourceY) * sourcePitch;
					for (uint64_t sourceX = left; sourceX < right; ++sourceX)
					{
						const size_t offset = rowOffset + static_cast<size_t>(sourceX) * Utils::Rgba8BytesPerPixel;
						for (size_t channel = 0; channel < Utils::Rgba8BytesPerPixel; ++channel)
							sums[channel] += std::to_integer<uint64_t>(source.Pixels[offset + channel]);
					}
				}
				const uint64_t count = (bottom - top) * (right - left);
				const size_t destination = (static_cast<size_t>(y) * width + x) * Utils::Rgba8BytesPerPixel;
				for (size_t channel = 0; channel < Utils::Rgba8BytesPerPixel; ++channel)
					result.Pixels[destination + channel] = static_cast<std::byte>(Utils::RoundedMean(sums[channel], count));
			}
		}
		return result;
	}

}
