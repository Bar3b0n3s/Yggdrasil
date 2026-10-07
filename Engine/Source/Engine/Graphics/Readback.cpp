#include "EnginePCH.h"
#include "Engine/Graphics/Readback.h"

#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GraphicsDevice.h"

namespace Engine {

	namespace Utils {

		// InvalidArgument unless subresource (`mipLevel`, `arraySlice`) of a texture with `desc` can be read into an Image.
		[[nodiscard]] static Status ValidateReadback(const nvrhi::TextureDesc& desc, uint32_t mipLevel, uint32_t arraySlice)
		{
			const nvrhi::FormatInfo& info = nvrhi::getFormatInfo(desc.format);
			if (info.hasDepth || info.hasStencil)
				return MakeError(ErrorCode::InvalidArgument, "cannot read back texture '{}': {} is a depth format", desc.debugName, info.name);
			if (desc.format == nvrhi::Format::UNKNOWN || info.blockSize != 1)
			{
				return MakeError(ErrorCode::InvalidArgument, "cannot read back texture '{}': {} is not an uncompressed color format",
					desc.debugName, info.name);
			}
			if (desc.sampleCount > 1)
			{
				return MakeError(ErrorCode::InvalidArgument, "cannot read back texture '{}': it is multisampled ({} samples)", desc.debugName,
					desc.sampleCount);
			}
			// An Image is two-dimensional; a volume texture has depth slices, not array slices.
			if (desc.dimension == nvrhi::TextureDimension::Texture3D)
				return MakeError(ErrorCode::InvalidArgument, "cannot read back texture '{}': it is a 3D texture", desc.debugName);
			if (mipLevel >= desc.mipLevels)
			{
				return MakeError(ErrorCode::InvalidArgument, "cannot read back mip {} of texture '{}', which has {} mip level(s)", mipLevel,
					desc.debugName, desc.mipLevels);
			}
			if (arraySlice >= desc.arraySize)
			{
				return MakeError(ErrorCode::InvalidArgument, "cannot read back array slice {} of texture '{}', which has {} slice(s)", arraySlice,
					desc.debugName, desc.arraySize);
			}
			return {};
		}

	}

	Readback::Readback(GraphicsDevice& device)
		: m_Device(&device)
	{
	}

	Readback::~Readback() = default;

	Result<Image> Readback::ReadTexture(nvrhi::ITexture& texture, uint32_t mipLevel, uint32_t arraySlice)
	{
		const nvrhi::TextureDesc& desc = texture.getDesc();
		ENGINE_TRY(Utils::ValidateReadback(desc, mipLevel, arraySlice));

		const uint32_t width = std::max(desc.width >> mipLevel, 1u);
		const uint32_t height = std::max(desc.height >> mipLevel, 1u);
		ENGINE_TRY_ASSIGN(Image image, CreateImage(width, height, desc.format));

		nvrhi::TextureDesc stagingDesc;
		stagingDesc.width = width;
		stagingDesc.height = height;
		stagingDesc.format = desc.format;
		stagingDesc.dimension = nvrhi::TextureDimension::Texture2D;
		stagingDesc.debugName = desc.debugName + ".Readback";
		ENGINE_TRY_ASSIGN(const nvrhi::StagingTextureHandle staging, m_Device->CreateStagingTexture(stagingDesc, nvrhi::CpuAccessMode::Read));
		if (m_CommandList == nullptr)
		{
			// Not an immediate command list: NVRHI's validation allows one open immediate list at a time, and a screenshot
			// may be read while the caller's frame has its own list open.
			ENGINE_TRY_ASSIGN(m_CommandList, m_Device->CreateCommandList(nvrhi::CommandListParameters().setEnableImmediateExecution(false)));
		}

		// NVRHI derives the texture's state in this command list from its keepInitialState or permanent state, as for every
		// engine render target (OffscreenTarget, RenderTargetPool), and orders the copy after the prior submissions' writes.
		m_CommandList->open();
		m_CommandList->copyTexture(staging, nvrhi::TextureSlice(), &texture, nvrhi::TextureSlice().setMipLevel(mipLevel).setArraySlice(arraySlice));
		m_CommandList->close();
		const uint64_t submission = m_Device->ExecuteCommandList(*m_CommandList);
		// Bounded (a hang or device loss is fatal), so mapping, which waits for the copy without a timeout, never waits.
		WaitForSubmission(*m_Device, submission, std::format("Readback of texture '{}'", desc.debugName));

		const size_t tightPitch = image.GetRowPitch();
		size_t rowPitch = 0;
		nvrhi::IDevice* nvrhiDevice = m_Device->GetNvrhiDevice();
		const void* mapped = nvrhiDevice->mapStagingTexture(staging, nvrhi::TextureSlice(), nvrhi::CpuAccessMode::Read, &rowPitch);
		if (mapped == nullptr)
			return MakeError(ErrorCode::Gpu, "cannot map the readback staging texture of texture '{}'", desc.debugName);
		if (rowPitch < tightPitch)
		{
			nvrhiDevice->unmapStagingTexture(staging);
			return MakeError(ErrorCode::Gpu, "the readback staging texture of texture '{}' has a row pitch of {} bytes, below the {} bytes of a row",
				desc.debugName, rowPitch, tightPitch);
		}

		// The staging texture's rows are padded to the device's copy alignment; the Image's are tightly packed.
		const auto* source = static_cast<const std::byte*>(mapped);
		for (uint32_t y = 0; y < height; ++y)
			std::memcpy(image.Pixels.data() + static_cast<size_t>(y) * tightPitch, source + static_cast<size_t>(y) * rowPitch, tightPitch);
		nvrhiDevice->unmapStagingTexture(staging);
		return image;
	}

}
