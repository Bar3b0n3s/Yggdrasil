#include "TestsPCH.h"

#include "Engine/Graphics/FramePacer.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Graphics")
	{
		TEST_CASE("FramePacer: a bounded wait returns the first slice result that is not a timeout" * doctest::skip(true))
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

		TEST_CASE("FramePacer: a wait that always times out becomes a hang after 100 slices" * doctest::skip(true))
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

		TEST_CASE("FramePacer: frame N waits for the submission of frame N - FramesInFlight"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
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
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
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
			// An already completed submission returns at once.
			WaitForSubmission(device, submission, "FramePacer test");
		}
	}

}
