#include "TestsPCH.h"

#include "Engine/Graphics/GpuOutOfMemory.h"

#include "Engine/Graphics/GpuRecreation.h"
#include "Engine/Graphics/GpuResourceTracker.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/TrianglePass.h"
#include "Support/HeadlessGpuFixture.h"
#include "Support/TrimCounter.h"

#include <functional>
#include <string>
#include <utility>
#include <vector>

// GPU out-of-memory after startup (M18, C18.4; Docs/Decisions/0029-m18-contract.md decisions 42, 43 and 46): what counts as
// out-of-memory, the device's trim, the retry rule, and the injected faults at the device's creation wrappers. The requests
// that use them are tested with their owners (SceneRenderer, ViewportCapture, Application, the hosts).

namespace Engine {

	namespace {

		// A render target as a resize or a capture creates one.
		nvrhi::TextureDesc MakeTargetDesc(std::string name)
		{
			nvrhi::TextureDesc desc;
			desc.width = 16;
			desc.height = 16;
			desc.format = nvrhi::Format::RGBA8_UNORM;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			desc.debugName = std::move(name);
			return desc;
		}

	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("IsGpuOutOfMemory: a Gpu error on a device that is not lost, never the loss or another error"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.4, decision 42): on a lost device a Gpu error is the loss (decision 28), and an OutOfMemory error has
			// already failed its request alone after its trim, so neither is trimmed for again.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			CHECK(IsGpuOutOfMemory(Error(ErrorCode::Gpu, "cannot create texture 'SceneColor'"), device));
			CHECK_FALSE(IsGpuOutOfMemory(Error(ErrorCode::OutOfMemory, "a request that failed alone"), device));
			CHECK_FALSE(IsGpuOutOfMemory(Error(ErrorCode::InvalidArgument, "a zero size"), device));

			device.GetDiagnostics().SetDeviceLost(true);
			device.ReportDeviceLost("GpuOutOfMemoryTests");
			CHECK_FALSE(IsGpuOutOfMemory(Error(ErrorCode::Gpu, "cannot create texture 'SceneColor': the device is lost"), device));
			const Status recovered = device.Recover(nullptr);
			REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
			CHECK(IsGpuOutOfMemory(Error(ErrorCode::Gpu, "cannot create texture 'SceneColor'"), device));
		}

		TEST_CASE("GraphicsDevice: TrimUnused runs every owner's trim hook and frees what they dropped"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.4, decision 43): the device first waits for its last submission, so nothing a hook drops is
			// still used by a submitted command list in any configuration (in Dist no tracker reference delays a destruction,
			// and NVRHI does not reference a texture used only by a clear, ADR 0009 decision 31); then the owners' trim hooks
			// run, last registered first; then it collects the garbage, so what the owners dropped is destroyed before the
			// request retries, without a frame.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			const GpuResourceTracker& tracker = device.GetResourceTracker();
			const uint64_t baseline = tracker.GetLiveCount(GpuResourceType::Texture);
			Result<nvrhi::TextureHandle> created = device.CreateTexture(MakeTargetDesc("Trimmed"));
			REQUIRE_MESSAGE(created.has_value(), created.error().ToString());
			nvrhi::TextureHandle held = std::move(*created);
			std::vector<std::string> calls;
			uint64_t completedAtTrim = 0;
			GpuRecreationHooks hooks;
			hooks.Name = "TrimmedOwner";
			hooks.Release = [&held]()
			{
				held = nullptr;
			};
			hooks.TrimUnused = [&held, &calls, &device, &completedAtTrim]()
			{
				completedAtTrim = device.GetCompletedSubmissionID();
				held = nullptr;
				calls.push_back("trim TrimmedOwner");
			};
			GpuRecreationRegistration registration = device.GetRecreation().Register(std::move(hooks));
			const Test::TrimCounter counter(device.GetRecreation());
			if (GpuResourceTracker::IsEnabled())
				REQUIRE(tracker.GetLiveCount(GpuResourceType::Texture) == baseline + 1);
			// The submission that last used the target, by a clear alone.
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			(*commandList)->open();
			(*commandList)->clearTextureFloat(held, nvrhi::AllSubresources, nvrhi::Color(0.25f, 0.5f, 0.75f, 1.0f));
			(*commandList)->close();
			const uint64_t submission = device.ExecuteCommandList(**commandList);

			device.TrimUnused();
			CHECK(calls == std::vector<std::string>{ "trim TrimmedOwner" });
			CHECK(counter.GetTrims() == 1);
			CHECK(completedAtTrim >= submission); // the hooks ran after the wait
			CHECK(held == nullptr);
			if (GpuResourceTracker::IsEnabled())
				CHECK(tracker.GetLiveCount(GpuResourceType::Texture) == baseline);
			CHECK_FALSE(device.IsLost());
			registration.Reset();
			commandList->Reset();
		}

		TEST_CASE("GraphicsDevice: TrimUnused trims nothing on a lost device or after a loss its wait meets"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 (C18.4, decision 43): the recovery releases everything, so a trim on a lost device runs no hook; and a hang the
			// driver resets during the trim's own wait (hang-reset strikes the first bounded wait after the arming) reports the
			// loss and ends the trim before any hook ran. Either way the device recovers afterwards.
			for (const GpuFault fault : { GpuFault::None, GpuFault::HangReset })
			{
				CAPTURE(GpuFaultToString(fault));
				Test::HeadlessGpuFixture gpu({ .InjectFault = fault });
				ENGINE_REQUIRE_GPU(gpu);
				GraphicsDevice& device = gpu.GetDevice();
				{
					const Test::TrimCounter counter(device.GetRecreation());
					Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
					REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
					(*commandList)->open();
					(*commandList)->close();
					// A submission for the trim's wait, then the first garbage collection, which arms hang-reset.
					static_cast<void>(device.ExecuteCommandList(**commandList));
					device.RunGarbageCollection();
					commandList->Reset();
					if (fault == GpuFault::None)
					{
						device.GetDiagnostics().SetDeviceLost(true);
						device.ReportDeviceLost("GpuOutOfMemoryTests");
					}
					device.TrimUnused();
					CHECK(device.IsLost());
					CHECK(counter.GetTrims() == 0);
				}
				const Status recovered = device.Recover(nullptr);
				REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
				CHECK(device.GetRecoveryCount() == 1);
				CHECK_FALSE(device.IsLost());
			}
		}

		TEST_CASE("GraphicsDevice: oom-rendertarget fails render-target creations after the first garbage collection, once or every time"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.4, decision 46): render targets and storage images created after the first frame fail as if
			// the device were out of memory, the first one or every one; sampled-only and staging textures are not targets.
			for (const GpuFault fault : { GpuFault::OomRenderTarget, GpuFault::OomRenderTargetPersistent })
			{
				CAPTURE(GpuFaultToString(fault));
				const bool persistent = fault == GpuFault::OomRenderTargetPersistent;
				Test::HeadlessGpuFixture gpu({ .InjectFault = fault });
				ENGINE_REQUIRE_GPU(gpu);
				GraphicsDevice& device = gpu.GetDevice();
				CHECK(device.CreateTexture(MakeTargetDesc("Startup")).has_value());
				device.RunGarbageCollection();

				nvrhi::TextureDesc sampled = MakeTargetDesc("Sampled");
				sampled.isRenderTarget = false;
				sampled.isShaderResource = true;
				sampled.initialState = nvrhi::ResourceStates::ShaderResource;
				CHECK(device.CreateTexture(sampled).has_value());
				nvrhi::TextureDesc staging = sampled;
				staging.isShaderResource = false;
				staging.initialState = nvrhi::ResourceStates::CopyDest;
				staging.debugName = "Staging";
				CHECK(device.CreateStagingTexture(staging, nvrhi::CpuAccessMode::Read).has_value());

				const Result<nvrhi::TextureHandle> struck = device.CreateTexture(MakeTargetDesc("Resized"));
				REQUIRE_FALSE(struck.has_value());
				CHECK(struck.error().GetCode() == ErrorCode::Gpu);
				CHECK(IsGpuOutOfMemory(struck.error(), device));
				CHECK(device.CreateTexture(MakeTargetDesc("Retried")).has_value() != persistent);
				nvrhi::TextureDesc storage = MakeTargetDesc("Storage");
				storage.isRenderTarget = false;
				storage.isUAV = true;
				storage.initialState = nvrhi::ResourceStates::UnorderedAccess;
				CHECK(device.CreateTexture(storage).has_value() != persistent);
				CHECK_FALSE(device.IsLost());
			}
		}

		TEST_CASE("GraphicsDevice: oom-pipeline fails pipeline creations after the first garbage collection, once or every time"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.4, decision 46): a pipeline created after the first frame (a debug view's, ImGui's for a new
			// format) fails as if the device were out of memory, the first one or every one.
			for (const GpuFault fault : { GpuFault::OomPipeline, GpuFault::OomPipelinePersistent })
			{
				CAPTURE(GpuFaultToString(fault));
				const bool persistent = fault == GpuFault::OomPipelinePersistent;
				Test::HeadlessGpuFixture gpu({ .InjectFault = fault });
				ENGINE_REQUIRE_GPU(gpu);
				GraphicsDevice& device = gpu.GetDevice();
				nvrhi::FramebufferInfo framebuffer;
				framebuffer.addColorFormat(nvrhi::Format::RGBA8_UNORM);
				CHECK(TrianglePass::Create(device, gpu.GetPipelines(), { .Framebuffer = framebuffer }).has_value());
				device.RunGarbageCollection();

				const Result<Scope<TrianglePass>> struck = TrianglePass::Create(device, gpu.GetPipelines(), { .Framebuffer = framebuffer });
				REQUIRE_FALSE(struck.has_value());
				CHECK(IsGpuOutOfMemory(struck.error(), device));
				CHECK(TrianglePass::Create(device, gpu.GetPipelines(), { .Framebuffer = framebuffer }).has_value() != persistent);
				CHECK_FALSE(device.IsLost());
			}
		}

		TEST_CASE("RetryAfterTrim: an out-of-memory attempt is retried once after a trim, and a second failure is OutOfMemory"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.4, decision 42): "trim what can be rebuilt, retry once, then fail only the request".
			for (const GpuFault fault : { GpuFault::OomRenderTarget, GpuFault::OomRenderTargetPersistent })
			{
				CAPTURE(GpuFaultToString(fault));
				Test::HeadlessGpuFixture gpu({ .InjectFault = fault });
				ENGINE_REQUIRE_GPU(gpu);
				GraphicsDevice& device = gpu.GetDevice();
				device.RunGarbageCollection();
				const Test::TrimCounter counter(device.GetRecreation());
				uint32_t attempts = 0;
				nvrhi::TextureHandle target;
				const Status created = RetryAfterTrim(device, "the test's resize", [&device, &attempts, &target]() -> Status
				{
					++attempts;
					ENGINE_TRY_ASSIGN(target, device.CreateTexture(MakeTargetDesc("RetriedTarget")));
					return {};
				});
				CHECK(attempts == 2);
				CHECK(counter.GetTrims() == 1);
				if (fault == GpuFault::OomRenderTarget)
				{
					CHECK_MESSAGE(created.has_value(), created.error().ToString());
					CHECK(target != nullptr);
				}
				else
				{
					REQUIRE_FALSE(created.has_value());
					CHECK(created.error().GetCode() == ErrorCode::OutOfMemory);
					CHECK(target == nullptr);
				}
				CHECK_FALSE(device.IsLost());
				target = nullptr;
			}
		}

		TEST_CASE("RetryAfterTrim: success, other errors, a lost device's errors and a recovery's errors are returned at once without a trim"
			* doctest::test_suite(Test::GpuSuite))
		{
			// M18 acceptance (C18.4, decision 42): only an out-of-memory creation is trimmed for, at most once per call (an
			// OutOfMemory error already failed alone after its own trim); a request's classification keeps a Gpu error that is
			// no out-of-memory as it is (the swapchain's VK_ERROR_INITIALIZATION_FAILED); on a lost device a Gpu error is the
			// loss; and while the registry runs a recovery's Recreate, an out-of-memory creation is returned as it is, so it
			// fails the recovery as at startup instead of trimming inside the registry.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			const Test::TrimCounter counter(device.GetRecreation());
			uint32_t attempts = 0;
			const auto run = [&device, &attempts](Status result, const std::function<bool(const Error&)>& isOutOfMemory)
			{
				attempts = 0;
				return RetryAfterTrim(device, "the test's request", [&attempts, &result]() -> Status
				{
					++attempts;
					return result;
				}, isOutOfMemory);
			};
			CHECK(run(Status{}, {}).has_value());
			CHECK(attempts == 1);
			const Status invalid = run(MakeError(ErrorCode::InvalidArgument, "a zero size"), {});
			REQUIRE_FALSE(invalid.has_value());
			CHECK(invalid.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(attempts == 1);
			const Status nested = run(MakeError(ErrorCode::OutOfMemory, "an inner request failed alone"), {});
			REQUIRE_FALSE(nested.has_value());
			CHECK(nested.error().GetCode() == ErrorCode::OutOfMemory);
			CHECK(attempts == 1);
			const Status otherwise = run(MakeError(ErrorCode::Gpu, "vkCreateSwapchainKHR (640x360) failed with ErrorInitializationFailed (-3)"),
				[](const Error& /*error*/)
			{
				return false;
			});
			REQUIRE_FALSE(otherwise.has_value());
			CHECK(otherwise.error().GetCode() == ErrorCode::Gpu);
			CHECK(attempts == 1);
			CHECK(counter.GetTrims() == 0);

			// An owner whose Recreate runs a request: the recovery's registry is running, so no trim.
			Status inRecreate{};
			uint32_t recreateAttempts = 0;
			GpuRecreationHooks hooks;
			hooks.Name = "RecreatingOwner";
			hooks.Release = []() {};
			hooks.Recreate = [&device, &inRecreate, &recreateAttempts]() -> Status
			{
				inRecreate = RetryAfterTrim(device, "the owner's recreation", [&recreateAttempts]() -> Status
				{
					++recreateAttempts;
					return MakeError(ErrorCode::Gpu, "cannot create texture 'Recreated'");
				});
				// The test reads the result; a failed Recreate would fail the recovery and leave the device to be destroyed.
				return {};
			};
			GpuRecreationRegistration recreating = device.GetRecreation().Register(std::move(hooks));

			device.GetDiagnostics().SetDeviceLost(true);
			device.ReportDeviceLost("GpuOutOfMemoryTests");
			const Status lost = run(MakeError(ErrorCode::Gpu, "cannot create texture 'SceneColor': the device is lost"), {});
			REQUIRE_FALSE(lost.has_value());
			CHECK(lost.error().GetCode() == ErrorCode::Gpu);
			CHECK(attempts == 1);
			const Status recovered = device.Recover(nullptr);
			REQUIRE_MESSAGE(recovered.has_value(), recovered.error().ToString());
			REQUIRE_FALSE(inRecreate.has_value());
			CHECK(inRecreate.error().GetCode() == ErrorCode::Gpu);
			CHECK(recreateAttempts == 1);
			CHECK(counter.GetTrims() == 0);
			recreating.Reset();
		}
	}

}
