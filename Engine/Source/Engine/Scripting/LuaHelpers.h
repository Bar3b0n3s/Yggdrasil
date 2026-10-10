#pragma once

#include "Engine/Asset/AssetHandle.h"
#include "Engine/Core/Error.h"
#include "Engine/Reflection/Value.h"
#include "Engine/Scripting/ScriptProxy.h"

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace Engine {

	class FieldInfo;
	struct ScriptCall;

	// Architecture §11.4 explicitly names Lua helpers. The proposed namespace exception is limited to this facade and
	// its private VM entry points; it does not permit exposing Luau types through public headers.
	namespace Lua {

		// Checks an existing stack slot without popping it. Only the explicit specializations below are supported.
		// Wrong types/ranges, NaN and +/-Inf raise a located Luau error with the active member and argument index, e.g.
		// "Entity:AddComponent: argument #2 expected string, got number". No string/number or boolean coercion. Integers
		// must be exact and in range; narrowing to float is checked. Strings are owned copies, including embedded NULs.
		// Vector2 accepts native vector with z == 0; Vector3 is native vector; Vector4 means tagged Color (rgba); quat
		// means tagged Quat (xyzw), finite and nonzero. Entity/proxy checks validate tags and copy identities; liveness is
		// checked separately so Entity:IsValid accepts stale identities. AssetHandle (an alias of UUID) means AssetRef
		// userdata, not Entity or numeric/string coercion. Errors unwind only to a protected entry; never catch them here.
		template<typename T>
		[[nodiscard]] T Check(ScriptCall& call, int index);

		// Pushes one value, copying owned data. Unsupported T has no generic definition. Finite/type validation is the
		// same as Check. Invalid asset/entity identities push nil; valid identities never carry C++ pointers. A vec2
		// pushes a native vector with z = 0. Native callbacks call these only while a protected entry is active.
		template<typename T>
		void Push(ScriptCall& call, const T& value);

		template<>
		[[nodiscard]] bool Check<bool>(ScriptCall& call, int index);
		template<>
		[[nodiscard]] int32_t Check<int32_t>(ScriptCall& call, int index);
		template<>
		[[nodiscard]] uint32_t Check<uint32_t>(ScriptCall& call, int index);
		template<>
		[[nodiscard]] float Check<float>(ScriptCall& call, int index);
		template<>
		[[nodiscard]] double Check<double>(ScriptCall& call, int index);
		template<>
		[[nodiscard]] std::string Check<std::string>(ScriptCall& call, int index);
		template<>
		[[nodiscard]] glm::vec2 Check<glm::vec2>(ScriptCall& call, int index);
		template<>
		[[nodiscard]] glm::vec3 Check<glm::vec3>(ScriptCall& call, int index);
		template<>
		[[nodiscard]] glm::vec4 Check<glm::vec4>(ScriptCall& call, int index);
		template<>
		[[nodiscard]] glm::quat Check<glm::quat>(ScriptCall& call, int index);
		template<>
		[[nodiscard]] AssetHandle Check<AssetHandle>(ScriptCall& call, int index);
		template<>
		[[nodiscard]] ScriptEntityIdentity Check<ScriptEntityIdentity>(ScriptCall& call, int index);
		template<>
		[[nodiscard]] ScriptProxyIdentity Check<ScriptProxyIdentity>(ScriptCall& call, int index);

		template<>
		void Push<bool>(ScriptCall& call, const bool& value);
		template<>
		void Push<int32_t>(ScriptCall& call, const int32_t& value);
		template<>
		void Push<uint32_t>(ScriptCall& call, const uint32_t& value);
		template<>
		void Push<float>(ScriptCall& call, const float& value);
		template<>
		void Push<double>(ScriptCall& call, const double& value);
		template<>
		void Push<std::string>(ScriptCall& call, const std::string& value);
		template<>
		void Push<glm::vec2>(ScriptCall& call, const glm::vec2& value);
		template<>
		void Push<glm::vec3>(ScriptCall& call, const glm::vec3& value);
		template<>
		void Push<glm::vec4>(ScriptCall& call, const glm::vec4& value);
		template<>
		void Push<glm::quat>(ScriptCall& call, const glm::quat& value);
		template<>
		void Push<AssetHandle>(ScriptCall& call, const AssetHandle& value);
		template<>
		void Push<ScriptEntityIdentity>(ScriptCall& call, const ScriptEntityIdentity& value);
		template<>
		void Push<ScriptProxyIdentity>(ScriptCall& call, const ScriptProxyIdentity& value);

		[[nodiscard]] int GetArgumentCount(ScriptCall& call);
		[[nodiscard]] bool IsNoneOrNil(ScriptCall& call, int index);
		void PushNil(ScriptCall& call);
		void PushString(ScriptCall& call, std::string_view value);
		// Validates against the active member's enum-slot metadata and canonical enum table and applies its optional default.
		// Dispatch owns the one per-value counter increment; this helper never increments again. Case-insensitive input
		// follows EnumInfo's script policy. Unknown values raise a located error with suggestions. Metadata/slot mismatches
		// are registration bugs, not a second map. For an omitted optional argument without a default, callers test
		// IsNoneOrNil and do not call CheckEnum; calling it anyway raises a type error rather than inventing an enum value.
		[[nodiscard]] int64_t CheckEnum(ScriptCall& call, int index, std::string_view enumName);
		// Reflection-driven recursive marshalling; conversion does not write a component or increment proxy counters.
		// Field validation/setters still run at WriteField. Array/map/struct/Variant schema traversal preserves canonical
		// order, detects cycles, and rejects unsupported userdata/functions/threads instead of leaking pointers.
		[[nodiscard]] Value CheckValue(ScriptCall& call, int index, const FieldInfo& field);
		void PushValue(ScriptCall& call, const Value& value, const FieldInfo& field);
		// Raises a located Luau error. The int return permits `return Lua::RaiseError(...)` in native callbacks; a real
		// implementation never returns. Error overload preserves its context/hint/location. No first-party throw/catch.
		int RaiseError(ScriptCall& call, std::string_view message);
		int RaiseError(ScriptCall& call, const Error& error);

	}

}
