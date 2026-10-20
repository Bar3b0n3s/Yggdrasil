#include "TestsPCH.h"
#include "Support/DeviceLossTest.h"

#include "Engine/Graphics/GpuRecreation.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/Readback.h"
#include "Support/HeadlessGpuFixture.h"

#include <optional>

namespace Engine {

	namespace {

		// A registered owner of a cleared target: its Use clears the target to a colour and reads it back. With `release`
		// false its Release keeps the target, as an owner that forgot a member would.
		class ClearedTargetOwner
		{
		public:
			ClearedTargetOwner(GraphicsDevice& device, bool release)
				: m_Device(&device)
			{
				REQUIRE(Create().has_value());
				ClearedTargetOwner* owner = this;
				GpuRecreationHooks hooks;
				hooks.Name = "ClearedTargetOwner";
				hooks.Release = [owner, release]()
				{
					if (release)
						owner->m_Target.reset();
				};
				hooks.Recreate = [owner]()
				{
					return owner->Create();
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
			GpuRecreationRegistration m_Recreation{};
		};

	}

	TEST_SUITE("Support")
	{
		TEST_CASE("DeviceLossTest: an owner that releases and recreates passes, and one that keeps an object fails"
			* doctest::test_suite(Test::GpuSuite) * doctest::skip(true))
		{
			// M18 acceptance (C18.3, Docs/Decisions/0029-m18-contract.md decision 32): the shared loss test every GPU owner
			// runs proves the same output and tracker counts after a recovery, and catches a missed release.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			{
				ClearedTargetOwner owner(gpu.GetDevice(), true);
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
				ClearedTargetOwner keeper(gpu.GetDevice(), false);
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
	}

}
