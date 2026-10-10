#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Core/DetMath.h"
#include "Engine/Reflection/Value.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/Private/ScriptCall.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include <lua.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>
#include <utility>

namespace Engine {

	namespace Utils {

		static bool IsMathVector(ScriptCall& call, int index)
		{
			return lua_type(call.State, index) == LUA_TVECTOR;
		}

		static float MathFloat(ScriptCall& call, double value)
		{
			if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
				Lua::RaiseError(call, "result is outside the finite vector range");
			return static_cast<float>(value);
		}

		static double MathClampedLerp(double from, double to, double amount)
		{
			if (amount <= 0.0)
				return from;
			if (amount >= 1.0)
				return to;
			// Fixed operation order, independent of the host STL's lerp implementation. Opposite-sign endpoints need
			// the weighted sum: their difference can overflow even when every point on the interval is representable.
			if ((from <= 0.0 && to >= 0.0) || (from >= 0.0 && to <= 0.0))
				return from * (1.0 - amount) + to * amount;
			return from + (to - from) * amount;
		}

		static int MathClamp(ScriptCall& call)
		{
			if (IsMathVector(call, 1))
			{
				const glm::vec3 value = Lua::Check<glm::vec3>(call, 1);
				const glm::vec3 minimum = Lua::Check<glm::vec3>(call, 2);
				const glm::vec3 maximum = Lua::Check<glm::vec3>(call, 3);
				if (minimum.x > maximum.x || minimum.y > maximum.y || minimum.z > maximum.z)
					return Lua::RaiseError(call, "minimum must not exceed maximum on any axis");
				Lua::Push(call, glm::clamp(value, minimum, maximum));
			}
			else
			{
				const double value = Lua::Check<double>(call, 1);
				const double minimum = Lua::Check<double>(call, 2);
				const double maximum = Lua::Check<double>(call, 3);
				if (minimum > maximum)
					return Lua::RaiseError(call, "minimum must not exceed maximum");
				Lua::Push(call, std::clamp(value, minimum, maximum));
			}
			return 1;
		}

		static int MathClamp01(ScriptCall& call)
		{
			if (IsMathVector(call, 1))
				Lua::Push(call, glm::clamp(Lua::Check<glm::vec3>(call, 1), glm::vec3(0.0f), glm::vec3(1.0f)));
			else
				Lua::Push(call, std::clamp(Lua::Check<double>(call, 1), 0.0, 1.0));
			return 1;
		}

		static int MathLerp(ScriptCall& call)
		{
			const double amount = Lua::Check<double>(call, 3);
			if (IsMathVector(call, 1))
			{
				const glm::vec3 from = Lua::Check<glm::vec3>(call, 1);
				const glm::vec3 to = Lua::Check<glm::vec3>(call, 2);
				glm::vec3 result(0.0f);
				for (int axis = 0; axis < 3; ++axis)
					result[axis] = MathFloat(call, MathClampedLerp(from[axis], to[axis], amount));
				Lua::Push(call, result);
			}
			else
			{
				const double from = Lua::Check<double>(call, 1);
				const double to = Lua::Check<double>(call, 2);
				Lua::Push(call, MathClampedLerp(from, to, amount));
			}
			return 1;
		}

		static double MathInverseLerp(double from, double to, double value)
		{
			if (from == to)
				return 0.0;
			if (from < to)
			{
				if (value <= from)
					return 0.0;
				if (value >= to)
					return 1.0;
			}
			else
			{
				if (value >= from)
					return 0.0;
				if (value <= to)
					return 1.0;
			}
			// Halving first keeps opposite, near-DBL_MAX endpoints from overflowing their difference.
			if (!std::isfinite(to - from))
				return (value * 0.5 - from * 0.5) / (to * 0.5 - from * 0.5);
			return (value - from) / (to - from);
		}

		static int MathInverseLerpBinding(ScriptCall& call)
		{
			const double from = Lua::Check<double>(call, 1);
			const double to = Lua::Check<double>(call, 2);
			const double value = Lua::Check<double>(call, 3);
			Lua::Push(call, MathInverseLerp(from, to, value));
			return 1;
		}

		static int MathRemap(ScriptCall& call)
		{
			const double value = Lua::Check<double>(call, 1);
			const double sourceMinimum = Lua::Check<double>(call, 2);
			const double sourceMaximum = Lua::Check<double>(call, 3);
			const double targetMinimum = Lua::Check<double>(call, 4);
			const double targetMaximum = Lua::Check<double>(call, 5);
			if (sourceMinimum == sourceMaximum)
				return Lua::RaiseError(call, "source interval must have nonzero length");
			Lua::Push(call, MathClampedLerp(targetMinimum, targetMaximum, MathInverseLerp(sourceMinimum, sourceMaximum, value)));
			return 1;
		}

		static int MathSmoothStep(ScriptCall& call)
		{
			const double from = Lua::Check<double>(call, 1);
			const double to = Lua::Check<double>(call, 2);
			const double amount = std::clamp(Lua::Check<double>(call, 3), 0.0, 1.0);
			Lua::Push(call, MathClampedLerp(from, to, amount * amount * (3.0 - 2.0 * amount)));
			return 1;
		}

		static std::pair<double, double> MathDampedStep(ScriptCall& call, double current, double target,
			double velocity, double smoothTime, double deltaTime)
		{
			if (deltaTime == 0.0)
				return { current, velocity };
			// Exact critically damped spring solution. DetMath keeps the decay identical in every configuration.
			// The minimum time bounds stiffness; once decay underflows, the limiting solution is precisely at rest.
			const double frequency = 2.0 / std::max(smoothTime, 0.0001);
			const double elapsed = frequency * deltaTime;
			if (elapsed > 745.0)
				return { target, 0.0 };
			const double decay = DetMath::Exp(-elapsed);
			const double displacement = current - target;
			const double impulse = (velocity + frequency * displacement) * deltaTime;
			const double value = target + (displacement + impulse) * decay;
			const double nextVelocity = (velocity - frequency * impulse) * decay;
			if (!std::isfinite(value) || !std::isfinite(nextVelocity))
				Lua::RaiseError(call, "spring calculation exceeds the finite number range");
			if ((current < target && value > target) || (current > target && value < target))
				return { target, 0.0 };
			return { value, nextVelocity };
		}

		static int MathSmoothDamp(ScriptCall& call)
		{
			const double smoothTime = Lua::Check<double>(call, 4);
			const double deltaTime = Lua::Check<double>(call, 5);
			if (smoothTime <= 0.0 || deltaTime < 0.0)
				return Lua::RaiseError(call, "smoothTime must be positive and dt must be nonnegative");
			if (IsMathVector(call, 1))
			{
				const glm::vec3 current = Lua::Check<glm::vec3>(call, 1);
				const glm::vec3 target = Lua::Check<glm::vec3>(call, 2);
				const glm::vec3 velocity = Lua::Check<glm::vec3>(call, 3);
				glm::vec3 result(0.0f);
				glm::vec3 nextVelocity(0.0f);
				for (int axis = 0; axis < 3; ++axis)
				{
					const auto [value, speed] = MathDampedStep(call, current[axis], target[axis], velocity[axis], smoothTime, deltaTime);
					result[axis] = MathFloat(call, value);
					nextVelocity[axis] = MathFloat(call, speed);
				}
				Lua::Push(call, result);
				Lua::Push(call, nextVelocity);
			}
			else
			{
				const double current = Lua::Check<double>(call, 1);
				const double target = Lua::Check<double>(call, 2);
				const double velocity = Lua::Check<double>(call, 3);
				const auto [value, speed] = MathDampedStep(call, current, target, velocity, smoothTime, deltaTime);
				Lua::Push(call, value);
				Lua::Push(call, speed);
			}
			return 2;
		}

		static int MathMoveTowards(ScriptCall& call)
		{
			const double maximumDelta = Lua::Check<double>(call, 3);
			if (maximumDelta < 0.0)
				return Lua::RaiseError(call, "maxDelta must be nonnegative");
			if (IsMathVector(call, 1))
			{
				const glm::vec3 current = Lua::Check<glm::vec3>(call, 1);
				const glm::vec3 target = Lua::Check<glm::vec3>(call, 2);
				const glm::dvec3 difference = glm::dvec3(target) - glm::dvec3(current);
				const double distance = std::sqrt(glm::dot(difference, difference));
				if (distance <= maximumDelta || distance == 0.0)
					Lua::Push(call, target);
				else
				{
					glm::vec3 result(0.0f);
					for (int axis = 0; axis < 3; ++axis)
						result[axis] = MathFloat(call, MathClampedLerp(current[axis], target[axis], maximumDelta / distance));
					Lua::Push(call, result);
				}
			}
			else
			{
				const double current = Lua::Check<double>(call, 1);
				const double target = Lua::Check<double>(call, 2);
				if (std::abs(target - current) <= maximumDelta)
					Lua::Push(call, target);
				else
					Lua::Push(call, current + (target > current ? maximumDelta : -maximumDelta));
			}
			return 1;
		}

		static int MathApproximately(ScriptCall& call)
		{
			const double a = Lua::Check<double>(call, 1);
			const double b = Lua::Check<double>(call, 2);
			const double epsilon = Lua::IsNoneOrNil(call, 3) ? 0.000001 : Lua::Check<double>(call, 3);
			if (epsilon < 0.0)
				return Lua::RaiseError(call, "epsilon must be nonnegative");
			const double scale = std::max({ 1.0, std::abs(a), std::abs(b) });
			// Normalize first so the comparison never turns inf <= inf into an accidental success.
			Lua::Push(call, std::abs(a / scale - b / scale) <= epsilon);
			return 1;
		}

		static double MathRepeat(double value, double length)
		{
			// fmod avoids overflow of value / length for subnormal positive lengths.
			const double remainder = std::fmod(value, length);
			const double result = remainder < 0.0 ? remainder + length : remainder;
			return result >= length || result == 0.0 ? 0.0 : result;
		}

		static double MathDeltaAngle(double current, double target)
		{
			// Reduce each endpoint first: two individually finite angles may have an infinite difference.
			const double delta = MathRepeat(MathRepeat(target, 360.0) - MathRepeat(current, 360.0), 360.0);
			return delta > 180.0 ? delta - 360.0 : delta;
		}

		static int MathDeltaAngleBinding(ScriptCall& call)
		{
			const double current = Lua::Check<double>(call, 1);
			const double target = Lua::Check<double>(call, 2);
			Lua::Push(call, MathDeltaAngle(current, target));
			return 1;
		}

		static int MathLerpAngle(ScriptCall& call)
		{
			const double current = Lua::Check<double>(call, 1);
			const double target = Lua::Check<double>(call, 2);
			const double amount = std::clamp(Lua::Check<double>(call, 3), 0.0, 1.0);
			Lua::Push(call, current + MathDeltaAngle(current, target) * amount);
			return 1;
		}

		static int MathSign(ScriptCall& call)
		{
			const double value = Lua::Check<double>(call, 1);
			Lua::Push(call, value == 0.0 ? 0.0 : (value > 0.0 ? 1.0 : -1.0));
			return 1;
		}

		static int MathRepeatBinding(ScriptCall& call)
		{
			const double value = Lua::Check<double>(call, 1);
			const double length = Lua::Check<double>(call, 2);
			if (length <= 0.0)
				return Lua::RaiseError(call, "length must be positive");
			Lua::Push(call, MathRepeat(value, length));
			return 1;
		}

		static int MathPingPong(ScriptCall& call)
		{
			const double value = Lua::Check<double>(call, 1);
			const double length = Lua::Check<double>(call, 2);
			if (length <= 0.0)
				return Lua::RaiseError(call, "length must be positive");
			if (length <= std::numeric_limits<double>::max() * 0.5)
			{
				const double phase = std::abs(std::fmod(value, length * 2.0));
				Lua::Push(call, phase <= length ? phase : length - (phase - length));
			}
			else
			{
				// No finite input reaches the second period when 2*length exceeds DBL_MAX.
				const double phase = std::abs(value);
				Lua::Push(call, phase <= length ? phase : length - (phase - length));
			}
			return 1;
		}

	}

	namespace ScriptBindings {

		Status RegisterMath(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions pure{ .Environments = ScriptApiEnvironment::All, .Mutates = false, .SetterMutates = false };
			api.Module("Math", "Deterministic interpolation, smoothing and angle utilities. All arguments and results must be finite.")
				.Function("Clamp", &Utils::MathClamp,
					"((value: number, minimum: number, maximum: number) -> number) & ((value: vector, minimum: vector, maximum: vector) -> vector)",
					"Clamps to ordered inclusive bounds; vector bounds apply component-wise. Reversed bounds are errors.", pure)
				.Function("Clamp01", &Utils::MathClamp01, "((value: number) -> number) & ((value: vector) -> vector)",
					"Clamps a number or every vector component to [0, 1].", pure)
				.Function("Lerp", &Utils::MathLerp,
					"((a: number, b: number, t: number) -> number) & ((a: vector, b: vector, t: number) -> vector)",
					"Interpolates with t clamped to [0, 1]; t=0 returns a and t=1 returns b.", pure)
				.Function("InverseLerp", &Utils::MathInverseLerpBinding, "(a: number, b: number, value: number) -> number",
					"Returns the clamped [0, 1] fraction along a to b, including descending intervals; equal endpoints return zero.", pure)
				.Function("Remap", &Utils::MathRemap,
					"(value: number, sourceMinimum: number, sourceMaximum: number, targetMinimum: number, targetMaximum: number) -> number",
					"Maps the clamped fraction along a nonempty source interval to the target interval; either may descend.", pure)
				.Function("SmoothStep", &Utils::MathSmoothStep, "(a: number, b: number, t: number) -> number",
					"Interpolates from a to b using 3t squared minus 2t cubed, with t clamped to [0, 1].", pure)
				.Function("SmoothDamp", &Utils::MathSmoothDamp,
					"((current: number, target: number, velocity: number, smoothTime: number, dt: number) -> (number, number)) & "
					"((current: vector, target: vector, velocity: vector, smoothTime: number, dt: number) -> (vector, vector))",
					"Exact critically damped spring step, component-wise for vectors, returning value and velocity without crossing the target. "
					"smoothTime must be positive (effective minimum 0.0001 seconds), dt nonnegative; zero dt preserves both inputs.",
					pure)
				.Function("MoveTowards", &Utils::MathMoveTowards,
					"((current: number, target: number, maxDelta: number) -> number) & ((current: vector, target: vector, maxDelta: number) -> vector)",
					"Moves at most nonnegative maxDelta toward target without overshooting; vectors use Euclidean distance.", pure)
				.Function("Approximately", &Utils::MathApproximately, "(a: number, b: number, epsilon: number?) -> boolean",
					"Compares within epsilon times max(1, abs(a), abs(b)); epsilon defaults to 0.000001 and must be nonnegative.", pure)
				.Function("DeltaAngle", &Utils::MathDeltaAngleBinding, "(current: number, target: number) -> number",
					"Returns the shortest signed difference in degrees in (-180, 180], choosing +180 for a half turn.", pure)
				.Function("LerpAngle", &Utils::MathLerpAngle, "(current: number, target: number, t: number) -> number",
					"Follows the shortest angular path in degrees with t clamped to [0, 1]; preserves current's full turns.", pure)
				.Function("Sign", &Utils::MathSign, "(value: number) -> number", "Returns -1, 0 or 1; both signed zeros return zero.", pure)
				.Function("Repeat", &Utils::MathRepeatBinding, "(value: number, length: number) -> number",
					"Wraps either sign of value into [0, length); length must be positive.", pure)
				.Function("PingPong", &Utils::MathPingPong, "(value: number, length: number) -> number",
					"Reflects a repeating value into [0, length] with period 2*length; length must be positive.", pure)
				.Constant("Pi", Value::FromFloat(std::numbers::pi_v<float>), "number", "Pi, rounded to engine float precision.")
				.Constant("Deg2Rad", Value::FromFloat(std::numbers::pi_v<float> / 180.0f), "number", "Multiplier from degrees to radians.")
				.Constant("Rad2Deg", Value::FromFloat(180.0f / std::numbers::pi_v<float>), "number", "Multiplier from radians to degrees.");
			return {};
		}

	}

}
