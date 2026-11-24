#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The Water pass's shared structs (M47, Docs/Decisions/0065-m47-water.md decision 3 and the W47.2 record; Renderer/WaterPass.h,
	// Renderer/WaterGeometry.h, Passes/Water.slang, Common/Waves.slang): one WaterRecord per water of the view's snapshot, in
	// snapshot order, in the view's records (t0 of the water's set 1, a structured buffer), and one WaterDrawConstants per draw
	// of a clipmap level (the push constants, at the mesh pipelines' DrawConstants slot and size, so the surface pipelines bind
	// the mesh forward passes' set 0 as it is).
	//
	// The clipmap (Renderer/WaterGeometry.h): WaterLevelCount levels of one WaterGridCells-cell square grid each, level 0's
	// cells WaterFinestCellSize metres and each later level's twice the one before, every level snapped to twice its own cell
	// around the camera, then the skirt out to the horizon. Every position the shaders compute is relative to the record's
	// Origin, the world (x, z) of level 0's corner, a multiple of twice the finest cell, so a vertex's offset from it is exact in
	// float.
	//
	// WaterRecord: Waves[i] the world unit direction (x, z), wave number k and amplitude A of wave i (below WaveCount), and
	// WavePhases[i] its steepness Q, its phase at Origin in this frame (k dot(D, Origin - water origin) + the snapshot's phase,
	// wrapped into [0, 2 pi) in double on the CPU) and in the previous frame (the same with the history's previous phase), so a
	// rest point x0 has the phase WavePhases[i].y + k dot(D, x0 - Origin) (Common/Waves.slang). BaseHeight is the base plane's
	// world height. CameraOffset is the camera's world (x, z) minus Origin, from which the waves fade with distance
	// (WaterFadeCellsPerWave). For the motion vectors: with WaterRecordFlagAdvanced a previous phase differs from this frame's,
	// so the shaders evaluate the previous frame's waves too; with WaterRecordFlagMoved, PreviousFromCurrent maps this frame's
	// world points of the water to the previous frame's (the history's PreviousWorld times the inverse of World). Without
	// either, a point's previous position is its current one, exactly. A Rectangle (WaterRecordFlagRectangle) is centred on FrameOrigin (its entity's world (x, z) minus
	// Origin) with its local X along the unit world (x, z) AxisX and its local Z a quarter turn from it, HalfExtents metres
	// along them, from Depth below its base to Headroom above it: its draws clamp their rest points into that footprint, and
	// an Ocean's fragments inside any Rectangle's volume write zero coverage. WaterCount is the snapshot's waters, the records
	// an Ocean's fragment tests. The look members are the snapshot item's (RenderSnapshot.h's WaterItem): ShallowColor (rgb)
	// and Clarity (a), DeepColor (rgb) and Roughness (a), Look (RefractionStrength, FoamAmount, ShoreFoamDepth,
	// DetailNormalStrength), Detail (DetailScale, DetailOffset, 0) and Underwater (UnderwaterColor, UnderwaterVisibility).
	// PickId is the picking pipeline's EntityId; PlanarSlot the view's planar reflection slot the water samples, or
	// WaterNoPlanarSlot.
#if defined(ENGINE_SHADER)
	static const uint MaxWaterRecordWaves = 8;
	static const uint WaterGridCells = 64;
	static const uint WaterLevelCount = 11;
	static const float WaterFinestCellSize = 0.25;
	static const float WaterLevelDistanceCells = 15.0;
	static const float WaterFadeCellsPerWave = 2.0;
	static const float WaterHorizonFadeEndCells = 30.0;
	static const float WaterHorizonFadeCells = 8.0;
	static const uint WaterRecordFlagRectangle = 1;
	static const uint WaterRecordFlagMoved = 2;
	static const uint WaterRecordFlagAdvanced = 4;
	static const uint WaterNoPlanarSlot = 0xFFFFFFFF;
#else
inline constexpr uint32_t MaxWaterRecordWaves = 8;
inline constexpr uint32_t WaterGridCells = 64;      // cells a side of every clipmap level
inline constexpr uint32_t WaterLevelCount = 11;     // level 0 (a whole grid) and 10 rings
inline constexpr float WaterFinestCellSize = 0.25f; // metres, level 0's cell
// The fade (Common/Waves.slang): at Chebyshev distance d from the camera a point lies in a level whose cell is at most
// max(WaterFinestCellSize, d / WaterLevelDistanceCells); a wave fades from full where its wavelength is twice
// WaterFadeCellsPerWave such cells to flat at WaterFadeCellsPerWave of them, and every wave fades to flat over the last
// WaterHorizonFadeCells of the last level's cells before WaterHorizonFadeEndCells of them, where the flat skirt begins.
inline constexpr float WaterLevelDistanceCells = 15.0f;
inline constexpr float WaterFadeCellsPerWave = 2.0f;
inline constexpr float WaterHorizonFadeEndCells = 30.0f;
inline constexpr float WaterHorizonFadeCells = 8.0f;
inline constexpr uint32_t WaterRecordFlagRectangle = 1; // a Rectangle: clamped to its footprint, masking the Ocean
inline constexpr uint32_t WaterRecordFlagMoved = 2;     // PreviousFromCurrent applies
inline constexpr uint32_t WaterRecordFlagAdvanced = 4;  // a previous phase differs from this frame's
inline constexpr uint32_t WaterNoPlanarSlot = 0xFFFFFFFFU;
#endif

	struct WaterRecord
	{
		Float4x4 PreviousFromCurrent;
		Float4 Waves[8];
		Float4 WavePhases[8];
		Float2 Origin;
		Float2 CameraOffset;
		Float2 FrameOrigin;
		Float2 AxisX;
		Float2 HalfExtents;
		float Depth = 0.0f;
		float Headroom = 0.0f;
		float BaseHeight = 0.0f;
		uint32_t WaveCount = 0;
		uint32_t WaterCount = 0;
		uint32_t Flags = 0;
		Float4 ShallowColor;
		Float4 DeepColor;
		Float4 Look;
		Float4 Detail;
		Float4 Underwater;
		uint32_t PickId = 0;
		uint32_t PlanarSlot = 0xFFFFFFFFU;
		uint32_t Padding0 = 0;
		uint32_t Padding1 = 0;
	};

	// The per-draw push constants: the level's corner (its cell (0, 0)) minus the record's Origin, its cell size, and the
	// record's index.
	struct WaterDrawConstants
	{
		Float2 LevelOrigin;
		float CellSize = 0.0f;
		uint32_t Water = 0;
	};

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(WaterRecord) == 480, "WaterRecord must match its 480-byte structured-buffer layout");
	static_assert(offsetof(WaterRecord, Waves) == 64 && offsetof(WaterRecord, WavePhases) == 192 && offsetof(WaterRecord, Origin) == 320
			&& offsetof(WaterRecord, Depth) == 360 && offsetof(WaterRecord, ShallowColor) == 384 && offsetof(WaterRecord, PickId) == 464,
		"WaterRecord members must sit where the structured-buffer layout puts them");
	static_assert(sizeof(WaterDrawConstants) == 16, "WaterDrawConstants must match the 16-byte push-constant range of DrawConstants");

}
#endif
