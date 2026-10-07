#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Buffer.h"
#include "Engine/Core/Result.h"

#include <nvrhi/nvrhi.h>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <vector>

// A CPU image (Architecture §8.1 Readback::ReadTexture's result, §8.13 screenshots, §15.4 golden images) and its PNG codec
// (stb_image_write for encoding, §8.13; stb_image for decoding the committed golden images). The codec lives here,
// beside the type Readback returns, because every user of screenshots sits above Graphics: Testing (ImageCompare),
// Automation/Methods (viewport.screenshot, which cannot include Testing), App (Runtime --screenshot-at), the Editor and
// EditorCore (thumbnails) (Docs/Decisions/0009-m5-decisions.md).

namespace Engine {

	// Pixels in one uncompressed format, rows top first and tightly packed (no row padding). A value type.
	struct Image
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		// An uncompressed, non-depth nvrhi format. Readback produces the texture's own format; screenshots and PNG
		// decoding produce RGBA8_UNORM (display-encoded values, §8.9).
		nvrhi::Format Format = nvrhi::Format::RGBA8_UNORM;
		std::vector<std::byte> Pixels{};

		// Bytes per pixel of Format (nvrhi::getFormatInfo); 0 for a block-compressed format.
		[[nodiscard]] uint32_t GetBytesPerPixel() const;
		// Width * GetBytesPerPixel().
		[[nodiscard]] size_t GetRowPitch() const;
		// The bytes of row `y` (< Height, asserted).
		[[nodiscard]] std::span<const std::byte> GetRow(uint32_t y) const;
		// True when the image has a size above zero (as CreateImage requires), the format is uncompressed and has no depth or
		// stencil, and Pixels.size() equals GetRowPitch() * Height. A default-constructed Image is not valid.
		[[nodiscard]] bool IsValid() const;
	};

	// An image of `width` x `height` pixels of `format`, zero-filled. Errors: InvalidArgument for a zero size, a
	// block-compressed or depth format, or a size whose byte count overflows.
	[[nodiscard]] Result<Image> CreateImage(uint32_t width, uint32_t height, nvrhi::Format format);

	// `image` as RGBA8_UNORM: RGBA8_UNORM is copied, BGRA8_UNORM swizzled, SRGBA8_UNORM and SBGRA8_UNORM reinterpreted as
	// their UNORM layout (the bytes are already display-encoded). Errors: InvalidArgument for any other format or an
	// invalid image.
	[[nodiscard]] Result<Image> ConvertToRgba8(const Image& image);

	// PNG bytes of `image` (8-bit RGBA, after ConvertToRgba8), deterministic for equal input. Errors: InvalidArgument as
	// ConvertToRgba8; Io when the encoder fails.
	[[nodiscard]] Result<Buffer> EncodePng(const Image& image);

	// The pixels of PNG `bytes` as RGBA8_UNORM (gray and RGB images are expanded, 16-bit channels reduced to 8). Errors:
	// Parse for data that is not a PNG or is truncated or corrupt, naming stb_image's reason. Never crashes on malformed
	// input.
	[[nodiscard]] Result<Image> DecodePng(std::span<const std::byte> bytes);

	// EncodePng, then FileSystem::WriteFileAtomic. Errors: those of EncodePng and of the write.
	[[nodiscard]] Status WritePng(const std::filesystem::path& path, const Image& image);
	// FileSystem::ReadFile, then DecodePng. Errors: those of the read (NotFound, Io) and of DecodePng.
	[[nodiscard]] Result<Image> ReadPng(const std::filesystem::path& path);

	// `image` scaled down so that its larger side is at most `maxDimension` (> 0), keeping the aspect ratio (each side at
	// least 1 pixel), with a box filter over RGBA8 (screenshot maxDimension, §13.5). An image that already fits is
	// returned unchanged. Deterministic. Errors: InvalidArgument as ConvertToRgba8, or for maxDimension 0.
	[[nodiscard]] Result<Image> DownscaleImage(const Image& image, uint32_t maxDimension);

}
