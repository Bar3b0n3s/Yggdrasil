#include "EnginePCH.h"
#include "Engine/Scripting/Sandbox.h"

#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/Assert.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/BuiltinComponents.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scripting/LoadTimeVm.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/Private/SandboxAccess.h"
#include "Engine/Scripting/Private/ScriptEngineAccess.h"
#include "Engine/Scripting/RegisterBindings.h"
#include "Engine/Scripting/RequireResolver.h"
#include "Engine/Scripting/ScriptCompiler.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"
#include "Engine/Scripting/Watchdog.h"

#include <Luau/Common.h>
#include <Luau/ExperimentalFlags.h>
#include <Luau/Require.h>
#include <lualib.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <exception>
#include <limits>
#include <memory>
#include <thread>
#include <utility>

namespace Engine {

	namespace {

		// ProcessContext owns these hooks. VM identities are never reset, including on process-context restart.
		std::atomic<bool> s_RuntimeInitialized{ false };
		std::atomic<uint64_t> s_NextVmIdentity{ 1 };
		Luau::AssertHandler s_PreviousAssertHandler = nullptr;

		int ScriptAssert(const char* expression, const char* file, int line, const char* function)
		{
			GetAssertHandler()({ .Kind = AssertKind::Assert, .Expression = expression ? expression : "", .Message = "Luau assertion failed", .File = file ? file : "", .Line = line > 0 ? static_cast<uint32_t>(line) : 0, .Function = function ? function : "" });
			return 1;
		}

		Error ExecutionError(const ScriptError& error)
		{
			ErrorLocation location{};
			location.File = error.Script;
			location.Line = error.Line;
			location.Column = error.Column;
			location.Entity = error.Entity;
			if (!error.JsonPointer.empty())
				location.JsonPointer = error.JsonPointer;
			const auto code = error.Kind == ScriptErrorKind::Timeout ? ErrorCode::Timeout : error.Kind == ScriptErrorKind::Compile ? ErrorCode::CompileFailed
																																   : ErrorCode::Script;
			return Error(code, error.Message).WithLocation(std::move(location));
		}

		struct ProtectedEntry
		{
			Sandbox& Owner;
			~ProtectedEntry() { Detail::SandboxAccess::LeaveProtected(Owner); }
		};

		struct StackRestore
		{
			lua_State* Vm = nullptr;
			int Top = 0;
			int Exceptions = std::uncaught_exceptions();
			~StackRestore()
			{
				// A VM exception carries its error value on this stack until Luau catches it.
				if (std::uncaught_exceptions() == Exceptions)
					lua_settop(Vm, Top);
			}
		};

		struct ContextRestore
		{
			Sandbox& Owner;
			~ContextRestore() { Detail::SandboxAccess::PopContext(Owner); }
		};

		Status CheckOutcome(const Result<ScriptCallResult>& outcome)
		{
			if (!outcome)
				return std::unexpected(outcome.error());
			if (outcome->Failure)
				return std::unexpected(ExecutionError(*outcome->Failure));
			if (outcome->Yielded)
				return MakeError(ErrorCode::Script, "module evaluation cannot yield");
			return {};
		}

	}

	struct Sandbox::State
	{
		struct PrintSink
		{
			std::vector<std::string>& Lines; // borrowed from this evaluation's detached result
			TrackingAllocator& Allocator;
			void* Reservation = nullptr;
			size_t Bytes = 0;
			~PrintSink() { static_cast<void>(Allocator.Reallocate(Reservation, Bytes, 0)); }
		};
		// One allocator block holds the complete immutable source map, including its lookup record and strings.
		struct LoadedMap
		{
			LoadedMap* Next = nullptr;
			size_t AllocationBytes = 0;
			std::string_view Identity{};
			std::string_view File{};
			std::string_view Pointer{};
			std::span<const uint32_t> Offsets{};
			uint32_t Prefix = 0;
		};

		struct ReloadCandidate
		{
			Ref<const ScriptData> Data{};
			bool Evaluated = false;
		};

		Sandbox* Owner = nullptr; // native back-reference; owns this state
		SandboxSpecification Specification{};
		std::thread::id OwnerThread = std::this_thread::get_id();
		Scope<TrackingAllocator> Allocator{};
		lua_State* Vm = nullptr; // sole owning VM handle, closed before allocator destruction
		ScriptWatchdog Watchdog{};
		Scope<RequireResolver> Resolver = CreateScope<RequireResolver>();
		Scope<RequireResolver> ReloadResolver{}; // staged graph, then the original graph until FinalizeReload
		Random LocalRandom{ 0 };
		Random ReloadRandom{ 0 };
		Scope<TypeRegistry> OwnedTypes{};
		Scope<ScriptApiRegistry> OwnedApi{};
		uint64_t Generation = 0;
		uint64_t NextChunk = 1;
		uint32_t Budget = 0;
		LoadedMap* Maps = nullptr; // owned allocator blocks, freed after VM closure
		int ModuleCache = LUA_NOREF;
		int ReloadCache = LUA_NOREF; // staged before commit; backup after provisional commit
		bool ReloadCommitted = false;
		uint32_t ReloadEvaluationDepth = 0;
		std::vector<ReloadCandidate> Candidates{};
		int RequireProxy = LUA_NOREF;
		std::string Navigation{};
		std::optional<ScriptErrorKind> SafetyFault{};
		std::optional<ScriptError> LastError{};
		ScriptExecutionContext Execution{};
		std::vector<Detail::SandboxExecutionContext> Contexts{};
		// Synchronous borrows, scoped around a protected entry; never retained in the VM.
		const std::function<int(ScriptCall&)>* NativeOperation = nullptr;
		PrintSink* Prints = nullptr; // borrowed for the active evaluation; nested evaluations restore the prior sink

		~State()
		{
			if (Vm)
				lua_close(Vm);
			while (Maps)
			{
				LoadedMap* next = Maps->Next;
				const size_t bytes = Maps->AllocationBytes;
				std::destroy_at(Maps);
				static_cast<void>(Allocator->Reallocate(Maps, bytes, 0));
				Maps = next;
			}
		}

		void AssertThread() const
		{
			ENGINE_CORE_VERIFY(OwnerThread == std::this_thread::get_id(), "Sandbox used outside its owner thread");
		}

		static State& From(lua_State* vm)
		{
			Sandbox* owner = Detail::SandboxAccess::FromState(vm);
			ENGINE_CORE_VERIFY(owner != nullptr, "unowned Luau state");
			return *owner->m_State;
		}

		static void* Allocate(void* context, void* memory, size_t oldSize, size_t newSize)
		{
			return static_cast<TrackingAllocator*>(context)->Reallocate(memory, oldSize, newSize);
		}

		static void ThreadLifecycle(lua_State* parent, lua_State* child)
		{
			lua_setthreaddata(child, parent ? lua_getthreaddata(parent) : nullptr);
		}

		static void Interrupt(lua_State* vm, int gc)
		{
			if (gc >= 0)
				return;
			auto& state = From(vm);
			if (const auto fault = Detail::SandboxAccess::CheckInterrupt(*state.Owner, gc))
				luaL_error(vm, "%s", *fault == ScriptErrorKind::Memory ? "script memory limit exceeded" : "script deadline exceeded");
		}

		static int InvokeNative(ScriptCall& call)
		{
			auto& state = *call.Sandbox->m_State;
			ENGINE_CORE_VERIFY(state.NativeOperation != nullptr, "missing synchronous Sandbox operation");
			return (*state.NativeOperation)(call);
		}

		Result<ScriptCallResult> RunNative(lua_State* vm, const std::function<int(ScriptCall&)>& operation)
		{
			struct Restore
			{
				State& Data;
				const std::function<int(ScriptCall&)>* Previous = nullptr;
				~Restore() { Data.NativeOperation = Previous; }
			} restore{ *this, NativeOperation };
			NativeOperation = &operation;
			ENGINE_TRY_ASSIGN(auto call, Detail::SandboxAccess::ThreadCall(*Owner, vm));
			return Lua::ProtectedCall(call, InvokeNative);
		}

		int ActiveCache() const { return ReloadCache != LUA_NOREF && !ReloadCommitted ? ReloadCache : ModuleCache; }
		RequireResolver& ActiveResolver() { return *(ReloadResolver && !ReloadCommitted ? ReloadResolver : Resolver); }

		bool PushCached(lua_State* vm, std::string_view path, int cache = LUA_NOREF)
		{
			lua_getref(vm, cache == LUA_NOREF ? ActiveCache() : cache);
			lua_pushlstring(vm, path.data(), path.size());
			lua_rawget(vm, -2);
			if (lua_isnil(vm, -1))
			{
				lua_pop(vm, 2);
				return false;
			}
			lua_rawgeti(vm, -1, 1); // An empty entry table is a cached nil, distinct from an absent entry.
			lua_remove(vm, -2);
			lua_remove(vm, -2);
			return true;
		}

		std::vector<std::string> CachePaths(lua_State* vm, int cache)
		{
			StackRestore restore{ vm, lua_gettop(vm) };
			lua_getref(vm, cache);
			const int table = lua_gettop(vm);
			std::vector<std::string> paths;
			for (int iterator = 0; (iterator = lua_rawiter(vm, table, iterator)) != -1;)
			{
				size_t size = 0;
				const char* path = lua_tolstring(vm, -2, &size);
				paths.emplace_back(path, size);
				lua_pop(vm, 2);
			}
			std::ranges::sort(paths);
			return paths;
		}

		void CacheValue(lua_State* vm, std::string_view path, int index)
		{
			index = lua_absindex(vm, index);
			lua_getref(vm, ActiveCache());
			lua_pushlstring(vm, path.data(), path.size());
			lua_createtable(vm, 1, 0);
			lua_pushvalue(vm, index);
			lua_rawseti(vm, -2, 1);
			lua_rawset(vm, -3);
			lua_pop(vm, 1);
		}

		Status Poll()
		{
			if (const auto fault = Detail::SandboxAccess::CheckInterrupt(*Owner, -1))
				return MakeError(*fault == ScriptErrorKind::Timeout ? ErrorCode::Timeout : ErrorCode::Script,
					"{}", *fault == ScriptErrorKind::Timeout ? "script deadline exceeded" : "script memory limit exceeded");
			return {};
		}

		Status PushRequired(ScriptCall& call, const VfsPath& path)
		{
			ENGINE_TRY(Poll());
			if (PushCached(call.State, path.GetPath()))
				return {};
			for (auto& candidate : Candidates)
				if (candidate.Data->SourceMap.Path == path.GetPath())
					return Detail::SandboxAccess::PushModule(call, *candidate.Data);
			if (Specification.CookedModules)
			{
				ENGINE_TRY_ASSIGN(auto script, Specification.CookedModules(path));
				ENGINE_TRY(Poll());
				if (!script || script->SourceMap.Path != path.GetPath())
					return MakeError(ErrorCode::Validation, "cooked module provider returned a mismatched origin for '{}'", path);
				return Detail::SandboxAccess::PushModule(call, *script);
			}
#if defined(ENGINE_DIST)
			return MakeError(ErrorCode::Unsupported, "Dist requires a trusted cooked module provider");
#else
			if (!Specification.SourceModules)
				return MakeError(ErrorCode::InvalidState, "no source module reader for '{}'", path);
			ENGINE_TRY_ASSIGN(auto source, ActiveResolver().ReadSource(*Specification.SourceModules, path));
			ENGINE_TRY(Poll());
			ENGINE_TRY_ASSIGN(auto compiled, ScriptCompiler::Compile({ .Path = path, .Source = source }));
			ENGINE_TRY(Poll());
			ScriptData script{};
			script.Bytecode = std::move(compiled.Bytecode);
			script.SourceMap = std::move(compiled.SourceMap);
			return Detail::SandboxAccess::PushModule(call, script);
#endif
		}

		static luarequire_WriteResult WriteNavigation(lua_State* vm, void*, char* buffer, size_t size, size_t* written)
		{
			auto path = From(vm).Navigation;
			if (!path.ends_with(".luau"))
				path += ".luau";
			*written = path.size();
			if (size < path.size())
				return WRITE_BUFFER_TOO_SMALL;
			std::memcpy(buffer, path.data(), path.size());
			return WRITE_SUCCESS;
		}

		static void RequireConfiguration(luarequire_Configuration* configuration)
		{
			configuration->is_require_allowed = [](lua_State* vm, void*, const char* name)
			{
				const auto location = Detail::SandboxAccess::Locate(*From(vm).Owner, name, 0);
				return location.File.starts_with("Assets/") && location.File.ends_with(".luau");
			};
			configuration->reset = [](lua_State* vm, void*, const char* name)
			{
				From(vm).Navigation = Detail::SandboxAccess::Locate(*From(vm).Owner, name, 0).File;
				return From(vm).Navigation.empty() ? NAVIGATE_NOT_FOUND : NAVIGATE_SUCCESS;
			};
			configuration->jump_to_alias = [](lua_State*, void*, const char*)
			{
				return NAVIGATE_NOT_FOUND;
			};
			configuration->to_parent = [](lua_State* vm, void*)
			{
				auto& path = From(vm).Navigation;
				const size_t slash = path.rfind('/');
				if (slash == std::string::npos)
					return NAVIGATE_NOT_FOUND;
				path.resize(slash);
				return NAVIGATE_SUCCESS;
			};
			configuration->to_child = [](lua_State* vm, void*, const char* name)
			{
				auto& path = From(vm).Navigation;
				path += '/';
				path += name;
				return NAVIGATE_SUCCESS;
			};
			configuration->is_module_present = [](lua_State*, void*)
			{
				return true;
			};
			configuration->get_chunkname = WriteNavigation;
			configuration->get_loadname = WriteNavigation;
			configuration->get_cache_key = WriteNavigation;
			configuration->get_config_status = [](lua_State*, void*)
			{
				return CONFIG_ABSENT;
			};
			configuration->get_alias = [](lua_State*, void*, const char*, char*, size_t, size_t*)
			{
				return WRITE_FAILURE;
			};
			configuration->load = [](lua_State* vm, void*, const char*, const char*, const char* loadName)
			{
				auto& state = From(vm);
				auto call = Detail::SandboxAccess::ThreadCall(*state.Owner, vm);
				if (!call)
					luaL_error(vm, "%s", call.error().GetMessageText().c_str());
				auto path = VfsPath::Parse(std::string("project://") + loadName);
				if (!path)
					return Lua::RaiseError(*call, path.error());
				const auto loaded = state.PushRequired(*call, *path);
				if (!loaded)
					return Lua::RaiseError(*call, loaded.error());
				return 1;
			};
		}

		static int Require(lua_State* vm)
		{
			auto& state = From(vm);
			auto call = Detail::SandboxAccess::ThreadCall(*state.Owner, vm, "require");
			if (!call)
				luaL_error(vm, "%s", call.error().GetMessageText().c_str());
			const auto request = Lua::Check<std::string>(*call, 1);
			lua_Debug frame{};
			int level = 1;
			while (lua_getinfo(vm, level++, "s", &frame) && frame.what[0] == 'C')
			{
			}
			if (!frame.source)
				return Lua::RaiseError(*call, "require has no authored module origin");
			const auto location = Detail::SandboxAccess::Locate(*state.Owner, frame.source, 0);
			auto importer = VfsPath::Parse(std::string("project://") + location.File);
			if (!importer)
				return Lua::RaiseError(*call, "require needs a project Assets .luau importer");
			const auto resolved = state.ActiveResolver().Resolve(*importer, request);
			if (!resolved)
				return Lua::RaiseError(*call, resolved.error());
			// Luau's public adapter handles navigation; the engine cache is authoritative, including nil and reload.
			// Clear its disposable cache before every lookup, so even an earlier unwound call cannot expose stale data.
			lua_pushcfunction(vm, luarequire_clearcacheentry, "require.clear");
			lua_pushlstring(vm, resolved->GetPath().data(), resolved->GetPath().size());
			lua_call(vm, 1, 0);
			lua_getref(vm, state.RequireProxy);
			lua_pushlstring(vm, request.data(), request.size());
			lua_pushstring(vm, frame.source);
			lua_call(vm, 2, 1);
			lua_pushcfunction(vm, luarequire_clearcacheentry, "require.clear");
			lua_pushlstring(vm, resolved->GetPath().data(), resolved->GetPath().size());
			lua_call(vm, 1, 0);
			return 1;
		}

		template<double (*Function)(double)>
		static int MathUnary(lua_State* vm)
		{
			lua_pushnumber(vm, Function(luaL_checknumber(vm, 1)));
			return 1;
		}

		template<double (*Function)(double, double)>
		static int MathBinary(lua_State* vm)
		{
			lua_pushnumber(vm, Function(luaL_checknumber(vm, 1), luaL_checknumber(vm, 2)));
			return 1;
		}

		static int MathLog(lua_State* vm)
		{
			const double value = luaL_checknumber(vm, 1);
			const double result = lua_isnoneornil(vm, 2) ? DetMath::Log(value) : DetMath::Log(value) / DetMath::Log(luaL_checknumber(vm, 2));
			lua_pushnumber(vm, result);
			return 1;
		}

		static int MathRandom(lua_State* vm)
		{
			auto& state = From(vm);
			auto call = Detail::SandboxAccess::ThreadCall(*state.Owner, vm, "math.random");
			if (!call)
				luaL_error(vm, "%s", call.error().GetMessageText().c_str());
			const int count = lua_gettop(vm);
			if (count > 2)
				return Lua::RaiseError(*call, "math.random expects zero, one or two arguments");
			const int32_t minimum = count == 2 ? Lua::Check<int32_t>(*call, 1) : 1;
			const int32_t maximum = count != 0 ? Lua::Check<int32_t>(*call, count) : 1;
			if (minimum > maximum)
				return Lua::RaiseError(*call, "math.random interval is empty");
			const auto writable = Detail::SandboxAccess::CheckRandomWritable(*state.Owner);
			if (!writable)
				return Lua::RaiseError(*call, writable.error());
			if (call->Engine)
			{
				const auto mutation = call->PrepareHostMutation();
				if (!mutation)
					return Lua::RaiseError(*call, mutation.error());
			}
			auto* random = Detail::SandboxAccess::GetRandom(*state.Owner);
			lua_pushnumber(vm, count == 0 ? random->NextDouble() : static_cast<double>(random->RangeInt(minimum, maximum)));
			return 1;
		}

		static int MathRandomSeed(lua_State* vm)
		{
			auto& state = From(vm);
			auto call = Detail::SandboxAccess::ThreadCall(*state.Owner, vm, "math.randomseed");
			if (!call)
				luaL_error(vm, "%s", call.error().GetMessageText().c_str());
			const auto seed = Lua::Check<uint32_t>(*call, 1);
			const auto writable = Detail::SandboxAccess::CheckRandomWritable(*state.Owner);
			if (!writable)
				return Lua::RaiseError(*call, writable.error());
			if (call->Engine)
			{
				const auto mutation = call->PrepareHostMutation();
				if (!mutation)
					return Lua::RaiseError(*call, mutation.error());
			}
			Detail::SandboxAccess::GetRandom(*state.Owner)->Seed(seed);
			return 0;
		}

		static int Print(lua_State* vm)
		{
			auto& state = From(vm);
			std::string text;
			const int count = lua_gettop(vm);
			for (int index = 1; index <= count; ++index)
			{
				size_t length = 0;
				const char* value = luaL_tolstring(vm, index, &length);
				if (index != 1)
					text += '\t';
				text.append(value, length);
				lua_pop(vm, 1);
			}
			Detail::SandboxAccess::CapturePrint(*state.Owner, text);
			return 0;
		}

		int Initialize(ScriptCall& call)
		{
			luaL_openlibs(call.State);
			// Environment reflection can expose another module's private globals through an exported closure.
			for (const char* name : { "os", "io", "package", "debug", "loadfile", "dofile", "loadstring", "collectgarbage", "getfenv", "setfenv" })
			{
				lua_pushnil(call.State);
				lua_setglobal(call.State, name);
			}
			lua_getglobal(call.State, "math");
			const luaL_Reg functions[] = {
				{ "sin", MathUnary<DetMath::Sin> }, { "cos", MathUnary<DetMath::Cos> }, { "tan", MathUnary<DetMath::Tan> },
				{ "asin", MathUnary<DetMath::ASin> }, { "acos", MathUnary<DetMath::ACos> }, { "atan", MathUnary<DetMath::ATan> },
				{ "sinh", MathUnary<DetMath::Sinh> }, { "cosh", MathUnary<DetMath::Cosh> }, { "tanh", MathUnary<DetMath::Tanh> },
				{ "exp", MathUnary<DetMath::Exp> }, { "log", MathLog }, { "log10", MathUnary<DetMath::Log10> },
				{ "pow", MathBinary<DetMath::Pow> }, { "atan2", MathBinary<DetMath::ATan2> },
				{ "random", MathRandom }, { "randomseed", MathRandomSeed }, { nullptr, nullptr }
			};
			luaL_register(call.State, nullptr, functions);
			lua_pop(call.State, 1);
			lua_pushcfunction(call.State, Print, "print");
			lua_setglobal(call.State, "print");
			luarequire_pushproxyrequire(call.State, RequireConfiguration, nullptr);
			RequireProxy = lua_ref(call.State, -1);
			lua_pop(call.State, 1);
			lua_pushcfunction(call.State, Require, "require");
			lua_setglobal(call.State, "require");
			lua_newtable(call.State);
			ModuleCache = lua_ref(call.State, -1);
			lua_pop(call.State, 1);
			const auto environment = Specification.Mode == SandboxMode::LoadTime ? ScriptApiEnvironment::LoadTime : Specification.IsTestRun && call.Engine ? ScriptApiEnvironment::Test
																																						   : ScriptApiEnvironment::Runtime;
#if defined(ENGINE_DIST)
			const RunModes defaultMode = RunModes::Dist;
#else
			const RunModes defaultMode = RunModes::Editor;
#endif
			const auto bound = Specification.Api->Bind(call, environment, call.Engine ? call.Engine->GetRunMode() : defaultMode);
			if (!bound)
				return Lua::RaiseError(call, bound.error());
			if (call.Engine)
			{
				lua_getglobal(call.State, "Log");
				if (lua_istable(call.State, -1))
				{
					lua_getfield(call.State, -1, "Info");
					if (lua_isfunction(call.State, -1))
						lua_setglobal(call.State, "print");
					else
						lua_pop(call.State, 1);
				}
				lua_pop(call.State, 1);
			}
			luaL_sandbox(call.State);
			lua_setsafeenv(call.State, LUA_GLOBALSINDEX, false);
			return 0;
		}

		Status Execute(ScriptCall& caller, const ScriptData& script, std::optional<UUID> entity, bool module = false)
		{
			const int base = lua_gettop(caller.State);
			StackRestore restore{ caller.State, base };
			ENGINE_TRY_ASSIGN(auto name, Detail::SandboxAccess::RegisterLoadedChunk(*Owner, script));
			ENGINE_TRY_ASSIGN(auto child, Detail::SandboxAccess::CreateThread(caller));
			auto context = Detail::SandboxAccess::GetContext(*Owner);
			context.Script = script.SourceMap.Path.empty() ? script.SourceMap.ChunkName.substr(1) : script.SourceMap.Path;
			ENGINE_TRY(Detail::SandboxAccess::PushContext(*Owner, context));
			ContextRestore contextRestore{ *Owner };
			if (entity)
			{
				ENGINE_TRY(CheckOutcome(RunNative(child, [entity](ScriptCall& call)
				{
					const auto pushed = Detail::ScriptEngineAccess::PushInstance(call, *entity);
					if (!pushed)
						return Lua::RaiseError(call, pushed.error());
					lua_setglobal(call.State, "self");
					return 0;
				})));
			}
			const auto bytes = AsStringView(script.Bytecode);
			const int loaded = luau_load(child, name.data(), bytes.data(), bytes.size(), 0);
			if (loaded != LUA_OK)
			{
				ScriptError error{};
				error.Kind = loaded == LUA_ERRMEM ? ScriptErrorKind::Memory : ScriptErrorKind::Compile;
				error.Script = context.Script;
				error.JsonPointer = script.SourceMap.JsonPointer;
				error.Message = lua_type(child, -1) == LUA_TSTRING ? lua_tostring(child, -1) : "unable to load trusted script bytecode";
				Detail::SandboxAccess::RecordFailure(*Owner, error);
				Lua::RaiseError(caller, error);
				return std::unexpected(ExecutionError(error));
			}
			ENGINE_TRY_ASSIGN(auto call, Detail::SandboxAccess::ThreadCall(*Owner, child));
			const auto outcome = Lua::ProtectedCall(call, 0, LUA_MULTRET);
			if (outcome && outcome->Failure)
				Lua::RaiseError(caller, *outcome->Failure);
			ENGINE_TRY(CheckOutcome(outcome));
			if (module && outcome->ResultCount > 1)
				return MakeError(ErrorCode::Script, "a required module must return at most one value");
			if (outcome->ResultCount == 0)
				lua_pushnil(caller.State);
			else
			{
				lua_pushvalue(child, 1);
				lua_xmove(child, caller.State, 1);
			}
			lua_remove(caller.State, base + 1); // child stays rooted until its return value has crossed to the caller
			restore.Top = base + 1;
			return {};
		}

		void RecordHostError(const Error& error)
		{
			ScriptError diagnostic{};
			diagnostic.Kind = error.GetCode() == ErrorCode::CompileFailed ? ScriptErrorKind::Compile : error.GetCode() == ErrorCode::Timeout ? ScriptErrorKind::Timeout
																																			 : ScriptErrorKind::Runtime;
			diagnostic.Script = error.GetLocation().File;
			diagnostic.Line = error.GetLocation().Line;
			diagnostic.Column = error.GetLocation().Column;
			diagnostic.JsonPointer = error.GetLocation().JsonPointer.value_or("");
			diagnostic.Message = error.GetMessageText();
			Detail::SandboxAccess::RecordFailure(*Owner, diagnostic);
		}

		void RecoverPublicFailure()
		{
			if (!Specification.Engine && Watchdog.GetDepth() == 0 && Allocator->GetState().NeedsRecovery && !Allocator->GetState().MustStop)
				static_cast<void>(Detail::SandboxAccess::RecoverMemory(*Owner));
		}
	};

	Status InitializeScriptingRuntime()
	{
		if (s_RuntimeInitialized.load(std::memory_order_acquire))
			return MakeError(ErrorCode::InvalidState, "scripting runtime is already initialized");
		for (auto* flag = Luau::FValue<bool>::list; flag; flag = flag->next)
			if (std::string_view(flag->name).starts_with("Luau") && !Luau::isAnalysisFlagExperimental(flag->name))
				flag->value = true;
		s_PreviousAssertHandler = Luau::assertHandler();
		Luau::assertHandler() = ScriptAssert;
		s_RuntimeInitialized.store(true, std::memory_order_release);
		return {};
	}

	void ShutdownScriptingRuntime()
	{
		if (s_RuntimeInitialized.exchange(false, std::memory_order_acq_rel))
			Luau::assertHandler() = s_PreviousAssertHandler;
	}

	Sandbox::Sandbox(ConstructionKey)
		: m_State(CreateScope<State>())
	{
		m_State->Owner = this;
	}

	Sandbox::~Sandbox() = default;

	Result<Scope<Sandbox>> Sandbox::Create(const SandboxSpecification& specification)
	{
		if (!Detail::IsScriptingRuntimeInitialized())
			return MakeError(ErrorCode::InvalidState, "scripting runtime is not initialized");
		if (specification.Mode != SandboxMode::Runtime && specification.Mode != SandboxMode::LoadTime)
			return MakeError(ErrorCode::InvalidArgument, "invalid sandbox mode");
#if defined(ENGINE_DIST)
		if (specification.Mode == SandboxMode::LoadTime)
			return MakeError(ErrorCode::Unsupported, "load-time VMs are unavailable in Dist");
#endif
		if (specification.Mode == SandboxMode::Runtime && !specification.RandomStream)
			return MakeError(ErrorCode::InvalidArgument, "runtime Sandbox needs a random stream");
		if (specification.Mode == SandboxMode::LoadTime && (specification.Host || specification.Engine))
			return MakeError(ErrorCode::InvalidArgument, "load-time Sandbox cannot borrow a runtime host");
		if (specification.Host && (!specification.Engine || specification.RandomStream != &specification.Host->GetRandom()))
			return MakeError(ErrorCode::InvalidArgument, "runtime host and random stream do not match the owning engine");
		if (specification.Engine && !specification.Host)
			return MakeError(ErrorCode::InvalidArgument, "an owning script engine requires a host");
		ENGINE_TRY_ASSIGN(auto budget, ScriptWatchdog::ResolveCallbackBudget(specification.CallbackBudgetMs, specification.IsTestRun));
		ENGINE_TRY_ASSIGN(auto allocator, TrackingAllocator::Create(specification.MemoryLimitMB));
		uint64_t identity = s_NextVmIdentity.load(std::memory_order_relaxed);
		do
		{
			if (identity == std::numeric_limits<uint64_t>::max())
				return MakeError(ErrorCode::InvalidState, "process VM identity space exhausted");
		} while (!s_NextVmIdentity.compare_exchange_weak(identity, identity + 1, std::memory_order_relaxed));
		auto sandbox = CreateScope<Sandbox>(ConstructionKey{});
		auto& state = *sandbox->m_State;
		state.Specification = specification;
		state.Generation = identity;
		state.Budget = specification.Mode == SandboxMode::LoadTime ? LoadTimeVm::RequireGraphBudgetMs : budget;
		state.Allocator = std::move(allocator);
		if (!state.Specification.Api)
		{
			state.OwnedTypes = CreateScope<TypeRegistry>();
			// The complete API includes reflected component types and enums even when runtime dispatch is unavailable.
			// Registration creates immutable metadata only; it neither creates a scene nor borrows a runtime host.
			RegisterBuiltinComponents(*state.OwnedTypes);
			state.OwnedTypes->Freeze();
			state.OwnedApi = CreateScope<ScriptApiRegistry>();
			ENGINE_TRY(RegisterBindings(*state.OwnedApi, *state.OwnedTypes));
			state.Specification.Api = state.OwnedApi.get();
		}
		if (!state.Specification.Api->IsFrozen())
			return MakeError(ErrorCode::InvalidState, "Sandbox requires a frozen script registry");
		state.Vm = lua_newstate(State::Allocate, state.Allocator.get());
		if (!state.Vm)
			return MakeError(ErrorCode::Script, "unable to allocate a scripting VM");
		lua_setthreaddata(state.Vm, sandbox.get());
		lua_callbacks(state.Vm)->userdata = sandbox.get();
		lua_callbacks(state.Vm)->userthread = State::ThreadLifecycle;
		lua_callbacks(state.Vm)->interrupt = State::Interrupt;
		ENGINE_TRY(CheckOutcome(state.RunNative(state.Vm, [&state](ScriptCall& call)
		{
			return state.Initialize(call);
		})));
		return sandbox;
	}

	namespace {

		struct PublicEntry
		{
			Sandbox& Owner;
			~PublicEntry()
			{
				Detail::SandboxAccess::LeaveProtected(Owner);
				const auto memory = Owner.GetMemoryState();
				if (!Detail::SandboxAccess::MainCall(Owner).Engine && Detail::SandboxAccess::GetWatchdog(Owner)->GetDepth() == 0 && memory.NeedsRecovery && !memory.MustStop)
					static_cast<void>(Detail::SandboxAccess::RecoverMemory(Owner));
			}
		};

	}

	Status Sandbox::RunSource(const VfsPath& path, std::string_view source)
	{
#if defined(ENGINE_DIST)
		static_cast<void>(path);
		static_cast<void>(source);
		return MakeError(ErrorCode::Unsupported, "source execution is unavailable in Dist");
#else
		m_State->AssertThread();
		m_State->LastError.reset();
		ENGINE_TRY(Detail::SandboxAccess::EnterProtected(*this, m_State->Specification.Engine ? ScriptExecutionOrigin::Gameplay : ScriptExecutionOrigin::Pure));
		PublicEntry entry{ *this };
		const auto compiled = ScriptCompiler::Compile({ .Path = path, .Source = source });
		if (!compiled)
		{
			m_State->RecordHostError(compiled.error());
			return std::unexpected(compiled.error());
		}
		ENGINE_TRY(m_State->Poll());
		ScriptData script{};
		script.Bytecode = compiled->Bytecode;
		script.SourceMap = compiled->SourceMap;
		return RunModule(script);
#endif
	}

	Status Sandbox::RunModule(const ScriptData& script)
	{
		m_State->AssertThread();
		m_State->LastError.reset();
		ENGINE_TRY(Detail::SandboxAccess::EnterProtected(*this, m_State->Specification.Engine ? ScriptExecutionOrigin::Gameplay : ScriptExecutionOrigin::Pure));
		PublicEntry entry{ *this };
		Status operation{};
		const auto outcome = m_State->RunNative(m_State->Vm, [this, &script, &operation](ScriptCall& call)
		{
			operation = script.SourceMap.Path.ends_with(".luau") ? Detail::SandboxAccess::PushModule(call, script) : m_State->Execute(call, script, std::nullopt);
			return 0;
		});
		ENGINE_TRY(CheckOutcome(outcome));
		if (!operation)
		{
			if (!m_State->LastError)
				m_State->RecordHostError(operation.error());
			return std::unexpected(operation.error());
		}
		return {};
	}

	Result<ScriptEvaluation> Sandbox::Evaluate(std::string_view source, const VfsPath& path, std::optional<UUID> entity)
	{
#if defined(ENGINE_DIST)
		static_cast<void>(source);
		static_cast<void>(path);
		static_cast<void>(entity);
		return MakeError(ErrorCode::Unsupported, "source evaluation is unavailable in Dist");
#else
		m_State->AssertThread();
		m_State->LastError.reset();
		ENGINE_TRY(Detail::SandboxAccess::EnterProtected(*this, ScriptExecutionOrigin::Eval));
		PublicEntry entry{ *this };
		const auto compiled = ScriptCompiler::Compile({ .Path = path, .Source = source, .Mode = ScriptCompileMode::ExpressionOrChunk });
		if (!compiled)
		{
			m_State->RecordHostError(compiled.error());
			return std::unexpected(compiled.error());
		}
		ENGINE_TRY(m_State->Poll());
		ScriptData script{};
		script.Bytecode = compiled->Bytecode;
		script.SourceMap = compiled->SourceMap;
		return ExecuteBytecode(script, entity);
#endif
	}

	Result<ScriptEvaluation> Sandbox::ExecuteBytecode(const ScriptData& script, std::optional<UUID> entity)
	{
		m_State->AssertThread();
		m_State->LastError.reset();
		if (entity)
		{
			if (!m_State->Specification.Engine)
				return MakeError(ErrorCode::InvalidState, "entity evaluation requires an active script engine");
			if (!m_State->Specification.Host->GetScene().FindEntityByID(*entity))
				return MakeError(ErrorCode::NotFound, "evaluation entity does not exist");
		}
		ENGINE_TRY(Detail::SandboxAccess::EnterProtected(*this, ScriptExecutionOrigin::Eval));
		PublicEntry entry{ *this };
		ScriptEvaluation evaluation{};
		auto context = Detail::SandboxAccess::GetContext(*this);
		context.Script = script.SourceMap.Path;
		if (context.Script.empty() && script.SourceMap.ChunkName.size() > 1)
			context.Script = script.SourceMap.ChunkName.substr(1);
		context.Callback = "Evaluate";
		context.Entity = {};
		context.EntityName.clear();
		if (m_State->Specification.Host)
			context.Tick = m_State->Specification.Host->GetFrameState().Tick;
		if (entity)
		{
			context.Entity = *entity;
			context.EntityName = m_State->Specification.Host->GetScene().FindEntityByID(*entity).GetName();
		}
		ENGINE_TRY(Detail::SandboxAccess::PushContext(*this, context));
		ContextRestore contextRestore{ *this };
		State::PrintSink sink{ evaluation.Prints, *m_State->Allocator };
		struct Capture
		{
			State& Data;
			State::PrintSink* Previous = nullptr;
			~Capture() { Data.Prints = Previous; }
		} capture{ *m_State, m_State->Prints };
		m_State->Prints = &sink;
		Status operation{};
		const auto outcome = m_State->RunNative(m_State->Vm, [this, &script, entity, &evaluation, &operation](ScriptCall& call)
		{
			operation = m_State->Execute(call, script, entity);
			if (!operation)
				return 0;
			auto json = Detail::SandboxAccess::ReadJson(call, -1);
			if (!json)
			{
				ErrorLocation location{};
				location.File = script.SourceMap.Path.empty() ? script.SourceMap.ChunkName.substr(1) : script.SourceMap.Path;
				if (!script.SourceMap.JsonPointer.empty())
					location.JsonPointer = script.SourceMap.JsonPointer;
				operation = std::unexpected(std::move(json.error()).WithLocation(std::move(location)));
			}
			else
				evaluation.Value = VariantValue(std::move(*json));
			return 0;
		});
		ENGINE_TRY(CheckOutcome(outcome));
		if (!operation)
		{
			if (!m_State->LastError)
				m_State->RecordHostError(operation.error());
			return std::unexpected(operation.error());
		}
		return evaluation;
	}

	std::optional<ScriptError> Sandbox::GetLastError() const
	{
		m_State->AssertThread();
		return m_State->LastError;
	}

	ScriptMemoryState Sandbox::GetMemoryState() const noexcept
	{
		return m_State->Allocator ? m_State->Allocator->GetState() : ScriptMemoryState{};
	}

	bool Sandbox::IsStopped() const noexcept
	{
		return GetMemoryState().MustStop;
	}

	bool IsScriptingRuntimeInitialized() noexcept
	{
		return Detail::IsScriptingRuntimeInitialized();
	}

	namespace Detail {

		bool IsScriptingRuntimeInitialized() noexcept
		{
			return s_RuntimeInitialized.load(std::memory_order_acquire);
		}

		void SandboxAccess::CapturePrint(Sandbox& sandbox, std::string_view message)
		{
			sandbox.m_State->AssertThread();
			auto* sink = sandbox.m_State->Prints;
			if (!sink || IsReloadEvaluating(sandbox))
				return;
			// Captured output must not bypass the VM limit by repeatedly allocating only host strings. Charge it
			// until the protected evaluation unwinds; the detached result then belongs to its host caller.
			const size_t maximum = std::numeric_limits<size_t>::max();
			const size_t overhead = sizeof(std::string) * 2 + 1;
			const size_t bytes = message.size() > maximum - overhead || sink->Bytes > maximum - overhead - message.size()
				? maximum
				: sink->Bytes + message.size() + overhead;
			void* reservation = sink->Allocator.Reallocate(sink->Reservation, sink->Bytes, bytes);
			if (!reservation)
				return; // the allocator latches Memory; the outer wrapper cannot accept success, including pcall
			sink->Reservation = reservation;
			sink->Bytes = bytes;
			if (CheckInterrupt(sandbox, -1))
				return;
			sink->Lines.emplace_back(message);
		}

		Result<ScriptCallResult> SandboxAccess::RunNative(Sandbox& sandbox, const std::function<int(ScriptCall&)>& operation)
		{
			return sandbox.m_State->RunNative(sandbox.m_State->Vm, operation);
		}

		uint64_t SandboxAccess::GetGeneration(const Sandbox& sandbox) noexcept
		{
			return sandbox.m_State->Generation;
		}

		Sandbox* SandboxAccess::FromState(lua_State* state) noexcept
		{
			if (!state)
				return nullptr;
			auto* owner = static_cast<Sandbox*>(lua_callbacks(state)->userdata);
			// Callback userdata belongs to the VM only when its allocator adapter is ours.
			void* allocatorData = nullptr;
			if (lua_getallocf(state, &allocatorData) != Sandbox::State::Allocate)
				return nullptr;
			if (!owner || lua_getthreaddata(state) != owner)
				return nullptr;
			return owner;
		}

		Result<lua_State*> SandboxAccess::CreateThread(ScriptCall& call)
		{
			if (!call.Sandbox || !call.State)
				return MakeError(ErrorCode::InvalidArgument, "thread creation requires a live Sandbox call");
			ENGINE_TRY(ThreadCall(*call.Sandbox, call.State));
			if (!GetExecution(*call.Sandbox))
				return MakeError(ErrorCode::InvalidState, "thread creation requires a protected entry");
			const int top = lua_gettop(call.State);
			lua_State* child = lua_newthread(call.State);
			auto& state = *call.Sandbox->m_State;
			const auto outcome = state.RunNative(child, [](ScriptCall& childCall)
			{
				luaL_sandboxthread(childCall.State);
				lua_setsafeenv(childCall.State, LUA_GLOBALSINDEX, false);
				if (IsReloadEvaluating(*childCall.Sandbox))
				{
					lua_pushcfunction(childCall.State, Sandbox::State::Print, "print");
					lua_setglobal(childCall.State, "print");
				}
				return 0;
			});
			const auto status = CheckOutcome(outcome);
			if (!status)
			{
				lua_settop(call.State, top);
				return std::unexpected(status.error());
			}
			return child;
		}

		Status SandboxAccess::PushModule(ScriptCall& call, const ScriptData& script)
		{
			if (!call.Sandbox || !call.State)
				return MakeError(ErrorCode::InvalidArgument, "module load requires a live Sandbox call");
			ENGINE_TRY(ThreadCall(*call.Sandbox, call.State));
			if (!GetExecution(*call.Sandbox))
				return MakeError(ErrorCode::InvalidState, "module load requires a protected entry");
			auto& state = *call.Sandbox->m_State;
			ENGINE_TRY_ASSIGN(auto path, VfsPath::Parse(std::string("project://") + script.SourceMap.Path));
			if (state.PushCached(call.State, path.GetPath()))
				return {};
			ENGINE_TRY(state.ActiveResolver().EnterModule(path));
			struct Leave
			{
				RequireResolver& Resolver;
				const VfsPath& Path;
				~Leave() { Resolver.LeaveModule(Path); }
			} leave{ state.ActiveResolver(), path };
			const int top = lua_gettop(call.State);
			StackRestore restore{ call.State, top };
			ENGINE_TRY(state.Execute(call, script, std::nullopt, true));
			ENGINE_TRY(state.Poll());
			state.CacheValue(call.State, path.GetPath(), -1);
			// A table insertion may itself cross soft memory. Do not publish an unsuccessful module.
			if (const auto polled = state.Poll(); !polled)
			{
				lua_getref(call.State, state.ActiveCache());
				lua_pushlstring(call.State, path.GetPath().data(), path.GetPath().size());
				lua_pushnil(call.State);
				lua_rawset(call.State, -3);
				return std::unexpected(polled.error());
			}
			for (auto& candidate : state.Candidates)
				if (candidate.Data->SourceMap.Path == path.GetPath())
					candidate.Evaluated = true;
			restore.Top = top + 1;
			return {};
		}

		Status SandboxAccess::PushModuleReturn(ScriptCall& call, const VfsPath& path)
		{
			if (!call.Sandbox || !call.State)
				return MakeError(ErrorCode::InvalidArgument, "module lookup requires a live Sandbox call");
			ENGINE_TRY(ThreadCall(*call.Sandbox, call.State));
			if (!GetExecution(*call.Sandbox))
				return MakeError(ErrorCode::InvalidState, "module lookup requires a protected entry");
			if (!call.Sandbox->m_State->PushCached(call.State, path.GetPath()))
				return MakeError(ErrorCode::NotFound, "module '{}' has no cached return", path);
			return {};
		}

		Result<std::vector<std::string>> SandboxAccess::GetLoadedModulePaths(ScriptCall& call)
		{
			if (!call.Sandbox || !call.State)
				return MakeError(ErrorCode::InvalidArgument, "module inventory requires a live Sandbox call");
			ENGINE_TRY(ThreadCall(*call.Sandbox, call.State));
			if (!GetExecution(*call.Sandbox))
				return MakeError(ErrorCode::InvalidState, "module inventory requires a protected entry");
			return call.Sandbox->m_State->CachePaths(call.State, call.Sandbox->m_State->ModuleCache);
		}

		Result<std::vector<std::string>> SandboxAccess::GetReloadModulePaths(ScriptCall& call)
		{
			if (!call.Sandbox || !call.State)
				return MakeError(ErrorCode::InvalidArgument, "reload inventory requires a live Sandbox call");
			ENGINE_TRY(ThreadCall(*call.Sandbox, call.State));
			const auto& state = *call.Sandbox->m_State;
			if (!GetExecution(*call.Sandbox) || state.ReloadCache == LUA_NOREF || state.ReloadCommitted)
				return MakeError(ErrorCode::InvalidState, "no staged module reload");
			return call.Sandbox->m_State->CachePaths(call.State, state.ReloadCache);
		}

		Status SandboxAccess::PushLiveModuleReturn(ScriptCall& call, const VfsPath& path)
		{
			if (!call.Sandbox || !call.State)
				return MakeError(ErrorCode::InvalidArgument, "live module lookup requires a live Sandbox call");
			ENGINE_TRY(ThreadCall(*call.Sandbox, call.State));
			auto& state = *call.Sandbox->m_State;
			if (!GetExecution(*call.Sandbox) || state.ReloadCommitted)
				return MakeError(ErrorCode::InvalidState, "live lookup requires an uncommitted protected entry");
			if (!state.PushCached(call.State, path.GetPath(), state.ModuleCache))
				return MakeError(ErrorCode::NotFound, "module '{}' has no live cached return", path);
			return {};
		}

		bool SandboxAccess::IsReloadEvaluating(const Sandbox& sandbox) noexcept
		{
			return sandbox.m_State->ReloadEvaluationDepth != 0;
		}

		Status SandboxAccess::BeginReload(ScriptCall& call, std::span<const Ref<const ScriptData>> candidates)
		{
			if (!call.Sandbox || !call.State)
				return MakeError(ErrorCode::InvalidArgument, "reload requires a live Sandbox call");
			ENGINE_TRY(ThreadCall(*call.Sandbox, call.State));
			if (!GetExecution(*call.Sandbox))
				return MakeError(ErrorCode::InvalidState, "reload requires a protected entry");
			auto& state = *call.Sandbox->m_State;
			if (state.ReloadCache != LUA_NOREF)
				return MakeError(ErrorCode::InvalidState, "a module reload transaction is already active");
			std::vector<Sandbox::State::ReloadCandidate> pinned;
			for (const auto& candidate : candidates)
			{
				if (!candidate)
					return MakeError(ErrorCode::InvalidArgument, "null reload candidate");
				ENGINE_TRY_ASSIGN(auto path, VfsPath::Parse(std::string("project://") + candidate->SourceMap.Path));
				if (!path.GetPath().starts_with("Assets/") || !path.GetPath().ends_with(".luau"))
					return MakeError(ErrorCode::Validation, "reload candidates require canonical Assets .luau paths");
				if (std::ranges::any_of(pinned, [&candidate](const auto& item)
				{
					return item.Data->SourceMap.Path == candidate->SourceMap.Path;
				}))
					return MakeError(ErrorCode::InvalidArgument, "duplicate reload candidate '{}'", candidate->SourceMap.Path);
				pinned.push_back({ candidate, false });
			}
			if (state.Resolver->HasActiveModules())
				return MakeError(ErrorCode::InvalidState, "reload cannot run while a live module initializer is active");
			StackRestore restore{ call.State, lua_gettop(call.State) };
			auto resolver = CreateScope<RequireResolver>();
			lua_newtable(call.State);
			state.ReloadCache = lua_ref(call.State, -1);
			state.ReloadResolver = std::move(resolver);
			state.ReloadRandom.Seed(0);
			state.Candidates = std::move(pinned);
			state.ReloadCommitted = false;
			return state.Poll();
		}

		Status SandboxAccess::EvaluateReloadModule(ScriptCall& call, const ScriptData& script)
		{
			if (!call.Sandbox)
				return MakeError(ErrorCode::InvalidArgument, "reload requires a live Sandbox call");
			auto& state = *call.Sandbox->m_State;
			if (state.ReloadCache == LUA_NOREF || state.ReloadCommitted)
				return MakeError(ErrorCode::InvalidState, "no staged module reload");
			const auto candidate = std::ranges::find_if(state.Candidates, [&script](const auto& item)
			{
				return item.Data.get() == &script;
			});
			if (candidate == state.Candidates.end())
				return MakeError(ErrorCode::InvalidArgument, "module is not a pinned reload candidate");
			struct EvaluationScope
			{
				Sandbox::State& Data;
				~EvaluationScope() { --Data.ReloadEvaluationDepth; }
			} scope{ state };
			++state.ReloadEvaluationDepth;
			return PushModule(call, script);
		}

		Status SandboxAccess::SetReloadModule(ScriptCall& call, const ScriptData& script, int valueIndex)
		{
			if (!call.Sandbox || !call.State)
				return MakeError(ErrorCode::InvalidArgument, "reload requires a live Sandbox call");
			ENGINE_TRY(ThreadCall(*call.Sandbox, call.State));
			auto& state = *call.Sandbox->m_State;
			if (!GetExecution(*call.Sandbox) || state.ReloadCache == LUA_NOREF || state.ReloadCommitted || std::ranges::any_of(state.Candidates, [](const auto& item)
			{
				return !item.Evaluated;
			}))
				return MakeError(ErrorCode::InvalidState, "all reload candidates must succeed before staging preserved identities");
			if (std::ranges::none_of(state.Candidates, [&script](const auto& item)
			{
				return item.Data.get() == &script;
			}))
				return MakeError(ErrorCode::InvalidArgument, "module is not a pinned reload candidate");
			if (lua_type(call.State, valueIndex) == LUA_TNONE)
				return MakeError(ErrorCode::InvalidArgument, "invalid reload value slot");
			state.CacheValue(call.State, script.SourceMap.Path, valueIndex);
			return state.Poll();
		}

		Status SandboxAccess::SetReloadModuleReturn(ScriptCall& call, const VfsPath& path, int valueIndex)
		{
			if (!call.Sandbox || !call.State)
				return MakeError(ErrorCode::InvalidArgument, "reload requires a live Sandbox call");
			ENGINE_TRY(ThreadCall(*call.Sandbox, call.State));
			auto& state = *call.Sandbox->m_State;
			if (!GetExecution(*call.Sandbox) || state.ReloadCache == LUA_NOREF || state.ReloadCommitted
				|| IsReloadEvaluating(*call.Sandbox) || std::ranges::any_of(state.Candidates, [](const auto& item)
			{
				return !item.Evaluated;
			}))
				return MakeError(ErrorCode::InvalidState, "all reload candidates must succeed before sealing the cache");
			if (path.GetScheme() != "project" || !path.GetPath().starts_with("Assets/") || !path.GetPath().ends_with(".luau")
				|| lua_type(call.State, valueIndex) == LUA_TNONE)
				return MakeError(ErrorCode::InvalidArgument, "invalid staged module path or value");
			state.CacheValue(call.State, path.GetPath(), valueIndex);
			return state.Poll();
		}

		Status SandboxAccess::CommitReload(ScriptCall& call)
		{
			if (!call.Sandbox || !call.State)
				return MakeError(ErrorCode::InvalidArgument, "reload requires a live Sandbox call");
			ENGINE_TRY(ThreadCall(*call.Sandbox, call.State));
			auto& state = *call.Sandbox->m_State;
			if (!GetExecution(*call.Sandbox) || state.ReloadCache == LUA_NOREF || state.ReloadCommitted || std::ranges::any_of(state.Candidates, [](const auto& item)
			{
				return !item.Evaluated;
			}))
				return MakeError(ErrorCode::InvalidState, "reload is not ready to commit");
			if (IsReloadEvaluating(*call.Sandbox))
				return MakeError(ErrorCode::InvalidState, "cannot commit from a reload initializer");
			StackRestore restore{ call.State, lua_gettop(call.State) };
			const auto livePaths = state.CachePaths(call.State, state.ModuleCache);
			const auto changed = [&state](std::string_view path)
			{
				return std::ranges::any_of(state.Candidates, [path](const auto& item)
				{
					return item.Data->SourceMap.Path == path;
				});
			};
			auto mergedResolver = CreateScope<RequireResolver>();
			for (const auto& edge : state.Resolver->GetRequires())
				if (!changed(edge.From))
				{
					ENGINE_TRY_ASSIGN(auto origin, VfsPath::Create("project", edge.From));
					ENGINE_TRY(mergedResolver->Resolve(origin, edge.Request));
				}
			for (const auto& edge : state.ReloadResolver->GetRequires())
				if (changed(edge.From) || !std::ranges::binary_search(livePaths, edge.From))
				{
					ENGINE_TRY_ASSIGN(auto origin, VfsPath::Create("project", edge.From));
					ENGINE_TRY(mergedResolver->Resolve(origin, edge.Request));
				}
			// Keep unrelated entries and unchanged dependencies, including cached nil. Candidate references were
			// canonicalized by the owner before this step; old live objects have never entered candidate execution.
			for (const auto& path : livePaths)
				if (!changed(path))
				{
					ENGINE_CORE_VERIFY(state.PushCached(call.State, path, state.ModuleCache), "missing live module");
					state.CacheValue(call.State, path, -1);
					lua_pop(call.State, 1);
				}
			ENGINE_TRY(state.Poll());
			state.ReloadResolver = std::move(mergedResolver);
			std::swap(state.Resolver, state.ReloadResolver);
			std::swap(state.ModuleCache, state.ReloadCache);
			state.ReloadCommitted = true;
			return {};
		}

		void SandboxAccess::RollbackReload(Sandbox& sandbox) noexcept
		{
			auto& state = *sandbox.m_State;
			state.AssertThread();
			if (state.ReloadCache == LUA_NOREF)
				return;
			if (state.ReloadCommitted)
			{
				std::swap(state.ModuleCache, state.ReloadCache);
				std::swap(state.Resolver, state.ReloadResolver);
			}
			lua_unref(state.Vm, state.ReloadCache);
			state.ReloadCache = LUA_NOREF;
			state.ReloadCommitted = false;
			state.Candidates.clear();
			state.ReloadResolver.reset();
		}

		void SandboxAccess::FinalizeReload(Sandbox& sandbox) noexcept
		{
			auto& state = *sandbox.m_State;
			state.AssertThread();
			if (state.ReloadCache == LUA_NOREF)
				return;
			ENGINE_CORE_VERIFY(state.ReloadCommitted, "FinalizeReload requires a provisional commit");
			lua_unref(state.Vm, state.ReloadCache);
			state.ReloadCache = LUA_NOREF;
			state.ReloadCommitted = false;
			state.Candidates.clear();
			state.ReloadResolver.reset();
		}

		ScriptCall SandboxAccess::MainCall(Sandbox& sandbox)
		{
			auto& state = *sandbox.m_State;
			state.AssertThread();
			return { .State = state.Vm, .Sandbox = &sandbox, .Engine = IsReloadEvaluating(sandbox) ? nullptr : state.Specification.Engine, .Execution = GetExecution(sandbox) };
		}

		Result<ScriptCall> SandboxAccess::ThreadCall(Sandbox& sandbox, lua_State* vm, std::string_view memberName)
		{
			auto& state = *sandbox.m_State;
			state.AssertThread();
			if (!vm || lua_mainthread(vm) != state.Vm || FromState(vm) != &sandbox)
				return MakeError(ErrorCode::InvalidArgument, "thread does not belong to this Sandbox");
			return ScriptCall{ .State = vm, .Sandbox = &sandbox, .Engine = IsReloadEvaluating(sandbox) ? nullptr : state.Specification.Engine, .MemberName = memberName, .Execution = GetExecution(sandbox) };
		}

		ScriptWatchdog* SandboxAccess::GetWatchdog(Sandbox& sandbox)
		{
			return &sandbox.m_State->Watchdog;
		}
		TrackingAllocator* SandboxAccess::GetAllocator(Sandbox& sandbox)
		{
			return sandbox.m_State->Allocator.get();
		}
		RequireResolver* SandboxAccess::GetResolver(Sandbox& sandbox)
		{
			return &sandbox.m_State->ActiveResolver();
		}
		ScriptApiRegistry* SandboxAccess::GetApi(Sandbox& sandbox)
		{
			return sandbox.m_State->Specification.Api;
		}
		Random* SandboxAccess::GetRandom(Sandbox& sandbox)
		{
			auto& state = *sandbox.m_State;
			return IsReloadEvaluating(sandbox)                      ? &state.ReloadRandom
				: state.Specification.Mode == SandboxMode::LoadTime ? &state.LocalRandom
																	: state.Specification.RandomStream;
		}
		uint32_t SandboxAccess::GetBudgetMs(const Sandbox& sandbox)
		{
			return sandbox.m_State->Budget;
		}
		double SandboxAccess::SampleClock(Sandbox& sandbox)
		{
			return sandbox.m_State->Specification.ClockSeconds ? sandbox.m_State->Specification.ClockSeconds() : lua_clock();
		}
		SandboxMode SandboxAccess::GetMode(const Sandbox& sandbox)
		{
			return sandbox.m_State->Specification.Mode;
		}

		bool SandboxAccess::IsReadOnly(const Sandbox& sandbox)
		{
			const auto& spec = sandbox.m_State->Specification;
			return spec.ReadOnly || (spec.Engine && spec.Engine->IsReadOnly());
		}

		Status SandboxAccess::CheckWritable(const Sandbox& sandbox)
		{
			const auto& state = *sandbox.m_State;
			if (IsReloadEvaluating(sandbox))
				return MakeError(ErrorCode::InvalidState, "host mutation is unavailable during reload initialization");
			if (IsReadOnly(sandbox))
				return MakeError(ErrorCode::InvalidState, "script engine is read-only");
			if (state.Specification.Mode != SandboxMode::Runtime || !state.Specification.Engine || !state.Specification.Host || state.Watchdog.GetDepth() == 0 || state.Execution.Origin == ScriptExecutionOrigin::Pure)
				return MakeError(ErrorCode::InvalidState, "mutation requires an active script engine execution");
			return {};
		}

		Status SandboxAccess::CheckRandomWritable(const Sandbox& sandbox)
		{
			if (IsReadOnly(sandbox))
				return MakeError(ErrorCode::InvalidState, "shared script random stream is read-only");
			return {};
		}

		Status SandboxAccess::EnterProtected(Sandbox& sandbox, ScriptExecutionOrigin origin)
		{
			auto& state = *sandbox.m_State;
			state.AssertThread();
			const auto memory = state.Allocator->GetState();
			if (memory.MustStop || memory.NeedsRecovery)
				return MakeError(ErrorCode::InvalidState, "Sandbox requires memory recovery or has stopped");
			const bool outermost = state.Watchdog.GetDepth() == 0;
			ENGINE_TRY(state.Watchdog.Enter(state.Budget, SampleClock(sandbox)));
			if (outermost)
			{
				state.SafetyFault.reset();
				state.Execution = { .Origin = origin };
			}
			return {};
		}

		ScriptExecutionContext* SandboxAccess::GetExecution(Sandbox& sandbox)
		{
			return sandbox.m_State->Watchdog.GetDepth() ? &sandbox.m_State->Execution : nullptr;
		}

		ScriptExecutionOrigin SandboxAccess::GetExecutionOrigin(const Sandbox& sandbox)
		{
			return sandbox.m_State->Watchdog.GetDepth() ? sandbox.m_State->Execution.Origin : ScriptExecutionOrigin::Pure;
		}

		void SandboxAccess::LeaveProtected(Sandbox& sandbox)
		{
			sandbox.m_State->Watchdog.Leave();
		}

		Status SandboxAccess::PushContext(Sandbox& sandbox, const SandboxExecutionContext& context)
		{
			sandbox.m_State->AssertThread();
			sandbox.m_State->Contexts.push_back(context);
			return {};
		}

		void SandboxAccess::PopContext(Sandbox& sandbox)
		{
			auto& contexts = sandbox.m_State->Contexts;
			ENGINE_CORE_VERIFY(!contexts.empty(), "unmatched Sandbox context pop");
			contexts.pop_back();
		}

		SandboxExecutionContext SandboxAccess::GetContext(const Sandbox& sandbox)
		{
			return sandbox.m_State->Contexts.empty() ? SandboxExecutionContext{} : sandbox.m_State->Contexts.back();
		}

		Result<std::string_view> SandboxAccess::RegisterLoadedChunk(Sandbox& sandbox, const ScriptData& script)
		{
			auto& state = *sandbox.m_State;
			state.AssertThread();
			const auto& map = script.SourceMap;
			if (script.Bytecode.empty() || script.Bytecode.front() == std::byte{ 0 } || map.ChunkName.size() < 2 || (map.ChunkName.front() != '@' && map.ChunkName.front() != '=') || !IsValidUtf8(map.ChunkName) || !IsValidUtf8(map.Path) || !IsValidUtf8(map.JsonPointer) || map.ChunkName.find('\0') != std::string::npos || map.Path.find('\0') != std::string::npos || map.JsonPointer.find('\0') != std::string::npos || map.GeneratedPrefixLines > 1 || map.LineOffsets.empty() || map.LineOffsets.front() != 0)
				return MakeError(ErrorCode::Validation, "invalid trusted script source map or bytecode envelope");
			if (map.Path.empty() && map.ChunkName.front() != '=')
				return MakeError(ErrorCode::Validation, "fileless script needs a synthetic diagnostic label");
			if (!map.Path.empty())
			{
				ENGINE_TRY_ASSIGN(auto path, VfsPath::Parse(std::string("project://") + map.Path));
				if (!path.GetPath().starts_with("Assets/") || path.GetPath() != map.Path)
					return MakeError(ErrorCode::Validation, "script diagnostic origin must be canonical beneath Assets");
			}
			if (!map.JsonPointer.empty() && map.JsonPointer.front() != '/')
				return MakeError(ErrorCode::Validation, "invalid script JSON pointer");
			for (size_t i = 0; i < map.JsonPointer.size(); ++i)
				if (map.JsonPointer[i] == '~' && (++i == map.JsonPointer.size() || (map.JsonPointer[i] != '0' && map.JsonPointer[i] != '1')))
					return MakeError(ErrorCode::Validation, "invalid script JSON pointer escape");
			for (size_t i = 0; i < map.LineOffsets.size(); ++i)
				if (map.LineOffsets[i] > map.SourceByteCount || (i && map.LineOffsets[i] <= map.LineOffsets[i - 1]))
					return MakeError(ErrorCode::Validation, "invalid authored script line offsets");
			if (state.NextChunk == std::numeric_limits<uint64_t>::max())
				return MakeError(ErrorCode::InvalidState, "VM chunk identity space exhausted");
			const auto identity = std::format("=engine/{}", state.NextChunk++);
			const uint64_t bytes = sizeof(Sandbox::State::LoadedMap) + static_cast<uint64_t>(map.LineOffsets.size()) * sizeof(uint32_t) + identity.size() + map.ChunkName.size() + map.Path.size() + map.JsonPointer.size() + 4;
			if (bytes > std::numeric_limits<size_t>::max())
				return MakeError(ErrorCode::Validation, "script map exceeds native address space");
			void* memory = state.Allocator->Reallocate(nullptr, 0, static_cast<size_t>(bytes));
			if (!memory)
				return MakeError(ErrorCode::Script, "script source map exceeds the VM memory budget");
			auto* record = std::construct_at(static_cast<Sandbox::State::LoadedMap*>(memory));
			record->Next = state.Maps;
			record->AllocationBytes = static_cast<size_t>(bytes);
			record->Prefix = map.GeneratedPrefixLines;
			auto* offsets = reinterpret_cast<uint32_t*>(record + 1);
			std::memcpy(offsets, map.LineOffsets.data(), map.LineOffsets.size() * sizeof(uint32_t));
			record->Offsets = { offsets, map.LineOffsets.size() };
			char* storage = reinterpret_cast<char*>(offsets + map.LineOffsets.size());
			const auto copy = [&storage](std::string_view text)
			{
				const auto result = std::string_view(storage, text.size());
				std::memcpy(storage, text.data(), text.size());
				storage[text.size()] = '\0';
				storage += text.size() + 1;
				return result;
			};
			record->Identity = copy(identity);
			const auto authored = copy(map.ChunkName);
			const auto path = copy(map.Path);
			record->File = path.empty() ? authored.substr(1) : path;
			record->Pointer = copy(map.JsonPointer);
			state.Maps = record;
			ENGINE_TRY(state.Poll());
			return record->Identity;
		}

		ErrorLocation SandboxAccess::Locate(const Sandbox& sandbox, std::string_view vmChunkName, uint32_t line, uint32_t column)
		{
			for (auto* record = sandbox.m_State->Maps; record; record = record->Next)
			{
				if (record->Identity != vmChunkName)
					continue;
				ErrorLocation location{};
				location.File = record->File;
				if (!record->Pointer.empty())
					location.JsonPointer = record->Pointer;
				if (line > record->Prefix && static_cast<uint64_t>(line - record->Prefix) <= record->Offsets.size())
				{
					location.Line = line - record->Prefix;
					location.Column = column;
				}
				return location;
			}
			return {};
		}

		std::optional<ScriptErrorKind> SandboxAccess::CheckInterrupt(Sandbox& sandbox, int gc)
		{
			if (gc >= 0)
				return std::nullopt;
			auto& state = *sandbox.m_State;
			if (state.Allocator->CheckInterrupt(gc) != ScriptMemoryAction::Continue)
				state.SafetyFault = ScriptErrorKind::Memory;
			if (state.SafetyFault != ScriptErrorKind::Memory && state.Watchdog.CheckInterrupt(gc, SampleClock(sandbox)))
				state.SafetyFault = ScriptErrorKind::Timeout;
			return state.SafetyFault;
		}

		std::optional<ScriptErrorKind> SandboxAccess::ClassifyFailure(const Sandbox& sandbox, int vmStatus)
		{
			const auto& state = *sandbox.m_State;
			if (vmStatus == LUA_ERRMEM || state.SafetyFault == ScriptErrorKind::Memory || state.Allocator->CheckInterrupt(-1) != ScriptMemoryAction::Continue)
				return ScriptErrorKind::Memory;
			if (state.SafetyFault)
				return state.SafetyFault;
			if (vmStatus == LUA_ERRSYNTAX)
				return ScriptErrorKind::Compile;
			if (vmStatus != LUA_OK && vmStatus != LUA_YIELD)
				return ScriptErrorKind::Runtime;
			return std::nullopt;
		}

		void SandboxAccess::ClearFailure(Sandbox& sandbox) noexcept
		{
			sandbox.m_State->AssertThread();
			sandbox.m_State->LastError.reset();
		}

		void SandboxAccess::RecordFailure(Sandbox& sandbox, const ScriptError& error)
		{
			sandbox.m_State->LastError = error;
			auto& stored = *sandbox.m_State->LastError;
			if (!stored.JsonPointer.empty())
			{
				const std::string prefix = stored.JsonPointer + ": ";
				if (!stored.Message.starts_with(prefix))
					stored.Message.insert(0, prefix);
			}
		}

		Status SandboxAccess::RecoverMemory(Sandbox& sandbox)
		{
			auto& state = *sandbox.m_State;
			state.AssertThread();
			const auto memory = state.Allocator->GetState();
			if (state.Watchdog.GetDepth() != 0 || !memory.NeedsRecovery)
				return MakeError(ErrorCode::InvalidState, "memory recovery requires an unwound first breach");
			if (memory.MustStop)
				return MakeError(ErrorCode::Script, "Sandbox stopped after repeated or hard memory exhaustion");
			const int collected = lua_gc(state.Vm, LUA_GCCOLLECT, 0);
			ENGINE_CORE_VERIFY(collected == 0, "full script GC failed");
			return state.Allocator->FinishRecovery();
		}

	}

}
