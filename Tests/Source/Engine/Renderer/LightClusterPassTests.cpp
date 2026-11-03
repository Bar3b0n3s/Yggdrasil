#include "TestsPCH.h"

#include "Engine/Renderer/LightClusterPass.h"

#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Renderer/LightClusters.h"
#include "Engine/Renderer/PassBindingCache.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Engine/Renderer/RenderStats.h"
#include "Shared/LightClusterConstants.h"
#include "Shared/ShaderLight.h"
#include "Shared/ViewConstants.h"
#include "Support/HeadlessGpuFixture.h"

#include <cstdint>

// The LightClusters pass of the clustered light loop (M23, C23.1, R-CLC; Docs/Decisions/0034-m23-contract.md decision 5,
// stream C): its constants, its mask buffer and its input checks. The grid is LightClustersTests.cpp's; the renders through
// the pass are SceneRenderer cases there.

namespace Engine {

	namespace {

		// A 60-degree perspective camera at the origin looking down -Z, clipping at 0.1 and 100 m.
		CameraData MakeCamera(uint32_t width, uint32_t height)
		{
			CameraData camera;
			camera.NearClip = 0.1f;
			camera.FarClip = 100.0f;
			camera.ViewportWidth = width;
			camera.ViewportHeight = height;
			camera.Projection = ComputeReverseZProjection(RenderProjection::Perspective, 60.0f, 10.0f, 0.1f, 100.0f, width, height);
			return camera;
		}

	}

	TEST_SUITE("Renderer")
	{
		// The pass's constants and mask buffer follow the grid (Shared/LightClusterConstants.h's layout).
		TEST_CASE("LightClusterPass: the constants and the mask buffer follow the grid")
		{
			const LightClusterGrid grid = ComputeLightClusterGrid(MakeCamera(1920, 1080), 1920, 1080);
			CHECK(GetLightClusterCount(grid) == 30 * 17 * 24);
			const LightClusterConstants clustered = MakeLightClusterConstants(grid, LightClusterModeClustered);
			CHECK(clustered.Grid == glm::uvec4(30, 17, 24, LightClusterWordCount));
			CHECK(clustered.Slicing == glm::vec4(grid.Near, grid.Far, grid.SliceScale, grid.SliceBias));
			CHECK(clustered.Mode == glm::uvec4(LightClusterModeClustered, 0, 0, 0));
			CHECK(MakeLightClusterConstants(grid, LightClusterModeBruteForce).Mode.x == LightClusterModeBruteForce);
			const nvrhi::BufferDesc masks = LightClusterPass::GetMaskBufferDesc(GetLightClusterCount(grid));
			CHECK(masks.byteSize == uint64_t{ 30 } * 17 * 24 * LightClusterWordCount * sizeof(uint32_t));
			CHECK(masks.structStride == sizeof(uint32_t));
			CHECK(masks.canHaveUAVs);
			CHECK(LightClusterPass::GetMaskBufferDesc(0).byteSize == LightClusterWordCount * sizeof(uint32_t));
			CHECK(LightClusterPass::GetConstantsDesc().byteSize == sizeof(LightClusterConstants));
			CHECK(LightClusterPass::GetConstantsDesc().isConstantBuffer);
		}
	}

	TEST_SUITE(Test::GpuSuite)
	{
		// Record checks its inputs before it records anything: every buffer, a grid of at least one cluster and a mask
		// buffer that holds the grid's clusters.
		TEST_CASE("LightClusterPass: Record refuses missing buffers and a mask buffer smaller than its grid's")
		{
			Test::HeadlessGpuFixture gpu;
			ENGINE_REQUIRE_GPU(gpu);
			GraphicsDevice& device = gpu.GetDevice();
			{
				Result<Scope<LightClusterPass>> pass = LightClusterPass::Create(device, gpu.GetPipelines());
				REQUIRE_MESSAGE(pass.has_value(), pass.error().ToString());
				const LightClusterGrid grid = ComputeLightClusterGrid(MakeCamera(128, 64), 128, 64);
				nvrhi::BufferDesc viewDesc;
				viewDesc.byteSize = sizeof(ViewConstants);
				viewDesc.isConstantBuffer = true;
				viewDesc.initialState = nvrhi::ResourceStates::ConstantBuffer;
				viewDesc.keepInitialState = true;
				nvrhi::BufferDesc lightsDesc;
				lightsDesc.byteSize = sizeof(ShaderLight);
				lightsDesc.structStride = sizeof(ShaderLight);
				lightsDesc.initialState = nvrhi::ResourceStates::ShaderResource;
				lightsDesc.keepInitialState = true;
				Result<nvrhi::BufferHandle> view = device.CreateBuffer(viewDesc);
				Result<nvrhi::BufferHandle> constants = device.CreateBuffer(LightClusterPass::GetConstantsDesc());
				Result<nvrhi::BufferHandle> lights = device.CreateBuffer(lightsDesc);
				Result<nvrhi::BufferHandle> masks = device.CreateBuffer(LightClusterPass::GetMaskBufferDesc(GetLightClusterCount(grid) - 1));
				REQUIRE(view.has_value());
				REQUIRE(constants.has_value());
				REQUIRE(lights.has_value());
				REQUIRE(masks.has_value());
				const LightClusterPassInputs complete{ .Grid = grid, .ViewConstants = *view, .ClusterConstants = *constants, .Lights = *lights, .Masks = *masks };
				LightClusterPassInputs withoutLights = complete;
				withoutLights.Lights = nullptr;
				LightClusterPassInputs emptyGrid = complete;
				emptyGrid.Grid = {};
				Result<nvrhi::CommandListHandle> list = device.CreateCommandList();
				REQUIRE(list.has_value());
				PassBindingCache bindings;
				RenderStats stats;
				RenderRecordingContext recording(stats, []()
				{
					return 0.0;
				});
				(*list)->open();
				// `complete`'s mask buffer holds one cluster fewer than the grid has.
				for (const LightClusterPassInputs& inputs : { complete, withoutLights, emptyGrid })
				{
					const Status refused = (*pass)->Record(**list, recording, bindings, inputs);
					REQUIRE_FALSE(refused.has_value());
					CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
				}
				(*list)->close();
				device.ExecuteCommandList(**list);
				device.WaitForIdle();
				CHECK(stats.Passes.empty());
				CHECK(bindings.GetSize() == 0);
			}
			device.RunGarbageCollection();
		}
	}

}
