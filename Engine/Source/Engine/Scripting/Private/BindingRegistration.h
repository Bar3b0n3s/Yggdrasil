#pragma once

#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Random.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/Private/ScriptCall.h"
#include "Engine/Scripting/ScriptReference.h"
#include "Engine/Scripting/ScriptTestHost.h"

namespace Engine {

	namespace Detail {

		// The registry owns this tag space. Payloads contain values only; no engine, scene or component addresses.
		enum class ScriptValueTag : int
		{
			Entity = 1,
			Quat = 2,
			Color = 3,
			AssetRef = 4,
			RandomGenerator = 5,
			TaskHandle = 6,
			Field = 7,
			ForwardedError = 8,
			ComponentBegin = 16
		};

		// Registers every built-in binding without freezing, so fixtures can append probes. The public entry freezes.
		[[nodiscard]] Status RegisterBuiltinBindings(ScriptApiRegistry& api, const TypeRegistry& types);

		// Detached metadata from an authenticated constructor result. Module tables with a similar shape do not qualify.
		// Field records and authentication live in the VM registry, are GC traced, and consume the VM allocator budget.
		struct ScriptRegistration
		{
			ScriptKind Kind = ScriptKind::Module;
			std::string Name{};
			std::vector<ScriptFieldSchema> Fields{};
			uint32_t CaseTimeoutTicks = 0;
			ErrorLocation Location{};
		};

		[[nodiscard]] Result<ScriptRegistration> InspectScriptRegistration(ScriptCall& call, int index);
		// Reload transaction only, inside protection. Push leaves exactly one opaque record (nil for an ordinary
		// Module); preserve it on a traced stack/table for rollback. Set copies a record returned by Push onto a class
		// identity, or removes authentication for nil. Records must NEVER be exposed to script or modified by callers.
		// Metadata includes the owned name/field schema/location and, for suites, the retained body/default timeout.
		// Root table patching alone does not update this private weak-key association.
		// Set(nil) is allocation-free when the association is already absent, including after a failed first install.
		[[nodiscard]] Status PushScriptRegistrationRecord(ScriptCall& call, int tableIndex);
		[[nodiscard]] Status SetScriptRegistrationRecord(ScriptCall& call, int tableIndex, int recordIndex);
		// A null assignment detaches; a nonnull assignment must load as persistent authenticated Behaviour data.
		[[nodiscard]] Status ValidateBehaviourAssignment(IScriptHost& host, AssetHandle handle);
		// Pushes one authenticated suite body. Stack unchanged on failure. Does not execute the function.
		[[nodiscard]] Status PushSuiteBody(ScriptCall& call, int index, ScriptCollectedSuite& suite, uint32_t& defaultTimeout);
		// D registers this callback as Test.Suite (All, Mutates=false); the constructor never runs its body.
		int CreateTestSuite(ScriptCall& call);
		// Private registry initialization, called by Bind before sandboxing.
		void InitializeBindingMetadata(ScriptCall& call);
		[[nodiscard]] int GetScriptValueTag(const ScriptApiRegistry& api, std::string_view type);
		[[nodiscard]] ErrorLocation ScriptCallerLocation(ScriptCall& call, int level = 1);

	}

	namespace Lua {

		// Detached JSON for host scene parameters: nil maps to null, dense positive integer keys to arrays, string
		// keys to objects (an empty table is an object). Rejects mixed/sparse keys, cycles, nonfinite numbers and
		// non-JSON values. Neither conversion invokes table metamethods; strings and keys retain embedded NULs.
		// Uses SandboxAccess's bounded JSON conversion; raises a located error before any host mutation on failure.
		[[nodiscard]] Json CheckJson(ScriptCall& call, int index);
		void PushJson(ScriptCall& call, const Json& value);

		// Construction-time conversion before the candidate component exists in the ECS.
		[[nodiscard]] Value CheckValueForOwner(ScriptCall& call, int index, const FieldInfo& field, const ResolveContext& resolve);

		// Borrowed for the native entry only. Setters validate their candidate before assigning. No host mutation guard:
		// these are local value payloads. Constructors use Push<T>, ordinary reads use Check<T>.
		[[nodiscard]] glm::vec4& CheckColorStorage(ScriptCall& call, int index);
		[[nodiscard]] glm::quat& CheckQuatStorage(ScriptCall& call, int index);
		[[nodiscard]] Random& CheckRandomStorage(ScriptCall& call, int index);
		void PushRandomGenerator(ScriptCall& call, const Random& random);
		void PushTaskHandle(ScriptCall& call, ScriptReference reference);

	}

	namespace ScriptBindings {

		[[nodiscard]] Status RegisterScript(ScriptApiRegistry& api);
		[[nodiscard]] Status RegisterEntity(ScriptApiRegistry& api);
		[[nodiscard]] Status RegisterComponents(ScriptApiRegistry& api);
		[[nodiscard]] Status RegisterScene(ScriptApiRegistry& api);
		[[nodiscard]] Status RegisterInput(ScriptApiRegistry& api);
		[[nodiscard]] Status RegisterTime(ScriptApiRegistry& api);
		[[nodiscard]] Status RegisterPhysics(ScriptApiRegistry& api);
		[[nodiscard]] Status RegisterAudio(ScriptApiRegistry& api);
		[[nodiscard]] Status RegisterAssets(ScriptApiRegistry& api);
		[[nodiscard]] Status RegisterTask(ScriptApiRegistry& api);
		[[nodiscard]] Status RegisterRandom(ScriptApiRegistry& api);
		[[nodiscard]] Status RegisterMath(ScriptApiRegistry& api);
		[[nodiscard]] Status RegisterQuat(ScriptApiRegistry& api);
		[[nodiscard]] Status RegisterColor(ScriptApiRegistry& api);
		[[nodiscard]] Status RegisterDebug(ScriptApiRegistry& api);
		[[nodiscard]] Status RegisterLog(ScriptApiRegistry& api);
		[[nodiscard]] Status RegisterApplication(ScriptApiRegistry& api);
		[[nodiscard]] Status RegisterTest(ScriptApiRegistry& api);

	}

}
