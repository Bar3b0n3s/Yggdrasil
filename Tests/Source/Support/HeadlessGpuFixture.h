#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Graphics/GraphicsSpecification.h"

#include <doctest/doctest.h>
#include <vulkan/vulkan_core.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// GPU tests (Architecture §15.2, §15.3): a headless device per test case, created on the Tests process's Vulkan loader
// (the Tests main loads it with VulkanLoaderPolicy::IfAvailable) with validation and synchronization validation on and
// the API cap of --vulkan-api, so Test.py's gpu suite runs every GPU test under API 1.4 and under 1.3.
//
// GPU test cases carry the suite decorator doctest::test_suite(Test::GpuSuite); golden test cases live in
// TEST_SUITE(Test::GoldenSuite) (Golden/GoldenTests.cpp). Both suites are taken out of the unit stage (Test.py runs the
// unit suite with --test-suite-exclude=GPU,Golden) and into the gpu and golden stages (§15.8). They begin with
// ENGINE_REQUIRE_GPU:
//
//     TEST_CASE("Readback: clear colour is exact" * doctest::test_suite(Test::GpuSuite))
//     {
//         Test::HeadlessGpuFixture gpu;
//         ENGINE_REQUIRE_GPU(gpu);
//         ... gpu.GetDevice() ...
//     }
//
// Without a usable device (no loader, no Vulkan 1.3 device, a missing validation layer) the test passes after printing
// "GPU test skipped: <reason>", because doctest has no runtime skip; with --require-gpu (CI.py and PreCommit.py pass it
// unless they get --gpu-optional) it fails with the reason.

namespace Engine {

	class GraphicsDevice;
	class PipelineFactory;
	class ShaderLibrary;
	class VirtualFileSystem;

	namespace Test {

		// The doctest suites of the gpu and golden stages (§15.8).
		inline constexpr const char* GpuSuite = "GPU";
		inline constexpr const char* GoldenSuite = "Golden";

		struct HeadlessGpuOptions
		{
			// VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT (§15.3): on for every GPU test.
			bool SynchronizationValidation = true;
			// GraphicsSpecification::InjectFault, for the in-process fault tests.
			GpuFault InjectFault = GpuFault::None;
			uint32_t FramesInFlight = 2;
		};

		// One headless GraphicsDevice (GraphicsSpecification with Validation, the options above and MaxApiVersion =
		// GetTestOptions().VulkanApi), a VirtualFileSystem with shaders:// mounted read-only on ENGINE_SHADER_DIRECTORY, a
		// ShaderLibrary on it and a PipelineFactory. Not copyable or movable; main thread only. At most one exists at a time
		// (GraphicsDevice.h).
		//
		// Destruction (or Reset) checks, as doctest CHECKs naming the counts. After GraphicsDevice::WaitForIdle and
		// RunGarbageCollection, every GpuResourceTracker live count is 0; the objects the fixture itself owns are destroyed
		// first. Then GraphicsDevice::Destroy tears the device down, and the GpuDiagnostics error and warning counts it
		// returns, which include the messages of the teardown itself, are 0: validation, synchronization-validation and
		// NVRHI messages fail the test (§15.3). The loader's notes about the machine's installation are logged without
		// counting (IsLoaderInstallationMessage). A test that provokes validation messages on purpose calls
		// GpuDiagnostics::ResetCounts.
		class HeadlessGpuFixture
		{
		public:
			explicit HeadlessGpuFixture(const HeadlessGpuOptions& options = {});
			~HeadlessGpuFixture();

			HeadlessGpuFixture(const HeadlessGpuFixture&) = delete;
			HeadlessGpuFixture& operator=(const HeadlessGpuFixture&) = delete;

			// Whether the device was created; when not, GetUnavailableReason says why.
			[[nodiscard]] bool IsAvailable() const;
			[[nodiscard]] const std::string& GetUnavailableReason() const;

			// The services; only when IsAvailable (asserted).
			[[nodiscard]] GraphicsDevice& GetDevice();
			[[nodiscard]] ShaderLibrary& GetShaders();
			[[nodiscard]] PipelineFactory& GetPipelines();
			[[nodiscard]] VirtualFileSystem& GetVfs();

			// Destroys the services and the device now, with the destruction checks; afterwards IsAvailable is false. For
			// tests of what happens at device destruction.
			void Reset();
		private:
			// Creates the VFS, the device and the services; on failure sets m_UnavailableReason and leaves nothing created.
			void Create(const HeadlessGpuOptions& options);
		private:
			std::string m_UnavailableReason;
			// Declared in creation order and destroyed by Reset in the reverse order, with the checks in between.
			Scope<VirtualFileSystem> m_Vfs;
			Scope<GraphicsDevice> m_Device;
			Scope<ShaderLibrary> m_Shaders;
			Scope<PipelineFactory> m_Pipelines;
		};

		// The arguments an Editor or Runtime process started by a GPU test needs to render like the fixture and to fail like
		// it: --gpu-validation=sync (validation and synchronization validation, §15.3), --expect-no-gpu-errors (the process
		// exits with ExitCode::Failed when its device reported any error or warning in its whole life, so a validation
		// message inside the process fails the test that checks its exit code), and "--vulkan-api 1.3" when the run caps the
		// API.
		[[nodiscard]] std::vector<std::string> GetGpuApplicationArguments();

		// The arguments a Tests child process started by a GPU test needs (a windowed swapchain child,
		// Support/WindowedChild.h): --vulkan-api=<cap>, and --require-gpu when this run has it.
		[[nodiscard]] std::vector<std::string> GetGpuTestsChildArguments();

		// What ENGINE_REQUIRE_GPU does for an unavailable device: FAIL_CHECK with the reason under --require-gpu, otherwise
		// MESSAGE("GPU test skipped: <reason>") plus the Warn log line "GPU test without a device (passes without running):
		// <reason>", which Scripts/Test.py counts (NO_DEVICE_PATTERN).
		void ReportGpuUnavailable(std::string_view reason);

		// The lines of `output`, the standard error of an Editor or Runtime process that a GPU test started with
		// GetGpuApplicationArguments, that report a GPU validation or NVRHI message: "Vulkan validation:" or "NVRHI:" at
		// Warn, Error or Critical level (spdlog's "[warning]", "[error]" and "[critical]"). Such a process must have none,
		// also when it ends in an expected fatal error (the fault tests); --expect-no-gpu-errors already fails a run that
		// reaches its shutdown with any.
		[[nodiscard]] std::vector<std::string> FindGpuMessageLines(std::string_view output);
		// FindGpuMessageLines plus every other line logged at Error or Critical level: what a clean run must not print.
		[[nodiscard]] std::vector<std::string> FindProblemLogLines(std::string_view output);

		// For a GPU test whose code under test creates its own device: an Editor or Runtime process started with
		// GetGpuApplicationArguments, a windowed child, or an in-process Application with RendererMode::Vulkan. Creates and
		// destroys a HeadlessGpuFixture first (one device per process at a time) and returns whether it had a device.
		// Without one it calls ReportGpuUnavailable, so the test is reported like ENGINE_REQUIRE_GPU (and fails under
		// --require-gpu) instead of failing on the process's exit code or the application's InitFailed; the test then
		// returns at once.
		[[nodiscard]] bool ProbeGpuForProcess();

		// Mounts the shaders this configuration's Shaders project compiled (ENGINE_SHADER_DIRECTORY) read-only at shaders://
		// in `vfs` and returns the root a ShaderLibrary takes, as development builds of the engine context do
		// (EngineContext.h, the Graphics step). Errors: those of NativeDirectoryMount::Create (a build without compiled
		// shaders, with a hint to build them) and of VirtualFileSystem::Mount.
		[[nodiscard]] Result<VfsPath> MountCompiledShaders(VirtualFileSystem& vfs);

		// Holds the GPU back for tests of what must not happen while a submission is in flight: a timeline semaphore that
		// the device's next submission waits for (HoldNextSubmission) until Open signals it from the host. Every later
		// submission on the graphics queue completes after the held one, so nothing submitted after HoldNextSubmission
		// completes before Open, however fast the GPU is, and the validation layer knows it (it reports, for example, an
		// object destroyed while a pending command buffer uses it). The destructor opens the gate, waits for idle and
		// destroys the semaphore. Not copyable or movable; main thread only.
		class GpuSubmissionGate
		{
		public:
			// Errors: Gpu when the semaphore cannot be created.
			[[nodiscard]] static Result<Scope<GpuSubmissionGate>> Create(GraphicsDevice& device);

			// Use Create. `device` is a documented back-reference that must outlive the gate.
			GpuSubmissionGate(GraphicsDevice& device, VkSemaphore semaphore);
			~GpuSubmissionGate();

			GpuSubmissionGate(const GpuSubmissionGate&) = delete;
			GpuSubmissionGate& operator=(const GpuSubmissionGate&) = delete;

			// Makes the device's next submission wait until Open (GraphicsDevice::QueueWaitForSemaphore).
			void HoldNextSubmission();
			// Signals the semaphore from the host, releasing the held submission and everything queued behind it. Once only;
			// later calls do nothing.
			void Open();
		private:
			GraphicsDevice* m_Device = nullptr; // documented back-reference
			VkSemaphore m_Semaphore = VK_NULL_HANDLE;
			bool m_IsOpen = false;
		};

	}

}

// At the start of a GPU test case (see the file comment): returns from the test case when `fixture` has no device.
#define ENGINE_REQUIRE_GPU(fixture) \
	do \
	{ \
		if (!(fixture).IsAvailable()) \
		{ \
			::Engine::Test::ReportGpuUnavailable((fixture).GetUnavailableReason()); \
			return; \
		} \
	} while (false)
