#include "TestsPCH.h"
#include "Support/DeviceLossTest.h"

#include "Engine/Graphics/GpuDiagnostics.h"
#include "Engine/Graphics/GpuRecreation.h"
#include "Engine/Graphics/GpuResourceTracker.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/Readback.h"
#include "Support/HeadlessGpuFixture.h"

#include <optional>

namespace Engine {

	namespace {

		// How a ClearedTargetOwner's hooks go wrong, as an owner's could.
		enum class OwnerFault
		{
			None,
			KeepsTarget,       // its Release keeps the target, as an owner that forgot a member would
			RecreatesExtraOne, // its Recreate creates one more target than its creation did
		};

		// A registered owner of a cleared target: its Use clears the target to a colour and reads it back.
		class ClearedTargetOwner
		{
		public:
			ClearedTargetOwner(GraphicsDevice& device, OwnerFault fault)
				: m_Device(&device)
			{
				REQUIRE(Create().has_value());
				ClearedTargetOwner* owner = this;
				GpuRecreationHooks hooks;
				hooks.Name = "ClearedTargetOwner";
				hooks.Release = [owner, fault]()
				{
					if (fault == OwnerFault::KeepsTarget)
						return;
					owner->m_Target.reset();
					owner->m_Extra.reset();
				};
				hooks.Recreate = [owner, fault]() -> Status
				{
					ENGINE_TRY(owner->Create());
					if (fault == OwnerFault::RecreatesExtraOne)
					{
						ENGINE_TRY_ASSIGN(OffscreenTarget extra,
							OffscreenTarget::Create(*owner->m_Device, { .Width = 4, .Height = 4, .DebugName = "ExtraTarget" }));
						owner->m_Extra.emplace(std::move(extra));
					}
					return {};
				};
				m_Recreation = device.GetRecreation().Register(std::move(hooks));
			}

			ClearedTargetOwner(const ClearedTargetOwner&) = delete;
			ClearedTargetOwner& operator=(const ClearedTargetOwner&) = delete;

			[[nodiscard]] Result<std::vector<std::byte>> Use()
			{
				if (!m_Target.has_value())
					return MakeError(ErrorCode::InvalidState, "the target was released");
				ENGINE_TRY_ASSIGN(nvrhi::CommandListHandle list, m_Device->CreateCommandList());
				list->open();
				list->clearTextureFloat(m_Target->GetColorTexture(), nvrhi::AllSubresources, nvrhi::Color(0.25f, 0.5f, 0.75f, 1.0f));
				list->close();
				static_cast<void>(m_Device->ExecuteCommandList(*list));
				Readback readback(*m_Device);
				ENGINE_TRY_ASSIGN(const Image image, readback.ReadTexture(*m_Target->GetColorTexture()));
				return image.Pixels;
			}
		private:
			[[nodiscard]] Status Create()
			{
				if (m_Target.has_value())
					return {};
				ENGINE_TRY_ASSIGN(OffscreenTarget target, OffscreenTarget::Create(*m_Device, { .Width = 4, .Height = 4, .DebugName = "ClearedTarget" }));
				m_Target.emplace(std::move(target));
				return {};
			}
		private:
			GraphicsDevice* m_Device = nullptr; // documented back-reference
			std::optional<OffscreenTarget> m_Target{};
			std::optional<OffscreenTarget> m_Extra{}; // OwnerFault::RecreatesExtraOne's
			GpuRecreationRegistration m_Recreation{};
		};

	}

	TEST_SUITE("Support")
	{
		TEST_CASE("DeviceLossTest: an owner that releases and recreates passes, and one that keeps an object fails"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.3, Docs/Decisions/0029-m18-contract.md decision 32): the shared loss test every GPU owner
			// runs proves the same output and tracker counts after a recovery, and catches a missed release.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				ClearedTargetOwner owner(gpu.GetDevice(), OwnerFault::None);
				Test::DeviceLossTest test;
				test.Owners = { "ClearedTargetOwner" };
				test.Use = [&owner]()
				{
					return owner.Use();
				};
				const Result<Test::DeviceLossReport> report = Test::RunDeviceLossTest(gpu, test);
				REQUIRE_MESSAGE(report.has_value(), report.error().ToString());
				CHECK_FALSE(report->Before.empty());
				CHECK(report->After == report->Before);
				CHECK_FALSE(report->OnLostDevice.has_value());
				CHECK(report->LiveAfter == report->LiveBefore);
				CHECK(report->RecoveryCount == 1);

				test.Owners = { "NoSuchOwner" };
				const Result<Test::DeviceLossReport> unknown = Test::RunDeviceLossTest(gpu, test);
				REQUIRE_FALSE(unknown.has_value());
				CHECK(unknown.error().GetCode() == ErrorCode::InvalidState);
				CHECK(unknown.error().ToString().contains("NoSuchOwner"));
			}
			{
				ClearedTargetOwner keeper(gpu.GetDevice(), OwnerFault::KeepsTarget);
				Test::DeviceLossTest keeperTest;
				keeperTest.Owners = { "ClearedTargetOwner" };
				keeperTest.Use = [&keeper]()
				{
					return keeper.Use();
				};
				const Result<Test::DeviceLossReport> kept = Test::RunDeviceLossTest(gpu, keeperTest);
				REQUIRE_FALSE(kept.has_value());
				CHECK(kept.error().GetCode() == ErrorCode::InvalidState);
				CHECK(kept.error().ToString().contains("Texture"));
			}
		}

		TEST_CASE("DeviceLossTest: different output, different live counts or another recovery after the recovery fail the test"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.3, Docs/Decisions/0029-m18-contract.md decision 32): the helper's own checks, which every owner's loss
			// test relies on, each failing with Conflict and naming what differed.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				// Output that differs after the recovery, at byte 5.
				ClearedTargetOwner owner(device, OwnerFault::None);
				const uint32_t recoveries = device.GetRecoveryCount();
				Test::DeviceLossTest test;
				test.Owners = { "ClearedTargetOwner" };
				test.Use = [&owner, &device, recoveries]() -> Result<std::vector<std::byte>>
				{
					ENGINE_TRY_ASSIGN(std::vector<std::byte> pixels, owner.Use());
					if (device.GetRecoveryCount() > recoveries && pixels.size() > 5)
						pixels[5] ^= std::byte{ 1 };
					return pixels;
				};
				const Result<Test::DeviceLossReport> differs = Test::RunDeviceLossTest(gpu, test);
				REQUIRE_FALSE(differs.has_value());
				CHECK(differs.error().GetCode() == ErrorCode::Conflict);
				CHECK(differs.error().ToString().contains("at byte 5"));
			}
			{
				// One more live texture after the recovery: both counts are named.
				ClearedTargetOwner owner(device, OwnerFault::RecreatesExtraOne);
				device.WaitForIdle();
				device.RunGarbageCollection();
				const std::string countsBefore = device.GetResourceTracker().DescribeLiveCounts();
				Test::DeviceLossTest test;
				test.Owners = { "ClearedTargetOwner" };
				test.Use = [&owner]()
				{
					return owner.Use();
				};
				const Result<Test::DeviceLossReport> grown = Test::RunDeviceLossTest(gpu, test);
				REQUIRE_FALSE(grown.has_value());
				device.WaitForIdle();
				device.RunGarbageCollection();
				const std::string countsAfter = device.GetResourceTracker().DescribeLiveCounts();
				REQUIRE(countsAfter != countsBefore);
				CHECK(grown.error().GetCode() == ErrorCode::Conflict);
				CHECK(grown.error().ToString().contains(countsBefore));
				CHECK(grown.error().ToString().contains(countsAfter));
			}
			{
				// A Use that recovers the device once more after the recovery: two recoveries, not one.
				ClearedTargetOwner owner(device, OwnerFault::None);
				const uint32_t recoveries = device.GetRecoveryCount();
				bool lostAgain = false;
				Test::DeviceLossTest test;
				test.Owners = { "ClearedTargetOwner" };
				test.Use = [&owner, &device, &lostAgain, recoveries]() -> Result<std::vector<std::byte>>
				{
					if (device.GetRecoveryCount() > recoveries && !std::exchange(lostAgain, true))
					{
						device.GetDiagnostics().SetDeviceLost(true);
						device.ReportDeviceLost("DeviceLossTestTests");
						ENGINE_TRY(device.Recover(nullptr));
					}
					return owner.Use();
				};
				const Result<Test::DeviceLossReport> twice = Test::RunDeviceLossTest(gpu, test);
				REQUIRE_FALSE(twice.has_value());
				CHECK(twice.error().GetCode() == ErrorCode::Conflict);
				CHECK(twice.error().ToString().contains("2 time(s)"));
			}
		}
	}

}
