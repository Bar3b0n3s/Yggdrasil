#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/Time.h"

#include <cstdint>

// The native module's runtime in a play session (Docs/Decisions/0030-m19-contract.md decision 28, C-NATIVE-STATIC): what the
// NativeStep phase runs and what the state hash's native block holds. A PlaySession of mode Play whose specification names
// a module (PlaySessionSpecification::Native) owns one for its whole life, Scene.Load included; Simulate (Play without
// gameplay code, §5.6) has none, like it has no scripts.
//
// NativeStep (§5.7, the ledger's PlaySessionPhase: after Tasks, before PreStepTransformUpdate). Step runs, once per tick:
//   1. the module's systems in registration order, each with an INativeStepContext (NativeApi.h) over the session: its
//      tick and fixed delta, its scene's entities in canonical order (Query lists those effectively active and holding the
//      component, as the call finds them), the components' data, the session's resources, its Random stream, the latched
//      step view of its input actions (PlayInput, InputPhase::Step, as scripts read it in OnFixedUpdate), DetMath and the log;
//      a component's data is read from its JSON in the scene (the entity's preserved components, NativeComponents.h) the
//      first time a system asks for it in the step, and a pointer stays valid until the system returns. The read is
//      NativeValueFromJson's migration by field name (decision 27), because only the starting scene is canonicalized
//      (PlaySession::Prepare): a scene Scene.Load loads and a prefab instantiated during play reach the step as their files
//      hold them, in the editor and in every Runtime alike. A value its type rejects (NATIVE_COMPONENT_INVALID) stops the
//      session fatally, like an exception that escapes a system: PlaySession::GetFatalError names the entity (its path and
//      id), the component and the first issue, and no later system or phase runs. A value read with notes
//      (NATIVE_FIELD_DROPPED, NATIVE_FIELD_RESET) is used migrated and written back canonical at the phase's end like a
//      changed one, and each note is logged as a warning once per session, component and field, never failing the step;
//   2. then every component whose bytes a system changed, or whose read migrated it, is written back to the scene as
//      canonical JSON (NativeValueToJson), and every component a system added or removed is added to or removed from its
//      entity's preserved components, so when NativeStep ends the scene holds the native data, scene.get, entity reads and
//      the state hash's scene part see it, and PreStepTransformUpdate propagates the Transforms the systems wrote before
//      physics (§5.7 step 5). Automation and editor writes of native components between ticks (native.setComponents
//      {target: "Play"}) reach the systems at the next step, since every step reads the scene.
// A system is called inside a try/catch (a §4.6 boundary, Lint's TryCatchFiles): an exception that escapes it stops the
// session fatally (PlaySession::GetFatalError, ScriptErrorKind of a session error) naming the system and the exception's
// message, and no later system or phase runs. A crash in native code is not caught: it is the risk the "Allow native code"
// preference accepts (decision 29).
//
// The native block (the ledger's state-hash block after the physics block): AppendStateHash appends, for every resource in
// registration order, its name and its canonical JSON (AppendNativeValueHash). Native components need nothing more: they
// are in the scene's canonical serialization, the hash's first part. A session without a module has no NativeWorld and
// appends nothing, so every existing replay keeps its hash.
//
// Resources start at their defaults when the session starts and keep their values across Scene.Load: they are session
// state, like the Random stream. Main thread only (§4.11); deterministic (§1.3).

namespace Engine {

	class NativeModuleTypes;
	class PlaySession;
	class XXH64Hasher;

	class NativeWorld
	{
	public:
		// Restricts construction to Create; CreateScope still reaches the constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class NativeWorld;
		};

		NativeWorld(ConstructionKey key, Ref<const NativeModuleTypes> types);
		~NativeWorld();

		NativeWorld(const NativeWorld&) = delete;
		NativeWorld& operator=(const NativeWorld&) = delete;

		// The runtime of `types` (non-null, asserted), its resources at their defaults. Errors: none today; a Result so a
		// later resource rule can refuse a session.
		[[nodiscard]] static Result<Scope<NativeWorld>> Create(Ref<const NativeModuleTypes> types);

		// The NativeStep phase of `session` (which owns this world) for `step` (see the file comment).
		void Step(PlaySession& session, const SimStep& step);

		// The native block (see the file comment). Pure.
		void AppendStateHash(XXH64Hasher& hasher) const;

		// Every resource's current value, {"<Name>": <canonical JSON>, ...} in registration order (native.status in play).
		[[nodiscard]] Json GetResources() const;

		[[nodiscard]] const NativeModuleTypes& GetTypes() const;
	private:
		// The types, the resources' bytes and the step's per-system state (NativeWorld.cpp).
		struct State;
	private:
		Scope<State> m_State;
	};

}
