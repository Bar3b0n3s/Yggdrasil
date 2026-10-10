#include "EnginePCH.h"
#include "Engine/Scripting/Private/BindingRegistration.h"

#include "Engine/Core/Utf8.h"
#include "Engine/Renderer/DebugDrawList.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"

#include <cmath>
#include <limits>

namespace Engine {

	namespace Utils {

		static glm::vec4 DebugColor(ScriptCall& call, int index)
		{
			return Lua::IsNoneOrNil(call, index) ? glm::vec4(1.0f) : Lua::Check<glm::vec4>(call, index);
		}

		static float DebugDuration(ScriptCall& call, int index)
		{
			const float duration = Lua::IsNoneOrNil(call, index) ? 0.0f : Lua::Check<float>(call, index);
			if (duration < 0.0f)
				Lua::RaiseError(call, "duration must be nonnegative");
			return duration;
		}

		static DebugDrawList& BindingDebugDraw(ScriptCall& call)
		{
			auto* list = call.Engine->GetHost().GetDebugDraw();
			if (list == nullptr)
				Lua::RaiseError(call, "debug draw service is unavailable");
			const auto permission = call.PrepareHostMutation();
			if (!permission)
				Lua::RaiseError(call, permission.error());
			return *list;
		}

		static int DebugDrawLine(ScriptCall& call)
		{
			const auto from = Lua::Check<glm::vec3>(call, 1);
			const auto to = Lua::Check<glm::vec3>(call, 2);
			const auto color = DebugColor(call, 3);
			const auto duration = DebugDuration(call, 4);
			BindingDebugDraw(call).AddLine(from, to, color, duration);
			return 0;
		}

		static int DebugDrawRay(ScriptCall& call)
		{
			const auto origin = Lua::Check<glm::vec3>(call, 1);
			const auto direction = Lua::Check<glm::vec3>(call, 2);
			const auto color = DebugColor(call, 3);
			const auto duration = DebugDuration(call, 4);
			const auto wide = glm::dvec3(direction);
			const double length = std::sqrt(glm::dot(wide, wide));
			if (length <= 0.0 || length > std::numeric_limits<float>::max())
				return Lua::RaiseError(call, "direction must have a positive finite float length");
			const auto end = glm::dvec3(origin) + wide;
			if (glm::any(glm::greaterThan(glm::abs(end), glm::dvec3(std::numeric_limits<float>::max()))))
				return Lua::RaiseError(call, "ray endpoint exceeds the float range");
			BindingDebugDraw(call).AddRay(origin, glm::vec3(wide / length), static_cast<float>(length), color, duration);
			return 0;
		}

		static int DebugDrawBox(ScriptCall& call)
		{
			const auto center = Lua::Check<glm::vec3>(call, 1);
			const auto half = Lua::Check<glm::vec3>(call, 2);
			const auto rotation = Lua::IsNoneOrNil(call, 3) ? glm::quat(1.0f, 0.0f, 0.0f, 0.0f) : Lua::Check<glm::quat>(call, 3);
			const auto color = DebugColor(call, 4);
			const auto duration = DebugDuration(call, 5);
			if (glm::any(glm::lessThanEqual(half, glm::vec3(0.0f))))
				return Lua::RaiseError(call, "half extents must be positive");
			const glm::dquat wide(rotation.w, rotation.x, rotation.y, rotation.z);
			const auto normalized = wide / std::sqrt(glm::dot(wide, wide));
			const glm::quat unit(static_cast<float>(normalized.w), static_cast<float>(normalized.x), static_cast<float>(normalized.y), static_cast<float>(normalized.z));
			BindingDebugDraw(call).AddBox(center, half, unit, color, duration);
			return 0;
		}

		static int DebugDrawSphere(ScriptCall& call)
		{
			const auto center = Lua::Check<glm::vec3>(call, 1);
			const auto radius = Lua::Check<float>(call, 2);
			const auto color = DebugColor(call, 3);
			const auto duration = DebugDuration(call, 4);
			if (radius <= 0.0f)
				return Lua::RaiseError(call, "radius must be positive");
			BindingDebugDraw(call).AddSphere(center, radius, color, duration);
			return 0;
		}

		static int DebugDrawArrow(ScriptCall& call)
		{
			const auto from = Lua::Check<glm::vec3>(call, 1);
			const auto to = Lua::Check<glm::vec3>(call, 2);
			const auto color = DebugColor(call, 3);
			const auto duration = DebugDuration(call, 4);
			BindingDebugDraw(call).AddArrow(from, to, 0.1f, color, duration);
			return 0;
		}

		static int DebugDrawText(ScriptCall& call)
		{
			const auto position = Lua::Check<glm::vec3>(call, 1);
			const auto text = Lua::Check<std::string>(call, 2);
			const auto color = DebugColor(call, 3);
			const auto duration = DebugDuration(call, 4);
			if (!IsValidUtf8(text) || text.find('\0') != std::string::npos)
				return Lua::RaiseError(call, "text must be UTF-8 without embedded NULs");
			BindingDebugDraw(call).AddText(position, text, 16.0f, color, duration);
			return 0;
		}

		static int DebugBreak(ScriptCall& call)
		{
			const auto permission = call.CheckWritable();
			if (!permission)
				return Lua::RaiseError(call, permission.error());
			if (call.Engine->IsTestMode())
			{
				auto* tests = call.Engine->GetTestHost();
				if (tests == nullptr)
					return Lua::RaiseError(call, "test host is unavailable");
				tests->OnDebugBreak();
			}
			else if (call.Engine->GetHost().GetEnvironment().IsEditor)
			{
				const auto mutation = call.PrepareHostMutation();
				if (!mutation)
					return Lua::RaiseError(call, mutation.error());
				call.Engine->GetHost().RequestPause();
			}
			return 0;
		}

	}

	namespace ScriptBindings {

		Status RegisterDebug(ScriptApiRegistry& api)
		{
			api.Module("Debug", "Depth-tested world-space debug primitives with white color and one-extraction duration by default.")
				.Function("DrawLine", &Utils::DebugDrawLine, "(a: vector, b: vector, color: Color?, duration: number?) -> ()", "Adds a line segment; duration is nonnegative simulation seconds.")
				.Function("DrawRay", &Utils::DebugDrawRay, "(origin: vector, direction: vector, color: Color?, duration: number?) -> ()", "Adds a ray segment whose length is the nonzero direction vector's magnitude.")
				.Function("DrawBox", &Utils::DebugDrawBox, "(center: vector, halfExtents: vector, rotation: Quat?, color: Color?, duration: number?) -> ()", "Adds a box with positive half extents and a normalized rotation, identity by default.")
				.Function("DrawSphere", &Utils::DebugDrawSphere, "(center: vector, radius: number, color: Color?, duration: number?) -> ()", "Adds a sphere with positive radius.")
				.Function("DrawArrow", &Utils::DebugDrawArrow, "(from: vector, to: vector, color: Color?, duration: number?) -> ()", "Adds an arrow with a 0.1 metre head.")
				.Function("DrawText", &Utils::DebugDrawText, "(position: vector, text: string, color: Color?, duration: number?) -> ()", "Adds UTF-8 text at a world position with the default 16-pixel size.")
				.Function("Break", &Utils::DebugBreak, "() -> ()", "Pauses editor Play; Runtime does nothing; test runs count the call without pausing in every mode.");
			return {};
		}

	}

}
