#include "EnginePCH.h"
#include "Engine/Graphics/OffscreenTarget.h"

#include "Engine/Graphics/GraphicsDevice.h"

// M5 contract stub (Roadmap rule 3): stream E (readback, ImageCompare, golden harness, screenshots) implements the
// textures, the framebuffer, resizing and clearing. Until then Create fails with Unsupported.

namespace Engine {

	OffscreenTarget::OffscreenTarget(OffscreenTargetSpecification specification)
		: m_Specification(std::move(specification))
	{
	}

	OffscreenTarget::OffscreenTarget(OffscreenTarget&& other) noexcept = default;

	OffscreenTarget& OffscreenTarget::operator=(OffscreenTarget&& other) noexcept = default;

	OffscreenTarget::~OffscreenTarget() = default;

	Result<OffscreenTarget> OffscreenTarget::Create(GraphicsDevice& /*device*/, const OffscreenTargetSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "OffscreenTarget::Create is not implemented yet");
	}

	Status OffscreenTarget::Resize(GraphicsDevice& /*device*/, uint32_t /*width*/, uint32_t /*height*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "OffscreenTarget::Resize is not implemented yet");
	}

	void OffscreenTarget::Clear(nvrhi::ICommandList& /*commandList*/) const
	{
		ENGINE_CONTRACT_STUB();
	}

	nvrhi::ITexture* OffscreenTarget::GetColorTexture() const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	nvrhi::ITexture* OffscreenTarget::GetDepthTexture() const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	nvrhi::IFramebuffer* OffscreenTarget::GetFramebuffer() const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

}
