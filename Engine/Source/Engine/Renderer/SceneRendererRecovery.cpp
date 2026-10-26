#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Core/Assert.h"
#include "Engine/Graphics/GpuProfiler.h"
#include "Engine/Graphics/GraphicsDevice.h"
#include "Engine/Graphics/RenderTargetPool.h"
#include "Engine/Renderer/AsyncPicker.h"
#include "Engine/Renderer/Private/SceneRendererState.h"
#include "Engine/Renderer/RenderPrepare.h"
#include "Engine/Renderer/ShadowPass.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <string>
#include <utility>

// A scene renderer's hooks in the recreation protocol (SceneRenderer::ReleaseGpuObjects and RecreateGpuObjects;
// Docs/Decisions/0029-m18-contract.md decisions 26 and 27): its per-view objects are released when the device is lost and
// created again on the recovered device.
//
// Release replaces the whole state with a new one that receives only what survives a loss (the back-references, the
// registration, the size, the stats of the last render, the once-per-renderer log flags and the view generation), so a
// per-view object a later milestone adds to SceneRenderer::State is released without code of its own (the placement rule's
// "resets every member but"). The RenderTargetPool keeps its object and drops its targets (RenderTargetPool::ReleaseAll),
// and the picker keeps its object and its ticket counter and drops its GPU objects (AsyncPicker::ReleaseGpuObjects), so a
// pending pick resolves as cancelled and no ticket is reused; the profiler, which creates its GPU objects on use, is made
// new as Create makes it. The renderer answers CancelPicks, PollPick and RequestPick between Release and Recreate.

namespace Engine {

	namespace Utils {

		// A buffer of `size` bytes kept in the renderer's state between command lists (keepInitialState): a constant buffer,
		// or a structured buffer of `structStride`-byte elements.
		[[nodiscard]] static Result<nvrhi::BufferHandle> CreateViewBuffer(GraphicsDevice& device, size_t size, uint32_t structStride, std::string debugName)
		{
			nvrhi::BufferDesc desc;
			desc.byteSize = size;
			desc.isConstantBuffer = structStride == 0;
			desc.structStride = structStride;
			desc.initialState = structStride == 0 ? nvrhi::ResourceStates::ConstantBuffer : nvrhi::ResourceStates::ShaderResource;
			desc.keepInitialState = true;
			desc.debugName = std::move(debugName);
			return device.CreateBuffer(desc);
		}

	}

	void SceneRenderer::ReleaseGpuObjects()
	{
		State& released = *m_State;
		ENGINE_CORE_VERIFY(released.Generation < std::numeric_limits<uint64_t>::max(), "view generation exhausted");
		Scope<State> kept = CreateScope<State>();
		State& state = *kept;
		state.Device = released.Device;
		state.PipelineSet = released.PipelineSet;
		state.Pipelines = released.Pipelines;
		state.Cache = released.Cache;
		state.Assets = released.Assets;
		// Moving a registration over an empty one calls nothing on the registry, so this runs inside ReleaseAll.
		state.Recreation = std::move(released.Recreation);
		state.ViewWidth = released.ViewWidth;
		state.ViewHeight = released.ViewHeight;
		// GetRenderStats' history and GetLastStats' summary both describe the last render; neither holds a GPU object.
		state.History = std::move(released.History);
		state.Stats = released.Stats;
		state.SpotLimitLogged = released.SpotLimitLogged;
		state.ShadowRejectionLogged = released.ShadowRejectionLogged;
		state.WireframeBudgetLogged = released.WireframeBudgetLogged;
		state.AllocationLimitLogged = released.AllocationLimitLogged;
		state.LightLimitLogged = released.LightLimitLogged;
		// The released image and its picks are gone: the next generation, as CancelPicks makes it, never reused.
		state.Generation = released.Generation + 1;

		state.TargetPool = std::move(released.TargetPool);
		state.TargetPool->ReleaseAll();
		state.Picker = std::move(released.Picker);
		state.Picker->ReleaseGpuObjects();
		state.Profiler = CreateScope<GpuProfiler>(*state.Device, state.Device->GetFramesInFlight());
		state.TimingSubmissions.resize(state.Device->GetFramesInFlight(), 0);

		// Everything else goes with the released state: the targets and the output stage's, the binding and material sets,
		// the per-view buffers, the old profiler's queries, a pending render's identity (PendingSubmission is false in the new
		// state, so no OnSubmitted is awaited) and the draws' borrowed pointers.
		m_State = std::move(kept);
	}

	Status SceneRenderer::RecreateGpuObjects()
	{
		State& state = *m_State;
		GraphicsDevice& device = *state.Device;
		// The targets follow the set's bloom format: the set registered before the renderer, so the recovery created its passes
		// again first (GpuRecreationRegistry::RecreateAll's order), as they exist when Create calls this.
		ENGINE_CORE_ASSERT(state.Pipelines->Bloom != nullptr, "SceneRenderer's per-view objects are created after its pipeline set's passes");
		// The per-view buffers and the targets of the renderer's size (Create makes them through this too), made aside and
		// kept only when every creation succeeded.
		ENGINE_TRY_ASSIGN(nvrhi::BufferHandle viewConstants,
			Utils::CreateViewBuffer(device, sizeof(ViewConstants), 0, "SceneRenderer.ViewConstants"));
		ENGINE_TRY_ASSIGN(nvrhi::BufferHandle environmentConstants,
			Utils::CreateViewBuffer(device, sizeof(EnvironmentConstants), 0, "SceneRenderer.EnvironmentConstants"));
		ENGINE_TRY_ASSIGN(nvrhi::BufferHandle lights,
			Utils::CreateViewBuffer(device, sizeof(ShaderLight) * MaxVisibleLights, sizeof(ShaderLight), "SceneRenderer.Lights"));
		ENGINE_TRY_ASSIGN(nvrhi::BufferHandle shadowConstants,
			Utils::CreateViewBuffer(device, sizeof(ShadowConstants), 0, "SceneRenderer.ShadowConstants"));
		ENGINE_TRY_ASSIGN(nvrhi::BufferHandle casterConstants, device.CreateBuffer(ShadowPass::GetCasterConstantsDesc()));
		ENGINE_TRY_ASSIGN(Detail::SceneRendererTargets targets, state.CreateTargets(state.ViewWidth, state.ViewHeight));
		state.ViewConstantsBuffer = std::move(viewConstants);
		state.EnvironmentConstantsBuffer = std::move(environmentConstants);
		state.LightBuffer = std::move(lights);
		state.ShadowConstantsBuffer = std::move(shadowConstants);
		state.ShadowCasterConstantsBuffer = std::move(casterConstants);
		state.Targets = std::move(targets);
		state.FinalTarget = 0;
		state.Lights.reserve(MaxVisibleLights);
		return {};
	}

}
