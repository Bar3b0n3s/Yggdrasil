#include "EnginePCH.h"
#include "Engine/Graphics/GraphicsDevice.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/FatalError.h"
#include "Engine/Graphics/HostImageUpload.h"

// M5 contract stub (Roadmap rule 3): stream A (loader, device, selection, creation wrappers, host image upload) implements
// device creation (with the allowlisted catch of vk::SystemError, §4.6 item 1), the creation wrappers, submission,
// garbage collection and the device-fault description. Until then Create fails with Unsupported, so no GraphicsDevice
// exists; the accessors of stored state are implemented.

namespace Engine {

	GraphicsDevice::GraphicsDevice(ConstructionKey /*key*/, const GraphicsSpecification& specification)
		: m_Specification(specification), m_Diagnostics(specification.InjectFault)
	{
	}

	GraphicsDevice::~GraphicsDevice()
	{
		ENGINE_CONTRACT_STUB();
	}

	Result<Scope<GraphicsDevice>> GraphicsDevice::Create(const GraphicsDeviceSpecification& /*specification*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphicsDevice::Create is not implemented yet");
	}

	nvrhi::IDevice* GraphicsDevice::GetNvrhiDevice() const
	{
		ENGINE_CONTRACT_STUB();
		return nullptr;
	}

	const GraphicsDeviceInfo& GraphicsDevice::GetInfo() const
	{
		return m_Info;
	}

	const GraphicsSpecification& GraphicsDevice::GetSpecification() const
	{
		return m_Specification;
	}

	GpuDiagnostics& GraphicsDevice::GetDiagnostics()
	{
		return m_Diagnostics;
	}

	const GpuDiagnostics& GraphicsDevice::GetDiagnostics() const
	{
		return m_Diagnostics;
	}

	GpuResourceTracker& GraphicsDevice::GetResourceTracker()
	{
		return m_ResourceTracker;
	}

	const GpuResourceTracker& GraphicsDevice::GetResourceTracker() const
	{
		return m_ResourceTracker;
	}

	HostImageUpload& GraphicsDevice::GetHostImageUpload()
	{
		ENGINE_CORE_VERIFY(m_HostImageUpload != nullptr, "GraphicsDevice::GetHostImageUpload before the device was created");
		return *m_HostImageUpload;
	}

	Result<nvrhi::TextureHandle> GraphicsDevice::CreateTexture(const nvrhi::TextureDesc& /*desc*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphicsDevice::CreateTexture is not implemented yet");
	}

	Result<nvrhi::StagingTextureHandle> GraphicsDevice::CreateStagingTexture(const nvrhi::TextureDesc& /*desc*/,
		nvrhi::CpuAccessMode /*cpuAccess*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphicsDevice::CreateStagingTexture is not implemented yet");
	}

	Result<nvrhi::BufferHandle> GraphicsDevice::CreateBuffer(const nvrhi::BufferDesc& /*desc*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphicsDevice::CreateBuffer is not implemented yet");
	}

	Result<nvrhi::SamplerHandle> GraphicsDevice::CreateSampler(const nvrhi::SamplerDesc& /*desc*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphicsDevice::CreateSampler is not implemented yet");
	}

	Result<nvrhi::ShaderHandle> GraphicsDevice::CreateShader(const nvrhi::ShaderDesc& /*desc*/, std::span<const std::byte> /*binary*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphicsDevice::CreateShader is not implemented yet");
	}

	Result<nvrhi::ShaderHandle> GraphicsDevice::CreateShaderSpecialization(nvrhi::IShader& /*baseShader*/,
		std::span<const nvrhi::ShaderSpecialization> /*constants*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphicsDevice::CreateShaderSpecialization is not implemented yet");
	}

	Result<nvrhi::InputLayoutHandle> GraphicsDevice::CreateInputLayout(std::span<const nvrhi::VertexAttributeDesc> /*attributes*/,
		nvrhi::IShader* /*vertexShader*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphicsDevice::CreateInputLayout is not implemented yet");
	}

	Result<nvrhi::BindingLayoutHandle> GraphicsDevice::CreateBindingLayout(const nvrhi::BindingLayoutDesc& /*desc*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphicsDevice::CreateBindingLayout is not implemented yet");
	}

	Result<nvrhi::BindingSetHandle> GraphicsDevice::CreateBindingSet(const nvrhi::BindingSetDesc& /*desc*/,
		nvrhi::IBindingLayout& /*layout*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphicsDevice::CreateBindingSet is not implemented yet");
	}

	Result<nvrhi::FramebufferHandle> GraphicsDevice::CreateFramebuffer(const nvrhi::FramebufferDesc& /*desc*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphicsDevice::CreateFramebuffer is not implemented yet");
	}

	Result<nvrhi::GraphicsPipelineHandle> GraphicsDevice::CreateGraphicsPipeline(const nvrhi::GraphicsPipelineDesc& /*desc*/,
		const nvrhi::FramebufferInfo& /*framebufferInfo*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphicsDevice::CreateGraphicsPipeline is not implemented yet");
	}

	Result<nvrhi::ComputePipelineHandle> GraphicsDevice::CreateComputePipeline(const nvrhi::ComputePipelineDesc& /*desc*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphicsDevice::CreateComputePipeline is not implemented yet");
	}

	Result<nvrhi::CommandListHandle> GraphicsDevice::CreateCommandList(const nvrhi::CommandListParameters& /*parameters*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphicsDevice::CreateCommandList is not implemented yet");
	}

	Result<nvrhi::EventQueryHandle> GraphicsDevice::CreateEventQuery()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphicsDevice::CreateEventQuery is not implemented yet");
	}

	Result<nvrhi::TimerQueryHandle> GraphicsDevice::CreateTimerQuery()
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphicsDevice::CreateTimerQuery is not implemented yet");
	}

	Result<nvrhi::TextureHandle> GraphicsDevice::CreateHandleForNativeTexture(VkImage /*image*/, const nvrhi::TextureDesc& /*desc*/)
	{
		ENGINE_CONTRACT_STUB();
		return MakeError(ErrorCode::Unsupported, "GraphicsDevice::CreateHandleForNativeTexture is not implemented yet");
	}

	uint64_t GraphicsDevice::ExecuteCommandList(nvrhi::ICommandList& /*commandList*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	uint64_t GraphicsDevice::ExecuteCommandLists(std::span<nvrhi::ICommandList* const> /*commandLists*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	uint64_t GraphicsDevice::GetLastSubmissionID() const
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	uint64_t GraphicsDevice::GetCompletedSubmissionID()
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	void GraphicsDevice::RunGarbageCollection()
	{
		ENGINE_CONTRACT_STUB();
	}

	void GraphicsDevice::WaitForIdle()
	{
		ENGINE_CONTRACT_STUB();
	}

	void GraphicsDevice::RaiseDeviceLost(std::string_view context)
	{
		ENGINE_CONTRACT_STUB();
		FatalError(FatalErrorKind::DeviceLost, context);
	}

	std::string GraphicsDevice::DescribeDeviceFault()
	{
		ENGINE_CONTRACT_STUB();
		return "VK_EXT_device_fault is not available";
	}

	VkInstance GraphicsDevice::GetVulkanInstance() const
	{
		ENGINE_CONTRACT_STUB();
		return VK_NULL_HANDLE;
	}

	VkPhysicalDevice GraphicsDevice::GetVulkanPhysicalDevice() const
	{
		ENGINE_CONTRACT_STUB();
		return VK_NULL_HANDLE;
	}

	VkDevice GraphicsDevice::GetVulkanDevice() const
	{
		ENGINE_CONTRACT_STUB();
		return VK_NULL_HANDLE;
	}

	VkQueue GraphicsDevice::GetGraphicsQueue() const
	{
		ENGINE_CONTRACT_STUB();
		return VK_NULL_HANDLE;
	}

	VkSemaphore GraphicsDevice::GetGraphicsQueueTimelineSemaphore() const
	{
		ENGINE_CONTRACT_STUB();
		return VK_NULL_HANDLE;
	}

	void GraphicsDevice::QueueWaitForSemaphore(VkSemaphore /*semaphore*/, uint64_t /*value*/)
	{
		ENGINE_CONTRACT_STUB();
	}

	void GraphicsDevice::QueueSignalSemaphore(VkSemaphore /*semaphore*/, uint64_t /*value*/)
	{
		ENGINE_CONTRACT_STUB();
	}

}
