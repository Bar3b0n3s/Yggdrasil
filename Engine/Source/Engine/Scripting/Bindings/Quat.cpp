#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Core/DetMath.h"
#include "Engine/Scene/TransformSystem.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include <lua.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <numbers>

namespace Engine {

	namespace Utils {

		static glm::dquat QuatWide(const glm::quat& value)
		{
			return glm::dquat(value.w, value.x, value.y, value.z);
		}

		static float QuatFloat(ScriptCall& call, double value)
		{
			if (!std::isfinite(value) || std::abs(value) > std::numeric_limits<float>::max())
				Lua::RaiseError(call, "result is outside the finite quaternion/vector range");
			return static_cast<float>(value);
		}

		static glm::quat QuatToFloat(ScriptCall& call, const glm::dquat& value)
		{
			const glm::quat result(QuatFloat(call, value.w), QuatFloat(call, value.x),
				QuatFloat(call, value.y), QuatFloat(call, value.z));
			if (result.x == 0.0f && result.y == 0.0f && result.z == 0.0f && result.w == 0.0f)
				Lua::RaiseError(call, "quaternion must be nonzero");
			return result;
		}

		static glm::dquat QuatUnit(ScriptCall& call, int index)
		{
			// The checked float payload fits double's squared range, including float subnormals.
			const glm::dquat value = QuatWide(Lua::Check<glm::quat>(call, index));
			return value / std::sqrt(glm::dot(value, value));
		}

		static int QuatNew(ScriptCall& call)
		{
			const float x = Lua::Check<float>(call, 1);
			const float y = Lua::Check<float>(call, 2);
			const float z = Lua::Check<float>(call, 3);
			const float w = Lua::Check<float>(call, 4);
			if (x == 0.0f && y == 0.0f && z == 0.0f && w == 0.0f)
				return Lua::RaiseError(call, "quaternion must be nonzero");
			Lua::Push(call, glm::quat(w, x, y, z));
			return 1;
		}

		static int QuatIdentity(ScriptCall& call)
		{
			Lua::Push(call, glm::quat(1.0f, 0.0f, 0.0f, 0.0f));
			return 1;
		}

		static int QuatFromEuler(ScriptCall& call)
		{
			const float x = Lua::Check<float>(call, 1);
			const float y = Lua::Check<float>(call, 2);
			const float z = Lua::Check<float>(call, 3);
			Lua::Push(call, TransformSystem::QuaternionFromEulerDegrees(glm::vec3(x, y, z)));
			return 1;
		}

		static int QuatAngleAxis(ScriptCall& call)
		{
			const double degrees = Lua::Check<double>(call, 1);
			const glm::dvec3 axis(Lua::Check<glm::vec3>(call, 2));
			const double length = std::sqrt(glm::dot(axis, axis));
			if (length == 0.0)
				return Lua::RaiseError(call, "axis must be nonzero");
			const auto angle = DetMath::SinCos(degrees * (std::numbers::pi / 360.0));
			const glm::dvec3 imaginary = axis * (angle.Sin / length);
			Lua::Push(call, QuatToFloat(call, glm::dquat(angle.Cos, imaginary)));
			return 1;
		}

		static int QuatLookRotation(ScriptCall& call)
		{
			const glm::dvec3 forward(Lua::Check<glm::vec3>(call, 1));
			const glm::dvec3 up = Lua::IsNoneOrNil(call, 2) ? glm::dvec3(0.0, 1.0, 0.0)
															: glm::dvec3(Lua::Check<glm::vec3>(call, 2));
			const double forwardLength = std::sqrt(glm::dot(forward, forward));
			const double upLength = std::sqrt(glm::dot(up, up));
			if (forwardLength == 0.0 || upLength == 0.0)
				return Lua::RaiseError(call, "forward and up must be nonzero");
			const glm::dvec3 direction = forward / forwardLength;
			const glm::dvec3 side = glm::cross(direction, up / upLength);
			const double sideLength = std::sqrt(glm::dot(side, side));
			if (sideLength <= 0.000001)
				return Lua::RaiseError(call, "forward and up must not be parallel");
			const glm::dvec3 right = side / sideLength;
			const glm::dvec3 correctedUp = glm::cross(right, direction);
			// Columns transform local +X, +Y and +Z. Engine forward is -Z, not +Z.
			const glm::dquat rotation = glm::quat_cast(glm::dmat3(right, correctedUp, -direction));
			Lua::Push(call, QuatToFloat(call, rotation / std::sqrt(glm::dot(rotation, rotation))));
			return 1;
		}

		static int QuatSlerp(ScriptCall& call)
		{
			const glm::dquat from = QuatUnit(call, 1);
			glm::dquat to = QuatUnit(call, 2);
			const double amount = std::clamp(Lua::Check<double>(call, 3), 0.0, 1.0);
			double cosine = glm::dot(from, to);
			if (cosine < 0.0)
			{
				to = -to;
				cosine = -cosine;
			}
			cosine = std::clamp(cosine, 0.0, 1.0);
			glm::dquat result(1.0, 0.0, 0.0, 0.0);
			if (cosine > 0.9995)
				result = from * (1.0 - amount) + to * amount;
			else
			{
				const double angle = DetMath::ACos(cosine);
				const double denominator = DetMath::Sin(angle);
				result = from * (DetMath::Sin((1.0 - amount) * angle) / denominator)
					+ to * (DetMath::Sin(amount * angle) / denominator);
			}
			Lua::Push(call, QuatToFloat(call, result / std::sqrt(glm::dot(result, result))));
			return 1;
		}

		static int QuatInverse(ScriptCall& call)
		{
			const glm::dquat value = QuatWide(Lua::Check<glm::quat>(call, 1));
			const double normSquared = glm::dot(value, value);
			Lua::Push(call, QuatToFloat(call, glm::dquat(value.w, -value.x, -value.y, -value.z) / normSquared));
			return 1;
		}

		static int QuatToEuler(ScriptCall& call)
		{
			const glm::quat value = Lua::Check<glm::quat>(call, 1);
			Lua::Push(call, TransformSystem::EulerDegreesFromQuaternion(value));
			return 1;
		}

		static int QuatNormalized(ScriptCall& call)
		{
			Lua::Push(call, QuatToFloat(call, QuatUnit(call, 1)));
			return 1;
		}

		static int QuatRotate(ScriptCall& call)
		{
			const glm::dquat rotation = QuatUnit(call, 1);
			const glm::dvec3 vector(Lua::Check<glm::vec3>(call, 2));
			const glm::dvec3 result = rotation * vector;
			Lua::Push(call, glm::vec3(QuatFloat(call, result.x), QuatFloat(call, result.y), QuatFloat(call, result.z)));
			return 1;
		}

		static int QuatMultiply(ScriptCall& call)
		{
			if (lua_type(call.State, 2) == LUA_TVECTOR)
				return QuatRotate(call);
			const glm::dquat left = QuatWide(Lua::Check<glm::quat>(call, 1));
			const glm::dquat right = QuatWide(Lua::Check<glm::quat>(call, 2));
			Lua::Push(call, QuatToFloat(call, left * right));
			return 1;
		}

		template<int Component>
		static int QuatGet(ScriptCall& call)
		{
			Lua::Push(call, Lua::Check<glm::quat>(call, 1)[Component]);
			return 1;
		}

		template<int Component>
		static int QuatSet(ScriptCall& call)
		{
			glm::quat candidate = Lua::Check<glm::quat>(call, 1);
			candidate[Component] = Lua::Check<float>(call, 2);
			if (candidate.x == 0.0f && candidate.y == 0.0f && candidate.z == 0.0f && candidate.w == 0.0f)
				return Lua::RaiseError(call, "quaternion must be nonzero");
			Lua::CheckQuatStorage(call, 1) = candidate;
			return 0;
		}

	}

	namespace ScriptBindings {

		Status RegisterQuat(ScriptApiRegistry& api)
		{
			const ScriptMemberOptions pure{ .Environments = ScriptApiEnvironment::All, .Mutates = false, .SetterMutates = false };
			api.Type("Quat", "A local nonzero quaternion, stored as finite float x, y, z, w components. Angles are degrees.")
				.Constructor("New", &Utils::QuatNew, "(x: number, y: number, z: number, w: number) -> Quat",
					"Creates an unnormalized quaternion; at least one component must be nonzero.", pure)
				.Constructor("Identity", &Utils::QuatIdentity, "() -> Quat", "Creates the identity rotation (0, 0, 0, 1).", pure)
				.Constructor("FromEuler", &Utils::QuatFromEuler, "(x: number, y: number, z: number) -> Quat",
					"Creates a unit rotation from degrees, applying Z, then X, then Y, matching Transform.EulerAngles.", pure)
				.Constructor("AngleAxis", &Utils::QuatAngleAxis, "(degrees: number, axis: vector) -> Quat",
					"Creates a unit rotation around a nonzero axis, normalizing its length.", pure)
				.Constructor("LookRotation", &Utils::QuatLookRotation, "(forward: vector, up: vector?) -> Quat",
					"Points local -Z along forward and aligns +Y to projected up (default +Y). Zero or nearly parallel directions are errors.", pure)
				.Constructor("Slerp", &Utils::QuatSlerp, "(a: Quat, b: Quat, t: number) -> Quat",
					"Interpolates normalized rotations along the shortest arc, with t clamped to [0, 1].", pure)
				.Method("Inverse", &Utils::QuatInverse, "(self: Quat) -> Quat",
					"Returns the algebraic inverse, preserving reciprocal magnitude; an unrepresentable result is an error.", pure)
				.Method("ToEuler", &Utils::QuatToEuler, "(self: Quat) -> vector",
					"Returns Z-X-Y Euler degrees with X in [-90, 90] and Y/Z in (-180, 180], matching the inspector.", pure)
				.Method("Normalized", &Utils::QuatNormalized, "(self: Quat) -> Quat", "Returns a new unit quaternion with the same rotation.", pure)
				.Method("Rotate", &Utils::QuatRotate, "(self: Quat, value: vector) -> vector",
					"Rotates a vector using the normalized quaternion; does not scale the vector.", pure)
				.Operator("__mul", &Utils::QuatMultiply,
					"((a: Quat, b: Quat) -> Quat) & ((rotation: Quat, value: vector) -> vector)",
					"Composes quaternions (right operand applied first), or rotates a vector as Rotate does.", pure)
				.Property("x", &Utils::QuatGet<0>, &Utils::QuatSet<0>, "number", "The local x component; writes preserve a nonzero finite quaternion.", pure)
				.Property("y", &Utils::QuatGet<1>, &Utils::QuatSet<1>, "number", "The local y component; writes preserve a nonzero finite quaternion.", pure)
				.Property("z", &Utils::QuatGet<2>, &Utils::QuatSet<2>, "number", "The local z component; writes preserve a nonzero finite quaternion.", pure)
				.Property("w", &Utils::QuatGet<3>, &Utils::QuatSet<3>, "number", "The local w component; writes preserve a nonzero finite quaternion.", pure);
			return {};
		}

	}

}
