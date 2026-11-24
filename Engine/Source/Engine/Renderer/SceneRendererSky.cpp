#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Renderer/Private/SceneRendererState.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SkyLighting.h"
#include "Engine/Renderer/SkyPass.h"

#include <glm/glm.hpp>

#include <array>
#include <cstddef>
#include <optional>
#include <utility>

// The scene renderer's sky (M45, Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Renderer/SkyLighting.h,
// Renderer/SkyPass.h), as M42's SceneRendererReflections.cpp holds the planar reflections': the Prepare stage's sky inputs
// (PrepareSky), the Ambient stage's SkyLighting rows (RecordSkyLighting, its ProbeRelight row in RecordProbeRelight), the Sky
// pass of a view (RecordSky) and the relight capture's two accessors (GetSceneDepthTexture, GetSkyRadianceTotals).

namespace Engine {

	void SceneRenderer::State::PrepareSky(Detail::SceneRendererFrame& frame, const RenderSnapshot& snapshot, const CameraData& camera, bool lit)
	{
		frame.Sky = false;
		frame.SkyLutUpdate = false;
		frame.SkyUpdate = false;
		frame.SkyUpdates = 0;
		frame.RelitIrradianceVolumes = 0;
		if (!lit || !snapshot.Sky.Enabled)
			return;
		// Created on the view's first render with a Sky. A creation that fails (out of memory) leaves the view lit by its
		// Environment as without a Sky, and the render returns the error, for which the host trims and renders again (M18
		// decision 44).
		if (!SkyResources.has_value())
		{
			Result<SkyViewResources> created = CreateSkyViewResources(*Device);
			if (!created)
			{
				frame.KeepFirstError(std::unexpected(std::move(created).error()));
				return;
			}
			SkyResources = std::move(*created);
		}
		frame.Sky = true;
		frame.SkyView = MakeSkyConstants(snapshot.Sky, snapshot.Lights, camera, snapshot.Fog);
		frame.SkyLuts = SkyLutKey{ .Haze = snapshot.Sky.Haze, .Overcast = snapshot.Sky.Overcast, .GroundAlbedo = snapshot.Sky.GroundAlbedo };
		frame.SkyLighting = MakeSkyLightingKey(frame.SkyView, snapshot.Sky);
		frame.SkyLutUpdate = SkyResources->LutKey != frame.SkyLuts;
		frame.SkyUpdate = frame.SkyLutUpdate || SkyResources->LightingKey != frame.SkyLighting;
	}

	void SceneRenderer::State::RecordSkyLighting(Detail::SceneRendererFrame& frame)
	{
		if (!frame.Sky)
			return;
		nvrhi::ICommandList& commandList = *frame.CommandList;
		RenderRecordingContext& recording = *frame.Recording;
		SkyViewResources& resources = *SkyResources;
		SkyLightingPass& pass = *Pipelines->SkyLighting;
		commandList.writeBuffer(resources.Constants, &frame.SkyView, sizeof(frame.SkyView));

		recording.BeginPass("SkyLuts", &commandList);
		Result<uint32_t> luts = pass.RecordLuts(commandList, SkyLightingBindings, resources, frame.SkyLutUpdate, frame.SkyView.CameraAltitude);
		recording.EndPass({ .Dispatches = luts ? *luts : 0U });
		if (!luts)
		{
			frame.KeepFirstError(std::unexpected(std::move(luts).error()));
			resources.LutKey.reset();
			resources.LightingKey.reset();
			return;
		}
		resources.LutKey = frame.SkyLuts;

		recording.BeginPass("SkyIbl", &commandList);
		uint32_t dispatches = 0;
		Status recorded = {};
		if (frame.SkyUpdate)
		{
			Result<uint32_t> update = pass.RecordImageBasedLight(commandList, SkyLightingBindings, resources, frame.SkyLighting.CloudOffset);
			if (update)
			{
				dispatches += *update;
				resources.LightingKey = frame.SkyLighting;
				frame.SkyUpdates = 1;
			}
			else
			{
				recorded = std::unexpected(std::move(update).error());
				resources.LightingKey.reset();
			}
		}
		if (recorded)
		{
			recorded = pass.RecordFinalize(commandList, SkyLightingBindings, resources);
			if (recorded)
			{
				++dispatches;
				// The view's fog and each drawn mirror slot's; the reflection probes' scales follow in the ProbeRelight row.
				std::array<nvrhi::IBuffer*, 1 + MaxPlanarReflectionsPerView> fogs{};
				size_t fogCount = 0;
				fogs[fogCount++] = FogConstantsBuffer.Get();
				for (uint32_t slot = 0; slot < frame.PlanarReflectionCount; ++slot)
					fogs[fogCount++] = ReflectionSlots[slot].FogConstantsBuffer.Get();
				RecordSkyResultCopies(commandList, resources.Results, EnvironmentConstantsBuffer, std::span<nvrhi::IBuffer* const>(fogs.data(), fogCount), nullptr, 0);
			}
		}
		recording.EndPass({ .Dispatches = dispatches });
		frame.KeepFirstError(recorded);
		if (recorded)
			RecordProbeRelight(frame);
	}

	void SceneRenderer::State::RecordProbeRelight(Detail::SceneRendererFrame& frame)
	{
		// W45.3 (decision 3, "Relit probes"): the view's relightable volumes, relit when the sky's light was regenerated this
		// render, its Intensity changed, or the view's copy holds their bakes (copied again, or never relit), then every frame
		// the reflection probes' relit intensities and their copies over ProbeConstants; nothing without a relightable volume in
		// the view.
		const Utils::ProbeShadingInputs& probes = frame.Probes;
		if (!probes.HasRelight() || ProbeResources.Coefficients == nullptr || ProbeResources.Constants == nullptr)
			return;
		nvrhi::ICommandList& commandList = *frame.CommandList;
		SkyViewResources& resources = *SkyResources;
		SkyLightingPass& pass = *Pipelines->SkyLighting;
		const bool relight = frame.SkyUpdates != 0 || !ProbeResources.HoldsRelight || ProbeResources.RelitIntensity != frame.SkyView.Intensity;
		const uint32_t reflections = probes.Constants.Counts.y;
		if (!relight && reflections == 0)
			return;
		frame.Recording->BeginPass("ProbeRelight", &commandList);
		uint32_t dispatches = 0;
		Status recorded = {};
		if (relight)
		{
			for (size_t index = 0; index < probes.Relights.size() && recorded; ++index)
			{
				if (probes.Relights[index] == nullptr)
					continue;
				const glm::uvec4& counts = probes.Constants.Volumes[index].Probes;
				recorded = pass.RecordProbeRelight(commandList, SkyLightingBindings, resources, *probes.Relights[index], *ProbeResources.Coefficients, counts.w,
					counts.x * counts.y * counts.z);
				if (recorded)
				{
					++dispatches;
					++frame.RelitIrradianceVolumes;
				}
			}
			ProbeResources.HoldsRelight = recorded.has_value();
			ProbeResources.RelitIntensity = frame.SkyView.Intensity;
		}
		if (recorded && reflections > 0)
		{
			commandList.writeBuffer(resources.ReflectionRelights, probes.ReflectionRelights.data(), static_cast<size_t>(reflections) * sizeof(glm::vec4));
			recorded = pass.RecordReflectionRelight(commandList, SkyLightingBindings, resources, *ProbeResources.Coefficients, reflections);
			if (recorded)
			{
				++dispatches;
				RecordSkyResultCopies(commandList, resources.Results, nullptr, {}, ProbeResources.Constants, reflections);
			}
		}
		frame.Recording->EndPass({ .Dispatches = dispatches });
		frame.KeepFirstError(recorded);
	}

	nvrhi::ITexture* SceneRenderer::GetSceneDepthTexture() const
	{
		return m_State->ImageGeneration != 0 ? m_State->Targets.SceneDepth.Get() : nullptr;
	}

	nvrhi::IBuffer* SceneRenderer::GetSkyRadianceTotals() const
	{
		return m_State->SkyResources.has_value() ? m_State->SkyResources->Totals.Get() : nullptr;
	}

	Status SceneRenderer::State::RecordSky(nvrhi::ICommandList& commandList, nvrhi::IFramebuffer& framebuffer, nvrhi::IBuffer& viewConstants, nvrhi::IBuffer& fogConstants,
		const std::optional<nvrhi::Rect>& scissor)
	{
		return Pipelines->Sky->Record(commandList, SkyBindings,
			{ .Framebuffer = &framebuffer,
				.ViewConstants = &viewConstants,
				.SkyConstants = SkyResources->Constants,
				.FogConstants = &fogConstants,
				.Transmittance = SkyResources->Transmittance,
				.SkyView = SkyResources->SkyView,
				.Scissor = scissor });
	}

}
