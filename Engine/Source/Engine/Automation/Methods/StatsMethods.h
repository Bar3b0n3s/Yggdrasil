#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"

#include <cstdint>
#include <string>
#include <vector>

namespace Engine {

	class AutomationMethodContext;
	class MethodRegistry;
	class TypeRegistry;
	struct RenderStats;

	struct StatsGetParams
	{
	};

	// Approximate diagnostic telemetry, never simulation state. Converts nonnegative observations to finite float:
	// NaN and negative values become zero; positive overflow (including infinity) saturates to float's maximum.
	// Hosts use this before narrowing their double observations or byte counters. Frame identities stay exact strings.
	[[nodiscard]] float ToStatsTelemetry(double value);

	struct StatsPassSummary
	{
		std::string Name{};
		float CpuMilliseconds = 0.0f;
		float GpuMilliseconds = 0.0f;
		bool GpuAvailable = false;
		uint32_t DrawCalls = 0;
		uint32_t Dispatches = 0;
		uint32_t Triangles = 0;
	};

	struct StatsViewSummary
	{
		std::string Name{}; // "scene" or "game"; never the transient screenshot view
		bool Available = false;
		std::string Frame{}; // exact decimal uint64, not rounded through JSON's floating number
		std::string GpuFrame{};
		bool GpuAvailable = false;
		uint32_t Width = 0;
		uint32_t Height = 0;
		float CpuMilliseconds = 0.0f;
		std::vector<StatsPassSummary> Passes{};
		uint32_t ShadowDraws = 0;
		uint32_t ShadowedSpotLights = 0;
		uint32_t DroppedSpotShadows = 0;
	};

	struct StatsGetResult
	{
		float Fps = 0.0f;
		float CpuMilliseconds = 0.0f;
		float DroppedSeconds = 0.0f; // accumulated scheduler dropped time; reset with the session
		uint32_t Entities = 0;
		uint32_t Bodies = 0;
		uint32_t Voices = 0;
		bool ScriptAvailable = false; // M13 supplies heap usage; false is not "zero memory used"
		float ScriptHeapBytes = 0.0f;
		float ScriptSoftLimitBytes = 0.0f;
		float ScriptHardLimitBytes = 0.0f;
		uint32_t MemoryAllocationCount = 0;
		uint32_t MaxMemoryAllocationCount = 0;
		std::vector<StatsViewSummary> Views{}; // scene then game; Runtime game only
	};

	// Owned diagnostic copy of one displayed renderer's history, preserving both CPU and delayed GPU identities.
	// No rendering, waits, asset access or mutation. An unavailable history produces only the supplied view name.
	[[nodiscard]] StatsViewSummary MakeStatsViewSummary(std::string name, const RenderStats& stats);

	namespace Automation {

		// Copies host observations only: no GPU wait, render, frame advance, cache eviction, or simulation mutation.
		// Main thread. CPU/body/voice data remains available with renderer none; view.Available/GpuAvailable report absent
		// samples explicitly. InvalidState without an open project/host; Unsupported only for a host lacking the service.
		[[nodiscard]] Result<StatsGetResult> StatsGet(AutomationMethodContext& context, const StatsGetParams& params);

	}

	void RegisterStatsMethodTypes(TypeRegistry& registry);
	// stats.get: read-only, AllowedInBatch, SupportsDryRun, Runtime subset, not launcher, not MCP tool.
	void RegisterStatsMethods(MethodRegistry& methods);

}
