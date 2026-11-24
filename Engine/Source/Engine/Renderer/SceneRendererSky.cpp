#include "EnginePCH.h"
#include "Engine/Renderer/SceneRenderer.h"

#include "Engine/Renderer/Private/SceneRendererState.h"
#include "Engine/Renderer/RenderStats.h"
#include "Engine/Renderer/SkyLighting.h"
#include "Engine/Renderer/SkyPass.h"

#include <array>
#include <cstddef>
#include <optional>
#include <utility>

// The scene renderer's sky (M45, Docs/Decisions/0063-m45-sky-and-time-of-day.md decision 3; Renderer/SkyLighting.h,
// Renderer/SkyPass.h), as M42's SceneRendererReflections.cpp holds the planar reflections': the Prepare stage's sky inputs
// (PrepareSky), the Ambient stage's SkyLighting rows (RecordSkyLighting) and the Sky pass of a view (RecordSky).

namespace Engine {

	void SceneRenderer::State::PrepareSky(Detail::SceneRendererFrame& frame, const RenderSnapshot& snapshot, const CameraData& camera, bool lit)
	{
		frame.Sky = false;
		frame.SkyLutUpdate = false;
		frame.SkyUpdate = false;
		frame.SkyUpdates = 0;
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
				// The view's fog and each drawn mirror slot's; the reflection probes' scales join with W45.3's relight.
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
