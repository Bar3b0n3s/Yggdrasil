#include "TestsPCH.h"

#include "Engine/Graphics/GpuResourceTracker.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/OffscreenTarget.h"
#include "Support/HeadlessGpuFixture.h"

namespace Engine {

	TEST_SUITE("Graphics")
	{
		TEST_CASE("GpuResourceTracker: live counts return to zero" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			GpuResourceTracker& tracker = device.GetResourceTracker();
			const uint64_t baseline = tracker.GetTotalLiveCount();
			{
				// A target (two textures and a framebuffer), a buffer, a sampler and a command list that a submission used.
				Result<OffscreenTarget> target = OffscreenTarget::Create(device, { .Width = 64, .Height = 32, .Depth = true });
				REQUIRE_MESSAGE(target.has_value(), target.error().ToString());
				nvrhi::BufferDesc bufferDesc;
				bufferDesc.byteSize = 1024;
				bufferDesc.debugName = "LeakCheck";
				Result<nvrhi::BufferHandle> buffer = device.CreateBuffer(bufferDesc);
				REQUIRE_MESSAGE(buffer.has_value(), buffer.error().ToString());
				Result<nvrhi::SamplerHandle> sampler = device.CreateSampler(nvrhi::SamplerDesc());
				REQUIRE_MESSAGE(sampler.has_value(), sampler.error().ToString());
				Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
				REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
				(*commandList)->open();
				target->Clear(**commandList);
				(*commandList)->close();
				device.ExecuteCommandList(**commandList);
				CHECK(tracker.GetLiveCount(GpuResourceType::Texture) >= 2);
				CHECK(tracker.GetLiveCount(GpuResourceType::Buffer) >= 1);
				CHECK(tracker.GetTotalLiveCount() > baseline);
			}
			// Dropped handles are counted as destroyed at the next garbage collection: NVRHI retires the submission's
			// references first, and the sweep then releases the framebuffer and, in the same call, the textures it held.
			device.WaitForIdle();
			device.RunGarbageCollection();
			INFO("live: ", tracker.DescribeLiveCounts());
			CHECK(tracker.GetTotalLiveCount() == baseline);
			// The fixture's destruction then checks zero live objects with nothing left at all.
		}

		TEST_CASE("GpuResourceTracker: HasOtherReferences discounts the tracker's own reference"
			* doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			const GpuResourceTracker& tracker = device.GetResourceTracker();
			nvrhi::TextureDesc desc;
			desc.width = 16;
			desc.height = 16;
			desc.format = nvrhi::Format::RGBA8_UNORM;
			desc.isRenderTarget = true;
			desc.initialState = nvrhi::ResourceStates::RenderTarget;
			desc.keepInitialState = true;
			desc.debugName = "OwnershipCheck";
			Result<nvrhi::TextureHandle> texture = device.CreateTexture(desc);
			REQUIRE_MESSAGE(texture.has_value(), texture.error().ToString());

			// Only this handle (and the tracker, which does not count) references the texture.
			CHECK_FALSE(tracker.HasOtherReferences(**texture, 1));
			{
				// A second handle is another reference, unless the caller counts it as its own.
				const nvrhi::TextureHandle copy = *texture;
				CHECK(tracker.HasOtherReferences(**texture, 1));
				CHECK_FALSE(tracker.HasOtherReferences(**texture, 2));
			}
			CHECK_FALSE(tracker.HasOtherReferences(**texture, 1));

			// A submission that used the texture references it until NVRHI retires it. The upload is writeTexture, which
			// records the reference; NVRHI's clearTextureFloat records none.
			const std::vector<std::byte> texels(static_cast<size_t>(desc.width) * desc.height * 4);
			Result<nvrhi::CommandListHandle> commandList = device.CreateCommandList();
			REQUIRE_MESSAGE(commandList.has_value(), commandList.error().ToString());
			(*commandList)->open();
			(*commandList)->writeTexture(*texture, 0, 0, texels.data(), static_cast<size_t>(desc.width) * 4);
			(*commandList)->close();
			device.ExecuteCommandList(**commandList);
			CHECK(tracker.HasOtherReferences(**texture, 1));
			device.WaitForIdle();
			device.RunGarbageCollection();
			CHECK_FALSE(tracker.HasOtherReferences(**texture, 1));
		}

		TEST_CASE("GpuResourceTracker: recorded host images count as created and destroyed")
		{
			GpuResourceTracker tracker;
			tracker.RecordCreated(GpuResourceType::HostImage);
			tracker.RecordCreated(GpuResourceType::HostImage);
			tracker.RecordDestroyed(GpuResourceType::HostImage);
			const GpuResourceCounts counts = tracker.GetCounts(GpuResourceType::HostImage);
			CHECK(counts.Created == 2);
			CHECK(counts.Destroyed == 1);
			CHECK(counts.GetLive() == 1);
			CHECK(tracker.GetLiveCount(GpuResourceType::HostImage) == 1);
			CHECK(tracker.GetTotalLiveCount() == 1);
			CHECK(tracker.DescribeLiveCounts() == "HostImage: 1");
			tracker.RecordDestroyed(GpuResourceType::HostImage);
			CHECK(tracker.GetTotalLiveCount() == 0);
			CHECK(tracker.DescribeLiveCounts() == "none");
		}

		TEST_CASE("GpuResourceTracker: every type has its enumerator name")
		{
			CHECK(GpuResourceTypeToString(GpuResourceType::Texture) == "Texture");
			CHECK(GpuResourceTypeToString(GpuResourceType::GraphicsPipeline) == "GraphicsPipeline");
			CHECK(GpuResourceTypeToString(GpuResourceType::HostImage) == "HostImage");
			for (size_t index = 0; index < GpuResourceTypeCount; ++index)
				CHECK_FALSE(GpuResourceTypeToString(static_cast<GpuResourceType>(index)).empty());
			CHECK(GpuResourceTracker::IsEnabled()); // Tests never build Dist
		}
	}

}
