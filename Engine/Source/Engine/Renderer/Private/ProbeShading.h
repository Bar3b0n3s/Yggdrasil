#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Shared/ProbeConstants.h"

#include <glm/glm.hpp>
#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>
#include <vector>

// The scene renderer's probes (M26, C26.1, R-GI-PROBE; Docs/Decisions/0037-m26-contract.md decision 16; stream Q-B;
// Architecture §8.6): what the forward passes' indirect term (Common/Probes.slang) reads, prepared by the Prepare stage from
// the snapshot's IrradianceVolumes and ReflectionProbes and the GpuResourceCache's mirrors of their data, and the view's GPU
// copies of those mirrors (the placement rule's place 2, ADR 0029 decision 27).
//
// The view's probes are the first MaxViewIrradianceVolumes irradiance volumes and MaxViewReflectionProbes reflection probes
// of the snapshot, in its order, that have a box (a finite, invertible world matrix and a finite, positive Size), that the
// view does not cull (IsOutsideView of the box) and whose data has a mirror (GpuResourceCache::GetIrradianceVolume, or
// GetEnvironment for a reflection probe, which report a missing or failed asset themselves); the rest are dropped (counted).
// A reflection probe's cube is its Environment's prefiltered specular cube (EnvironmentData::SpecularFaceSize, SpecularMipCount
// levels), RGBA16_FLOAT or BC6H_UFLOAT, which the view copies into one cube array of the first kept probe's format: a later
// probe whose cube has another format (an Environment imported with another Compression on a device with BC support) is
// dropped too (counted). Each list is then ordered by its boxes' world volumes, the largest first, ties in snapshot order
// (ProbeConstants). Intensity is clamped to [0, MaxEnvironmentIntensity] (a non-finite one is 0) and a non-finite or negative
// BlendDistance is 0.
//
// M45 (Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3, "Relit probes"): in a view a Sky lights, each kept volume
// whose data is relightable brings its GPU relight block (GpuIrradianceVolume::Relight), which the view's ProbeRelight row
// relights into the view's IrradianceProbes copy (now UAV-capable), and each reflection probe its relight entry: the relit
// constant term of the probe nearest its centre of the smallest relightable volume whose box holds that centre, that probe's
// baked constant term's luminance and its Intensity (Shared/SkyConstants.h's ReflectionRelights layout), from which
// CSProbeRelight scales its Intensity. A view whose copy holds relit probes and whose sky no longer relights them copies its
// volumes' bakes again.

namespace Engine {

	class GpuResourceCache;
	class GraphicsDevice;

	namespace Utils {

		// Which data a mirror holds: its asset and the asset's version (AssetManager::GetVersion, which only increases for a
		// handle), so two equal ids name equal data even when the cache released one mirror and uploaded another in between.
		// The view compares its copies by these ids and holds no reference to a mirror (the cache stays its one owner).
		struct ProbeMirrorId
		{
			AssetHandle Asset{};
			uint64_t Version = 0;

			bool operator==(const ProbeMirrorId&) const = default;
		};

		// The view's probes for one render.
		struct ProbeShadingInputs
		{
			// What b4 holds; Counts.x and Counts.y entries of its lists are set.
			ProbeConstants Constants{};
			// The GpuIrradianceVolume buffer of each of Constants' volumes, in their order, and the float4 coefficients they
			// hold together (the view's copy holds them one after another, ProbeVolumeConstants::Probes.w each one's first).
			std::vector<nvrhi::BufferHandle> Volumes{};
			std::vector<ProbeMirrorId> VolumeIds{}; // the data of each of Volumes, in their order
			uint32_t CoefficientCount = 0;
			// The specular cube of each of Constants' reflection probes, in their order: layer group i of the view's cube array
			// (ProbeReflectionConstants::Indices.x is i).
			std::vector<nvrhi::TextureHandle> Cubes{};
			std::vector<ProbeMirrorId> CubeIds{}; // the Environment of each of Cubes, in their order
			// The probes beyond the view's limits, and the reflection probes whose cube's format differs from the first's.
			uint32_t DroppedVolumes = 0;
			uint32_t DroppedReflectionProbes = 0;
			uint32_t MismatchedReflectionProbes = 0;
			// M45, with `relight` (PrepareProbeShading): each of Volumes' relight block (null for a volume baked without one), in
			// their order, and each of Constants' reflection probes' relight entry (x -1 outside every relit volume); without it,
			// Relights is all null.
			std::vector<nvrhi::BufferHandle> Relights{};
			std::array<glm::vec4, MaxViewReflectionProbes> ReflectionRelights{};

			// Whether a volume of Volumes is relit (a non-null entry of Relights).
			[[nodiscard]] bool HasRelight() const;
		};

		// The view's probes in `snapshot` seen by `camera` (see the file comment), resolving their data through `cache` (which
		// may upload: the caller holds no mirror reference across it). Empty (Counts 0, no mirror resolved) when `enabled` is
		// false: no camera, a view other than Lit, the wireframe view, or RenderQualitySettings::Probes false. M45: `relight`, a
		// Sky lights the view, so its relightable volumes and its reflection probes take their relight inputs.
		[[nodiscard]] ProbeShadingInputs PrepareProbeShading(GpuResourceCache& cache, const RenderSnapshot& snapshot, const CameraData& camera, bool enabled,
			bool relight);

		// The view's copies of its probes' mirrors (place 2), each null until a render needs it: the ProbeConstants buffer (b4),
		// the volumes' coefficients one after another (t13, a structured buffer of float4 grown to the view's count) and the
		// reflection probes' cubes (t14, a cube array of their cubes' format, size and mip count, grown to the view's count),
		// with the ids of the data the last copies took, in their order, so a render whose probes did not change copies nothing.
		// Ids, never the mirrors' handles: the view holds no mirror, so a mirror the cache releases (CollectStale, TrimUnused,
		// a scene unloaded) is freed whatever the view last drew (W26.1's review). Released with the renderer's state on a
		// device loss, and created again by the next render. M45: the coefficients are UAV-capable, HoldsRelight says that the
		// relightable volumes' copies hold the ProbeRelight row's relight rather than their bakes, and RelitIntensity is the
		// Sky's Intensity that relight took (the sky's lighting key leaves it out, the finalize applying it every frame).
		struct ProbeViewResources
		{
			nvrhi::BufferHandle Constants{};
			nvrhi::BufferHandle Coefficients{};
			nvrhi::TextureHandle Cubes{};
			std::vector<ProbeMirrorId> CopiedVolumes{};
			std::vector<ProbeMirrorId> CopiedCubes{};
			bool HoldsRelight = false;
			float RelitIntensity = 0.0f;
		};

		// Creates what `inputs` needs of `resources` on `device`: the constants buffer, and the coefficient buffer or the cube
		// array when they are missing or too small (a new one forgets its copies). Errors: Gpu (or OutOfMemory) for a creation
		// that failed, after which `inputs` uses no probe of the kind whose resource is missing (its count and list cleared),
		// so the render goes on with the Environment alone.
		[[nodiscard]] Status EnsureProbeViewResources(GraphicsDevice& device, ProbeViewResources& resources, ProbeShadingInputs& inputs);

		// Records `inputs`' upload into `resources` (EnsureProbeViewResources first): the constants, and the mirrors' copies
		// when they differ from the last copies (every face level of every cube, the volumes' coefficients at their offsets),
		// or, M45, when the copies hold a relight that `inputs` does not ask for (their bakes again; any copy of the volumes
		// clears HoldsRelight). Nothing without the constants buffer.
		void RecordProbeUploads(nvrhi::ICommandList& commandList, ProbeViewResources& resources, const ProbeShadingInputs& inputs);

		// The forward set's neutral probe resources (startup objects of the pipeline set, created by one command list executed
		// at once): t13's one zero float4 and t14's one 1 x 1 cube of zeros, bound while the view has no volume or no
		// reflection probe (the shader reads them only below ProbeConstants' counts).
		[[nodiscard]] Result<nvrhi::BufferHandle> CreateEmptyIrradianceProbes(GraphicsDevice& device);
		[[nodiscard]] Result<nvrhi::TextureHandle> CreateEmptyReflectionProbeCubes(GraphicsDevice& device);

	}

}
