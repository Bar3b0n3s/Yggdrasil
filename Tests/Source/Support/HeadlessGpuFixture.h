#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Graphics/GraphicsSpecification.h"

#include <doctest/doctest.h>

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// GPU tests (Architecture §15.2, §15.3): a headless device per test case, created on the Tests process's Vulkan loader
// (the Tests main loads it with VulkanLoaderPolicy::IfAvailable) with validation and synchronization validation on and
// the API cap of --vulkan-api, so Test.py's gpu suite runs every GPU test under API 1.4 and under 1.3.
//
// GPU test cases carry the suite decorator doctest::test_suite(Test::GpuSuite) (golden ones Test::GoldenSuite), which
// takes them out of the unit stage (Test.py runs the unit suite with --test-suite-exclude=GPU,Golden) and into the gpu
// and golden stages (§15.8). They begin with ENGINE_REQUIRE_GPU:
//
//     TEST_CASE("Readback: clear colour is exact" * doctest::test_suite(Test::GpuSuite))
//     {
//         Test::HeadlessGpuFixture gpu;
//         ENGINE_REQUIRE_GPU(gpu);
//         ... gpu.GetDevice() ...
//     }
//
// Without a usable device (no loader, no Vulkan 1.3 device, a missing validation layer) the test passes after printing
// "GPU test skipped: <reason>", because doctest has no runtime skip; with --require-gpu, which CI.py always passes on
// this machine, it fails with the reason.

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
		// Destruction (or Reset) checks, as doctest CHECKs naming the counts: the device's GpuDiagnostics error count is 0
		// (validation and synchronization-validation messages fail the test, §15.3), and after GraphicsDevice::WaitForIdle and
		// RunGarbageCollection every GpuResourceTracker live count is 0, except for the objects the fixture itself owns, which
		// it destroys first. A test that provokes validation messages on purpose calls GpuDiagnostics::ResetCounts.
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
			std::string m_UnavailableReason;
		};

		// The arguments an Editor or Runtime process started by a GPU test needs to render like the fixture and to fail like
		// it: --gpu-validation=sync (validation and synchronization validation, §15.3), --expect-no-gpu-errors (the process
		// exits with ExitCode::Failed when its device reported any error, so a validation error inside the process fails the
		// test that checks its exit code), and "--vulkan-api 1.3" when the run caps the API.
		[[nodiscard]] std::vector<std::string> GetGpuApplicationArguments();

		// The arguments a Tests child process started by a GPU test needs (a windowed swapchain child,
		// Support/WindowedChild.h): --vulkan-api=<cap>, and --require-gpu when this run has it.
		[[nodiscard]] std::vector<std::string> GetGpuTestsChildArguments();

		// What ENGINE_REQUIRE_GPU does for an unavailable device: FAIL_CHECK with the reason under --require-gpu, otherwise
		// MESSAGE("GPU test skipped: <reason>").
		void ReportGpuUnavailable(std::string_view reason);

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
