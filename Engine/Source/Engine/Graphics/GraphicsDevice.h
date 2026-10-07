#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/DeviceSelection.h"
#include "Engine/Graphics/GpuDiagnostics.h"
#include "Engine/Graphics/GpuResourceTracker.h"
#include "Engine/Graphics/GraphicsSpecification.h"

#include <nvrhi/nvrhi.h>
#include <vulkan/vulkan_core.h>

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// The Vulkan device behind NVRHI (Architecture §8.1, §8.14): instance, debug messenger, physical-device selection,
// logical device, the NVRHI device (wrapped by NVRHI's validation layer when validation is on), and the null-checked
// creation wrappers through which every engine GPU object is made.

namespace Engine {

	class HostImageUpload;
	class Window;

	struct GraphicsDeviceSpecification
	{
		GraphicsSpecification Graphics{};
		// The window the process presents to. Set for windowed processes: the instance enables GLFW's surface extensions
		// (glfwGetRequiredInstanceExtensions) and the device needs a queue family that can present
		// (glfwGetPhysicalDevicePresentationSupport) and VK_KHR_swapchain. Null for headless processes, which create no
		// surface extensions (§8.1). Only read during Create: a documented parameter, never stored.
		Window* PresentWindow = nullptr;
		// VkApplicationInfo::pApplicationName.
		std::string ApplicationName{};
	};

	// What the created device is and which optional paths it took (§8.1). Filled once by Create.
	struct GraphicsDeviceInfo
	{
		std::string DeviceName{};
		GpuDeviceType Type = GpuDeviceType::Other;
		uint32_t VendorID = 0;
		uint32_t DeviceID = 0;
		uint32_t DriverVersion = 0;
		VkDriverId DriverID{};
		// GetDeviceClass(VendorID, DriverVersion, DriverID): the golden-image directory name (§15.4).
		std::string DeviceClass{};
		// The API version the instance and device use: min(GraphicsSpecification::MaxApiVersion, loader, device).
		VulkanApiVersion ApiVersion = VulkanApiVersion::Vulkan13;
		// Optional paths in use (each false under --vulkan-api=1.3 when it is a 1.4 capability).
		bool HostImageCopy = false;
		bool DeviceFault = false;
		bool DepthClamp = false;
		bool FillModeNonSolid = false;
		bool PortabilitySubset = false;
		// Bloom's storage format: R11G11B10_FLOAT, or RGBA16_FLOAT when unsupported as storage (DeviceSelection.h).
		nvrhi::Format BloomFormat = nvrhi::Format::R11G11B10_FLOAT;
		// Whether validation, and synchronization validation, are on.
		bool Validation = false;
		bool SynchronizationValidation = false;
		// VkPhysicalDeviceLimits::maxMemoryAllocationCount (RenderStats warns above 2,000 allocations, §8.14 item 5).
		uint32_t MaxMemoryAllocationCount = 0;
		// The graphics (and present) queue family.
		uint32_t GraphicsQueueFamily = 0;
	};

	// One GPU device. Not copyable or movable; held by EngineContext in a Scope (null with RendererMode::None).
	//
	// At most one GraphicsDevice exists at a time in a process (asserted): vulkan.hpp's default dispatcher, which NVRHI
	// calls through, is process-level state that Create initializes with this device's instance and device (§3 rule 5,
	// §8.1). Tests therefore create devices one after another, never side by side.
	//
	// Thread safety: main thread only, like every NVRHI call (§4.11), except the GpuDiagnostics it owns.
	//
	// Fatal paths (§8.1, §8.14 item 6): device loss, a hang and GPU out-of-memory of a render target or pipeline end in
	// FatalError, whose handler writes the autosave first (the editor's hook) and then the crash report. RaiseDeviceLost
	// puts the VK_EXT_device_fault description into the FatalError message, so the report's Reason line carries it.
	class GraphicsDevice
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class GraphicsDevice;
		};

		// Use Create. Stores the specification and builds the diagnostics with its InjectFault.
		GraphicsDevice(ConstructionKey key, const GraphicsSpecification& specification);
		// vkDeviceWaitIdle through the dispatcher's C entry point (a failure is logged, never fatal and never thrown during
		// teardown), NVRHI's runGarbageCollection (so retired command lists drop their references), the HostImageUpload (its
		// remaining host images), a final tracker Sweep, the live counts reported as a GpuDiagnostics error when any is
		// above zero (the tests and application teardown assert zero), the tracker's ReleaseAll, then the §8.14 item 4 order
		// for what the device owns: NVRHI device, VkDevice, debug messenger, VkInstance. Objects created through the
		// wrappers must be gone by then.
		~GraphicsDevice();

		GraphicsDevice(const GraphicsDevice&) = delete;
		GraphicsDevice& operator=(const GraphicsDevice&) = delete;

		// Creates the device (§8.1). Requires VulkanDispatch to be initialized. Steps: instance with apiVersion
		// min(MaxApiVersion, loader version) (below 1.3 is Unsupported), the validation layer and VK_EXT_debug_utils when
		// Validation is on (plus synchronization validation when requested), VK_KHR_portability_enumeration on macOS,
		// GLFW's surface extensions when PresentWindow is set; VULKAN_HPP_DEFAULT_DISPATCHER.init(instance); one
		// DeviceCandidate per physical device and SelectDevice (override: GraphicsSpecification::GpuOverride, else the
		// ENGINE_GPU environment variable); a device with the 1.3 feature set NVRHI needs (dynamicRendering,
		// synchronization2, timelineSemaphore) plus the selected optional features and extensions (hostImageCopy only at API
		// 1.4, VK_EXT_device_fault, VK_KHR_portability_subset, VK_KHR_swapchain when presenting);
		// VULKAN_HPP_DEFAULT_DISPATCHER.init(device); nvrhi::vulkan::createDevice with the enabled extension lists and this
		// device's GpuDiagnostics as errorCB, wrapped by nvrhi::validation::createValidationLayer when validation is on.
		// vk::SystemError thrown during these steps is caught here (§4.6 item 1) and becomes a Gpu error.
		// Logs one Info line naming the device, its type, the API version, the device class and the optional paths.
		// Errors, in the order they are checked: InvalidArgument for FramesInFlight 0, or an InjectFault other than None in
		// Dist (before any Vulkan call); InvalidState without an initialized VulkanDispatch or while another GraphicsDevice
		// exists; Unsupported for an API version
		// below 1.3, a missing validation layer while Validation is on, or no acceptable device (SelectDevice); NotFound
		// for an override matching no device; Gpu for a failed Vulkan call or a null NVRHI device. Nothing stays created on
		// failure.
		[[nodiscard]] static Result<Scope<GraphicsDevice>> Create(const GraphicsDeviceSpecification& specification);

		// Destroys `device` (the destructor's teardown) and returns its GpuDiagnostics counts afterwards, which include the
		// messages of the teardown itself: the leak report of objects still alive and the validation layer's reports at
		// vkDestroyDevice and vkDestroyInstance. For the checks that must see a device's whole life: HeadlessGpuFixture and
		// --expect-no-gpu-errors (EngineContext::DestroyGraphics). A null `device` gives zero counts.
		[[nodiscard]] static GpuMessageCounts Destroy(Scope<GraphicsDevice> device);

		// The NVRHI device (the validation wrapper when validation is on), for command lists and other non-creating calls.
		// Engine code never calls its create* functions directly (§8.14 item 7): use the wrappers below.
		[[nodiscard]] nvrhi::IDevice* GetNvrhiDevice() const;
		[[nodiscard]] const GraphicsDeviceInfo& GetInfo() const;
		[[nodiscard]] const GraphicsSpecification& GetSpecification() const;
		[[nodiscard]] GpuDiagnostics& GetDiagnostics();
		[[nodiscard]] const GpuDiagnostics& GetDiagnostics() const;
		[[nodiscard]] GpuResourceTracker& GetResourceTracker();
		[[nodiscard]] const GpuResourceTracker& GetResourceTracker() const;
		// The immutable-texture upload paths (§8.1 item 1), owned by the device.
		[[nodiscard]] HostImageUpload& GetHostImageUpload();

		// Creation wrappers (§8.14 item 7). Each calls the NVRHI function, turns a null result into an ErrorCode::Gpu error
		// naming the object type and the descriptor's debugName ("cannot create texture 'SceneColor' (1920x1080
		// RGBA16_FLOAT)"), and tracks the object (GpuResourceTracker), except CreateHandleForNativeTexture. CreateTexture
		// also fails with Gpu, without calling NVRHI, when GpuDiagnostics::ShouldFailTextureCreation(desc)
		// (--gpu-inject-fault=oom-texture). None of them ever crashes on a null result; the caller decides between a
		// placeholder (asset uploads) and FatalError(OutOfMemory) (render targets and pipelines created at startup or
		// resize, §8.14 item 7).
		[[nodiscard]] Result<nvrhi::TextureHandle> CreateTexture(const nvrhi::TextureDesc& desc);
		[[nodiscard]] Result<nvrhi::StagingTextureHandle> CreateStagingTexture(const nvrhi::TextureDesc& desc, nvrhi::CpuAccessMode cpuAccess);
		[[nodiscard]] Result<nvrhi::BufferHandle> CreateBuffer(const nvrhi::BufferDesc& desc);
		[[nodiscard]] Result<nvrhi::SamplerHandle> CreateSampler(const nvrhi::SamplerDesc& desc);
		[[nodiscard]] Result<nvrhi::ShaderHandle> CreateShader(const nvrhi::ShaderDesc& desc, std::span<const std::byte> binary);
		[[nodiscard]] Result<nvrhi::ShaderHandle> CreateShaderSpecialization(nvrhi::IShader& baseShader,
			std::span<const nvrhi::ShaderSpecialization> constants);
		[[nodiscard]] Result<nvrhi::InputLayoutHandle> CreateInputLayout(std::span<const nvrhi::VertexAttributeDesc> attributes,
			nvrhi::IShader* vertexShader);
		[[nodiscard]] Result<nvrhi::BindingLayoutHandle> CreateBindingLayout(const nvrhi::BindingLayoutDesc& desc);
		[[nodiscard]] Result<nvrhi::BindingSetHandle> CreateBindingSet(const nvrhi::BindingSetDesc& desc, nvrhi::IBindingLayout& layout);
		[[nodiscard]] Result<nvrhi::FramebufferHandle> CreateFramebuffer(const nvrhi::FramebufferDesc& desc);
		[[nodiscard]] Result<nvrhi::GraphicsPipelineHandle> CreateGraphicsPipeline(const nvrhi::GraphicsPipelineDesc& desc,
			const nvrhi::FramebufferInfo& framebufferInfo);
		[[nodiscard]] Result<nvrhi::ComputePipelineHandle> CreateComputePipeline(const nvrhi::ComputePipelineDesc& desc);
		[[nodiscard]] Result<nvrhi::CommandListHandle> CreateCommandList(const nvrhi::CommandListParameters& parameters = {});
		[[nodiscard]] Result<nvrhi::EventQueryHandle> CreateEventQuery();
		[[nodiscard]] Result<nvrhi::TimerQueryHandle> CreateTimerQuery();
		// Wraps an image the engine created itself (swapchain images, HostImageUpload's host-copied images) with
		// createHandleForNativeTexture(nvrhi::ObjectTypes::VK_Image, ...). NVRHI does not own `image`; whoever created it
		// destroys it after the handle and every submission using it are gone. The wrapper is not tracked: the tracker's
		// reference would keep it alive past the moment its creator destroys the image. Its creator counts the image instead
		// (HostImageUpload as GpuResourceType::HostImage), and the swapchain's images live exactly as long as the swapchain.
		[[nodiscard]] Result<nvrhi::TextureHandle> CreateHandleForNativeTexture(VkImage image, const nvrhi::TextureDesc& desc);

		// Executes closed command lists on the graphics queue and returns the submission ID (the value the queue's timeline
		// semaphore reaches when they complete). Afterwards: GpuDiagnostics::OnSubmitted, then, when the device-lost flag is
		// set, RaiseDeviceLost (§8.1: NVRHI reports device loss inside submit through its message callback). Every
		// submission of the engine goes through these two functions.
		uint64_t ExecuteCommandList(nvrhi::ICommandList& commandList);
		uint64_t ExecuteCommandLists(std::span<nvrhi::ICommandList* const> commandLists);
		// The ID of the last submission; 0 before the first.
		[[nodiscard]] uint64_t GetLastSubmissionID() const;
		// The highest submission ID the GPU has completed (vkGetSemaphoreCounterValue on the graphics queue's timeline
		// semaphore; never waits). A VK_ERROR_DEVICE_LOST result calls RaiseDeviceLost.
		[[nodiscard]] uint64_t GetCompletedSubmissionID();

		// Once per frame, after the frame's last submission (§8.2): NVRHI's runGarbageCollection, which retires the
		// completed command lists and their references, then the tracker's Sweep, which releases what only the tracker
		// still holds, then HostImageUpload::CollectGarbage, the host images' deferred release.
		void RunGarbageCollection();

		// vkDeviceWaitIdle, for shutdown and resize (§8.1), through the dispatcher's C entry point, so it never throws
		// (§4.6 item 2) and works outside the frame boundary too. VK_ERROR_DEVICE_LOST, or the device-lost flag already set,
		// calls RaiseDeviceLost; any other failure ends the process with FatalError(GetFatalErrorKind(result)) (out of
		// memory is OutOfMemory). Never waits on its own beyond what vkDeviceWaitIdle does.
		void WaitForIdle();

		// Ends the process through FatalError(DeviceLost) with "<context>: device lost" followed by DescribeDeviceFault()
		// when VK_EXT_device_fault is enabled (§8.14 item 6). The fatal-error handler then writes the autosave and the
		// crash report. Callable from any submission or wait path on the main thread.
		[[noreturn]] void RaiseDeviceLost(std::string_view context);
		// vkGetDeviceFaultInfoEXT as text (description, address infos, vendor infos); "VK_EXT_device_fault is not
		// available" when the extension is not enabled, "no fault information" when the driver reports none. The query is
		// valid only on a device that is really lost (the validation layer rejects it otherwise), so a healthy device, and
		// one whose loss is only the injected device-lost fault, report "no fault information" without querying.
		[[nodiscard]] std::string DescribeDeviceFault();

		// Native handles for the code that calls Vulkan itself through the dispatcher's C entry points: Swapchain and
		// HostImageUpload (§8.1). Valid for the device's lifetime.
		[[nodiscard]] VkInstance GetVulkanInstance() const;
		[[nodiscard]] VkPhysicalDevice GetVulkanPhysicalDevice() const;
		[[nodiscard]] VkDevice GetVulkanDevice() const;
		[[nodiscard]] VkQueue GetGraphicsQueue() const;
		// The graphics queue's timeline semaphore (nvrhi::vulkan::IDevice::getQueueSemaphore), which reaches each
		// submission ID when that submission completes (FramePacer's bounded waits, §8.1).
		[[nodiscard]] VkSemaphore GetGraphicsQueueTimelineSemaphore() const;
		// Queues a semaphore wait or signal that NVRHI attaches to the next executeCommandLists on the graphics queue
		// (nvrhi::vulkan::IDevice::queueWaitForSemaphore / queueSignalSemaphore; value 0 for binary semaphores). The
		// swapchain's acquire and present semaphores go through these (§8.1).
		void QueueWaitForSemaphore(VkSemaphore semaphore, uint64_t value);
		void QueueSignalSemaphore(VkSemaphore semaphore, uint64_t value);

		// GraphicsSpecification::FramesInFlight.
		[[nodiscard]] uint32_t GetFramesInFlight() const { return m_Specification.FramesInFlight; }
	private:
		// The steps of Create, in order; each fills the members it creates, which the destructor tears down when a later
		// step fails.
		[[nodiscard]] Status CreateInstance(const GraphicsDeviceSpecification& specification);
		[[nodiscard]] Status CreateLogicalDevice(const GraphicsDeviceSpecification& specification);
		[[nodiscard]] Status CreateNvrhiDevice();
		// Logs the Info line that names the device and its optional paths.
		void LogDeviceInfo() const;
		// The destructor's teardown (see there); runs once, from the destructor or from Destroy.
		void Shutdown();
	private:
		GraphicsSpecification m_Specification;
		GraphicsDeviceInfo m_Info;
		GpuDiagnostics m_Diagnostics;
		GpuResourceTracker m_ResourceTracker;
		Scope<HostImageUpload> m_HostImageUpload; // created by Create once the device exists

		// The Vulkan objects, destroyed by the destructor in the reverse order of creation (§8.14 item 4).
		VkInstance m_Instance = VK_NULL_HANDLE;
		VkDebugUtilsMessengerEXT m_DebugMessenger = VK_NULL_HANDLE;
		VkPhysicalDevice m_PhysicalDevice = VK_NULL_HANDLE;
		VkDevice m_Device = VK_NULL_HANDLE;
		VkQueue m_GraphicsQueue = VK_NULL_HANDLE;
		VkSemaphore m_GraphicsQueueTimelineSemaphore = VK_NULL_HANDLE; // owned by NVRHI's queue
		// The extensions enabled at instance and device creation, which NVRHI is told about.
		std::vector<std::string> m_InstanceExtensions;
		std::vector<std::string> m_DeviceExtensions;
		// NVRHI's Vulkan device (an nvrhi::vulkan::IDevice, for the queue semaphore calls) and the device engine code uses:
		// the same object, or NVRHI's validation layer around it when validation is on.
		nvrhi::DeviceHandle m_VulkanNvrhiDevice;
		nvrhi::DeviceHandle m_NvrhiDevice;
		uint64_t m_LastSubmissionID = 0;
		// Whether this device counts as the process's live device (VulkanDispatch::RegisterDevice).
		bool m_IsRegistered = false;
		// Set by Shutdown, which runs once.
		bool m_IsShutDown = false;
	};

}
