#pragma once

#include "Engine/Automation/Methods/AutomationTypes.h"
#include "Engine/Automation/Protocol/JsonRpc.h"
#include "Engine/Automation/Protocol/MethodContext.h"
#include "Engine/Core/Base.h"
#include "Engine/Core/Error.h"
#include "Engine/Core/EventLog.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

// What the method domains of Engine/Automation/Methods share with each other and with the editor's own domains (the
// param-struct conventions of Engine/Automation/Protocol/MethodRegistry.h made concrete): located errors, entity
// references and summaries, the components results report, log cursors, and the play state's event and disconnect rule
// both hosts apply. They moved here from EditorCore's Automation/Private/MethodSupport.h with the handlers of the Runtime
// subset (Docs/Decisions/0012-m7-decisions.md decision 12), so the Editor and the Runtime resolve and report the same way;
// EditorCore's MethodSupport.h includes this header.

namespace Engine {

	class ComponentInfo;
	class ConstEntity;
	class Entity;
	class PlaySession;
	class Scene;
	class VariantValue;

	namespace Utils {

		// `error` with its issues replaced by `issues`; code, message, contexts, location and hint are kept.
		[[nodiscard]] Error ReplaceIssues(const Error& error, std::vector<ErrorIssue> issues);

		// `error` relocated below `prefix` (a JSON pointer such as "/components/RigidBody"): the location's pointer and every
		// issue's pointer are prefixed; an unset location pointer becomes `prefix` itself.
		[[nodiscard]] Error PrefixPointers(const Error& error, std::string_view prefix);

		// `error` located at the param `pointer`: the location's pointer is set to it, and issues without a pointer (located
		// at the root of whatever produced them) are moved there too.
		[[nodiscard]] Error LocateAtParam(const Error& error, std::string_view pointer);

		// An error of `code` located at the param `pointer`, with an optional hint.
		[[nodiscard]] Error MakeParamError(ErrorCode code, std::string_view pointer, std::string message, std::string hint = {});

		// The 16 lowercase hex digits of a valid UUID, or "" for the invalid one.
		[[nodiscard]] std::string FormatOptionalUUID(UUID id);

		// Resolves an EntityRef (§13.4) in `scene`, the rule of AutomationMethodContext::ResolveEntity that every host
		// implements with it: 16 hex digits (either case) is an exact id; 6 to 15 hex digits a unique prefix of an entity's
		// id; text starting with '/' an entity path (Scene::ResolveEntityPath). `pointer` locates the param in errors.
		// Errors: InvalidArgument for anything else, or for an ambiguous prefix or path (every candidate listed as an
		// ErrorIssue with its path); NotFound for a valid reference that names no entity (with "did you mean" suggestions
		// for paths).
		[[nodiscard]] Result<Entity> ResolveEntityReference(Scene& scene, std::string_view reference, std::string_view pointer);

		// The IEntityReferenceResolver a host's context returns (MethodContext::GetEntityReferenceResolver;
		// Docs/Decisions/0030-m19-contract.md decision 7, A-EREF): EntityRef values in component data resolved in `scene` by
		// ResolveEntityReference's rule, so component data and entity params resolve alike. `scene` is the scene parameter of
		// the phase 1 ledger's "Entity paths and scenes": the editor passes its active scene (the open edit scene, or the play
		// scene for a request whose target is Play, §13.4 "Target") until M20 passes the request's scene target. A documented
		// back-reference: the resolver lives no longer than the request, and the scene outlives it.
		class SceneEntityReferenceResolver final : public IEntityReferenceResolver
		{
		public:
			explicit SceneEntityReferenceResolver(Scene& scene);

			// ResolveEntityReference(scene, reference, pointer)'s entity id; its errors unchanged.
			[[nodiscard]] Result<UUID> ResolveEntityReference(std::string_view reference, std::string_view pointer) override;
		private:
			Scene* m_Scene = nullptr; // the scene references resolve in (see the class comment)
		};

		// {id, name, path} of `entity` (valid, asserted): AutomationMethodContext::MakeEntitySummary of every host.
		[[nodiscard]] EntitySummary SummarizeEntity(ConstEntity entity);

		// The components of `entity` that results report (serializable and not entity-level, Hidden ones included because
		// they are readable), in registry order, which is the order scene files use.
		[[nodiscard]] std::vector<const ComponentInfo*> GetEntityComponents(ConstEntity entity);

		// The registry names of the native components `entity` holds ("Native.<Name>", IsNativeComponentName;
		// Docs/Decisions/0030-m19-contract.md decision 27), in the entity's order, which is by name once a module made them
		// canonical (Native/NativeComponents.h). They live among the entity's preserved components, outside the type
		// registry, so results list them after GetEntityComponents' (the native test game's friction NT-07). A name is not
		// checked against a loaded module: native.getComponents reads them through it.
		[[nodiscard]] std::vector<std::string> GetEntityNativeComponents(ConstEntity entity);

		// The JSON of `entity`'s native component `name` as the scene holds it, borrowed until the entity's components change;
		// null when the entity has no native component of that name.
		[[nodiscard]] const VariantValue* FindEntityNativeComponent(ConstEntity entity, std::string_view name);

		// The decimal 64-bit cursor of log.read and events.read: "" is 0 (the oldest held entry), digits are the sequence
		// number, and "end" is nullopt (read nothing; the caller answers with the next sequence number). Errors:
		// InvalidArgument at "/cursor" for anything else.
		[[nodiscard]] Result<std::optional<uint64_t>> ParseSequenceCursor(std::string_view cursor);

		// PlayStateChanged (§4.9) for the run state `session` is in now (GetPlayRunState, PlayMethods.h; null: Edit), with
		// its tick (none in Edit): the event every host appends when the run state changes (the play methods, the editor's
		// play controller, a lockstep owner's disconnect).
		[[nodiscard]] EngineEvent MakePlayStateChangedEvent(const PlaySession* session);

		// What ReleaseDisconnectedLockstep changed.
		struct DisconnectedLockstep
		{
			bool Released = false;     // the client owned lockstep: lockstep is released and play paused
			bool StateChanged = false; // the run state changed (the session was running): append MakePlayStateChangedEvent
		};

		// §13.2 "Disconnect", the rule of both hosts when a client goes (after its pending operations were cancelled): when
		// `client` (not NoClient) owns the lockstep of `session`, lockstep is released and play paused, so the session never
		// stays locked in "agent controls time". Nothing changes otherwise.
		[[nodiscard]] DisconnectedLockstep ReleaseDisconnectedLockstep(PlaySession& session, ClientId client);

	}

}
