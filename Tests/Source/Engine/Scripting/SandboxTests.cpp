#include "TestsPCH.h"
#include "Engine/Scripting/Sandbox.h"

#include "Engine/Asset/IScriptDiagnosticsProvider.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Core/Random.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptCompiler.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <array>
#include <format>
#include <map>
#include <string>
#include <string_view>

namespace Engine {

	namespace {

		class SandboxModuleReader final : public IScriptModuleReader
		{
		public:
			[[nodiscard]] Result<std::string> ReadModule(const VfsPath& path) override
			{
				const auto found = Sources.find(path.ToString());
				if (found == Sources.end())
					return MakeError(ErrorCode::NotFound, "missing module '{}'", path);
				return found->second;
			}
		public:
			std::map<std::string, std::string> Sources{};
		};

		struct MathProbe
		{
			std::string_view Name{};
			double (*Function)(double) = nullptr;
		};

		int PureSandboxProbe(ScriptCall& call)
		{
			Lua::Push(call, true);
			return 1;
		}

		int UnavailableSandboxProbe(ScriptCall& /*call*/)
		{
			FAIL("Engine-less Runtime must reject unavailable metadata before invoking this callback");
			return 0;
		}

		class SandboxBudgetReader final : public IScriptModuleReader
		{
		public:
			[[nodiscard]] Result<std::string> ReadModule(const VfsPath& /*path*/) override
			{
				*Now = 0.3;
				return std::string("return true");
			}
		public:
			double* Now = nullptr; // borrowed test clock, assigned before reading and outlives the sandbox
		};

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("Sandbox: engine-less Runtime restricts metadata availability while retaining runtime limits" * doctest::skip())
		{
			TypeRegistry types;
			types.Freeze();
			ScriptApiRegistry api;
			api.Module("Probe", "Sandbox capability probe.")
				.Function("Pure", &PureSandboxProbe, "() -> boolean", "Available without a runtime engine.",
					{ .Environments = ScriptApiEnvironment::All, .Mutates = false })
				.Function("RuntimeOnly", &UnavailableSandboxProbe, "() -> ()", "Requires an active script engine.",
					{ .Environments = ScriptApiEnvironment::Runtime, .Mutates = false });
			REQUIRE(api.Freeze(types).has_value());
			Random random(71);
			double now = 0.0;
			SandboxBudgetReader reader;
			reader.Now = &now;
			SandboxSpecification specification{};
			specification.Mode = SandboxMode::Runtime;
			specification.IsTestRun = true;
			specification.CallbackBudgetMs = 1000;
			specification.MemoryLimitMB = 2;
			specification.Api = &api;
			specification.RandomStream = &random;
			specification.SourceModules = &reader;
			specification.ClockSeconds = [&now]
			{
				return now;
			};
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			auto path = VfsPath::Parse("project://Assets/EngineLess.luau");
			REQUIRE(path.has_value());
			const auto pure = (*sandbox)->Evaluate("return Probe.Pure()", *path);
			REQUIRE(pure.has_value());
			CHECK(pure->Value.Get() == Json(true));
			CHECK_FALSE((*sandbox)->Evaluate("Probe.RuntimeOnly()", *path).has_value());
			const auto error = (*sandbox)->GetLastError();
			REQUIRE(error.has_value());
			CHECK(error->Kind == ScriptErrorKind::Runtime);
			CHECK(error->Script == "Assets/EngineLess.luau");
			CHECK(error->Message.find("requires an active script engine") != std::string::npos);
			CHECK(error->Message.find("not available at load time") == std::string::npos);
			const auto budget = (*sandbox)->Evaluate("return require('./Dependency')", *path);
			REQUIRE(budget.has_value()); // 300 ms exceeds load-time's budget but fits the configured runtime budget.
			CHECK(budget->Value.Get() == Json(true));
			CHECK((*sandbox)->GetMemoryState().SoftLimitBytes == 2u * 1024 * 1024);
			const auto coverage = api.GetCoverage(RunModes::Editor);
			REQUIRE(coverage.has_value());
			for (const auto& member : coverage->Members)
				CHECK(member.Calls == 0);
		}

		TEST_CASE("Sandbox: escape suite" * doctest::skip())
		{
			Random random(23);
			SandboxModuleReader reader;
			reader.Sources.emplace("project://Assets/A.luau", "PrivateValue = 7; return {}");
			reader.Sources.emplace("project://Assets/B.luau", "assert(PrivateValue == nil); return {}");
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.SourceModules = &reader;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			auto path = VfsPath::Parse("project://Assets/Escape.luau");
			REQUIRE(path.has_value());
			const std::array sources{
				"assert(io == nil and package == nil and loadfile == nil and dofile == nil and loadstring == nil)",
				"assert(os == nil or next(os) == nil); assert(string.dump == nil)",
				"assert(debug == nil or (debug.getregistry == nil and debug.getupvalue == nil and debug.setupvalue == nil))",
				"assert(not pcall(function() math.sin = function() return 0 end end))",
				"assert(not pcall(function() setmetatable(_G, {}) end))",
				"if getfenv then assert(not pcall(function() getfenv(0).math.sin = nil end)) end",
				"if setfenv then assert(not pcall(function() setfenv(math.sin, {}) end)) end",
				"assert(not pcall(function() require('../Outside') end))",
				"require('./A'); require('./B'); assert(PrivateValue == nil)",
			};
			for (const std::string_view source : sources)
			{
				CAPTURE(source);
				// Evaluation is deliberately fresh while required module return values remain cached in this same VM.
				CHECK((*sandbox)->Evaluate(source, *path).has_value());
			}
		}

		TEST_CASE("Sandbox: optimized direct and aliased math calls use DetMath after sandboxing" * doctest::skip())
		{
			Random random(29);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			auto path = VfsPath::Parse("project://Assets/Math.luau");
			REQUIRE(path.has_value());
			const std::array<MathProbe, 12> probes{
				MathProbe{ "sin", DetMath::Sin },
				MathProbe{ "cos", DetMath::Cos },
				MathProbe{ "tan", DetMath::Tan },
				MathProbe{ "asin", DetMath::ASin },
				MathProbe{ "acos", DetMath::ACos },
				MathProbe{ "atan", DetMath::ATan },
				MathProbe{ "exp", DetMath::Exp },
				MathProbe{ "log", DetMath::Log },
				MathProbe{ "sinh", DetMath::Sinh },
				MathProbe{ "cosh", DetMath::Cosh },
				MathProbe{ "tanh", DetMath::Tanh },
				MathProbe{ "log10", DetMath::Log10 },
			};
			for (const MathProbe& probe : probes)
			{
				for (const double argument : { 0.0000123456789, 0.123456789, 0.73123456789 })
				{
					CAPTURE(probe.Name);
					CAPTURE(argument);
					const std::string source = std::format(
						"local x = tonumber('{:.17g}'); local alias = math.{}; local m = math; "
						"assert(math.{}(x) == {:.17g}); assert(alias(x) == {:.17g}); assert(m.{}(x) == {:.17g})",
						argument, probe.Name, probe.Name, probe.Function(argument), probe.Function(argument), probe.Name, probe.Function(argument));
					CHECK((*sandbox)->Evaluate(source, *path).has_value());
					CHECK((*sandbox)->Evaluate("--!optimize 2\n" + source, *path).has_value());
					CHECK((*sandbox)->Evaluate("--!native\n" + source, *path).has_value());
					const std::string constant = std::format(
						"local alias = math.{}; local m = math; return {{ math.{}({:.17g}), alias({:.17g}), m.{}({:.17g}) }}",
						probe.Name, probe.Name, argument, argument, probe.Name, argument);
					const auto folded = (*sandbox)->Evaluate("--!optimize 2\n" + constant, *path);
					REQUIRE(folded.has_value());
					const double expected = probe.Function(argument);
					CHECK(folded->Value.Get() == Json::array({ expected, expected, expected }));
				}
			}
			const std::string binary = std::format(
				"local x = tonumber('1.23456789'); local y = tonumber('0.731'); local p = math.pow; local a = math.atan2; "
				"assert(math.pow(x, y) == {:.17g}); assert(p(x, y) == {:.17g}); "
				"assert(math.atan2(x, y) == {:.17g}); assert(a(x, y) == {:.17g})",
				DetMath::Pow(1.23456789, 0.731), DetMath::Pow(1.23456789, 0.731),
				DetMath::ATan2(1.23456789, 0.731), DetMath::ATan2(1.23456789, 0.731));
			CHECK((*sandbox)->Evaluate(binary, *path).has_value());
			CHECK((*sandbox)->Evaluate("--!optimize 2\n" + binary, *path).has_value());
		}

		TEST_CASE("Sandbox: math random shares the injected session stream" * doctest::skip())
		{
			Random random(31);
			Random expected(31);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			auto path = VfsPath::Parse("project://Assets/Random.luau");
			REQUIRE(path.has_value());
			const auto value = (*sandbox)->Evaluate("return math.random()", *path);
			REQUIRE(value.has_value());
			CHECK(value->Value.Get() == Json(expected.NextDouble()));
			CHECK(random.GetState() == expected.GetState());
			CHECK((*sandbox)->Evaluate("math.randomseed(97)", *path).has_value());
			expected.Seed(97);
			CHECK(random.GetState() == expected.GetState());
		}

		TEST_CASE("Sandbox: read-only evaluation rejects shared random mutations before changing the stream" * doctest::skip())
		{
			Random random(53);
			const auto initial = random.GetState();
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.ReadOnly = true;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			auto path = VfsPath::Parse("project://Assets/__Automation/ReadOnly.luau");
			REQUIRE(path.has_value());
			for (const std::string_view source : { "return math.random()", "return math.random(1, 7)", "math.randomseed(97)",
					 "local r = math.random; return r()", "local seed = math.randomseed; seed(97)" })
			{
				for (const std::string_view directive : { "", "--!optimize 2\n", "--!native\n" })
				{
					CAPTURE(source);
					CAPTURE(directive);
					CHECK_FALSE((*sandbox)->Evaluate(std::string(directive) + std::string(source), *path).has_value());
					CHECK(random.GetState() == initial);
					const auto error = (*sandbox)->GetLastError();
					REQUIRE(error.has_value());
					CHECK(error->Kind == ScriptErrorKind::Runtime);
				}
			}
			const auto pure = (*sandbox)->Evaluate("local t = {}; t.Value = math.sin(0); return t.Value", *path);
			REQUIRE(pure.has_value());
			CHECK(pure->Value.Get() == Json(0));
			CHECK(random.GetState() == initial);
		}

		TEST_CASE("Sandbox: evaluation returns detached JSON and ordered prints from the owned VM" * doctest::skip())
		{
			Random random(37);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			auto path = VfsPath::Parse("project://Assets/Eval.luau");
			REQUIRE(path.has_value());
			const auto evaluated = (*sandbox)->Evaluate("print('first'); print('second'); return { Answer = 42 }", *path);
			REQUIRE(evaluated.has_value());
			CHECK(evaluated->Value.Get() == Json{ { "Answer", 42 } });
			CHECK(evaluated->Prints == std::vector<std::string>{ "first", "second" });
			const auto compilation = ScriptCompiler::Compile({ .Path = *path, .Source = "return 42" });
			REQUIRE(compilation.has_value());
			ScriptData script;
			script.Bytecode = compilation->Bytecode;
			script.SourceMap = compilation->SourceMap;
			const auto cooked = (*sandbox)->ExecuteBytecode(script);
			REQUIRE(cooked.has_value());
			CHECK(cooked->Value.Get() == Json(42));
		}

		TEST_CASE("Sandbox: external execution slices notify once and deferred work retains its origin" * doctest::skip())
		{
			SUBCASE("nested host writes and shared random use the outer slice")
			{
				FAIL("Using one ScriptEngine-owned Sandbox and a counting public IScriptHost, evaluate two validated host writes "
					 "including Instantiate-triggered OnCreate and shared math.random. Assert OnExternalMutation occurs exactly "
					 "once before the first write, remains notified when a later write faults, and errors publish exactly once");
			}
			SUBCASE("deferred external writes get a fresh latch")
			{
				FAIL("Schedule a deferred task from eval and from a test-driver case. After each yielding slice, begin a new "
					 "recording and resume through the same Sandbox. Assert each later slice retains the external origin and "
					 "notifies once before its first host write, without retaining a ScriptCall or execution-context pointer");
			}
			SUBCASE("permission checks and recorded input do not invalidate")
			{
				FAIL("Assert ordinary gameplay callbacks, recorded Test.Inject input, pure local mutations and local random "
					 "generators do not notify; rejected read-only or invalid-argument host writes neither notify nor mutate. "
					 "Pure load-time calls cannot acquire runtime origin or a host by nesting or resuming");
			}
		}

		TEST_CASE("Sandbox: retained closures keep their original maps across overlapping eval and hot reload" * doctest::skip())
		{
			SUBCASE("overlapping evaluations reuse an authored label")
			{
				FAIL("Through one public ScriptEngine retain a closure from a wrapped expression, then load a return chunk "
					 "with the SAME authored ChunkName but different Path, JsonPointer, line offsets and GeneratedPrefixLines. "
					 "Keep both closures reachable and invoke them after several further evaluations. Assert each error and "
					 "traceback keeps its own authored file/pointer/line/column, with no private VM identity in public output");
			}
			SUBCASE("hot reload preserves maps for retained old functions")
			{
				FAIL("Keep a script-visible reference to an old function while public ScriptEngine::Reload patches its module "
					 "under the same authored label. Invoke the saved old function and the new module function after reload; "
					 "assert original versus new source locations, including nested traceback frames. Repeat after a failed "
					 "reload rollback; neither successful nor failed candidate loads may replace the retained old map");
			}
			SUBCASE("interned map lifetime and budget do not depend on closure collection")
			{
				FAIL("Load repeated and distinct compilations under a small VM limit. All source-map strings, line offsets, "
					 "indexes and retained comparison bytes remain charged through full GC until VM destruction. Alter each "
					 "map field or bytecode independently under the same label/hash and assert no identity aliasing through "
					 "public diagnostics. Exact duplicates may share; metadata exhaustion follows first recovery then stop");
			}
		}

		TEST_CASE("Sandbox: evaluation rejects non-finite cyclic and non-JSON results with owned diagnostics" * doctest::skip())
		{
			Random random(41);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox.has_value());
			auto path = VfsPath::Parse("project://Assets/Invalid.luau");
			REQUIRE(path.has_value());
			for (const std::string_view source : { "return 0/0", "local a = {}; a.Self = a; return a", "return function() end" })
			{
				CAPTURE(source);
				CHECK_FALSE((*sandbox)->Evaluate(source, *path).has_value());
				const auto error = (*sandbox)->GetLastError();
				REQUIRE(error.has_value());
				CHECK(error->Kind == ScriptErrorKind::Runtime);
				CHECK(error->Script == "Assets/Invalid.luau");
			}
		}
	}

}
