#pragma once

#include "Shared/ShaderTypes.h"

#if !defined(ENGINE_SHADER)
	#include <cstddef>
	#include <cstdint>

namespace Engine {

#endif

	// The view's wind (M46, Docs/Decisions/0064-m46-mesh-lods-draw-distance-and-wind.md decision 3G; Common/Wind.slang): one
	// constant buffer per renderer, written once per render by the scene renderer from the snapshot's RenderWind
	// (MakeWindConstants, Renderer/MeshWind.h) and bound at b8 of set 0 in every mesh layout: the Scene program's
	// prepass, overdraw and forward layouts (the Terrain program declares it unread, since it shares the forward layout),
	// both Shadow variants and SelectionMask. Every vertex entry that draws meshes reads it for an instance with
	// DrawFlagWind (Shared/DrawConstants.h), whose record's WindResponse holds its material's wind block.
	// Each half holds one render's wind: the current one, and the previous render's for the prepass's motion vectors (equal
	// to the current after a history reset). The waves' phases are computed on the CPU in double precision and wrapped into
	// [0, 1), so no single-precision time loses precision in a long session:
	//   - Direction: the wind's horizontal direction (world X and Z), unit; (1, 0) without one;
	//   - Strength, GustStrength (m/s) and Turbulence: the Wind's;
	//   - InverseGustWavelength: GustFrequency / Strength per metre (the gusts travel with the wind), 0 when either is 0;
	//   - GustPhase: frac(GustFrequency x Time); HalfGustPhase: frac(GustFrequency x Time / 2), the turbulence's slower wave;
	//   - BranchPhase: frac(WindBranchFrequency x Time); FlutterPhase: frac(WindFlutterFrequency x Time).
	// Without a Wind every member is 0 and Direction (1, 0), so nothing moves; no draw sets DrawFlagWind then anyway.
	struct WindConstants
	{
		Float2 Direction = Float2(1.0f, 0.0f);
		float Strength = 0.0f;
		float GustStrength = 0.0f;
		float Turbulence = 0.0f;
		float InverseGustWavelength = 0.0f;
		float GustPhase = 0.0f;
		float HalfGustPhase = 0.0f;
		float BranchPhase = 0.0f;
		float FlutterPhase = 0.0f;
		uint32_t Padding0 = 0;
		uint32_t Padding1 = 0;
		Float2 PreviousDirection = Float2(1.0f, 0.0f);
		float PreviousStrength = 0.0f;
		float PreviousGustStrength = 0.0f;
		float PreviousTurbulence = 0.0f;
		float PreviousInverseGustWavelength = 0.0f;
		float PreviousGustPhase = 0.0f;
		float PreviousHalfGustPhase = 0.0f;
		float PreviousBranchPhase = 0.0f;
		float PreviousFlutterPhase = 0.0f;
		uint32_t Padding2 = 0;
		uint32_t Padding3 = 0;
	};

	// The fixed frequencies of the branches' sway and the leaves' flutter, in Hz (decision 3G).
#if defined(ENGINE_SHADER)
	static const float WindBranchFrequency = 0.4;
	static const float WindFlutterFrequency = 2.5;
#else
inline constexpr double WindBranchFrequency = 0.4;
inline constexpr double WindFlutterFrequency = 2.5;
#endif

	// The ranges InstanceRecord::WindResponse's four unorm8 bytes span (MaterialWind's, Asset/MaterialData.h): byte 0 Bend
	// over [0, 1], byte 1 Branch over [0, 1] m, byte 2 Flutter over [0, 0.25] m and byte 3 Stiffness over [0, 1].
#if defined(ENGINE_SHADER)
	static const float WindFlutterRange = 0.25;
#else
inline constexpr float WindFlutterRange = 0.25f;
#endif

#if !defined(ENGINE_SHADER)
	static_assert(sizeof(WindConstants) == 96, "WindConstants must match its 96-byte constant-buffer layout");
	static_assert(offsetof(WindConstants, Strength) == 8 && offsetof(WindConstants, Turbulence) == 16 && offsetof(WindConstants, GustPhase) == 24
			&& offsetof(WindConstants, BranchPhase) == 32 && offsetof(WindConstants, PreviousDirection) == 48
			&& offsetof(WindConstants, PreviousStrength) == 56 && offsetof(WindConstants, PreviousTurbulence) == 64
			&& offsetof(WindConstants, PreviousGustPhase) == 72 && offsetof(WindConstants, PreviousBranchPhase) == 80,
		"WindConstants members must sit where the constant-buffer layout puts them");

}
#endif
