#include "TestsPCH.h"
#include "Support/DeviceLossTest.h"

#include "Engine/Graphics/GpuRecreation.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Support/HeadlessGpuFixture.h"

#include <algorithm>
#include <format>
#include <string>
#include <utility>

namespace Engine {

	namespace Test {

		namespace Utils {

			// InvalidState naming the first owner of `owners` that `device` has not registered, `when` ("before the loss").
			[[nodiscard]] static Status CheckOwnersRegistered(GraphicsDevice& device, const std::vector<std::string>& owners, std::string_view when)
			{
				const std::vector<std::string> registered = device.GetRecreation().GetOwnerNames();
				for (const std::string& owner : owners)
				{
					if (std::ranges::find(registered, owner) == registered.end())
					{
						std::string names;
						for (const std::string& name : registered)
							names += names.empty() ? name : ", " + name;
						return MakeError(ErrorCode::InvalidState, "the owner '{}' is not registered with the device {} (registered: {})", owner, when,
							names.empty() ? std::string("none") : names);
					}
				}
				return {};
			}

			// GpuResourceTracker::GetLiveCount of every type, in GpuResourceType order.
			[[nodiscard]] static std::array<uint64_t, GpuResourceTypeCount> ReadLiveCounts(const GpuResourceTracker& tracker)
			{
				std::array<uint64_t, GpuResourceTypeCount> counts{};
				for (size_t type = 0; type < GpuResourceTypeCount; ++type)
					counts[type] = tracker.GetLiveCount(static_cast<GpuResourceType>(type));
				return counts;
			}

			// "Texture: 2, CommandList: 1", or "none", as GpuResourceTracker::DescribeLiveCounts names live counts.
			[[nodiscard]] static std::string DescribeCounts(const std::array<uint64_t, GpuResourceTypeCount>& counts)
			{
				std::string text;
				for (size_t type = 0; type < GpuResourceTypeCount; ++type)
				{
					if (counts[type] == 0)
						continue;
					if (!text.empty())
						text += ", ";
					text += std::format("{}: {}", GpuResourceTypeToString(static_cast<GpuResourceType>(type)), counts[type]);
				}
				return text.empty() ? std::string("none") : text;
			}

		}

		Result<DeviceLossReport> RunDeviceLossTest(HeadlessGpuFixture& gpu, const DeviceLossTest& test)
		{
			if (!gpu.IsAvailable())
				return MakeError(ErrorCode::InvalidState, "the loss test's fixture has no device: {}", gpu.GetUnavailableReason());
			if (!test.Use)
				return MakeError(ErrorCode::InvalidState, "the loss test has no Use");
			GraphicsDevice& device = gpu.GetDevice();
			ENGINE_TRY(Utils::CheckOwnersRegistered(device, test.Owners, "before the loss"));
			DeviceLossReport report;
			const uint32_t recoveriesBefore = device.GetRecoveryCount();

			// 1. The owners' output and the tracker's counts on the healthy device.
			ENGINE_TRY_ASSIGN(report.Before, WithContext(test.Use(), "before the loss"));
			device.RunGarbageCollection();
			report.LiveBefore = Utils::ReadLiveCounts(device.GetResourceTracker());

			// 2. A loss injected and reported as an NVRHI report after a submission would be.
			device.GetDiagnostics().SetDeviceLost(true);
			device.ReportDeviceLost("Test::RunDeviceLossTest");

			// 3. The owners on the lost device: whatever they return, they return.
			Result<std::vector<std::byte>> lost = test.Use();
			if (!lost.has_value())
				report.OnLostDevice = std::unexpected(std::move(lost).error());

			// 4. The headless recovery.
			ENGINE_TRY(WithContext(device.Recover(nullptr), "while recovering the device"));
			ENGINE_TRY(Utils::CheckOwnersRegistered(device, test.Owners, "after the recovery"));

			// 5. The owners again on the recovered device.
			ENGINE_TRY_ASSIGN(report.After, WithContext(test.Use(), "after the recovery"));
			device.RunGarbageCollection();
			report.LiveAfter = Utils::ReadLiveCounts(device.GetResourceTracker());
			report.RecoveryCount = device.GetRecoveryCount();

			if (report.After != report.Before)
			{
				const size_t common = std::min(report.Before.size(), report.After.size());
				size_t first = 0;
				while (first < common && report.Before[first] == report.After[first])
					++first;
				return MakeError(ErrorCode::Conflict,
					"the owners' output after the recovery differs from the output before the loss at byte {} ({} bytes before, {} after)", first,
					report.Before.size(), report.After.size());
			}
			if (report.LiveAfter != report.LiveBefore)
			{
				return MakeError(ErrorCode::Conflict, "the live GPU objects after the recovery ({}) differ from those before the loss ({})",
					Utils::DescribeCounts(report.LiveAfter), Utils::DescribeCounts(report.LiveBefore));
			}
			if (report.RecoveryCount != recoveriesBefore + 1)
			{
				return MakeError(ErrorCode::Conflict, "the device recovered {} time(s) during the loss test, not once",
					report.RecoveryCount - recoveriesBefore);
			}
			return report;
		}

	}

}
