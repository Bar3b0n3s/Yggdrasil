#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <string>

// A render target that is not a swapchain image (Architecture §8.1 "Headless", §8.13): where a headless frame's final
// image lands, and what every screenshot and golden test renders into before Readback::ReadTexture copies it out.

namespace Engine {

	class GraphicsDevice;

	struct OffscreenTargetSpecification
	{
		uint32_t Width = 0;
		uint32_t Height = 0;
		// The color format: RGBA8_UNORM for final images (display-encoded values, §8.9), or any other render-target format.
		nvrhi::Format ColorFormat = nvrhi::Format::RGBA8_UNORM;
		// Adds a D32 depth attachment (reverse-Z: cleared to 0, §8.3).
		bool Depth = false;
		// The clear value ClearColor of the color attachment and the debug name prefix of both textures ("<Name>.Color",
		// "<Name>.Depth").
		nvrhi::Color ClearColor{ 0.0f, 0.0f, 0.0f, 1.0f };
		std::string DebugName{ "OffscreenTarget" };
	};

	// Owns its textures and framebuffer (§8.14 item 1). Movable, not copyable; main thread only (§4.11).
	class OffscreenTarget
	{
	public:
		// The color texture is a render target that can also be sampled and copied from (isRenderTarget, isShaderResource,
		// initial state RenderTarget, keepInitialState), so ImGui::Image and Readback both read it. Errors: InvalidArgument
		// for a zero size or a non-render-target color format; Gpu when a texture or the framebuffer cannot be created
		// (GraphicsDevice wrappers; the caller decides whether that is fatal).
		[[nodiscard]] static Result<OffscreenTarget> Create(GraphicsDevice& device, const OffscreenTargetSpecification& specification);

		OffscreenTarget(OffscreenTarget&& other) noexcept;
		OffscreenTarget& operator=(OffscreenTarget&& other) noexcept;
		OffscreenTarget(const OffscreenTarget&) = delete;
		OffscreenTarget& operator=(const OffscreenTarget&) = delete;
		~OffscreenTarget();

		// Recreates the textures at the new size; a no-op for the current size. The previous textures are released (NVRHI
		// defers their destruction until submissions using them complete). Errors: those of Create.
		[[nodiscard]] Status Resize(GraphicsDevice& device, uint32_t width, uint32_t height);

		// Records clears of the color attachment to ClearColor and of the depth attachment to 0 into `commandList`.
		void Clear(nvrhi::ICommandList& commandList) const;

		[[nodiscard]] nvrhi::ITexture* GetColorTexture() const;
		// nullptr without a depth attachment.
		[[nodiscard]] nvrhi::ITexture* GetDepthTexture() const;
		[[nodiscard]] nvrhi::IFramebuffer* GetFramebuffer() const;
		[[nodiscard]] const OffscreenTargetSpecification& GetSpecification() const { return m_Specification; }
		[[nodiscard]] uint32_t GetWidth() const { return m_Specification.Width; }
		[[nodiscard]] uint32_t GetHeight() const { return m_Specification.Height; }
	private:
		explicit OffscreenTarget(OffscreenTargetSpecification specification);
	private:
		OffscreenTargetSpecification m_Specification;
	};

}
