#pragma once

#include "Engine/Core/Base.h"

#include <nvrhi/nvrhi.h>

#include <array>
#include <cstdint>

// What Application::OnRender receives (Architecture §4.1, §8.2): one frame's open command list and its final target,
// which is the acquired swapchain image in a windowed process and the offscreen target in a headless one (§8.13). The
// application records into the command list; the frame executes it, records ImGui (Application::OnImGuiRender) into the
// same target, submits and presents after OnRender returns.

namespace Engine {

	class GpuProfiler;
	class GraphicsDevice;

	// The colour Application clears every frame's final target to before OnRender, RGBA as stored in the UNORM target
	// (display-encoded values, §8.9; a clear writes them unconverted). The editor screenshot clears its target to the same
	// colour, so it shows what the window shows (ImGui/ImGuiScreenshot.h).
	inline constexpr std::array<float, 4> FrameClearColor = { 0.0f, 0.0f, 0.0f, 1.0f };

	// Plain per-frame data; every pointer is a documented back-reference valid only during the OnRender call.
	struct RenderContext
	{
		GraphicsDevice* Device = nullptr;
		// Open; cleared to FrameClearColor already. Never closed or executed by the application.
		nvrhi::ICommandList* CommandList = nullptr;
		// The frame's final target: one color attachment (BGRA8_UNORM or RGBA8_UNORM, holding display-encoded values, §8.9).
		nvrhi::IFramebuffer* Framebuffer = nullptr;
		nvrhi::ITexture* Target = nullptr;
		uint32_t Width = 0;
		uint32_t Height = 0;
		// FramePacer::GetFrameSlot and GetFrameIndex.
		uint32_t FrameSlot = 0;
		uint64_t FrameIndex = 0;
		// The application's GpuProfiler, already in this frame (GpuProfiler::BeginFrame ran): passes wrap their work in
		// GpuProfileScope (§8.2), and the frame's ImGui pass is timed by the same profiler. Never null during OnRender.
		GpuProfiler* Profiler = nullptr;
	};

}
