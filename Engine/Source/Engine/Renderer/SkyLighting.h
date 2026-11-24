#pragma once

#include "Engine/Core/Base.h"
#include "Shared/SkyConstants.h"

#include <nvrhi/nvrhi.h>

#include <cstdint>
#include <span>

// The sky's lighting on the GPU (M45, Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Architecture §8.6): what a
// view with a Sky records in the Ambient stage's SkyLighting declaration (rows SkyLuts, SkyIbl and ProbeRelight), after its
// CPU constants and before the forward passes read them.
//
// The copy into constant buffers (decision 3, "The copy"). The view's constant buffers are static (isConstantBuffer, initial
// state ConstantBuffer, kept), written from the CPU by RecordViewSetup every frame (writeBuffer: vkCmdUpdateBuffer, so in the
// CopyDest state); the sky's computes write the values that follow the sky into the view's SkyResults buffer (a structured
// buffer of float4, Shared/SkyConstants.h's layout), and RecordSkyResultCopies copies them over the CPU-written bytes with
// copyBuffer, whose automatic barriers move each buffer to CopySource or CopyDest and back to ConstantBuffer before a pass
// binds it. Since writeBuffer and copyBuffer both write in CopyDest, between which NVRHI places no barrier, the copies first
// move their destinations to ConstantBuffer (a barrier completing the CPU writes; synchronization validation reports the
// write-after-write hazard without it). Every Vulkan buffer of the vendored NVRHI is created with TRANSFER_SRC and
// TRANSFER_DST. copyBuffer, unlike
// writeBuffer, ends an open render pass only when a barrier is pending, so the copies are recorded outside every render pass:
// at the end of the Ambient stage, right after the sky's dispatches. No layout changes: the forward, terrain, skybox, SSR,
// probe and fog shaders read the constants they always read. Proved on the device, under validation and synchronization
// validation at API 1.4 and 1.3, by "SkyLighting: a buffer copied into a view's constant buffers after their CPU writes
// reaches the passes that read them".

namespace Engine {

	// Records the copies of `results` (SkyResultsCount float4 elements, Shared/SkyConstants.h) into the view's constant buffers,
	// after the command list's CPU writes of them: the irradiance (SkyResultsIrradiance, 144 bytes) over
	// EnvironmentConstants::IrradianceSH9 of `environment`; the fog's colour (SkyResultsFogColor, 12 bytes) over
	// FogConstants::Color of each of `fogs` (the view's, then each drawn mirror slot's); and, for each i below
	// `reflectionProbes` (at most MaxViewReflectionProbes), SkyResultsReflectionScales + i's x (4 bytes) over
	// ProbeConstants::Reflections[i].AxisScale.w of `probes`; the destinations first move to ConstantBuffer (see the file
	// comment). A null `environment` or `probes` and a null entry of `fogs` are skipped; `results` is never null (asserted).
	// Main thread, outside a render pass.
	void RecordSkyResultCopies(nvrhi::ICommandList& commandList, nvrhi::IBuffer* results, nvrhi::IBuffer* environment, std::span<nvrhi::IBuffer* const> fogs,
		nvrhi::IBuffer* probes, uint32_t reflectionProbes);

}
