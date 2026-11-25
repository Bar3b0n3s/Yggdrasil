#include "EnginePCH.h"
#include "Engine/Renderer/MeshWind.h"

#include <algorithm>
#include <cmath>

namespace Engine {

	namespace Utils {

		// frac(frequency x time) in [0, 1) as a float: computed in double precision, and 0 where the float rounds up to 1.
		[[nodiscard]] static float WrapPhase(double frequency, double time)
		{
			const double cycles = frequency * time;
			const auto phase = static_cast<float>(cycles - std::floor(cycles));
			return phase < 1.0f ? phase : 0.0f;
		}

		// `value` clamped to [0, range] and rounded to unorm8 over the range.
		[[nodiscard]] static uint32_t ToUnorm8(float value, float range)
		{
			const float clamped = std::isfinite(value) ? std::clamp(value, 0.0f, range) : 0.0f;
			return static_cast<uint32_t>(std::lround(clamped / range * 255.0f));
		}

	}

	WindFrame MakeWindFrame(const RenderWind& wind)
	{
		WindFrame frame;
		if (!wind.Enabled)
			return frame;
		const double x = wind.Direction.x;
		const double z = wind.Direction.z;
		const double length = std::sqrt(x * x + z * z);
		if (length > 0.0 && std::isfinite(length))
			frame.Direction = glm::vec2(static_cast<float>(x / length), static_cast<float>(z / length));
		frame.Strength = wind.Strength;
		frame.GustStrength = wind.GustStrength;
		frame.Turbulence = wind.Turbulence;
		const double frequency = wind.GustFrequency;
		if (wind.Strength > 0.0f && wind.GustFrequency > 0.0f)
			frame.InverseGustWavelength = static_cast<float>(frequency / static_cast<double>(wind.Strength));
		const double time = std::isfinite(wind.Time) ? wind.Time : 0.0;
		frame.GustPhase = Utils::WrapPhase(frequency, time);
		frame.HalfGustPhase = Utils::WrapPhase(frequency * 0.5, time);
		frame.BranchPhase = Utils::WrapPhase(WindBranchFrequency, time);
		frame.FlutterPhase = Utils::WrapPhase(WindFlutterFrequency, time);
		return frame;
	}

	WindConstants MakeWindConstants(const WindFrame& current, const WindFrame& previous)
	{
		WindConstants constants;
		constants.Direction = current.Direction;
		constants.Strength = current.Strength;
		constants.GustStrength = current.GustStrength;
		constants.Turbulence = current.Turbulence;
		constants.InverseGustWavelength = current.InverseGustWavelength;
		constants.GustPhase = current.GustPhase;
		constants.HalfGustPhase = current.HalfGustPhase;
		constants.BranchPhase = current.BranchPhase;
		constants.FlutterPhase = current.FlutterPhase;
		constants.PreviousDirection = previous.Direction;
		constants.PreviousStrength = previous.Strength;
		constants.PreviousGustStrength = previous.GustStrength;
		constants.PreviousTurbulence = previous.Turbulence;
		constants.PreviousInverseGustWavelength = previous.InverseGustWavelength;
		constants.PreviousGustPhase = previous.GustPhase;
		constants.PreviousHalfGustPhase = previous.HalfGustPhase;
		constants.PreviousBranchPhase = previous.BranchPhase;
		constants.PreviousFlutterPhase = previous.FlutterPhase;
		return constants;
	}

	uint32_t PackWindResponse(const MaterialWind& wind)
	{
		return Utils::ToUnorm8(wind.Bend, 1.0f) | (Utils::ToUnorm8(wind.Branch, 1.0f) << 8) | (Utils::ToUnorm8(wind.Flutter, WindFlutterRange) << 16)
			| (Utils::ToUnorm8(wind.Stiffness, 1.0f) << 24);
	}

}
