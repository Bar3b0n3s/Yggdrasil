#include "TestsPCH.h"

#include "Engine/Graphics/RenderTargetPool.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	static nvrhi::TextureDesc MakeTargetDesc(uint32_t width, nvrhi::Format format, const char* name)
	{
		nvrhi::TextureDesc desc;
		desc.width = width;
		desc.height = 64;
		desc.format = format;
		desc.isRenderTarget = true;
		desc.initialState = nvrhi::ResourceStates::RenderTarget;
		desc.keepInitialState = true;
		desc.debugName = name;
		return desc;
	}

	TEST_SUITE("Graphics")
	{
		TEST_CASE("RenderTargetPool: equal descs reuse a free target and different ones do not"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			RenderTargetPool pool(gpu.GetDevice());
			nvrhi::ITexture* first = nullptr;
			{
				Result<nvrhi::TextureHandle> acquired = pool.Acquire(MakeTargetDesc(128, nvrhi::Format::RGBA16_FLOAT, "SceneColor"));
				REQUIRE_MESSAGE(acquired.has_value(), acquired.error().ToString());
				first = acquired->Get();
				CHECK(pool.GetAcquiredCount() == 1);
				// While held, an equal desc gets a second target.
				Result<nvrhi::TextureHandle> second = pool.Acquire(MakeTargetDesc(128, nvrhi::Format::RGBA16_FLOAT, "Other"));
				REQUIRE(second.has_value());
				CHECK(second->Get() != first);
				CHECK(pool.GetTargetCount() == 2);
			}
			// Released: an equal desc (whatever its name) reuses it; a different format or size does not.
			CHECK(pool.GetAcquiredCount() == 0);
			Result<nvrhi::TextureHandle> reused = pool.Acquire(MakeTargetDesc(128, nvrhi::Format::RGBA16_FLOAT, "Renamed"));
			REQUIRE(reused.has_value());
			CHECK(pool.GetTargetCount() == 2); // one of the two free targets, no new one
			Result<nvrhi::TextureHandle> otherFormat = pool.Acquire(MakeTargetDesc(128, nvrhi::Format::RGBA8_UNORM, "Ldr"));
			REQUIRE(otherFormat.has_value());
			Result<nvrhi::TextureHandle> otherSize = pool.Acquire(MakeTargetDesc(256, nvrhi::Format::RGBA16_FLOAT, "Big"));
			REQUIRE(otherSize.has_value());
			CHECK(otherFormat->Get() != reused->Get());
			CHECK(otherSize->Get() != reused->Get());
		}

		TEST_CASE("RenderTargetPool: EndFrame releases targets idle for longer than keepFrames"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			RenderTargetPool pool(gpu.GetDevice(), 2);
			{
				Result<nvrhi::TextureHandle> acquired = pool.Acquire(MakeTargetDesc(64, nvrhi::Format::R8_UNORM, "Mask"));
				REQUIRE(acquired.has_value());
			}
			CHECK(pool.GetTargetCount() == 1);
			pool.EndFrame();
			pool.EndFrame();
			CHECK(pool.GetTargetCount() == 1); // idle for keepFrames frames: kept
			pool.EndFrame();
			CHECK(pool.GetTargetCount() == 0); // idle for longer: released

			Result<nvrhi::TextureHandle> held = pool.Acquire(MakeTargetDesc(64, nvrhi::Format::R8_UNORM, "Held"));
			REQUIRE(held.has_value());
			for (int frame = 0; frame < 5; ++frame)
				pool.EndFrame();
			CHECK(pool.GetTargetCount() == 1); // an acquired target is never released
			held->Reset();
			pool.ReleaseFree();
			CHECK(pool.GetTargetCount() == 0);
		}

		TEST_CASE("RenderTargetPool: a free target is released only after the submissions that used it completed"
			* doctest::test_suite(Test::GpuSuite))
		{
			// NVRHI's command lists do not reference textures used only by clears, so the pool must not release a target that
			// a pending submission cleared: the validation layer would report the destroyed image as in use, and the GPU would
			// write freed memory.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			RenderTargetPool pool(device, 0); // release free targets at the first EndFrame
			Result<Scope<Test::GpuSubmissionGate>> gate = Test::GpuSubmissionGate::Create(device);
			REQUIRE_MESSAGE(gate.has_value(), gate.error().ToString());
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());

			// A target acquired since the last EndFrame may still be recorded into an open command list: never released.
			{
				Result<nvrhi::TextureHandle> acquired = pool.Acquire(MakeTargetDesc(32, nvrhi::Format::RGBA8_UNORM, "Recorded"));
				REQUIRE(acquired.has_value());
			}
			pool.ReleaseFree();
			CHECK(pool.GetTargetCount() == 1);

			// The frame clears the target, and its submission is held back on the GPU.
			{
				Result<nvrhi::TextureHandle> acquired = pool.Acquire(MakeTargetDesc(32, nvrhi::Format::RGBA8_UNORM, "Cleared"));
				REQUIRE(acquired.has_value());
				(*commandList)->open();
				(*commandList)->clearTextureFloat(*acquired, nvrhi::AllSubresources, nvrhi::Color(0.5f, 0.25f, 0.0f, 1.0f));
				(*commandList)->close();
				(*gate)->HoldNextSubmission();
				device.ExecuteCommandList(**commandList);
			}
			pool.EndFrame();
			CHECK(pool.GetTargetCount() == 1); // free for longer than keepFrames, but its submission has not completed
			pool.ReleaseFree();
			CHECK(pool.GetTargetCount() == 1);
			device.RunGarbageCollection();
			CHECK(pool.GetAcquiredCount() == 0);

			// Once the submission completed, the target goes.
			(*gate)->Open();
			device.WaitForIdle();
			pool.ReleaseFree();
			CHECK(pool.GetTargetCount() == 0);
			gate->reset();
			commandList->Reset();
		}
	}

}
