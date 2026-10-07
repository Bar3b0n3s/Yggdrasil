#include "EnginePCH.h"
#include "Engine/Graphics/OffscreenTarget.h"

#include "Engine/Graphics/GraphicsDevice.h"

namespace Engine {

	namespace Utils {

		// The depth attachment's format and its reverse-Z clear value (§8.3: the far plane is 0).
		constexpr nvrhi::Format OffscreenDepthFormat = nvrhi::Format::D32;
		constexpr float OffscreenDepthClear = 0.0f;

		// InvalidArgument unless `specification` describes a target the device can render to.
		[[nodiscard]] static Status ValidateSpecification(GraphicsDevice& device, const OffscreenTargetSpecification& specification)
		{
			if (specification.Width == 0 || specification.Height == 0)
			{
				return MakeError(ErrorCode::InvalidArgument, "offscreen target '{}' needs a size above zero, got {}x{}", specification.DebugName,
					specification.Width, specification.Height);
			}

			const nvrhi::FormatInfo& info = nvrhi::getFormatInfo(specification.ColorFormat);
			const bool colorFormat = specification.ColorFormat != nvrhi::Format::UNKNOWN && info.blockSize == 1 && !info.hasDepth
				&& !info.hasStencil;
			const nvrhi::FormatSupport required = nvrhi::FormatSupport::RenderTarget | nvrhi::FormatSupport::Texture;
			if (!colorFormat || (device.GetNvrhiDevice()->queryFormatSupport(specification.ColorFormat) & required) != required)
			{
				return MakeError(ErrorCode::InvalidArgument, "offscreen target '{}' cannot use {} as its color format: the device cannot render to it",
					specification.DebugName, info.name);
			}
			return {};
		}

	}

	OffscreenTarget::OffscreenTarget(OffscreenTargetSpecification specification)
		: m_Specification(std::move(specification))
	{
	}

	OffscreenTarget::OffscreenTarget(OffscreenTarget&& other) noexcept = default;

	OffscreenTarget& OffscreenTarget::operator=(OffscreenTarget&& other) noexcept = default;

	OffscreenTarget::~OffscreenTarget() = default;

	Result<OffscreenTarget> OffscreenTarget::Create(GraphicsDevice& device, const OffscreenTargetSpecification& specification)
	{
		ENGINE_TRY(Utils::ValidateSpecification(device, specification));
		OffscreenTarget target(specification);
		ENGINE_TRY(target.CreateResources(device));
		return target;
	}

	Status OffscreenTarget::Resize(GraphicsDevice& device, uint32_t width, uint32_t height)
	{
		if (width == m_Specification.Width && height == m_Specification.Height)
			return {};

		// Build the resized target completely before replacing this one, so a failure leaves the current textures in place.
		OffscreenTargetSpecification specification = m_Specification;
		specification.Width = width;
		specification.Height = height;
		ENGINE_TRY_ASSIGN(OffscreenTarget resized, Create(device, specification));
		*this = std::move(resized);
		return {};
	}

	void OffscreenTarget::Clear(nvrhi::ICommandList& commandList) const
	{
		commandList.clearTextureFloat(m_ColorTexture, nvrhi::AllSubresources, m_Specification.ClearColor);
		if (m_DepthTexture != nullptr)
			commandList.clearDepthStencilTexture(m_DepthTexture, nvrhi::AllSubresources, true, Utils::OffscreenDepthClear, false, 0);
	}

	nvrhi::ITexture* OffscreenTarget::GetColorTexture() const
	{
		return m_ColorTexture.Get();
	}

	nvrhi::ITexture* OffscreenTarget::GetDepthTexture() const
	{
		return m_DepthTexture.Get();
	}

	nvrhi::IFramebuffer* OffscreenTarget::GetFramebuffer() const
	{
		return m_Framebuffer.Get();
	}

	Status OffscreenTarget::CreateResources(GraphicsDevice& device)
	{
		// The color texture stays in RenderTarget between command lists (keepInitialState), so passes, ImGui::Image and
		// Readback each transition it from and back to that state.
		nvrhi::TextureDesc colorDesc;
		colorDesc.width = m_Specification.Width;
		colorDesc.height = m_Specification.Height;
		colorDesc.format = m_Specification.ColorFormat;
		colorDesc.debugName = m_Specification.DebugName + ".Color";
		colorDesc.isRenderTarget = true;
		colorDesc.isShaderResource = true;
		colorDesc.setClearValue(m_Specification.ClearColor);
		colorDesc.enableAutomaticStateTracking(nvrhi::ResourceStates::RenderTarget);
		ENGINE_TRY_ASSIGN(m_ColorTexture, device.CreateTexture(colorDesc));

		nvrhi::FramebufferDesc framebufferDesc;
		framebufferDesc.addColorAttachment(m_ColorTexture);
		if (m_Specification.Depth)
		{
			nvrhi::TextureDesc depthDesc;
			depthDesc.width = m_Specification.Width;
			depthDesc.height = m_Specification.Height;
			depthDesc.format = Utils::OffscreenDepthFormat;
			depthDesc.debugName = m_Specification.DebugName + ".Depth";
			depthDesc.isRenderTarget = true;
			depthDesc.isShaderResource = true;
			depthDesc.setClearValue(nvrhi::Color(Utils::OffscreenDepthClear));
			depthDesc.enableAutomaticStateTracking(nvrhi::ResourceStates::DepthWrite);
			ENGINE_TRY_ASSIGN(m_DepthTexture, device.CreateTexture(depthDesc));
			framebufferDesc.setDepthAttachment(m_DepthTexture);
		}

		ENGINE_TRY_ASSIGN(m_Framebuffer, device.CreateFramebuffer(framebufferDesc));
		return {};
	}

}
