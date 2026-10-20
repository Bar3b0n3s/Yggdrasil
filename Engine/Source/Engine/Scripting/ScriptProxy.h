#pragma once

#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Reflection/Value.h"

#include <glm/glm.hpp>

#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string_view>

namespace Engine {

	class ConstEntity;
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
		// NotFound for a stale/dead entity; used before calling Scene's assertion-based Entity operations. Writes and every
		// other mutating member call this, so an entity pending destruction refuses them ("entity <id> was destroyed").
		[[nodiscard]] static Status ValidateEntity(IScriptHost& host, ScriptEntityIdentity entity);
		// The read rule for entities pending destruction (M20, W-P02; Docs/Decisions/0031-m20-contract.md decision 4): as
		// ValidateEntity, except that an entity of the current scene generation marked for destruction (Scene.Destroy or a
		// destroyed ancestor earlier in the tick: PendingDestroyTag) is accepted until the flush that destroys it. Every
		// member the binding layer registers as not mutating (ScriptMemberOptions::Mutates false) and every proxy field read
		// validates with this, so a partner's collision callback can still read an entity destroyed earlier in the same
		// dispatch; IsValid stays false for it, the scene's searches keep leaving it out, and after the flush it is NotFound
		// as before. The systems' own state (physics bodies, audio voices) is not covered: their functions keep refusing an
		// entity pending destruction as they do now.
		[[nodiscard]] static Status ValidateEntityForRead(IScriptHost& host, ScriptEntityIdentity entity);
		// Checks entity generation/liveness, index, ScriptVisible and component presence. NotFound for stale/missing data,
		// InvalidArgument for an out-of-range index or a component not exposed to scripts. Writes and component methods
		// that act on the entity or a system's state call this.
		[[nodiscard]] static Status ValidateComponent(IScriptHost& host, ScriptProxyIdentity proxy);
		// As ValidateComponent with the entity checked by ValidateEntityForRead: what proxy field reads and the component
		// methods registered as not mutating call, so the components of an entity pending destruction stay readable until
		// the flush (M20, W-P02). The entity is then Scene::FindReadableEntityByID's.
		[[nodiscard]] static Status ValidateComponentForRead(IScriptHost& host, ScriptProxyIdentity proxy);
		// Exact component name; unknown/nonvisible names fail with NotFound and suggestions; absent components return
		// nullopt. GetShortcut also applies !Hidden && !EntityLevel && !NoShortcut. Entity members take precedence in
		// dispatch and Freeze rejects collisions. GetComponent("Script") is legal; the .Script shortcut is absent. Both are
		// reads: the entity is checked by ValidateEntityForRead, so an entity pending destruction still returns its
		// proxies, whose reads work until the flush.
		[[nodiscard]] static Result<std::optional<ScriptProxyIdentity>> GetComponent(IScriptHost& host,
			ScriptEntityIdentity entity, std::string_view component);
		[[nodiscard]] static Result<std::optional<ScriptProxyIdentity>> GetShortcut(IScriptHost& host,
			ScriptEntityIdentity entity, std::string_view component);

		// Fresh resolution each time. Only Scriptable && !Hidden fields are exposed; FieldMeta.Modes is enforced. Reads
		// return owned reflected values, an entity pending destruction's included (ValidateComponentForRead); writes refuse
		// it (ValidateComponent). Successful reads/writes increment the registry's canonical field counters once.
		// Writes check call.CheckWritable BEFORE mutation, validate against FieldMeta/resolved schemas and type-level
		// rules, then call PrepareHostMutation immediately before ComponentAccess::SetFieldValue (patch + notifications).
		// Rejected validation leaves the scene, recording and counters unchanged; a fault after mutation notification
		// cannot make an external write recordable again. Read-only fields return InvalidState; invalid values Validation;
		// missing data NotFound.
		// Transform fast paths obey these same validation, patch, interpolation and coverage rules.
		[[nodiscard]] static Result<Value> ReadField(ScriptCall& call, ScriptProxyIdentity proxy, std::string_view field);
		[[nodiscard]] static Status WriteField(ScriptCall& call, ScriptProxyIdentity proxy, std::string_view field,
			const Value& value);

		// Whether TransformSystem::SetWorldPosition can store `position` for a child of `parent`: the translation relative to
		// the parent, computed exactly as SetWorldPosition computes it, must be finite. It is not when the position lies
		// beyond what the parent chain expresses in float (a tiny parent scale, a huge parent translation) or the parent's
		// world matrix cannot be inverted. Validation for that or a non-finite position; an invalid parent (a root) accepts
		// every finite position. Hosts and writers call this before any mutation, so script input never reaches
		// SetWorldPosition's assertion or stores Inf (WorldPosition writes, Scene.Instantiate's position).
		[[nodiscard]] static Status ValidateWorldPosition(ConstEntity parent, const glm::vec3& position);
	};

}
