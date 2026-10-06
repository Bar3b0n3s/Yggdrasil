#include "EnginePCH.h"
#include "Engine/Graphics/Image.h"

#include "Engine/Core/Assert.h"

// M5 contract stub (Roadmap rule 3): stream E (readback, ImageCompare, golden harness, screenshots) implements the
// conversions and the PNG codec on top of Graphics/ThirdParty/StbImageImplementation.cpp. The Image accessors are
// implemented.

namespace Engine {

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
		return GetBytesPerPixel() != 0 && !info.hasDepth && !info.hasStencil && Pixels.size() == GetRowPitch() * Height;
	}

	Result<Image> CreateImage(uint32_t /*width*/, uint32_t /*height*/, nvrhi::Format /*format*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "CreateImage is not implemented yet");
	}

	Result<Image> ConvertToRgba8(const Image& /*image*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ConvertToRgba8 is not implemented yet");
	}

	Result<Buffer> EncodePng(const Image& /*image*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "EncodePng is not implemented yet");
	}

	Result<Image> DecodePng(std::span<const std::byte> /*bytes*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "DecodePng is not implemented yet");
	}

	Status WritePng(const std::filesystem::path& /*path*/, const Image& /*image*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "WritePng is not implemented yet");
	}

	Result<Image> ReadPng(const std::filesystem::path& /*path*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "ReadPng is not implemented yet");
	}

	Result<Image> DownscaleImage(const Image& /*image*/, uint32_t /*maxDimension*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "DownscaleImage is not implemented yet");
	}

}
