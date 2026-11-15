#pragma once

#include "Engine/Core/Base.h"
#include "Engine/Core/Result.h"
#include "Engine/Core/UUID.h"
#include "Engine/Core/VfsPath.h"
#include "Engine/Reflection/VariantValue.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Scripting/TrackingAllocator.h"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	class IScriptHost;
	class IScriptModuleReader;
	class Random;
	class ScriptApiRegistry;
	class ScriptEngine;
	class TypeRegistry;
	struct ScriptData;

	namespace Detail {

		struct SandboxAccess;

	}

	// ProcessContext's Luau startup step, on the main thread before VMs, compilers, Analysis or import jobs exist.
	// Initializes every released non-experimental Luau fast flag and installs the engine assert bridge once (§3).
	// InvalidState when already initialized. Workers never change these globals. No VM is created by this call.
	[[nodiscard]] Status InitializeScriptingRuntime();
	// Read-only process status; workers may query it but must never initialize or change runtime flags.
	[[nodiscard]] bool IsScriptingRuntimeInitialized() noexcept;
	// Main-thread, after every VM/Analysis user and import job has stopped; reverses owned process hooks. A no-op
	// when initialization never succeeded. Does not reset flags while any worker could still read them.
	void ShutdownScriptingRuntime();

	enum class SandboxMode : uint8_t
	{
		Runtime,
		LoadTime
	};

	// Detached evaluation result, shared with ScriptEngine. Value is the first return value (null for no return/nil);
	// Value owns immutable JSON through VariantValue (Get() reads it); Prints contains output in call order.
	// No VM strings, pointers or table identities escape, and public headers need no complete Json definition.
	struct ScriptEvaluation
	{
		VariantValue Value{};
		std::vector<std::string> Prints{};
	};

	struct SandboxSpecification
	{
		SandboxMode Mode = SandboxMode::Runtime;
		uint32_t MemoryLimitMB = 256;
		uint32_t CallbackBudgetMs = 1000;
		bool IsTestRun = false;
		// For standalone evaluation. An owning ScriptEngine's ReadOnly policy also applies; false cannot override it.
		// Shared session random advancement/reseeding is a mutation; local Random.New instances remain writable.
		bool ReadOnly = false;
		// Borrowed back-references, outlive Sandbox. Host may be null for pure execution; runtime API calls then fail
		// with located errors. Runtime RandomStream is required and is the same stream returned by Host, if present.
		IScriptHost* Host = nullptr;
		// Optional owning engine; null for load-time/pure execution. Engine owns this Sandbox, never another VM.
		// Null with Runtime retains runtime limits but permits only members that are Runtime eligible and LoadTime available;
		// other APIs reject before their callbacks with "<API> requires an active script engine". No runtime counters.
		ScriptEngine* Engine = nullptr;
		// Frozen registry, borrowed for VM lifetime. Null builds a VM-owned PureScriptApi (below); no duplicate function
		// table. A PureScriptApi's registry built once may be passed instead and shared by VMs without an engine, on any
		// threads. Load-time dispatch rejects unavailable APIs before any host use.
		ScriptApiRegistry* Api = nullptr;
		Random* RandomStream = nullptr;
		IScriptModuleReader* SourceModules = nullptr;
		// Copied provider for trusted, engine-produced/hash-verified ScriptData. Dist requires this for require and
		// never falls back to SourceModules. Callable captures must outlive Sandbox; it runs on the VM owner thread.
		std::function<Result<Ref<const ScriptData>>(const VfsPath&)> CookedModules{};
		// The safety clocks of the callback budget (ScriptWatchdog), never simulation time: copied callables with explicit
		// captures, each a finite, nonnegative and monotonic seconds source on the VM's thread. ClockSeconds is the wall clock
		// every interrupt compares (empty: lua_clock); CpuClockSeconds is what the budget counts, sampled only at an outermost
		// entry, at host periods' boundaries and once the wall deadline has passed (empty: ThreadClock::GetCpuSeconds, the VM
		// thread's CPU time; Docs/Decisions/0039-script-budget-cpu-time.md). A supplied ClockSeconds without a CpuClockSeconds
		// is the budget's only clock (the load-time VMs pass the import thread's CPU time there). Supplying both makes tests
		// exact without sleeps.
		std::function<double()> ClockSeconds{};
		std::function<double()> CpuClockSeconds{};
	};

	// The pure script API: every built-in component type (RegisterBuiltinComponents) and every binding (RegisterBindings),
	// both frozen, with no host. A Sandbox without SandboxSpecification::Api builds one for itself. One built once can be
	// borrowed by many VMs instead (Docs/Decisions/0041-script-check-performance.md decision 3): it is immutable after
	// Create, and a VM without an engine binds and dispatches it without touching its coverage counters (ScriptApiRegistry),
	// so load-time VMs on different import threads may share it. The editor builds one, and its script imports' load-time
	// VMs borrow it (ImportContext::GetLoadTimeScriptApi). Not copyable or movable: the API borrows the type registry.
	class PureScriptApi
	{
	public:
		// Restricts construction to Create while allowing CreateScope to call the public constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class PureScriptApi;
		};

		explicit PureScriptApi(ConstructionKey key); // use Create
		~PureScriptApi();
		PureScriptApi(const PureScriptApi&) = delete;
		PureScriptApi& operator=(const PureScriptApi&) = delete;

		// Registers and freezes both registries on the calling thread. Errors: those of RegisterBindings.
		[[nodiscard]] static Result<Scope<PureScriptApi>> Create();

		// The frozen API, for SandboxSpecification::Api and LoadTimeVmSpecification::Api; valid for this object's lifetime.
		[[nodiscard]] ScriptApiRegistry& GetApi() { return *m_Api; }
	private:
		Scope<TypeRegistry> m_Types{}; // declared first, so it is destroyed after the API that borrows it
		Scope<ScriptApiRegistry> m_Api{};
	};

	// Owns one VM, its allocator, deadline stack, resolver, module-return cache and immutable loaded source maps.
	// Every loaded compilation has a private VM chunk identity separate from its authored diagnostic ChunkName;
	// identities/maps remain interned until VM destruction, even across eval overlap, hot reload and closure release.
	// A reused authored label never replaces an older map. Identity reuse requires byte-for-byte identical bytecode
	// AND every source-map field; hashes alone do not establish equality. Map/index storage and any retained comparison
	// bytes count against the same allocator budget, including its recovery/stop policy. Private identities are neither
	// persisted nor exposed as diagnostic paths; locations always use the map belonging to the executing closure.
	// Own-thread only; runtime VMs on
	// the main thread, isolated load-time VMs on import workers. ScriptEngine uses the private SandboxAccess bridge
	// for callback/thread operations through ScriptCall; no VM pointer, vendor registry reference or stack is public.
	class Sandbox
	{
	public:
		// Restricts construction to Create while allowing CreateScope to call the public constructor.
		class ConstructionKey
		{
			ConstructionKey() = default;
			friend class Sandbox;
		};

		explicit Sandbox(ConstructionKey key); // use Create
		~Sandbox();
		Sandbox(const Sandbox&) = delete;
		Sandbox& operator=(const Sandbox&) = delete;

		// Requires process initialization. Registers libraries/bindings before sealing shared globals and library
		// tables; each module has private globals. No os capabilities, file/network/loading/debug escape functions.
		// DetMath replaces all §4.12 math transcendental functions; random/randomseed use the session stream, or an
		// owned fixed seed 0 for LoadTime. LoadTime exposes pure libraries + Script/Field/Color/Quat/Math/Test.Suite.
		// rebound math.random/randomseed check read-only policy before touching a shared runtime stream, including
		// calls through aliases. A pure load-time VM may use its own fixed-seed stream without a runtime host.
		// With a runtime host, validated random mutations also call ScriptCall::PrepareHostMutation immediately before
		// changing the stream, preserving the outer execution origin and its once-per-slice notification latch.
		// In LoadTime, every other engine API gives a located "<API> is not available at load time" error; Host is null.
		// Runtime with no Engine permits only members that are Runtime eligible and LoadTime available; every other API rejects
		// before callback/host access with "<API> requires an active script engine". It retains its configured runtime
		// deadline/allocator and injected random stream; it does not become a LoadTime VM. Hosted Runtime is unchanged.
		// After luaL_sandbox AND every luaL_sandboxthread, clear safeenv through the public API: readonly protection
		// remains but optimized builtin dispatch cannot bypass rebound functions. Apply also to require/task threads.
		// LoadTime always uses a 250 ms graph deadline. Dist rejects LoadTime with Unsupported; runtime remains capped.
		// Errors: InvalidState before initialization; InvalidArgument for invalid limits, mode or missing runtime stream.
		[[nodiscard]] static Result<Scope<Sandbox>> Create(const SandboxSpecification& specification);

		// Development-only top-level execution, compiled solely by ScriptCompiler. SourceModules supplies requires;
		// one deadline spans root and all requires, cache is per Sandbox. A failed module is never cached as success.
		// Successful return values remain privately cached (including functions); nothing exposes vendor references.
		// Dist: Unsupported before compilation, reading source or creating an execution thread.
		[[nodiscard]] Status RunSource(const VfsPath& path, std::string_view source);
		// Executes only trusted engine-compiled ScriptData from the cooker/verified asset loader. A hash alone does
		// not authenticate arbitrary bytecode. No source fallback in Dist. SourceMap.Path is diagnostic attribution;
		// require retains RequireResolver's strict .luau importer policy. Synthetic labels never become file paths.
		[[nodiscard]] Status RunModule(const ScriptData& script);

		// Executes a fresh snippet in this VM under the same protected boundary, deadlines, require cache and access
		// policy as ScriptEngine. Does not cache the snippet as a required module. Optional entity supplies self through
		// the owning engine. Path is optional diagnostic origin; no source file is required or probed. Evaluation uses
		// ScriptCompileMode::ExpressionOrChunk, so both expressions and complete return chunks retain authored locations.
		// Supplying entity without an engine is InvalidState; a missing entity is NotFound. Source is Unsupported in Dist.
		// Converts the first return to finite, acyclic JSON (scalars, arrays, string-keyed objects; a vector, Color or Quat at
		// any depth as the array of its components, reflection's JSON spelling: ADR 0038, friction F01), rejecting other
		// functions/threads/userdata, mixed-key tables or cycles with located Script errors. Captures print strings.
		// Native JSON conversion has a separate allowance of min(4 MiB, MemoryLimitMB MiB), charging 256 bytes per
		// expanded value/table entry and four per string/key byte. Repeated aliases are charged each time. Exhaustion
		// rejects the value with Script/Runtime without a VM allocator breach; the next evaluation remains usable.
		// Traversal polls the inherited deadline, including after the snippet returns; depth is at most MaxJsonDepth.
		// Loaded source maps use private immutable identities, so retained closures from earlier calls still resolve
		// their own path/pointer/wrapper offset when a later call reuses the same authored diagnostic label.
		[[nodiscard]] Result<ScriptEvaluation> Evaluate(std::string_view source, const VfsPath& path,
			std::optional<UUID> entity = std::nullopt);
		// Same evaluation semantics for trusted engine-produced bytecode, but for its value, which stays strict JSON: a
		// vector, Color or Quat is refused like every other value that is not JSON, so a predicate that returns one by
		// mistake fails instead of holding as a truthy array (ADR 0038's milestone review; only Evaluate, script.eval's
		// path, converts them). Available in Dist. Replay Expect predicates (every player's, test.run's and input.record's
		// expectations) and play.waitFor use this path, with no source compilation or second VM. Module/class loading uses
		// RunModule instead.
		// Embedded diagnostics name SourceMap.Path, prefix Message with its JSON pointer when present (so replay
		// Expect index survives cooking), and map bytecode lines through GeneratedPrefixLines to authored source lines.
		// A fileless chunk uses its synthetic diagnostic label. Evaluation entries carry private Eval origin; nested calls
		// inherit it and deferred tasks retain the origin value for a fresh notification latch on their later resume.
		[[nodiscard]] Result<ScriptEvaluation> ExecuteBytecode(const ScriptData& script,
			std::optional<UUID> entity = std::nullopt);

		// Owned last failure, cleared on a new public execution. Script/Timeout failures additionally carry the
		// structured kind, location and traceback here. Compiler failures map to Compile. The protected owner maps
		// instance/tick/callback identity and publishes via IScriptHost; this class never logs duplicate diagnostics.
		[[nodiscard]] std::optional<ScriptError> GetLastError() const;
		[[nodiscard]] ScriptMemoryState GetMemoryState() const noexcept;
		// Second/hard breach prevents further execution (InvalidState). First breach unwinds with Memory, lets the
		// owner disable the instance, then full-GC/rearm through the private wrapper before another call can execute.
		// Public execution without an instance discards the failed scope and performs that recovery before returning.
		[[nodiscard]] bool IsStopped() const noexcept;
	private:
		struct State;
		// Evaluate's and ExecuteBytecode's execution and conversion of the first return: `componentArrays` converts a
		// vector, Color or Quat at any depth to the array of its components (Evaluate), otherwise it is refused
		// (ExecuteBytecode).
		[[nodiscard]] Result<ScriptEvaluation> ExecuteEvaluation(const ScriptData& script, std::optional<UUID> entity, bool componentArrays);

		Scope<State> m_State{};
		friend struct Detail::SandboxAccess;
	};

}
