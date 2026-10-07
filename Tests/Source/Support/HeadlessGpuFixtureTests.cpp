#include "TestsPCH.h"

#include "Support/HeadlessGpuFixture.h"

#include "Engine/Core/VirtualFileSystem.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/ShaderLibrary.h"
#include "Support/ChildProcess.h"
#include "Support/TestOptions.h"

namespace Engine {

	namespace {

		// Replaces the run's TestOptions for its lifetime and restores them afterwards, even when a REQUIRE ends the test.
		class ScopedTestOptions
		{
		public:
			explicit ScopedTestOptions(Test::TestOptions options)
				: m_Saved(Test::GetTestOptions())
			{
				Test::SetTestOptions(std::move(options));
			}

			~ScopedTestOptions() { Test::SetTestOptions(m_Saved); }

			ScopedTestOptions(const ScopedTestOptions&) = delete;
			ScopedTestOptions& operator=(const ScopedTestOptions&) = delete;
		private:
			Test::TestOptions m_Saved;
		};

	}

	TEST_SUITE("Support")
	{
		TEST_CASE("HeadlessGpuFixture: creates a validated headless device with the run's API cap and the shaders mounted"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			CHECK(gpu.GetUnavailableReason().empty());
			const GraphicsDeviceInfo& info = gpu.GetDevice().GetInfo();
			CHECK(info.Validation);
			CHECK(info.SynchronizationValidation);
			CHECK(info.ApiVersion <= Test::GetTestOptions().VulkanApi);
			CHECK(gpu.GetVfs().IsMounted(ShaderScheme));
			const Result<const ShaderReflection*> reflection = gpu.GetShaders().GetReflection("Triangle", "VSMain");
			CHECK_MESSAGE(reflection.has_value(), (reflection.has_value() ? std::string() : reflection.error().ToString()));
			gpu.Reset();
			CHECK_FALSE(gpu.IsAvailable());
		}

		TEST_CASE("GpuSubmissionGate: a held submission completes only after the gate opens"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<Scope<Test::GpuSubmissionGate>> gate = Test::GpuSubmissionGate::Create(device);
			REQUIRE_MESSAGE(gate.has_value(), gate.error().ToString());
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());

			(*commandList)->open();
			(*commandList)->close();
			(*gate)->HoldNextSubmission();
			const uint64_t held = device.ExecuteCommandList(**commandList);
			// Later submissions complete after the held one, whatever they contain.
			(*commandList)->open();
			(*commandList)->close();
			const uint64_t later = device.ExecuteCommandList(**commandList);
			CHECK(device.GetCompletedSubmissionID() < held);

			(*gate)->Open();
			(*gate)->Open(); // once only
			device.WaitForIdle();
			CHECK(device.GetCompletedSubmissionID() >= later);
			commandList->Reset();
			gate->reset();
		}

		TEST_CASE("HeadlessGpuFixture: process output is searched for GPU messages and error lines")
		{
			const std::string output = "[12:00:00.000] [Engine] [info] Graphics device: 'GPU'\r\n"
									   "[12:00:00.001] [Engine] [trace] NVRHI: an info message\n"
									   "[12:00:00.002] [Engine] [warning] Vulkan validation: [VUID-x] a warning\n"
									   "[12:00:00.003] [Engine] [warning] Vulkan loader (Warning): an old layer\n"
									   "[12:00:00.004] [App] [error] Cannot open the project\n"
									   "[12:00:00.005] [Engine] [critical] NVRHI: a fatal message";
			const std::vector<std::string> gpu = Test::FindGpuMessageLines(output);
			const std::vector<std::string> expectedGpu = {
				"[12:00:00.002] [Engine] [warning] Vulkan validation: [VUID-x] a warning",
				"[12:00:00.005] [Engine] [critical] NVRHI: a fatal message",
			};
			CHECK(gpu == expectedGpu);
			const std::vector<std::string> problems = Test::FindProblemLogLines(output);
			const std::vector<std::string> expectedProblems = {
				"[12:00:00.002] [Engine] [warning] Vulkan validation: [VUID-x] a warning",
				"[12:00:00.004] [App] [error] Cannot open the project",
				"[12:00:00.005] [Engine] [critical] NVRHI: a fatal message",
			};
			CHECK(problems == expectedProblems);
			CHECK(Test::FindProblemLogLines("[12:00:00.000] [Engine] [info] fine\n").empty());
		}

		TEST_CASE("HeadlessGpuFixture: GPU process arguments follow the run's options")
		{
			struct Row
			{
				VulkanApiVersion Api = VulkanApiVersion::Vulkan14;
				bool RequireGpu = false;
			};
			const std::array<Row, 4> rows = { {
				{ VulkanApiVersion::Vulkan14, false },
				{ VulkanApiVersion::Vulkan14, true },
				{ VulkanApiVersion::Vulkan13, false },
				{ VulkanApiVersion::Vulkan13, true },
			} };
			for (const Row& row : rows)
			{
				CAPTURE(std::string(VulkanApiVersionToString(row.Api)));
				CAPTURE(row.RequireGpu);
				Test::TestOptions options = Test::GetTestOptions();
				options.VulkanApi = row.Api;
				options.RequireGpu = row.RequireGpu;
				const ScopedTestOptions scoped(std::move(options));

				// Editor and Runtime processes validate like the fixture and fail on any device error. --gpu-validation takes
				// its value only after '=' (an Optional option); the API cap appears only when the run caps it.
				const std::vector<std::string> application = Test::GetGpuApplicationArguments();
				CHECK(std::ranges::find(application, std::string("--gpu-validation=sync")) != application.end());
				CHECK(std::ranges::find(application, std::string("--expect-no-gpu-errors")) != application.end());
				const auto cap = std::ranges::find(application, std::string("--vulkan-api"));
				CHECK((cap != application.end()) == (row.Api == VulkanApiVersion::Vulkan13));
				if (cap != application.end())
				{
					REQUIRE(std::next(cap) != application.end());
					CHECK(*std::next(cap) == "1.3");
				}

				// Tests children always name the cap, and require a device exactly when this run does.
				const std::vector<std::string> child = Test::GetGpuTestsChildArguments();
				const std::string api = std::format("--vulkan-api={}", VulkanApiVersionToString(row.Api));
				CHECK(std::ranges::find(child, api) != child.end());
				CHECK((std::ranges::find(child, std::string("--require-gpu")) != child.end()) == row.RequireGpu);
			}
		}

		// Permanently skipped by design, not a contract stub: the target of the test below, which runs it in a child with
		// --no-skip, once with and once without --require-gpu. Under --require-gpu it fails on purpose.
		TEST_CASE("HeadlessGpuFixture: the unavailable-device target reports through ENGINE_REQUIRE_GPU"
			* doctest::test_suite(Test::ChildTargetSuite) * doctest::skip(true))
		{
			Test::ReportGpuUnavailable("the target has no device");
		}

		TEST_CASE("HeadlessGpuFixture: an unavailable device is a skip message, or a failure under --require-gpu")
		{
			const std::string target = "--test-case=HeadlessGpuFixture: the unavailable-device target reports through ENGINE_REQUIRE_GPU";
			const std::vector<std::string> skipping = { "--no-skip", target };
			const Result<Test::ChildProcessResult> skipped = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath, skipping,
				std::chrono::seconds(60));
			REQUIRE_MESSAGE(skipped.has_value(), skipped.error().ToString());
			INFO("child output: ", skipped->StandardOutput, skipped->StandardError);
			CHECK(skipped->ExitCode == 0);
			CHECK(skipped->StandardOutput.contains("GPU test skipped: the target has no device"));
			// The log line Scripts/Test.py counts (NO_DEVICE_PATTERN); child logs go to stderr.
			CHECK(skipped->StandardError.contains("GPU test without a device (passes without running): the target has no device"));

			const std::vector<std::string> requiring = { "--no-skip", target, "--require-gpu" };
			const Result<Test::ChildProcessResult> failed = Test::RunChildProcess(Test::GetTestOptions().ExecutablePath, requiring,
				std::chrono::seconds(60));
			REQUIRE_MESSAGE(failed.has_value(), failed.error().ToString());
			INFO("child output: ", failed->StandardOutput, failed->StandardError);
			CHECK(failed->ExitCode == 1);
			CHECK(failed->StandardOutput.contains("GPU test needs a device (--require-gpu): the target has no device"));
		}
	}

}
