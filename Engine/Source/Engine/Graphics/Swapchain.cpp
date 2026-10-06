#include "EnginePCH.h"
#include "Engine/Graphics/Swapchain.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Platform/Window.h"

// M5 contract stub (Roadmap rule 3): stream B (swapchain, present, frame pacing, fault handling) implements the surface,
// the swapchain, its semaphores and the VkResult state machine of Architecture §8.1, through the dispatcher's C entry
// points. Until then Create fails with Unsupported, so no Swapchain exists.

namespace Engine {

	SwapchainAction ClassifyAcquireResult(VkResult /*result*/)
	{
		ENGINE_CONTRACT_STUB();
		return SwapchainAction::Fail;
	}

	SwapchainAction ClassifyPresentResult(VkResult /*result*/)
	{
		ENGINE_CONTRACT_STUB();
		return SwapchainAction::Fail;
	}

	Swapchain::Swapchain(ConstructionKey /*key*/)
	{
	}

	Swapchain::~Swapchain()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<Scope<Swapchain>> Swapchain::Create(GraphicsDevice& /*device*/, Window& /*window*/, const SwapchainSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Swapchain::Create is not implemented yet");
	}

	Result<SwapchainAcquireStatus> Swapchain::AcquireNextImage(uint32_t /*frameSlot*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "Swapchain::AcquireNextImage is not implemented yet");
	}

	void Swapchain::QueueFrameSemaphores()
	{
		ENGINE_CONTRACT_STUB();
	}

	void Swapchain::Present()
	{
		ENGINE_CONTRACT_STUB();
	}

	void Swapchain::RequestRecreate()
	{
		ENGINE_CONTRACT_STUB();
	}

	void Swapchain::InjectAcquireResultForTesting(VkResult /*result*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void Swapchain::InjectPresentResultForTesting(VkResult /*result*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	nvrhi::ITexture* Swapchain::GetCurrentTexture() const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	nvrhi::IFramebuffer* Swapchain::GetCurrentFramebuffer() const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	nvrhi::Format Swapchain::GetFormat() const
	{
		ENGINE_CONTRACT_STUB();
		return nvrhi::Format::UNKNOWN;
	}

	uint32_t Swapchain::GetWidth() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	uint32_t Swapchain::GetHeight() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	uint32_t Swapchain::GetImageCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	VkPresentModeKHR Swapchain::GetPresentMode() const
	{
		ENGINE_CONTRACT_STUB();
		return VK_PRESENT_MODE_FIFO_KHR;
	}

	bool Swapchain::IsMinimized() const
	{
		ENGINE_CONTRACT_STUB();
		return false;
	}

	uint32_t Swapchain::GetRecreationCount() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	std::string_view SwapchainActionToString(SwapchainAction /*action*/)
	{
		ENGINE_CONTRACT_STUB();
		return "Unknown";
	}

}
