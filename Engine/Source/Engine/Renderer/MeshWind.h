#pragma once

#include "Engine/Asset/MaterialData.h"
#include "Engine/Core/Base.h"
#include "Engine/Renderer/RenderSnapshot.h"
#include "Shared/WindConstants.h"

#include <glm/glm.hpp>

#include <cstdint>

// The wind's GPU data (Architecture §8.3 "Wind"; M46, Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision
// 3G): the view's WindConstants from the snapshot's RenderWind, and an instance record's WindResponse from its material's
// wind block. Pure; the scene renderer calls them once per render and once per wind draw.

namespace Engine {

	// One render's half of WindConstants (Shared/WindConstants.h), as MakeWindFrame computes it.
	struct WindFrame
	{
		glm::vec2 Direction = glm::vec2(1.0f, 0.0f);
		float Strength = 0.0f;
		float GustStrength = 0.0f;
		float Turbulence = 0.0f;
		float InverseGustWavelength = 0.0f;
		float GustPhase = 0.0f;
		float HalfGustPhase = 0.0f;
		float BranchPhase = 0.0f;
		float FlutterPhase = 0.0f;

		bool operator==(const WindFrame&) const = default;
	};

	// The wind of `wind` at its Time: Direction's X and Z normalized in double precision ((1, 0) without them), Strength,
	// GustStrength and Turbulence, GustFrequency / Strength per metre (0 when either is 0), and the gust's, the half-rate
	// gust's, the branches' (WindBranchFrequency) and the flutter's (WindFlutterFrequency) phases frac(frequency x Time) in
	// [0, 1), computed in double precision. A disabled wind gives the zero frame (Direction (1, 0), every other member 0),
	// which moves nothing. Pure.
	[[nodiscard]] WindFrame MakeWindFrame(const RenderWind& wind);

	// The view's WindConstants: `current`, and `previous` (the previous render's frame; the current one after a history
	// reset) for the prepass's motion vectors. Pure.
	[[nodiscard]] WindConstants MakeWindConstants(const WindFrame& current, const WindFrame& previous);

	// InstanceRecord::WindResponse of a draw whose material's wind block is `wind`: Bend, Branch, Flutter and Stiffness, each
	// clamped to its range ([0, 1], [0, 1] m, [0, WindFlutterRange] m and [0, 1]) and rounded to unorm8 over it, in bytes 0 to
	// 3. Pure.
	[[nodiscard]] uint32_t PackWindResponse(const MaterialWind& wind);

}
