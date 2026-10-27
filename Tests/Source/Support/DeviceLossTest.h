#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Graphics/GpuResourceTracker.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

// The shared loss test of the recreation protocol (M18, C18.3; Docs/Decisions/0029-m18-contract.md decision 32): every GPU
// owner that does not present, the M18 owners and every later one (M21's sprites and tilemaps, M22's UI, M23's passes,
// M26's particles and temporal history), proves with one GPU test that it survives a device loss, through this helper:
//
//     TEST_CASE("QuadBatcher: a device loss recreates its pipelines and draws the same image" * doctest::test_suite(Test::GpuSuite))
//     {
//         Test::HeadlessGpuFixture gpu;
//         ENGINE_REQUIRE_GPU(gpu);
//         ... create the owners under test on gpu.GetDevice() ...
//         Test::DeviceLossTest test;
//         test.Owners = { "SceneRendererPipelines", "SceneRenderer" };
//         test.Use = [&setup]() -> Result<std::vector<std::byte>> { ... render, read back, return the pixels ... };
//         const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
//         REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
//     }
//
// The fixture's device has validation and synchronization validation on, and fails the test at its end on any message of
// the device's whole life (HeadlessGpuFixture.h), the recovered device's included.
//
// The helper's device is headless and recovers with Recover(nullptr), so it cannot reach what only a presenting device
// does: a swapchain and its surface released before the old VkInstance is destroyed and created again on the new one,
// Recover(window) scoring presentation support again, and the acquire and present paths' loss reports. An owner that
// presents (the Application frame's swapchain, M24's platform windows) proves its loss in a windowed child instead, as
// "Application: a windowed run recovers its swapchain after an injected loss and presents the next frame without a GPU
// message" does (ApplicationTests.cpp): a windowed application with ExpectNoGpuErrors loses its device during a frame and
// presents the frames after the recovery.

namespace Engine {

	namespace Test {

		class HeadlessGpuFixture;

		// One owner's (or a group's) loss test.
		struct DeviceLossTest
		{
			// GpuRecreationRegistry::GetOwnerNames entries of the owners under test, each of which must be registered with the
			// fixture's device before the loss and after the recovery (an owner keeps its one registration across a recovery).
			std::vector<std::string> Owners{};
			// Uses the owners under test once on the fixture's device and returns what they produced, compared byte for byte:
			// the pixels of a rendered and read-back image, a read-back buffer. Called three times: before the loss, on the
			// lost device (where it must return, an error or not, without a GPU message), and after the recovery. Main thread;
			// it submits and waits as the owners' users do, and leaves no submission pending (the next call may be the
			// recovery). Required.
			std::function<Result<std::vector<std::byte>>()> Use{};
		};

		// What a passing loss test observed, for checks of its own.
		struct DeviceLossReport
		{
			std::vector<std::byte> Before{}; // Use's output before the loss
			std::vector<std::byte> After{};  // Use's output after the recovery, equal to Before
			Status OnLostDevice{};           // what Use returned on the lost device (any error is allowed)
			// GpuResourceTracker::GetLiveCount per GpuResourceType (in its order) after a garbage collection: after the first
			// Use, and after the last one, which are equal (zeros without the tracker, in Dist, where the Tests never run).
			std::array<uint64_t, GpuResourceTypeCount> LiveBefore{};
			std::array<uint64_t, GpuResourceTypeCount> LiveAfter{};
			uint32_t RecoveryCount = 0; // GraphicsDevice::GetRecoveryCount afterwards: one more than before
		};

		// Runs a loss test on `gpu`'s device, in order:
		//   1. Use, then a GraphicsDevice::RunGarbageCollection, recording the tracker's live counts.
		//   2. A loss injected (GpuDiagnostics::SetDeviceLost(true)) and reported (GraphicsDevice::ReportDeviceLost), as an
		//      NVRHI report after a submission would be; the device IsLost.
		//   3. Use on the lost device; it must return.
		//   4. GraphicsDevice::Recover(nullptr), the headless recovery: every owner releases, the device is created again,
		//      every owner creates again what it needs.
		//   5. Use again, then a garbage collection, recording the live counts again.
		// Errors (each names what differed, so the test's REQUIRE_MESSAGE shows it): InvalidState when the fixture has no
		// device, `test.Use` is empty, or an owner of `test.Owners` is not registered before the loss or after the recovery;
		// the first Use's error, with "before the loss" as context; the recovery's error; the last Use's error, with "after
		// the recovery"; Conflict when the outputs differ (naming the first differing byte and both sizes), when the live
		// counts differ (naming both GpuResourceTracker descriptions), or when GetRecoveryCount did not grow by one.
		[[nodiscard]] Result<DeviceLossReport> RunDeviceLossTest(HeadlessGpuFixture& gpu, const DeviceLossTest& test);

	}

}
