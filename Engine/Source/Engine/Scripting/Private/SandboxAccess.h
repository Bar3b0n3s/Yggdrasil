#pragma once

#include "Engine/Core/Json/Json.h"
#include "Engine/Scripting/Private/ScriptCall.h"
#include "Engine/Scripting/Sandbox.h"

#include <span>

namespace Engine {

	class RequireResolver;
	class ScriptWatchdog;

	namespace Detail {

		[[nodiscard]] bool IsScriptingRuntimeInitialized() noexcept;

		// Copied per-entry metadata, never an entity/component reference. Push before dispatch and capture errors before
		// PopContext. Nested requires retain caller entity/tick/callback while changing the source path as appropriate.
		// This diagnostic stack is separate from ScriptExecutionContext's origin/mutation latch; pushing it never starts
		// a new execution slice or changes that origin/latch.
		struct SandboxExecutionContext
		{
			std::string Script{}; // project-relative Assets/... path or synthetic diagnostic label
			std::string Callback{};
			UUID Entity{};
			std::string EntityName{};
			uint64_t Tick = 0;
		};

		// Private bridge to the ONE state owned by Sandbox. All calls are on its owner thread, borrow a successfully
		// created Sandbox, and never create another VM. No VM/src headers are required. ProtectedCall/ProtectedResume
		// own stack guards and traceback capture through public Luau APIs; ScriptEngine alone publishes runtime errors.
		struct SandboxAccess
		{
			// Shared evaluation/binding conversion. Protected entry only; leaves the stack unchanged. Rejects cycles,
			// non-JSON values and depth > MaxJsonDepth. Repeated aliases count on every expansion: the native allowance
			// is min(4 MiB, VM soft limit), charged 256 bytes per value/entry and four per string/key byte. This bounds
			// temporary/output storage separately from VM allocations. Limit rejection is Script (Runtime), not a VM
			// allocator breach; the caller may continue after cleanup. Polls the inherited safety deadline throughout.
			[[nodiscard]] static Result<Json> ReadJson(ScriptCall& call, int index);

			// Appends to the active evaluation's ordered print sink; otherwise a no-op. Never writes a log entry.
			// The registered Log.Info/print callback owns logging, so forwarding here cannot duplicate its output.
			static void CapturePrint(Sandbox& sandbox, std::string_view message);
			// Synchronous native operation through B's protected trampoline. The callable is borrowed for this call only.
			// Results remain on the main stack; callers restore/pop them. Uses the already active outer origin if any.
			[[nodiscard]] static Result<ScriptCallResult> RunNative(Sandbox& sandbox, const std::function<int(ScriptCall&)>& operation);
			// Process-wide nonzero VM identity (ADR0019), never a host/session serial and never reset on reinitialization.
			[[nodiscard]] static uint64_t GetGeneration(const Sandbox& sandbox) noexcept;
			// Live states only. Owner data is installed by Sandbox on the root and every child; null for foreign states.
			// Sandbox owns callback userdata and thread-data slots. Binding code must not overwrite them.
			[[nodiscard]] static Sandbox* FromState(lua_State* state) noexcept;
			// Protected entry only. Leaves one thread object rooted on call.State's stack; caller must pin before popping.
			// Installs owner data, private globals and safeenv=false. Returned state borrows the same VM.
			[[nodiscard]] static Result<lua_State*> CreateThread(ScriptCall& call);
			// Protected entry only. Loads once or pushes the cached return of this canonical ScriptData origin.
			// Success always leaves exactly ONE value, including nil. Failure leaves the caller stack unchanged.
			[[nodiscard]] static Status PushModule(ScriptCall& call, const ScriptData& script);
			// Cache-only lookup: NotFound is distinct from cached nil. Same single-value/unchanged-failure stack rule.
			[[nodiscard]] static Status PushModuleReturn(ScriptCall& call, const VfsPath& path);
			// Canonically ordered authored module paths in the live cache (never the provisional candidate cache).
			// Protected entry only. Includes modules loaded by evaluation, and entries whose return is nil.
			[[nodiscard]] static Result<std::vector<std::string>> GetLoadedModulePaths(ScriptCall& call);
			// Staged-cache inventory and live-cache lookup, independent of the cache selected for candidate execution.
			[[nodiscard]] static Result<std::vector<std::string>> GetReloadModulePaths(ScriptCall& call);
			[[nodiscard]] static Status PushLiveModuleReturn(ScriptCall& call, const VfsPath& path);
			// One private cache transaction. Candidates are pinned for its lifetime; canonical paths must be unique.
			// Starts EMPTY: even unchanged dependencies re-execute privately, with no live closures/upvalues shared.
			// Candidate execution uses load-time API availability and a private seed-0 RNG, under the inherited runtime
			// deadline/allocator. Live cache and resolver stay untouched. The owner canonicalizes exports after evaluation.
			[[nodiscard]] static Status BeginReload(ScriptCall& call, std::span<const Ref<const ScriptData>> candidates);
			[[nodiscard]] static Status EvaluateReloadModule(ScriptCall& call, const ScriptData& script);
			// Registry dispatch must select LoadTime availability while true, before any callback/counter. ThreadCall
			// supplies a null Engine in this scope. It ends before patch/commit; retained code later has normal runtime APIs.
			[[nodiscard]] static bool IsReloadEvaluating(const Sandbox& sandbox) noexcept;
			// Parent owns in-place class/table patching after all candidate evaluations succeed. This replaces a staged
			// cache result with the parent's preserved table identity, without changing the live cache yet.
			[[nodiscard]] static Status SetReloadModule(ScriptCall& call, const ScriptData& script, int valueIndex);
			// For unchanged dependencies, replace the privately evaluated return with its live canonical value after
			// reconnecting candidate references. New dependencies keep their new return. No script executes during sealing.
			[[nodiscard]] static Status SetReloadModuleReturn(ScriptCall& call, const VfsPath& path, int valueIndex);
			// Provisionally publishes only after every candidate succeeded, retaining the old cache for rollback even
			// after this call returns. The outer wrapper can still reject a timeout/memory latch. Never patches tables.
			[[nodiscard]] static Status CommitReload(ScriptCall& call);
			// Restores the old cache even after provisional commit. Never mutates module objects. Safe after protected
			// unwinding, idempotent; parent independently restores its backed-up table fields without allocation.
			static void RollbackReload(Sandbox& sandbox) noexcept;
			// Releases backups only AFTER the outer protected wrapper reports success. No VM allocation or callbacks.
			static void FinalizeReload(Sandbox& sandbox) noexcept;
			// Stack-only facades; Engine is nullable for pure/load-time and engine-less Runtime VMs. ThreadCall accepts a live root/child state
			// already owned by this Sandbox, checked with lua_mainthread; null/foreign states return InvalidArgument.
			// MemberName borrows registry text until dispatch returns. No ownership or registry/thread reference escapes.
			// Execution borrows GetExecution(); outer owners enter a scope before constructing a runtime call. Nested
			// wrappers reuse it. Neither facade owns the execution context, and no deferred record may keep its address.
			[[nodiscard]] static ScriptCall MainCall(Sandbox& sandbox);
			[[nodiscard]] static Result<ScriptCall> ThreadCall(Sandbox& sandbox, lua_State* state, std::string_view memberName = {});

			// Borrowed services, nonnull for a live Sandbox. GetRandom selects the shared runtime stream or the owned
			// seed-0 load-time stream. GetApi returns the frozen registry selected/created during Sandbox::Create.
			[[nodiscard]] static ScriptWatchdog* GetWatchdog(Sandbox& sandbox);
			[[nodiscard]] static TrackingAllocator* GetAllocator(Sandbox& sandbox);
			[[nodiscard]] static RequireResolver* GetResolver(Sandbox& sandbox);
			[[nodiscard]] static ScriptApiRegistry* GetApi(Sandbox& sandbox);
			[[nodiscard]] static Random* GetRandom(Sandbox& sandbox);
			[[nodiscard]] static uint32_t GetBudgetMs(const Sandbox& sandbox); // effective runtime budget, or load-time 250
			[[nodiscard]] static double SampleClock(Sandbox& sandbox);
			// Configured mode, never the effective API-availability subset. Engine-less Runtime still returns Runtime;
			// registry dispatch permits only members that are Runtime eligible and LoadTime available, rejecting others before callbacks with a located
			// "<API> requires an active script engine" rejection. Do not borrow LoadTime's 250 ms policy or error text.
			[[nodiscard]] static SandboxMode GetMode(const Sandbox& sandbox);

			// Effective policy is specification.ReadOnly OR the owning engine's IsReadOnly(). Host mutation additionally
			// requires runtime mode, an engine/host and active non-Pure execution; failure is InvalidState. Local values do not call
			// CheckWritable. Registry dispatch and host helpers use this same guard; load-time availability is checked first.
			[[nodiscard]] static bool IsReadOnly(const Sandbox& sandbox);
			[[nodiscard]] static Status CheckWritable(const Sandbox& sandbox);
			// Rebound math.random/randomseed call this BEFORE advancing/reseeding; InvalidState for read-only runtime.
			// It permits the owned pure load-time stream and does not gate local Random.New generators or DetMath calls.
			// Permission only: host-backed overrides must also call ScriptCall::PrepareHostMutation after validation,
			// immediately before changing the stream. A standalone hostless stream cannot invalidate a recording.
			[[nodiscard]] static Status CheckRandomWritable(const Sandbox& sandbox);

			// Every protected wrapper (and the import owner's outer require-graph scope) enters/leaves here. Entry rejects
			// terminal/pending recovery with InvalidState, validates the clock, and enters the watchdog's inherited deadline.
			// An idle-to-active entry owns a fresh ScriptExecutionContext {origin, false}; nested entries INHERIT the same
			// context regardless of their origin argument. Only outer entry resets safety/mutation latches and deadline.
			// Owners choose Gameplay for frame/lifecycle, TestDriver for suite/case, Eval for evaluation, Pure for hostless
			// extraction/setup. Nested lifecycle calls caused by an Eval/TestDriver write inherit that external origin.
			// The import scope begins before source reads/compilation. CheckInterrupt(-1) at these host boundaries too.
			[[nodiscard]] static Status EnterProtected(Sandbox& sandbox, ScriptExecutionOrigin origin = ScriptExecutionOrigin::Pure);
			// Borrowed until the OUTERMOST LeaveProtected; null when idle. PrepareHostMutation uses this one latch, and
			// notifies exactly once before the first validated external write, even if that write subsequently faults.
			[[nodiscard]] static ScriptExecutionContext* GetExecution(Sandbox& sandbox);
			// Deferred task/case records copy ONLY this value. Each later outer resume enters with the retained origin
			// and a fresh latch, so a recording started between slices is invalidated by the next external write too.
			[[nodiscard]] static ScriptExecutionOrigin GetExecutionOrigin(const Sandbox& sandbox); // Pure when idle
			// Exactly one leave per successful entry, including yields/errors; no stack access, recovery or publication.
			// Classify/copy the failure BEFORE leaving. A later outer task/test resume then receives a fresh deadline.
			static void LeaveProtected(Sandbox& sandbox);

			[[nodiscard]] static Status PushContext(Sandbox& sandbox, const SandboxExecutionContext& context);
			static void PopContext(Sandbox& sandbox);                                        // LIFO; asserts on unmatched pop
			[[nodiscard]] static SandboxExecutionContext GetContext(const Sandbox& sandbox); // empty when no context
			// Inside a protected entry, BEFORE luau_load: owns/interns an immutable copy of the source map and returns its private
			// VM-loaded name (borrowed, stable until Sandbox destruction). The loader MUST use this name, never the
			// authored SourceMap.ChunkName. Distinct loads get distinct identities; optional deduplication requires exact
			// bytecode bytes and equality of EVERY source-map field, not only a label/path/hash. No old identity is rebound.
			// Map/index allocations and any retained equality-comparison bytes use the VM's TrackingAllocator budget;
			// metadata remains charged until VM teardown, including after failed loads or collection of old closures.
			// Validation for malformed metadata; Script for budget failure under the same memory recovery/stop policy.
			// No file access, VM creation or bytecode execution. SourceMap.ChunkName itself is preserved as authored.
			[[nodiscard]] static Result<std::string_view> RegisterLoadedChunk(Sandbox& sandbox, const ScriptData& script);
			// vmChunkName is the PRIVATE VM-loaded name returned above and reported by the executing frame, never an
			// authored label. Looks up that exact immutable record; overlapping eval/reload closures cannot see a newer
			// map for the same authored label. No authored-label fallback on a miss; unknown identities return empty
			// locations and never leak the private name. File is the record's diagnostic Path, with
			// optional JsonPointer; GeneratedPrefixLines maps VM lines back to authored lines, preserving columns. Unknown
			// or generated-only coordinates remain 0, never underflow. A fileless map uses its authored synthetic label.
			// Wrappers use File for ScriptError.Script/trace frames and prefix the error
			// Message with JsonPointer when present. Thus embedded replay failures identify the replay and Expect index.
			[[nodiscard]] static ErrorLocation Locate(const Sandbox& sandbox, std::string_view vmChunkName, uint32_t line, uint32_t column = 0);

			// Never raises or publishes. gc >= 0 immediately returns nullopt WITHOUT sampling time or consuming faults.
			// Otherwise checks allocator before watchdog, latching Memory ahead of Timeout until the outer entry ends.
			// The interrupt trampoline raises only outside GC. Wrappers also poll before accepting successful results, so
			// script pcall cannot swallow a safety failure. Memory accounting remains latched until recovery/terminal stop.
			[[nodiscard]] static std::optional<ScriptErrorKind> CheckInterrupt(Sandbox& sandbox, int gc);
			// Typed status/latch classification, never message matching: memory/ERRMEM wins, then timeout, then syntax
			// (Compile); other error statuses are Runtime. OK/YIELD is nullopt only without a safety latch. Copies no frames;
			// wrappers capture those before unwinding/resetting, attach GetContext(), then return ScriptCallResult::Failure.
			[[nodiscard]] static std::optional<ScriptErrorKind> ClassifyFailure(const Sandbox& sandbox, int vmStatus);
			// Stores an owned last failure only. Does not assign stream IDs, deduplicate, log, disable instances or notify
			// IScriptHost. The outer runtime owner publishes once; the load-time owner converts to an import Error.
			static void RecordFailure(Sandbox& sandbox, const ScriptError& error);
			// Starts an explicit host operation's diagnostic capture; never changes its deadline or safety latch.
			static void ClearFailure(Sandbox& sandbox) noexcept;
			// After protected unwinding and owner instance-disable/discard, with no active deadline. Full GC then allocator
			// FinishRecovery; inability to return under soft latches stop. InvalidState unless first recovery is pending.
			// No error publication here; second/hard breaches stop and are never rearmed by this operation.
			[[nodiscard]] static Status RecoverMemory(Sandbox& sandbox);
		};

	}

}
