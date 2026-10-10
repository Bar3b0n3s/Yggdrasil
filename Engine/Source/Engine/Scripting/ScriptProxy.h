#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/Value.h"

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace Engine {

	class IScriptHost;
	struct ScriptCall;

	// The entire Entity userdata payload: no Scene*, Entity, EnTT handle, engine pointer or component address. Generation
	// is nonzero and comes from IScriptHost::GetSceneGeneration(). Lua equality compares UUIDs (§11.5); access ALSO
	// checks generation and current liveness, so equality cannot authorize access to a replacement scene/entity.
	struct ScriptEntityIdentity
	{
		UUID ID{};
		uint64_t SceneGeneration = 0;
	};

	// A proxy copies its entity identity, including generation. The index is ComponentInfo::GetIndex() in the host's
	// frozen TypeRegistry, never a persistent or serialized identity. Every access re-resolves; removing a component
	// invalidates access until that type is added again to the same live entity. No retained component references.
	struct ScriptProxyIdentity
	{
		ScriptEntityIdentity Entity{};
		size_t ComponentTypeIndex = std::numeric_limits<size_t>::max();
	};

	// Stateless main-thread operations. Host/VM/registry back-references are obtained from the call, never stored in
	// userdata. The host outlives a call. Result values own their data. Failed external accesses never assert.
	class ScriptProxy
	{
	public:
		// False for zero/stale generation, dead/marked entities, and invalid IDs. Never dereferences an old scene.
		[[nodiscard]] static bool IsValid(IScriptHost& host, ScriptEntityIdentity entity);
		// NotFound for a stale/dead entity; used before calling Scene's assertion-based Entity operations.
		[[nodiscard]] static Status ValidateEntity(IScriptHost& host, ScriptEntityIdentity entity);
		// Checks entity generation/liveness, index, ScriptVisible and component presence. NotFound for stale/missing data,
		// InvalidArgument for an out-of-range index or a component not exposed to scripts.
		[[nodiscard]] static Status ValidateComponent(IScriptHost& host, ScriptProxyIdentity proxy);
		// Exact component name; unknown/nonvisible names fail with NotFound and suggestions; absent components return
		// nullopt. GetShortcut also applies !Hidden && !EntityLevel && !NoShortcut. Entity members take precedence in
		// dispatch and Freeze rejects collisions. GetComponent("Script") is legal; the .Script shortcut is absent.
		[[nodiscard]] static Result<std::optional<ScriptProxyIdentity>> GetComponent(IScriptHost& host,
			ScriptEntityIdentity entity, std::string_view component);
		[[nodiscard]] static Result<std::optional<ScriptProxyIdentity>> GetShortcut(IScriptHost& host,
			ScriptEntityIdentity entity, std::string_view component);

		// Fresh resolution each time. Only Scriptable && !Hidden fields are exposed; FieldMeta.Modes is enforced. Reads
		// return owned reflected values. Successful reads/writes increment the registry's canonical field counters once.
		// Writes check call.CheckWritable BEFORE mutation, validate against FieldMeta/resolved schemas and type-level
		// rules, then call PrepareHostMutation immediately before ComponentAccess::SetFieldValue (patch + notifications).
		// Rejected validation leaves the scene, recording and counters unchanged; a fault after mutation notification
		// cannot make an external write recordable again. Read-only fields return InvalidState; invalid values Validation;
		// missing data NotFound.
		// Transform fast paths obey these same validation, patch, interpolation and coverage rules.
		[[nodiscard]] static Result<Value> ReadField(ScriptCall& call, ScriptProxyIdentity proxy, std::string_view field);
		[[nodiscard]] static Status WriteField(ScriptCall& call, ScriptProxyIdentity proxy, std::string_view field,
			const Value& value);
	};

}
