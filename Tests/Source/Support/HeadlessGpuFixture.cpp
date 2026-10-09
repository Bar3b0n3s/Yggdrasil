#include "TestsPCH.h"
#include "Support/HeadlessGpuFixture.h"

#include "Engine/Core/Assert.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Mounts/NativeDirectoryMount.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GpuDiagnostics.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/PipelineFactory.h"
#include "Engine/Graphics/ShaderLibrary.h"
#include "Support/TestOptions.h"
#include "Support/Utf8Path.h"

#include <vulkan/vulkan.hpp>

#if !defined(ENGINE_SHADER_DIRECTORY)
	#error "ENGINE_SHADER_DIRECTORY must be defined for the Tests project (Dependencies.lua, ApplyFirstPartySettings)"
#endif

namespace Engine {

	namespace Test {

		namespace Utils {

			// The API version applications request when no --vulkan-api caps it (GraphicsSpecification's default).
			constexpr VulkanApiVersion DefaultApplicationApiVersion = GraphicsSpecification{}.MaxApiVersion;

			// The level tags of the console log pattern (Core/Log.cpp: "[%H:%M:%S.%e] [%n] [%l] %v").
			constexpr std::string_view WarnTag = "[warning]";
			constexpr std::string_view ErrorTag = "[error]";
			constexpr std::string_view CriticalTag = "[critical]";

			// The lines of `output` that `predicate` accepts, without their line ends.
			template<typename Predicate>
			static std::vector<std::string> FindLines(std::string_view output, Predicate predicate)
			{
				std::vector<std::string> lines;
				while (!output.empty())
				{
					const size_t end = output.find('\n');
					std::string_view line = output.substr(0, end);
					output = end == std::string_view::npos ? std::string_view() : output.substr(end + 1);
					if (line.ends_with('\r'))
						line.remove_suffix(1);
					if (predicate(line))
						lines.emplace_back(line);
				}
				return lines;
			}

			static bool IsErrorLine(std::string_view line)
			{
				return line.contains(ErrorTag) || line.contains(CriticalTag);
			}

			static bool IsGpuMessageLine(std::string_view line)
			{
				const bool gpuSource = line.contains("Vulkan validation:") || line.contains("NVRHI:");
				return gpuSource && (line.contains(WarnTag) || IsErrorLine(line));
			}

		}

		std::vector<std::string> FindGpuMessageLines(std::string_view output)
		{
			return Utils::FindLines(output, &Utils::IsGpuMessageLine);
		}

		std::vector<std::string> FindProblemLogLines(std::string_view output)
		{
			return Utils::FindLines(output, [](std::string_view line)
			{
				return Utils::IsGpuMessageLine(line) || Utils::IsErrorLine(line);
			});
		}

		Result<VfsPath> MountCompiledShaders(VirtualFileSystem& vfs)
		{
			Result<Scope<NativeDirectoryMount>> mount =
				NativeDirectoryMount::Create(PathFromUtf8(ENGINE_SHADER_DIRECTORY), MountAccess::ReadOnly);
			if (!mount.has_value())
			{
				Error error = std::move(mount).error().WithContext("while mounting the compiled shaders");
				return std::unexpected(std::move(error).WithHint("build this configuration's Shaders project (python Scripts/Build.py)"));
			}
			ENGINE_TRY(vfs.Mount(ShaderScheme, std::move(*mount)));
			return VfsPath::Create(ShaderScheme, "");
		}

		HeadlessGpuFixture::HeadlessGpuFixture(const HeadlessGpuOptions& options)
		{
			Create(options);
		}

		HeadlessGpuFixture::~HeadlessGpuFixture()
		{
			Reset();
		}

		bool HeadlessGpuFixture::IsAvailable() const
		{
			return m_Device != nullptr;
		}

		const std::string& HeadlessGpuFixture::GetUnavailableReason() const
		{
			return m_UnavailableReason;
		}

		GraphicsDevice& HeadlessGpuFixture::GetDevice()
		{
			ENGINE_CORE_ASSERT(IsAvailable(), "HeadlessGpuFixture::GetDevice without a device ({})", m_UnavailableReason);
			return *m_Device;
		}

		ShaderLibrary& HeadlessGpuFixture::GetShaders()
		{
			ENGINE_CORE_ASSERT(IsAvailable(), "HeadlessGpuFixture::GetShaders without a device ({})", m_UnavailableReason);
			return *m_Shaders;
		}

		PipelineFactory& HeadlessGpuFixture::GetPipelines()
		{
			ENGINE_CORE_ASSERT(IsAvailable(), "HeadlessGpuFixture::GetPipelines without a device ({})", m_UnavailableReason);
			return *m_Pipelines;
		}

		VirtualFileSystem& HeadlessGpuFixture::GetVfs()
		{
			ENGINE_CORE_ASSERT(IsAvailable(), "HeadlessGpuFixture::GetVfs without a device ({})", m_UnavailableReason);
			return *m_Vfs;
		}

		void HeadlessGpuFixture::Reset()
		{
			if (m_Device == nullptr)
				return;

			// The fixture's own GPU objects go first (the library caches shaders), so whatever is still alive after the garbage
			// collection belongs to the test.
			m_Pipelines.reset();
			m_Shaders.reset();
			m_Device->WaitForIdle();
			m_Device->RunGarbageCollection();
			const GpuResourceTracker& tracker = m_Device->GetResourceTracker();
			CHECK_MESSAGE(tracker.GetTotalLiveCount() == 0,
				"GPU objects are still alive at the end of the test (§8.14 item 5): " << tracker.DescribeLiveCounts());

			// The counts after the teardown, which include its own messages (leaks, the validation layer's reports).
			const GpuMessageCounts counts = GraphicsDevice::Destroy(std::move(m_Device));
			CHECK_MESSAGE(counts.Errors == 0,
				"the GPU device reported " << counts.Errors << " validation or other error(s) (§15.3); the log names them");
			CHECK_MESSAGE(counts.Warnings == 0,
				"the GPU device reported " << counts.Warnings
										   << " validation or NVRHI warning(s); GPU tests run with zero validation messages; the log names them");
			m_Vfs.reset();
		}

		void HeadlessGpuFixture::Create(const HeadlessGpuOptions& options)
		{
			GraphicsDeviceSpecification specification;
			specification.Graphics.Validation = true;
			specification.Graphics.SynchronizationValidation = options.SynchronizationValidation;
			specification.Graphics.MaxApiVersion = GetTestOptions().VulkanApi;
			specification.Graphics.FramesInFlight = options.FramesInFlight;
			specification.Graphics.InjectFault = options.InjectFault;
			specification.Graphics.DisableDepthClamp = options.DisableDepthClamp;
			specification.ApplicationName = "Tests";
			Result<Scope<GraphicsDevice>> device = GraphicsDevice::Create(specification);
			if (!device.has_value())
			{
				m_UnavailableReason = device.error().ToString();
				return;
			}

			// A build without compiled shaders is broken, not a machine without a GPU: that fails the test in every mode.
			Scope<VirtualFileSystem> vfs = CreateScope<VirtualFileSystem>();
			Result<VfsPath> shaderRoot = MountCompiledShaders(*vfs);
			if (!shaderRoot.has_value())
			{
				m_UnavailableReason = shaderRoot.error().ToString();
				FAIL_CHECK(m_UnavailableReason);
				return;
			}

			m_Vfs = std::move(vfs);
			m_Device = std::move(*device);
			m_Shaders = CreateScope<ShaderLibrary>(m_Device.get(), *m_Vfs, std::move(*shaderRoot));
			m_Pipelines = CreateScope<PipelineFactory>(*m_Device, *m_Shaders);
		}

		std::vector<std::string> GetGpuApplicationArguments()
		{
			std::vector<std::string> arguments = { "--gpu-validation=sync", "--expect-no-gpu-errors" };
			const VulkanApiVersion api = GetTestOptions().VulkanApi;
			if (api != Utils::DefaultApplicationApiVersion)
			{
				arguments.emplace_back("--vulkan-api");
				arguments.emplace_back(VulkanApiVersionToString(api));
			}
			return arguments;
		}

		std::vector<std::string> GetGpuTestsChildArguments()
		{
			std::vector<std::string> arguments = { std::format("--vulkan-api={}", VulkanApiVersionToString(GetTestOptions().VulkanApi)) };
			if (GetTestOptions().RequireGpu)
				arguments.emplace_back("--require-gpu");
			return arguments;
		}

		void ReportGpuUnavailable(std::string_view reason)
		{
			if (GetTestOptions().RequireGpu)
			{
				FAIL_CHECK("GPU test needs a device (--require-gpu): " << std::string(reason));
				return;
			}
			MESSAGE("GPU test skipped: " << std::string(reason));
			// Scripts/Test.py counts these lines (whichever doctest reporter runs) and reports the run as a warning: keep the
			// text in step with its NO_DEVICE_PATTERN.
			ENGINE_CORE_WARN("GPU test without a device (passes without running): {}", reason);
		}

		bool ProbeGpuForProcess()
		{
			HeadlessGpuFixture probe;
			if (!probe.IsAvailable())
			{
				ReportGpuUnavailable(probe.GetUnavailableReason());
				return false;
			}
			return true;
		}

		Result<Scope<GpuSubmissionGate>> GpuSubmissionGate::Create(GraphicsDevice& device)
		{
			VkSemaphoreTypeCreateInfo typeInfo{};
			typeInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO;
			typeInfo.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
			typeInfo.initialValue = 0;
			VkSemaphoreCreateInfo createInfo{};
			createInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
			createInfo.pNext = &typeInfo;
			VkSemaphore semaphore = VK_NULL_HANDLE;
			const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkCreateSemaphore(device.GetVulkanDevice(), &createInfo, nullptr, &semaphore);
			if (result != VK_SUCCESS)
				return MakeError(ErrorCode::Gpu, "vkCreateSemaphore of the submission gate failed: {}", VkResultToString(result));
			return CreateScope<GpuSubmissionGate>(device, semaphore);
		}

		GpuSubmissionGate::GpuSubmissionGate(GraphicsDevice& device, VkSemaphore semaphore)
			: m_Device(&device), m_Semaphore(semaphore)
		{
		}

		GpuSubmissionGate::~GpuSubmissionGate()
		{
			// A pending wait must not outlive its semaphore.
			Open();
			m_Device->WaitForIdle();
			VULKAN_HPP_DEFAULT_DISPATCHER.vkDestroySemaphore(m_Device->GetVulkanDevice(), m_Semaphore, nullptr);
		}

		void GpuSubmissionGate::HoldNextSubmission()
		{
			m_Device->QueueWaitForSemaphore(m_Semaphore, 1);
		}

		void GpuSubmissionGate::Open()
		{
			if (m_IsOpen)
				return;
			m_IsOpen = true;
			VkSemaphoreSignalInfo signalInfo{};
			signalInfo.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SIGNAL_INFO;
			signalInfo.semaphore = m_Semaphore;
			signalInfo.value = 1;
			const VkResult result = VULKAN_HPP_DEFAULT_DISPATCHER.vkSignalSemaphore(m_Device->GetVulkanDevice(), &signalInfo);
			CHECK_MESSAGE(result == VK_SUCCESS, "vkSignalSemaphore of the submission gate failed: " << VkResultToString(result));
		}

	}

}
