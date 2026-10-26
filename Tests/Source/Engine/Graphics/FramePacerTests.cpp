#include "TestsPCH.h"

#include "Engine/Graphics/FramePacer.h"

#include "Engine/App/ExitCode.h"
#include "Engine/Core/Log.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Support/DeathTest.h"
#include "Support/HeadlessGpuFixture.h"

#include <cstdint>
#include <limits>

namespace Engine {

	// Paces empty frames on a headless device with the hang fault until a bounded wait ends the process, after its budget and
	// its reset wait: the child of the hang test. Without a device there is nothing to wait for, which the parent reports as a
	// skip.
	ENGINE_DEATH_TEST("Graphics/FramePacerInjectedHang")
	{
		Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::Hang });
		if (!gpu.IsAvailable())
		{
			ENGINE_CORE_WARN("No GPU in the frame-pacer child: {}", gpu.GetUnavailableReason());
			return;
		}
		GraphicsDevice& device = gpu.GetDevice();
		FramePacer pacer(device, 2);
		Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
		if (!commandList.has_value())
		{
			ENGINE_CORE_ERROR("The frame-pacer child cannot create a command list: {}", commandList.error());
			return;
		}
		for (int frame = 0; frame < 4; ++frame)
		{
			pacer.BeginFrame();
			ENGINE_CORE_WARN("Frame {} began", frame);
			(*commandList)->open();
			(*commandList)->close();
			pacer.EndFrame(device.ExecuteCommandList(**commandList));
		}
		ENGINE_CORE_ERROR("Every frame began under the hang fault");
	}

	// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decision 41): waits for a submission on a headless device under the
	// hang-reset fault without a reset wait (--gpu-reset-wait 0), whose budget then ends the process before the injected reset
	// could be reported: the child of the reset-wait-off test. Without a device there is nothing to wait for, which the parent
	// reports as a skip.
	ENGINE_DEATH_TEST("Graphics/FramePacerHangResetWithoutResetWait")
	{
		Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::HangReset, .ResetWaitMilliseconds = 0 });
		if (!gpu.IsAvailable())
		{
			ENGINE_CORE_WARN("No GPU in the frame-pacer child: {}", gpu.GetUnavailableReason());
			return;
		}
		GraphicsDevice& device = gpu.GetDevice();
		Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
		if (!commandList.has_value())
		{
			ENGINE_CORE_ERROR("The frame-pacer child cannot create a command list: {}", commandList.error());
			return;
		}
		(*commandList)->open();
		(*commandList)->close();
		// The first garbage collection arms the fault, as at the end of an application's first frame.
		device.RunGarbageCollection();
		WaitForSubmission(device, device.ExecuteCommandList(**commandList), "FramePacer reset-wait child");
		ENGINE_CORE_ERROR("The wait returned under the hang-reset fault without a reset wait (device lost: {})", device.IsLost());
	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("FramePacer: a bounded wait returns the first slice result that is not a timeout")
		{
			// The state machine of §8.1 without a GPU or a clock: slices are counted, never timed.
			struct Row
			{
				std::vector<GpuWaitSliceResult> Slices{};
				GpuWaitResult Expected = GpuWaitResult::Signaled;
				size_t ExpectedCalls = 0;
			};
			const std::vector<Row> rows = {
				{ { GpuWaitSliceResult::Signaled }, GpuWaitResult::Signaled, 1 },
				{ { GpuWaitSliceResult::Timeout, GpuWaitSliceResult::Timeout, GpuWaitSliceResult::Signaled }, GpuWaitResult::Signaled, 3 },
				{ { GpuWaitSliceResult::Timeout, GpuWaitSliceResult::DeviceLost }, GpuWaitResult::DeviceLost, 2 },
				{ { GpuWaitSliceResult::Failed }, GpuWaitResult::Failed, 1 },
			};
			for (const Row& row : rows)
			{
				size_t calls = 0;
				const GpuWaitResult result = RunBoundedGpuWait([&row, &calls]()
				{
					return row.Slices[std::min(calls++, row.Slices.size() - 1)];
				});
				CHECK(result == row.Expected);
				CHECK(calls == row.ExpectedCalls);
			}
		}

		TEST_CASE("FramePacer: a wait that always times out becomes a hang after 100 slices")
		{
			// 100 slices of 100 ms: the 10 s budget of §8.1, after which the caller ends the process with GpuHang.
			size_t calls = 0;
			const GpuWaitResult result = RunBoundedGpuWait([&calls]()
			{
				++calls;
				return GpuWaitSliceResult::Timeout;
			});
			CHECK(result == GpuWaitResult::Hang);
			CHECK(calls == GpuWaitMaxSlices);
			CHECK(GpuWaitMaxSlices * GpuWaitSliceNanoseconds == 10'000'000'000ull);

			size_t smallBudget = 0;
			CHECK(RunBoundedGpuWait([&smallBudget]()
			{
				++smallBudget;
				return GpuWaitSliceResult::Timeout;
			}, 3)
				== GpuWaitResult::Hang);
			CHECK(smallBudget == 3);
			CHECK(GpuWaitResultToString(GpuWaitResult::Hang) == "Hang");
		}

		TEST_CASE("FramePacer: the reset wait is whole slices, rounded up")
		{
			// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decisions 40 and 41): a number of slices, never wall time.
			static_assert(GetGpuResetWaitSliceCount(DefaultGpuResetWaitMilliseconds) == 150);
			CHECK(GetGpuResetWaitSliceCount(0) == 0);
			CHECK(GetGpuResetWaitSliceCount(1) == 1);
			CHECK(GetGpuResetWaitSliceCount(100) == 1);
			CHECK(GetGpuResetWaitSliceCount(101) == 2);
			CHECK(GetGpuResetWaitSliceCount(MaxGpuResetWaitMilliseconds) == 1'200);
			// A specification set in code may exceed the option's range (GraphicsSpecification::ResetWaitMilliseconds).
			CHECK(GetGpuResetWaitSliceCount(std::numeric_limits<uint32_t>::max()) == 42'949'673);
		}

		TEST_CASE("FramePacer: the reset wait runs only after the budget timed out, and only both timing out is a hang")
		{
			// M18 acceptance (C18.4, decision 40, R-HANG), the state machine without a GPU or a clock: after the 10 s budget a
			// wait goes on for the reset wait, a loss report there is a device loss (recovered like any loss), a completion
			// there was a slow GPU, and only a reset wait that times out too is the fatal hang. Slices are counted, never
			// timed; a row's last slice repeats.
			const auto afterBudget = [](std::vector<GpuWaitSliceResult> tail)
			{
				std::vector<GpuWaitSliceResult> slices(GpuWaitMaxSlices, GpuWaitSliceResult::Timeout);
				slices.insert(slices.end(), tail.begin(), tail.end());
				return slices;
			};
			struct Row
			{
				std::vector<GpuWaitSliceResult> Slices{};
				uint32_t ResetSlices = 0;
				GpuWaitResult Expected = GpuWaitResult::Signaled;
				bool InResetWait = false;
				size_t ExpectedCalls = 0;
			};
			const std::vector<Row> rows = {
				{ { GpuWaitSliceResult::Timeout, GpuWaitSliceResult::Signaled }, 150, GpuWaitResult::Signaled, false, 2 },
				{ { GpuWaitSliceResult::Timeout, GpuWaitSliceResult::DeviceLost }, 150, GpuWaitResult::DeviceLost, false, 2 },
				{ afterBudget({ GpuWaitSliceResult::Timeout, GpuWaitSliceResult::DeviceLost }), 150, GpuWaitResult::DeviceLost, true, GpuWaitMaxSlices + 2 },
				{ afterBudget({ GpuWaitSliceResult::Timeout, GpuWaitSliceResult::Timeout, GpuWaitSliceResult::Signaled }), 150, GpuWaitResult::Signaled, true,
					GpuWaitMaxSlices + 3 },
				{ afterBudget({ GpuWaitSliceResult::Failed }), 150, GpuWaitResult::Failed, true, GpuWaitMaxSlices + 1 },
				{ afterBudget({ GpuWaitSliceResult::Timeout }), 150, GpuWaitResult::Hang, true, GpuWaitMaxSlices + 150 },
				// No reset wait (--gpu-reset-wait 0): the budget alone, as before M18.
				{ afterBudget({ GpuWaitSliceResult::DeviceLost }), 0, GpuWaitResult::Hang, false, GpuWaitMaxSlices },
			};
			for (const Row& row : rows)
			{
				CAPTURE(row.ResetSlices);
				CAPTURE(row.ExpectedCalls);
				size_t calls = 0;
				const GpuWaitOutcome outcome = RunGpuWaitWithReset([&row, &calls]()
				{
					return row.Slices[std::min(calls++, row.Slices.size() - 1)];
				}, GpuWaitMaxSlices, row.ResetSlices);
				CHECK(outcome.Result == row.Expected);
				CHECK(outcome.InResetWait == row.InResetWait);
				CHECK(calls == row.ExpectedCalls);
			}
		}

		TEST_CASE("FramePacer: frame N waits for the submission of frame N - FramesInFlight"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			FramePacer pacer(device, 2);
			CHECK(pacer.GetFramesInFlight() == 2);

			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			std::vector<uint64_t> submissions;
			for (uint64_t frame = 0; frame < 6; ++frame)
			{
				pacer.BeginFrame();
				CHECK(pacer.GetFrameIndex() == frame);
				CHECK(pacer.GetFrameSlot() == frame % 2);
				// Once a frame begins, the submission of the frame two before it has completed.
				if (frame >= 2)
					CHECK(device.GetCompletedSubmissionID() >= submissions[frame - 2]);
				(*commandList)->open();
				(*commandList)->close();
				submissions.push_back(device.ExecuteCommandList(**commandList));
				pacer.EndFrame(submissions.back());
			}
			device.WaitForIdle();
		}

		TEST_CASE("FramePacer: WaitForSubmission returns once the submission completed"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			(*commandList)->open();
			(*commandList)->close();
			const uint64_t submission = device.ExecuteCommandList(**commandList);
			WaitForSubmission(device, submission, "FramePacer test");
			CHECK(device.GetCompletedSubmissionID() >= submission);
			// An already completed submission returns at once, and submission 0 (nothing submitted) never waits.
			WaitForSubmission(device, submission, "FramePacer test");
			WaitForSubmission(device, 0, "FramePacer test");
		}

		TEST_CASE("FramePacer: a lost device's frames wait for nothing, and a pacer made on it has no event queries"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 28): a lost device runs nothing more, so its frames
			// never wait for a submission, and a pacer its owner creates on it is no out-of-memory error.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<Scope<Test::GpuSubmissionGate>> gate = Test::GpuSubmissionGate::Create(device);
				REQUIRE_MESSAGE(gate.has_value(), gate.error().ToString());
				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
				FramePacer pacer(device, 1);
				pacer.BeginFrame();
				(*commandList)->open();
				(*commandList)->close();
				(*gate)->HoldNextSubmission();
				pacer.EndFrame(device.ExecuteCommandList(**commandList));

				// The first frame's submission cannot complete before the gate opens, so on a working device the next frame
				// of its slot would wait until its bounded wait ended the process with GpuHang.
				device.GetDiagnostics().SetDeviceLost(true);
				device.ReportDeviceLost("FramePacerTests");
				pacer.BeginFrame();
				CHECK(pacer.GetFrameIndex() == 1);
				pacer.EndFrame(device.GetLastSubmissionID());

				FramePacer lostPacer(device, 2);
				lostPacer.BeginFrame();
				lostPacer.EndFrame(device.GetLastSubmissionID());
				lostPacer.BeginFrame();
				lostPacer.EndFrame(device.GetLastSubmissionID());

				// The lost device's collection waits for idle, so the opened gate's semaphore is free for its destructor.
				(*gate)->Open();
				device.RunGarbageCollection();
			}

			// A pacer on the recovered device paces again, on the new queue's timeline.
			const Status recovered = device.Recover(nullptr);
			REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
			CHECK(device.GetLastSubmissionID() == 0);
			FramePacer pacer(device, 2);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			std::vector<uint64_t> submissions;
			for (uint64_t frame = 0; frame < 4; ++frame)
			{
				pacer.BeginFrame();
				if (frame >= 2)
					CHECK(device.GetCompletedSubmissionID() >= submissions[frame - 2]);
				(*commandList)->open();
				(*commandList)->close();
				submissions.push_back(device.ExecuteCommandList(**commandList));
				pacer.EndFrame(submissions.back());
			}
			device.WaitForIdle();
		}

		TEST_CASE("FramePacer: a hang reset in the reset wait reports an injected loss once, and the device recovers"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.4, Docs/Decisions/0029-m18-contract.md decisions 40 and 46): "a driver-reset hang recovers like
			// a loss". Under hang-reset the first bounded wait after the device's first garbage collection spends its budget
			// without waiting, the reset wait's first slice reports the device lost, and the wait returns instead of ending the
			// process; the device recovers like any loss, and the fault never strikes again.
			Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::HangReset });
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
				(*commandList)->open();
				(*commandList)->close();
				// Before the first garbage collection (an application's startup and first frame) a wait waits for the GPU.
				const uint64_t startup = device.ExecuteCommandList(**commandList);
				WaitForSubmission(device, startup, "FramePacer test");
				CHECK_FALSE(device.IsLost());
				device.RunGarbageCollection();

				(*commandList)->open();
				(*commandList)->close();
				const uint64_t hung = device.ExecuteCommandList(**commandList);
				WaitForSubmission(device, hung, "FramePacer test");
				REQUIRE(device.IsLost());
				const std::optional<GpuDeviceLossReport> report = device.GetLossReport();
				REQUIRE(report.has_value());
				CHECK(report->Injected);
				CHECK(report->Context.contains("FramePacer test"));
				device.RunGarbageCollection();
			}

			const Status recovered = device.Recover(nullptr);
			REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
			CHECK(device.GetRecoveryCount() == 1);
			// Once per process: the recovered device's frames pace and wait for the GPU again.
			FramePacer pacer(device, 2);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			for (uint64_t frame = 0; frame < 4; ++frame)
			{
				pacer.BeginFrame();
				(*commandList)->open();
				(*commandList)->close();
				pacer.EndFrame(device.ExecuteCommandList(**commandList));
				device.RunGarbageCollection();
			}
			CHECK_FALSE(device.IsLost());
			CHECK(device.GetRecoveryCount() == 1);
			device.WaitForIdle();
		}

		TEST_CASE("FramePacer: under hang-reset-persistent a device that hangs again after its recovery is recovered again"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.4, decisions 40 and 46): a recovery is not once per process; a hang after it (never during it:
			// a recovery waits through no bounded wait) is reset and recovered again.
			Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::HangResetPersistent });
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			device.RunGarbageCollection();
			for (uint32_t hang = 1; hang <= 2; ++hang)
			{
				CAPTURE(hang);
				{
					Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
					REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
					(*commandList)->open();
					(*commandList)->close();
					WaitForSubmission(device, device.ExecuteCommandList(**commandList), "FramePacer test");
					REQUIRE(device.IsLost());
					device.RunGarbageCollection();
				}
				const Status recovered = device.Recover(nullptr);
				REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
				CHECK(device.GetRecoveryCount() == hang);
				device.RunGarbageCollection();
			}
		}

		TEST_CASE("FramePacer: under hang-reset the first frame wait after the arming is reset even when the GPU finished"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.4, Docs/Decisions/0029-m18-contract.md decision 46): while GpuDiagnostics::HasBoundedWaitFault, BeginFrame
			// waits for its slot's submission instead of polling the slot's event query, so the injected reset strikes an
			// application's third frame (the first whose slot has a submission, with two frames in flight) however fast the GPU
			// finished the first one, and the frame boundary recovers it.
			Test::HeadlessGpuFixture gpu({ .InjectFault = GpuFault::HangReset });
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				FramePacer pacer(device, 2);
				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
				for (uint64_t frame = 0; frame < 2; ++frame)
				{
					pacer.BeginFrame();
					(*commandList)->open();
					(*commandList)->close();
					pacer.EndFrame(device.ExecuteCommandList(**commandList));
					// The first collection, at the end of the first frame, arms the fault.
					device.RunGarbageCollection();
				}
				// Both frames completed, so the third frame's slot query has signaled.
				device.WaitForIdle();
				REQUIRE_FALSE(device.IsLost());

				pacer.BeginFrame();
				REQUIRE(device.IsLost());
				const std::optional<GpuDeviceLossReport> report = device.GetLossReport();
				REQUIRE(report.has_value());
				CHECK(report->Injected);
				CHECK(report->Context.contains("FramePacer::BeginFrame"));
				pacer.EndFrame(device.GetLastSubmissionID());
				device.RunGarbageCollection();
			}

			const Status recovered = device.Recover(nullptr);
			REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
			CHECK(device.GetRecoveryCount() == 1);
		}

		TEST_CASE("FramePacer: without a reset wait an injected hang reset ends the wait with GpuHang" * doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.4, decision 41): a reset wait of 0 (--gpu-reset-wait 0) waits for no reset, so a hang is fatal when its
			// budget is spent, as before M18: the reset that hang-reset injects in the reset wait's first slice never comes. The
			// fatal error says the reset wait is off rather than naming one that never ran.
			const Result<Test::DeathTestResult> child =
				Test::RunDeathTest("Graphics/FramePacerHangResetWithoutResetWait", std::chrono::seconds(60));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stderr: ", child->StandardError);
			if (child->StandardError.contains("No GPU in the frame-pacer child"))
			{
				Test::ReportGpuUnavailable("no GPU device in the child process");
				return;
			}
			CHECK(child->ExitCode == ExitCode::Crash);
			CHECK(child->StandardError.contains("Fatal error (GpuHang)"));
			CHECK(child->StandardError.contains("--gpu-reset-wait 0"));
			CHECK_FALSE(child->StandardError.contains("ms reset wait"));
			CHECK_FALSE(child->StandardError.contains("The wait returned"));
		}

		TEST_CASE("FramePacer: the hang fault ends the first wait for a frame slot with GpuHang" * doctest::test_suite(Test::GpuSuite))
		{
			// §8.14 item 8: under --gpu-inject-fault=hang every wait slice times out, the 10 s budget's and, from M18 (C18.4,
			// Docs/Decisions/0029-m18-contract.md decision 40), the reset wait's, since no loss report arrives, so the third
			// frame (the first whose slot has a submission, with two frames in flight) spends both waits and ends the process,
			// however fast the GPU finished the first frame. The injected slices do not wait, so neither costs wall time.
			const Result<Test::DeathTestResult> child = Test::RunDeathTest("Graphics/FramePacerInjectedHang", std::chrono::seconds(60));
			REQUIRE_MESSAGE(child.has_value(), child.error().ToString());
			INFO("child stderr: ", child->StandardError);
			if (child->StandardError.contains("No GPU in the frame-pacer child"))
			{
				Test::ReportGpuUnavailable("no GPU device in the child process");
				return;
			}
			CHECK(child->ExitCode == ExitCode::Crash);
			CHECK(child->StandardError.contains("Fatal error (GpuHang)"));
			CHECK(child->StandardError.contains("Injected GPU fault (hang)"));
			CHECK(child->StandardError.contains("Frame 1 began"));
			CHECK_FALSE(child->StandardError.contains("Frame 2 began"));
		}
	}

}
