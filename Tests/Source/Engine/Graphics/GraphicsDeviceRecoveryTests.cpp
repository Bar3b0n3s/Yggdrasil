#include "TestsPCH.h"

#include "Engine/Graphics/GraphicsDevice.h"

#include "Engine/Graphics/FramePacer.h"
#include "Engine/Graphics/GpuRecreation.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Engine/Graphics/Readback.h"
#include "Support/ExpectLog.h"
#include "Support/HeadlessGpuFixture.h"

#include <limits>

// The device's recovery from a loss (M18, C18.3; Docs/Decisions/0029-m18-contract.md decisions 28 and 29). The losses are
// injected (GpuDiagnostics::SetDeviceLost(true)): the Vulkan device is not really lost, so the validation layer checks the
// whole recovery, and DescribeDeviceFault never queries a device that is not lost.

namespace Engine {

	namespace {

		// A registered owner of one sampled texture: its Release drops it, its Recreate creates it again, and both are logged.
		class TextureOwner
		{
		public:
			TextureOwner(GraphicsDevice& device, std::string name, std::vector<std::string>& calls)
				: m_Device(&device), m_Name(std::move(name)), m_Calls(&calls)
			{
				REQUIRE(Create().has_value());
				TextureOwner* owner = this;
				GpuRecreationHooks hooks;
				hooks.Name = m_Name;
				hooks.Release = [owner]()
				{
					owner->m_Calls->push_back("release " + owner->m_Name);
					owner->m_Texture = nullptr;
				};
				hooks.Recreate = [owner]()
				{
					owner->m_Calls->push_back("recreate " + owner->m_Name);
					return owner->Create();
				};
				m_Recreation = device.GetRecreation().Register(std::move(hooks));
			}

			TextureOwner(const TextureOwner&) = delete;
			TextureOwner& operator=(const TextureOwner&) = delete;

			[[nodiscard]] nvrhi::ITexture* GetTexture() const { return m_Texture.Get(); }
		private:
			[[nodiscard]] Status Create()
			{
				nvrhi::TextureDesc desc;
				desc.width = 4;
				desc.height = 4;
				desc.format = nvrhi::Format::RGBA8_UNORM;
				desc.initialState = nvrhi::ResourceStates::ShaderResource;
				desc.keepInitialState = true;
				desc.debugName = m_Name;
				ENGINE_TRY_ASSIGN(m_Texture, m_Device->CreateTexture(desc));
				return {};
			}
		private:
			GraphicsDevice* m_Device = nullptr; // documented back-reference
			std::string m_Name;
			std::vector<std::string>* m_Calls = nullptr; // documented back-reference
			nvrhi::TextureHandle m_Texture{};
			GpuRecreationRegistration m_Recreation{};
		};

		// Injects a loss as the test hooks do and reports it, as the submission that meets it would.
		void LoseDevice(GraphicsDevice& device)
		{
			device.GetDiagnostics().SetDeviceLost(true);
			device.ReportDeviceLost("GraphicsDeviceRecoveryTests");
		}

	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("GraphicsDevice: a reported loss refuses creations and skips submissions and waits until the recovery"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.3, decision 28): a lost device does no GPU work, and every path returns to its caller.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = 8, .Height = 8, .DebugName = "LossTarget" });
			REQUIRE_MESSAGE(target.has_value(), target.error().ToString());
			Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
			REQUIRE_MESSAGE(list.has_value(), list.error().ToString());
			CHECK_FALSE(device.IsLost());
			CHECK_FALSE(device.GetLossReport().has_value());

			LoseDevice(device);
			CHECK(device.IsLost());
			REQUIRE(device.GetLossReport().has_value());
			CHECK(device.GetLossReport()->Context == "GraphicsDeviceRecoveryTests");
			CHECK(device.GetLossReport()->Injected);
			device.ReportDeviceLost("a second report of the same loss");
			CHECK(device.GetLossReport()->Context == "GraphicsDeviceRecoveryTests");

			const Result<OffscreenTarget> refused = OffscreenTarget::Create(device, { .Width = 8, .Height = 8, .DebugName = "Refused" });
			REQUIRE_FALSE(refused.has_value());
			CHECK(refused.error().GetCode() == ErrorCode::Gpu);
			const uint64_t last = device.GetLastSubmissionID();
			(*list)->open();
			(*list)->clearTextureFloat(target->GetColorTexture(), nvrhi::AllSubresources, nvrhi::Color(1.0f));
			(*list)->close();
			CHECK(device.ExecuteCommandList(**list) == last);
			CHECK(device.GetCompletedSubmissionID() == last);
			WaitForSubmission(device, last + 1, "a submission the lost device never ran");
			device.WaitForIdle();
			Readback readback(device);
			const Result<Image> image = readback.ReadTexture(*target->GetColorTexture());
			REQUIRE_FALSE(image.has_value());
			CHECK(image.error().GetCode() == ErrorCode::Gpu);
		}

		TEST_CASE("GraphicsDevice: Recover releases every owner, creates the device again and recreates the owners in order"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.3, decisions 26 and 29): §8.14 item 4's order, the device object and every reference to it
			// kept, the loss count grown, and the tracker's counts back where they were.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			std::vector<std::string> calls;
			TextureOwner first(device, "First", calls);
			TextureOwner second(device, "Second", calls);
			CHECK(device.GetRecreation().GetOwnerNames() == std::vector<std::string>{ "ShaderLibrary", "First", "Second" });
			device.RunGarbageCollection();
			const std::string liveBefore = device.GetResourceTracker().DescribeLiveCounts();
			const GpuMessageCounts messagesBefore = device.GetDiagnostics().GetCounts();

			LoseDevice(device);
			const Status recovered = device.Recover(nullptr);
			REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
			CHECK(calls == std::vector<std::string>{ "release Second", "release First", "recreate First", "recreate Second" });
			CHECK_FALSE(device.IsLost());
			CHECK_FALSE(device.GetLossReport().has_value());
			CHECK(device.GetRecoveryCount() == 1);
			CHECK(first.GetTexture() != nullptr);
			CHECK(second.GetTexture() != nullptr);
			device.RunGarbageCollection();
			CHECK(device.GetResourceTracker().DescribeLiveCounts() == liveBefore);
			CHECK(device.GetDiagnostics().GetCounts().Errors == messagesBefore.Errors);
			CHECK(device.GetDiagnostics().GetCounts().Warnings == messagesBefore.Warnings);

			// The recovered device works, and recovers again.
			Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = 8, .Height = 8, .DebugName = "AfterRecovery" });
			REQUIRE_MESSAGE(target.has_value(), target.error().ToString());
			Readback readback(device);
			CHECK(readback.ReadTexture(*target->GetColorTexture()).has_value());
		}

		TEST_CASE("GraphicsDevice: Recover refuses a GPU object that an unregistered owner still holds"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.3, decisions 27 and 29): the placement rule's check; a missed owner is found by its loss test
			// rather than by a crash after a real loss.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			Result<OffscreenTarget> unregistered = OffscreenTarget::Create(device, { .Width = 8, .Height = 8, .DebugName = "Unregistered" });
			REQUIRE_MESSAGE(unregistered.has_value(), unregistered.error().ToString());

			LoseDevice(device);
			const Status recovered = device.Recover(nullptr);
			REQUIRE_FALSE(recovered.has_value());
			CHECK(recovered.error().GetCode() == ErrorCode::InvalidState);
			CHECK(recovered.error().ToString().contains("Texture"));
			CHECK(device.GetRecoveryCount() == 0);
		}

		TEST_CASE("GraphicsDevice: an unrecoverable loss fails Recover after its one retry"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.3, decisions 29 and 30): the device creation fails twice, without calling Vulkan and without
			// the reset wait, and the caller ends the process (RaiseDeviceLost). The loss continues: the device stays lost,
			// without Vulkan objects, so the fixture's destruction waits for nothing. The reset wait is the largest there is, so
			// a recovery that waited it would end the run at the test's time limit instead of passing.
			Test::HeadlessGpuFixture gpu({ .ResetWaitMilliseconds = std::numeric_limits<uint32_t>::max() });
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			device.GetDiagnostics().InjectUnrecoverableDeviceLoss();
			device.ReportDeviceLost("GraphicsDeviceRecoveryTests");
			Test::ExpectLog attempts(LogLevel::Warn, "creating the device again failed");
			Test::ExpectLog last(LogLevel::Warn, std::format("(attempt {0} of {0})", GraphicsDevice::MaxDeviceCreationAttempts));
			const Status recovered = device.Recover(nullptr);
			REQUIRE_FALSE(recovered.has_value());
			CHECK(recovered.error().GetCode() == ErrorCode::Gpu);
			// One creation and its one retry (MaxDeviceCreationAttempts), no more and no fewer.
			CHECK(attempts.GetMatchCount() == GraphicsDevice::MaxDeviceCreationAttempts);
			CHECK(last.GetMatchCount() == 1);
			CHECK(device.GetRecoveryCount() == 0);
			CHECK(device.IsLost());
			CHECK(device.GetLossReport().has_value());
		}

		TEST_CASE("GraphicsDevice: a loss reported while the owners are recreated fails the recovery"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.3, decision 29): a second loss during the recovery stays fatal.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			GraphicsDevice* lost = &device;
			GpuRecreationHooks hooks;
			hooks.Name = "LosesTheDeviceAgain";
			hooks.Release = []() {};
			hooks.Recreate = [lost]() -> Status
			{
				lost->GetDiagnostics().SetDeviceLost(true);
				return {};
			};
			GpuRecreationRegistration losing = device.GetRecreation().Register(std::move(hooks));
			LoseDevice(device);
			const Status recovered = device.Recover(nullptr);
			REQUIRE_FALSE(recovered.has_value());
			CHECK(recovered.error().GetCode() == ErrorCode::Gpu);
			CHECK(device.GetRecoveryCount() == 0);
			CHECK(device.IsLost());
		}
	}

}
