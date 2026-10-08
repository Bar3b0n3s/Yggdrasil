#include "TestsPCH.h"

#include "Engine/Renderer/PassBindingCache.h"

#include "Engine/Graphics/GpuResourceTracker.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Support/HeadlessGpuFixture.h"

#include <string>

// The per-view binding sets of the shared passes (Renderer/PassBindingCache.h; Docs/Decisions/0013-m8-decisions.md decision
// 7), implemented by the M8 contract.

namespace Engine {

	namespace {

		// A compute set-0 layout with one constant buffer at b0, the shape of a pass's per-view constants.
		nvrhi::BindingLayoutDesc MakeConstantsLayoutDesc()
		{
			nvrhi::BindingLayoutDesc layout;
			layout.visibility = nvrhi::ShaderType::Compute;
			layout.registerSpace = 0;
			layout.registerSpaceIsDescriptorSet = true;
			layout.bindings = { nvrhi::BindingLayoutItem::ConstantBuffer(0) };
			return layout;
		}

		nvrhi::BufferHandle CreateConstants(GraphicsDevice& device, const char* name)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = 256;
			desc.isConstantBuffer = true;
			desc.initialState = nvrhi::ResourceStates::ConstantBuffer;
			desc.keepInitialState = true;
			desc.debugName = name;
			Result<nvrhi::BufferHandle> buffer = device.CreateBuffer(desc);
			REQUIRE_MESSAGE(buffer.has_value(), buffer.error().ToString());
			return *buffer;
		}

		nvrhi::BindingSetDesc MakeConstantsSet(nvrhi::IBuffer* constants)
		{
			nvrhi::BindingSetDesc desc;
			desc.bindings = { nvrhi::BindingSetItem::ConstantBuffer(0, constants) };
			return desc;
		}

	}

	TEST_SUITE("Renderer")
	{
		TEST_CASE("PassBindingCache: equal descs share one set, and unused sets are released per period" * doctest::test_suite(Test::GpuSuite))
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<nvrhi::BindingLayoutHandle> layout = device.CreateBindingLayout(MakeConstantsLayoutDesc());
				Result<nvrhi::BindingLayoutHandle> otherLayout = device.CreateBindingLayout(MakeConstantsLayoutDesc());
				REQUIRE(layout.has_value());
				REQUIRE(otherLayout.has_value());
				const nvrhi::BufferHandle first = CreateConstants(device, "PassBindingCacheTests.First");
				const nvrhi::BufferHandle second = CreateConstants(device, "PassBindingCacheTests.Second");

				PassBindingCache cache;
				const Result<nvrhi::IBindingSet*> a = cache.GetOrCreate(device, MakeConstantsSet(first), **layout);
				const Result<nvrhi::IBindingSet*> again = cache.GetOrCreate(device, MakeConstantsSet(first), **layout);
				REQUIRE_MESSAGE(a.has_value(), a.error().ToString());
				REQUIRE(again.has_value());
				CHECK(*a == *again);
				CHECK((*a)->getLayout() == layout->Get());
				CHECK(cache.GetSize() == 1);

				// Another resource, or the same desc for another layout object, is another set (NVRHI binds a set only with
				// the pipeline's own layout objects).
				const Result<nvrhi::IBindingSet*> b = cache.GetOrCreate(device, MakeConstantsSet(second), **layout);
				const Result<nvrhi::IBindingSet*> c = cache.GetOrCreate(device, MakeConstantsSet(first), **otherLayout);
				REQUIRE(b.has_value());
				REQUIRE(c.has_value());
				CHECK(*b != *a);
				CHECK(*c != *a);
				CHECK((*c)->getLayout() == otherLayout->Get());
				CHECK(cache.GetSize() == 3);

				// Everything was used in this period, so nothing goes; the next period keeps only what it uses.
				cache.ReleaseUnused();
				CHECK(cache.GetSize() == 3);
				const Result<nvrhi::IBindingSet*> kept = cache.GetOrCreate(device, MakeConstantsSet(first), **layout);
				REQUIRE(kept.has_value());
				CHECK(*kept == *a);
				cache.ReleaseUnused();
				CHECK(cache.GetSize() == 1);
				cache.ReleaseUnused();
				CHECK(cache.GetSize() == 0);

				// Clear drops everything at once.
				static_cast<void>(cache.GetOrCreate(device, MakeConstantsSet(first), **layout));
				static_cast<void>(cache.GetOrCreate(device, MakeConstantsSet(second), **layout));
				CHECK(cache.GetSize() == 2);
				cache.Clear();
				CHECK(cache.GetSize() == 0);
			}
			device.RunGarbageCollection();
		}

		TEST_CASE("PassBindingCache: a cached set keeps its resources alive until it is released" * doctest::test_suite(Test::GpuSuite))
		{
			// Why a view's caches must go with its targets (SceneRenderer::Resize and its destructor): the set references the
			// resources it binds, so a resource dropped by its owner lives on while its set is cached.
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<nvrhi::BindingLayoutHandle> layout = device.CreateBindingLayout(MakeConstantsLayoutDesc());
				REQUIRE(layout.has_value());
				PassBindingCache cache;
				device.RunGarbageCollection();
				const uint64_t baseline = device.GetResourceTracker().GetLiveCount(GpuResourceType::GpuBuffer);
				{
					const nvrhi::BufferHandle constants = CreateConstants(device, "PassBindingCacheTests.Constants");
					REQUIRE(cache.GetOrCreate(device, MakeConstantsSet(constants), **layout).has_value());
				}
				device.RunGarbageCollection();
				CHECK(device.GetResourceTracker().GetLiveCount(GpuResourceType::GpuBuffer) == baseline + 1);
				cache.Clear();
				device.WaitForIdle();
				device.RunGarbageCollection();
				INFO("live: ", device.GetResourceTracker().DescribeLiveCounts());
				CHECK(device.GetResourceTracker().GetLiveCount(GpuResourceType::GpuBuffer) == baseline);
			}
			device.RunGarbageCollection();
		}
	}

}
