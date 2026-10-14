#include "TestsPCH.h"
#include "Engine/Scripting/Sandbox.h"

#include "Engine/Asset/IScriptDiagnosticsProvider.h"
#include "Engine/Core/DetMath.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Core/Log.h"
#include "Engine/Core/Random.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/LuaHelpers.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptCompiler.h"
#include "Engine/Scripting/ScriptLimits.h"
#include "Support/DeathTest.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>
#include <nlohmann/json.hpp>

#include <array>
#include <cmath>
#include <format>
#include <map>
#include <string>
#include <string_view>
#include <utility>

namespace Engine {

	namespace {

		class SandboxModuleReader final : public IScriptModuleReader
		{
		public:
			[[nodiscard]] Result<std::string> ReadModule(const VfsPath& path) override
			{
				Reads.push_back(path.ToString());
				const auto found = Sources.find(path.ToString());
				if (found == Sources.end())
					return MakeError(ErrorCode::NotFound, "missing module '{}'", path);
				return found->second;
			}
		public:
			std::map<std::string, std::string> Sources{};
			std::vector<std::string> Reads{};
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

		int NestedEvaluationProbe(ScriptCall& call)
		{
			const bool explicitError = Lua::Check<bool>(call, 1);
			const auto path = VfsPath::Create("project", "Assets/NestedEvaluation.luau");
			if (!path)
				return Lua::RaiseError(call, path.error());
			const auto result = Lua::GetEngine(call)->Evaluate(explicitError ? "error('nested evaluation failure')" : "Task.Delay(0, function() Scene.CreateEntity('Inner') end)", *path);
			Lua::Push(call, result.has_value());
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

	// Shuts the scripting runtime down while a VM is open: the Lua states counter of the teardown invariants (Architecture
	// §4.1) must be zero by then.
	ENGINE_DEATH_TEST("Scripting/ShutdownWithAnOpenVm")
	{
		SandboxSpecification specification{};
		specification.Mode = SandboxMode::LoadTime;
		auto sandbox = Sandbox::Create(specification);
		if (!sandbox.has_value())
		{
			ENGINE_CORE_ERROR("The child cannot open a VM: {}", sandbox.error());
			return;
		}
		ShutdownScriptingRuntime();
	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("Sandbox: shutting the scripting runtime down while a VM is open asserts")
		{
			ENGINE_CHECK_DEATH("Scripting/ShutdownWithAnOpenVm", "ShutdownScriptingRuntime with 1 scripting VMs still open");
		}

		TEST_CASE("Sandbox: engine-less Runtime restricts metadata availability while retaining runtime limits")
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
			REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
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

		TEST_CASE("Sandbox: escape suite")
		{
			Random random(23);
			SandboxModuleReader reader;
			reader.Sources.emplace("project://Assets/A.luau", "PrivateValue = 7; return {}");
			reader.Sources.emplace("project://Assets/B.luau", "assert(PrivateValue == nil); return {}");
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.SourceModules = &reader;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
			auto path = VfsPath::Parse("project://Assets/Escape.luau");
			REQUIRE(path.has_value());
			const std::array sources{
				"assert(io == nil and package == nil and loadfile == nil and dofile == nil and loadstring == nil)",
				"assert(os == nil or next(os) == nil); assert(string.dump == nil)",
				// The GC-paced heap size is wall-clock driven: no timing-dependent value may reach a gameplay branch.
				"assert(collectgarbage == nil and gcinfo == nil)",
				"assert(not pcall(function() vector.angle = function() return 0 end end))",
				"assert(debug == nil or (debug.getregistry == nil and debug.getupvalue == nil and debug.setupvalue == nil))",
				"assert(getfenv == nil and setfenv == nil)",
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

		TEST_CASE("Sandbox: optimized direct and aliased math calls use DetMath after sandboxing")
		{
			Random random(29);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
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

		TEST_CASE("Sandbox: vector.angle uses DetMath in direct aliased and optimized calls")
		{
			Random random(30);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
			auto path = VfsPath::Parse("project://Assets/Angle.luau");
			REQUIRE(path.has_value());
			struct AngleProbe
			{
				glm::vec3 A{};
				glm::vec3 B{};
				glm::vec3 Axis{};
			};
			// Components with long float mantissas: Luau's lveclib forms the cross product in float and calls the C runtime's
			// atan2, so its result differs from the engine's double, DetMath evaluation.
			const std::array<AngleProbe, 4> probes{
				AngleProbe{ { 0.1f, 0.2f, 0.3f }, { 0.7f, 0.11f, 0.13f }, { 0.0f, 0.0f, 1.0f } },
				AngleProbe{ { 1.0f, 0.0f, 0.0f }, { 0.0f, 1.0f, 0.0f }, { 0.0f, 0.0f, -1.0f } },
				AngleProbe{ { 3.3f, -1.7f, 0.9f }, { -2.1f, 0.4f, 5.5f }, { 0.3f, 0.3f, 0.3f } },
				AngleProbe{ { 0.123456f, 9.87654f, -4.4f }, { 7.25f, -0.003f, 1.0e-3f }, { -1.0f, 2.0f, 0.5f } },
			};
			for (const AngleProbe& probe : probes)
			{
				const glm::dvec3 a(probe.A), b(probe.B);
				const glm::dvec3 cross(a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x);
				const double magnitude = DetMath::ATan2(std::sqrt(cross.x * cross.x + cross.y * cross.y + cross.z * cross.z), a.x * b.x + a.y * b.y + a.z * b.z);
				const double sign = cross.x * probe.Axis.x + cross.y * probe.Axis.y + cross.z * probe.Axis.z < 0.0 ? -1.0 : 1.0;
				const std::string vectors = std::format("local a = vector.create({:.9g}, {:.9g}, {:.9g}); local b = vector.create({:.9g}, {:.9g}, {:.9g}); local axis = vector.create({:.9g}, {:.9g}, {:.9g}); ",
					probe.A.x, probe.A.y, probe.A.z, probe.B.x, probe.B.y, probe.B.z, probe.Axis.x, probe.Axis.y, probe.Axis.z);
				const std::string source = vectors + "local alias = vector.angle; local v = vector; return { vector.angle(a, b), alias(a, b), v.angle(a, b, axis) }";
				for (const std::string_view directive : { "", "--!optimize 2\n", "--!native\n" })
				{
					CAPTURE(source);
					CAPTURE(directive);
					const auto result = (*sandbox)->Evaluate(std::string(directive) + source, *path);
					REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
					CHECK(result->Value.Get() == Json::array({ magnitude, magnitude, sign * magnitude }));
				}
			}
		}

		TEST_CASE("Sandbox: repeated execution of identical bytecode reuses one source map")
		{
			Random random(32);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.MemoryLimitMB = 1;
			specification.ClockSeconds = []
			{
				return 0.0;
			};
			auto sandbox = Sandbox::Create(specification);
			REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
			const auto path = VfsPath::Parse("project://Assets/Tests/Predicate.replay");
			REQUIRE(path.has_value());
			const auto compiled = ScriptCompiler::Compile({ .Path = *path, .Source = "1 + 1 == 2", .Mode = ScriptCompileMode::Expression, .ChunkName = "=replay/Predicate/Expect/0", .JsonPointer = "/Expect/0/Luau" });
			REQUIRE_MESSAGE(compiled.has_value(), (compiled ? "" : compiled.error().ToString()));
			ScriptData predicate;
			predicate.Bytecode = compiled->Bytecode;
			predicate.SourceMap = compiled->SourceMap;
			// play.waitFor runs its predicate once per tick. One new private map per execution (about 130 bytes before
			// interning) would cross this 1 MiB limit long before 20,000 ticks and stop the VM with a memory error.
			for (int tick = 0; tick < 20000; ++tick)
			{
				const auto value = (*sandbox)->ExecuteBytecode(predicate);
				REQUIRE_MESSAGE(value.has_value(), (value ? "" : value.error().ToString()));
				REQUIRE(value->Value.Get() == Json(true));
			}
			CHECK((*sandbox)->GetMemoryState().SoftBreachCount == 0);
			// Interning keeps the exact map: a failing copy of the same predicate still reports its authored location.
			const auto failing = ScriptCompiler::Compile({ .Path = *path, .Source = "error('expected failure')", .Mode = ScriptCompileMode::Chunk, .ChunkName = "=replay/Predicate/Expect/0", .JsonPointer = "/Expect/0/Luau" });
			REQUIRE(failing.has_value());
			ScriptData broken;
			broken.Bytecode = failing->Bytecode;
			broken.SourceMap = failing->SourceMap;
			for (int attempt = 0; attempt < 2; ++attempt)
			{
				CHECK_FALSE((*sandbox)->ExecuteBytecode(broken).has_value());
				const auto error = (*sandbox)->GetLastError();
				REQUIRE(error.has_value());
				CHECK(error->Script == "Assets/Tests/Predicate.replay");
				CHECK(error->JsonPointer == "/Expect/0/Luau");
				CHECK(error->Line == 1);
				CHECK(error->Message.find("engine/") == std::string::npos);
			}
		}

		TEST_CASE("Sandbox: error locations raised for another frame name the authored file")
		{
			Random random(34);
			SandboxModuleReader reader;
			reader.Sources.emplace("project://Assets/Board.luau", "local Board = {}\nfunction Board.Place(x)\n\tif x < 0 then\n\t\terror(\"bad column\", 2)\n\tend\nend\nreturn Board");
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.SourceModules = &reader;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
			const auto path = VfsPath::Parse("project://Assets/Game.luau");
			REQUIRE(path.has_value());
			// Level 2 names the caller (Game.luau line 2), another chunk than the raising frame (Board.luau line 4).
			CHECK_FALSE((*sandbox)->RunSource(*path, "local Board = require('./Board')\nBoard.Place(-1)\nreturn {}").has_value());
			auto error = (*sandbox)->GetLastError();
			REQUIRE(error.has_value());
			CHECK(error->Script == "Assets/Board.luau");
			CHECK(error->Line == 4);
			CHECK(error->Message == "Assets/Game.luau:2: bad column");
			// A caught VM error string rethrown without a position keeps the location of its origin, never a private name.
			const auto eval = VfsPath::Parse("project://Assets/Rethrow.luau");
			REQUIRE(eval.has_value());
			CHECK_FALSE((*sandbox)->Evaluate("local ok, message = pcall(function()\n\tlocal t = nil\n\treturn t.x\nend)\nerror(message, 0)", *eval).has_value());
			error = (*sandbox)->GetLastError();
			REQUIRE(error.has_value());
			CHECK(error->Line == 5);
			CHECK(error->Message.find("engine/") == std::string::npos);
			CHECK(error->Message.starts_with("Assets/Rethrow.luau:3: attempt to index nil"));
			// The same rewrite applies to print, so captured output never names a private chunk either.
			const auto printed = (*sandbox)->Evaluate("local ok, message = pcall(function() local t = nil; return t.x end)\nprint(message)\nreturn true", *eval);
			REQUIRE(printed.has_value());
			REQUIRE(printed->Prints.size() == 1);
			CHECK(printed->Prints[0].starts_with("Assets/Rethrow.luau:1: attempt to index nil"));
		}

		TEST_CASE("Sandbox: a cooked runtime require cycle is detected instead of recursing")
		{
			Random random(35);
			std::map<std::string, Ref<const ScriptData>> cooked;
			for (const auto& [path, source] : { std::pair<std::string_view, std::string_view>{ "Assets/A.luau", "return require('./B')" }, std::pair<std::string_view, std::string_view>{ "Assets/B.luau", "return require('./A')" } })
			{
				const auto origin = VfsPath::Create("project", path);
				REQUIRE(origin.has_value());
				const auto compiled = ScriptCompiler::Compile({ .Path = *origin, .Source = source });
				REQUIRE(compiled.has_value());
				auto data = CreateRef<ScriptData>();
				data->Bytecode = compiled->Bytecode;
				data->SourceMap = compiled->SourceMap;
				cooked.emplace(std::string(path), data);
			}
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.CookedModules = [&cooked](const VfsPath& path) -> Result<Ref<const ScriptData>>
			{
				const auto found = cooked.find(std::string(path.GetPath()));
				if (found == cooked.end())
					return MakeError(ErrorCode::NotFound, "no cooked module '{}'", path);
				return found->second;
			};
			auto sandbox = Sandbox::Create(specification);
			REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
			const Status result = (*sandbox)->RunModule(*cooked.at("Assets/A.luau"));
			REQUIRE_FALSE(result.has_value());
			CAPTURE(result.error().ToString());
			CHECK(result.error().ToString().find("require cycle: Assets/A.luau -> Assets/B.luau -> Assets/A.luau") != std::string::npos);
			CHECK_FALSE((*sandbox)->IsStopped());
		}

		TEST_CASE("Sandbox: print joins at most the log message limit however its arguments alias one string")
		{
			Random random(36);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.MemoryLimitMB = 8;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
			const auto path = VfsPath::Parse("project://Assets/PrintLimit.luau");
			REQUIRE(path.has_value());
			// About 1 MiB of VM memory names a 7 GB message through 7,000 aliases; the host assembles only the capped prefix,
			// and the line keeps at most the cap, its truncation marker included.
			const auto marker = [](uint64_t bytes)
			{
				return std::format(" [truncated: {} bytes]", bytes);
			};
			const size_t straddle = MaxScriptLogMessageBytes - marker(MaxScriptLogMessageBytes).size() - 1;
			const std::string source = std::format("local s = string.rep('x', 2^20); local t = table.create(7000, s); print(table.unpack(t))\n"
												   "print(string.rep('y', {}) .. '\xC3\xA9' .. string.rep('z', 100)); print('short', 2); return true",
				straddle);
			const auto result = (*sandbox)->Evaluate(source, *path);
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			REQUIRE(result->Prints.size() == 3);
			const uint64_t total = 7000ull * 1048576ull + 6999ull;
			CHECK(result->Prints[0] == std::string(MaxScriptLogMessageBytes - marker(total).size(), 'x') + marker(total));
			CHECK(result->Prints[0].size() == MaxScriptLogMessageBytes);
			// The two-byte sequence straddles the cut before the marker: truncation drops it whole instead of splitting it.
			const uint64_t straddleTotal = straddle + 2 + 100;
			REQUIRE(straddleTotal > MaxScriptLogMessageBytes);
			REQUIRE(marker(straddleTotal).size() == marker(MaxScriptLogMessageBytes).size());
			CHECK(result->Prints[1] == std::string(straddle, 'y') + marker(straddleTotal));
			CHECK(result->Prints[2] == "short\t2");
			CHECK_FALSE((*sandbox)->IsStopped());
		}

		TEST_CASE("Sandbox: a raised error keeps a bounded message and its innermost traceback frames")
		{
			Random random(37);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.MemoryLimitMB = 8;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
			const auto path = VfsPath::Parse("project://Assets/Deep.luau");
			REQUIRE(path.has_value());
			// A 600,000-byte message of two-byte sequences, raised 101 calls deep (102 authored frames with the chunk).
			const std::string source = "local function descend(n)\n"
									   "\tif n == 0 then\n"
									   "\t\terror(string.rep('\xC3\xA9', 300000))\n"
									   "\tend\n"
									   "\treturn descend(n - 1) + 1\n"
									   "end\n"
									   "return descend(100)";
			CHECK_FALSE((*sandbox)->Evaluate(source, *path).has_value());
			const auto error = (*sandbox)->GetLastError();
			REQUIRE(error.has_value());
			CHECK(error->Script == "Assets/Deep.luau");
			CHECK(error->Line == 3);
			const std::string marker = " [truncated: 600000 bytes]";
			CHECK(error->Message.size() <= MaxScriptLogMessageBytes);
			CHECK(error->Message.size() > MaxScriptLogMessageBytes - marker.size() - 2);
			CHECK(error->Message.ends_with(marker));
			CHECK(error->Message.starts_with("\xC3\xA9\xC3\xA9"));
			CHECK(IsValidUtf8(error->Message));
			REQUIRE(error->Traceback.size() == MaxScriptErrorTracebackFrames);
			for (size_t index = 0; index + 1 < error->Traceback.size(); ++index)
			{
				CHECK(error->Traceback[index].Script == "Assets/Deep.luau");
				CHECK(error->Traceback[index].Function == "descend");
			}
			CHECK(error->Traceback.front().Line == 3);
			CHECK(error->Traceback[1].Line == 5);
			CHECK(error->Traceback.back().Script.empty());
			CHECK(error->Traceback.back().Line == 0);
			CHECK(error->Traceback.back().Function == std::format("[truncated: {} more frames]", 102 - (MaxScriptErrorTracebackFrames - 1)));
			CHECK_FALSE((*sandbox)->IsStopped());
		}

		TEST_CASE("Sandbox: a require request longer than its limit is refused before it is recorded")
		{
			Random random(38);
			SandboxModuleReader reader;
			reader.Sources.emplace("project://Assets/Board.luau", "return {}");
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.SourceModules = &reader;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
			const auto path = VfsPath::Parse("project://Assets/Game.luau");
			REQUIRE(path.has_value());
			// 551 "./" segments and the module name: 1,107 bytes naming Assets/Board.luau, which resolves at 1,023 bytes.
			CHECK_FALSE((*sandbox)->RunSource(*path, "require(string.rep('./', 551) .. 'Board')\nreturn {}").has_value());
			const auto error = (*sandbox)->GetLastError();
			REQUIRE(error.has_value());
			CHECK(error->Message.find(std::format("is 1107 bytes; the limit is {}", MaxScriptRequireBytes)) != std::string::npos);
			CHECK(error->Line == 1);
			const auto within = (*sandbox)->RunSource(*path, "local board = require(string.rep('./', 509) .. 'Board')\nreturn {}");
			CHECK_MESSAGE(within.has_value(), (within ? std::string() : within.error().ToString()));
			CHECK_FALSE((*sandbox)->IsStopped());
		}

		TEST_CASE("Sandbox: math random shares the injected session stream")
		{
			Random random(31);
			Random expected(31);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
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

		TEST_CASE("Sandbox: read-only evaluation rejects shared random mutations before changing the stream")
		{
			Random random(53);
			const auto initial = random.GetState();
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.ReadOnly = true;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
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

		TEST_CASE("Sandbox: evaluation returns detached JSON and ordered prints from the owned VM")
		{
			Random random(37);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
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

		TEST_CASE("Sandbox: captured print output shares the memory budget and releases on unwind")
		{
			Random random(38);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.MemoryLimitMB = 1;
			specification.ClockSeconds = []
			{
				return 0.0;
			};
			auto sandbox = Sandbox::Create(specification);
			REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
			const auto path = VfsPath::Parse("project://Assets/PrintBudget.luau");
			REQUIRE(path.has_value());
			CHECK_FALSE((*sandbox)->Evaluate("local text = string.rep('x', 4096); for i = 1, 1024 do print(text) end", *path).has_value());
			REQUIRE((*sandbox)->GetLastError().has_value());
			CHECK((*sandbox)->GetLastError()->Kind == ScriptErrorKind::Memory);
			CHECK((*sandbox)->GetMemoryState().SoftBreachCount == 1);
			CHECK_FALSE((*sandbox)->IsStopped());
			const auto recovered = (*sandbox)->Evaluate("print('recovered'); return true", *path);
			REQUIRE(recovered.has_value());
			CHECK(recovered->Prints == std::vector<std::string>{ "recovered" });
		}

		TEST_CASE("Sandbox: cached nil is distinct from missing and failed modules remain retryable")
		{
			Random random(39);
			SandboxModuleReader reader;
			reader.Sources.emplace("project://Assets/Nil.luau", "math.random(); return nil");
			reader.Sources.emplace("project://Assets/Failure.luau", "error('module failed')");
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.SourceModules = &reader;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
			const auto path = VfsPath::Parse("project://Assets/Cache.luau");
			REQUIRE(path.has_value());
			Random expected = random;
			static_cast<void>(expected.NextDouble());
			const auto nil = (*sandbox)->Evaluate("assert(require('./Nil') == nil); assert(require('./Nil.luau') == nil); return true", *path);
			REQUIRE(nil.has_value());
			CHECK(reader.Reads == std::vector<std::string>{ "project://Assets/Nil.luau" });
			CHECK(random.GetState() == expected.GetState());
			REQUIRE((*sandbox)->Evaluate("assert(not pcall(require, './Failure')); return true", *path).has_value());
			reader.Sources["project://Assets/Failure.luau"] = "return 13";
			const auto retry = (*sandbox)->Evaluate("return require('./Failure')", *path);
			REQUIRE(retry.has_value());
			CHECK(retry->Value.Get() == Json(13));
			CHECK(reader.Reads == std::vector<std::string>{ "project://Assets/Nil.luau", "project://Assets/Failure.luau", "project://Assets/Failure.luau" });
		}

		TEST_CASE("Sandbox: a required module rejects multiple return values")
		{
			Random random(40);
			SandboxModuleReader reader;
			reader.Sources.emplace("project://Assets/Multiple.luau", "return 1, 2");
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.SourceModules = &reader;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
			const auto path = VfsPath::Parse("project://Assets/Root.luau");
			REQUIRE(path.has_value());
			CHECK_FALSE((*sandbox)->Evaluate("return require('./Multiple')", *path).has_value());
			REQUIRE((*sandbox)->GetLastError().has_value());
			CHECK((*sandbox)->GetLastError()->Message.find("at most one value") != std::string::npos);
		}

		TEST_CASE("Sandbox: external execution slices notify once and deferred work retains its origin")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			SUBCASE("nested host writes and shared random use the outer slice")
			{
				REQUIRE(fixture.Evaluate("math.random(); math.randomseed(7); math.random()").has_value());
				CHECK(fixture.ExternalMutations.size() == 1);
				fixture.ExternalMutations.clear();
				Test::ExpectLog expectedFailure(LogLevel::Error, "after write");
				CHECK_FALSE(fixture.Evaluate("math.random(); error('after write')").has_value());
				CHECK(fixture.ExternalMutations.size() == 1);
				CHECK(fixture.Errors.size() == 1);
			}
			SUBCASE("deferred external writes get a fresh latch")
			{
				const auto spawned = fixture.Evaluate("Task.Spawn(function() math.random(); Task.WaitTicks(1); math.random(); math.random() end); return nil");
				REQUIRE_MESSAGE(spawned.has_value(), (spawned ? "" : spawned.error().ToString()));
				CHECK(fixture.ExternalMutations.size() == 1);
				fixture.ExternalMutations.clear();
				++fixture.Frame.Tick;
				fixture.GetEngine()->ResumeTasks();
				CHECK(fixture.ExternalMutations.size() == 1);
				CHECK(fixture.Errors.empty());
			}
			SUBCASE("permission checks and local mutations do not invalidate")
			{
				const auto before = fixture.GetRandom().GetState();
				Test::ExpectLog expectedArgument(LogLevel::Error, "math.randomseed: argument #1");
				CHECK_FALSE(fixture.Evaluate("math.randomseed('invalid')").has_value());
				CHECK(fixture.ExternalMutations.empty());
				CHECK(fixture.GetRandom().GetState() == before);
				REQUIRE(fixture.Evaluate("local r = Random.New(3); r:Number(); local t = {}; t.x = 1").has_value());
				CHECK(fixture.ExternalMutations.empty());
				Test::ScriptTestFixture readOnly({ .ReadOnly = true });
				REQUIRE(readOnly.Start().has_value());
				const auto initial = readOnly.GetRandom().GetState();
				Test::ExpectLog expectedReadOnly(LogLevel::Error, "shared script random stream is read-only");
				CHECK_FALSE(readOnly.Evaluate("math.random()").has_value());
				CHECK(readOnly.ExternalMutations.empty());
				CHECK(readOnly.GetRandom().GetState() == initial);
			}
		}

		TEST_CASE("Sandbox: failed nested evaluations restore the resumed caller and publish once")
		{
			Test::ScriptTestFixtureSpecification specification{};
			specification.ConfigureApi = [](ScriptApiRegistry& api) -> Status
			{
				api.Module("Probe", "Nested evaluation observations.").Function("Nested", &NestedEvaluationProbe, "(explicitError: boolean) -> boolean", "Evaluate a failing ownerless chunk from a resumed task.");
				return {};
			};
			Test::ScriptTestFixture fixture(specification);
			REQUIRE(fixture.Start().has_value());
			Test::ExpectLog expectedResult(LogLevel::Error, "value is not a JSON scalar or table");
			const auto spawned = fixture.Evaluate(R"(
Task.Spawn(function()
	assert(not Probe.Nested(false))
	Task.WaitTicks(1)
	assert(not Probe.Nested(true))
	Task.WaitTicks(1)
	Scene.CreateEntity("Outer")
end)
return nil
)");
			REQUIRE_MESSAGE(spawned.has_value(), (spawned ? "" : spawned.error().ToString()));
			REQUIRE(fixture.Errors.size() == 1);
			CHECK(expectedResult.GetMatchCount() == 1);
			Test::ExpectLog expectedError(LogLevel::Error, "nested evaluation failure");
			fixture.Frame.Tick = 1;
			fixture.GetEngine()->ResumeTasks();
			REQUIRE(fixture.Errors.size() == 2);
			CHECK(expectedError.GetMatchCount() == 1);
			CHECK_FALSE(fixture.Errors.back().Entity.IsValid());
			CHECK(fixture.Errors.back().Script == "Assets/NestedEvaluation.luau");
			fixture.Frame.Tick = 2;
			fixture.GetEngine()->ResumeTasks();
			CHECK(fixture.GetScene().GetEntityCount() == 2);
			CHECK(fixture.Errors.size() == 2);
			const auto recovered = fixture.Evaluate("return 19");
			REQUIRE_MESSAGE(recovered.has_value(), (recovered ? "" : recovered.error().ToString()));
			CHECK(recovered->Value.Get() == Json(19));
		}

		TEST_CASE("Sandbox: retained closures keep their original maps across overlapping eval and hot reload")
		{
			SUBCASE("overlapping evaluations reuse an authored label")
			{
				Random random(79);
				SandboxModuleReader reader;
				reader.Sources.emplace("project://Assets/Store.luau", "return {}");
				SandboxSpecification specification{};
				specification.RandomStream = &random;
				specification.SourceModules = &reader;
				auto sandbox = Sandbox::Create(specification);
				REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
				auto oldPath = VfsPath::Parse("project://Assets/Old.luau");
				auto newPath = VfsPath::Parse("project://Assets/New.luau");
				REQUIRE(oldPath.has_value());
				REQUIRE(newPath.has_value());
				const auto oldCode = ScriptCompiler::Compile({ .Path = *oldPath,
					.Source = "(function()\nrequire('./Store').Old = function()\nerror('old')\nend\nend)()",
					.Mode = ScriptCompileMode::Expression,
					.ChunkName = "=same",
					.JsonPointer = "/Expect/0/Luau" });
				const auto newCode = ScriptCompiler::Compile({ .Path = *newPath,
					.Source = "require('./Store').New = function()\nerror('new')\nend",
					.ChunkName = "=same",
					.JsonPointer = "/Expect/1/Luau" });
				REQUIRE(oldCode.has_value());
				REQUIRE(newCode.has_value());
				for (const auto* code : { &*oldCode, &*newCode })
				{
					ScriptData script;
					script.Bytecode = code->Bytecode;
					script.SourceMap = code->SourceMap;
					REQUIRE((*sandbox)->ExecuteBytecode(script).has_value());
				}
				for (int i = 0; i < 3; ++i)
					REQUIRE((*sandbox)->Evaluate("return true", *newPath).has_value());
				CHECK_FALSE((*sandbox)->Evaluate("require('./Store').Old()", *newPath).has_value());
				const auto oldError = (*sandbox)->GetLastError();
				REQUIRE(oldError.has_value());
				CHECK(oldError->Script == "Assets/Old.luau");
				CHECK(oldError->JsonPointer == "/Expect/0/Luau");
				CHECK(oldError->Line == 3);
				CHECK(oldError->Message.find("engine/") == std::string::npos);
				CHECK_FALSE((*sandbox)->Evaluate("require('./Store').New()", *oldPath).has_value());
				const auto newError = (*sandbox)->GetLastError();
				REQUIRE(newError.has_value());
				CHECK(newError->Script == "Assets/New.luau");
				CHECK(newError->JsonPointer == "/Expect/1/Luau");
				CHECK(newError->Line == 2);
			}
			SUBCASE("hot reload preserves maps for retained old functions")
			{
				Test::ScriptTestFixture fixture;
				const AssetHandle handle(703);
				REQUIRE(fixture.AddScript(handle, "Assets/Versioned.luau",
								   "local C = {}\nfunction C:Fail()\nerror('old body')\nend\nreturn Script.Define('C', C)")
						.has_value());
				const auto entity = fixture.GetScene().CreateEntity("Versioned");
				entity.AddComponent<ScriptComponent>().Script.SetHandle(handle);
				REQUIRE(fixture.Start().has_value());
				REQUIRE(fixture.Evaluate("self.Saved = self.Fail", entity.GetUUID()).has_value());
				REQUIRE(fixture.AddScript(handle, "Assets/Versioned.luau",
								   "local C = {}\n\n\nfunction C:Fail()\nerror('new body')\nend\nreturn Script.Define('C', C)")
						.has_value());
				REQUIRE(fixture.GetEngine()->Reload(handle).has_value());
				Test::ExpectLog expectedOld(LogLevel::Error, "old body");
				CHECK_FALSE(fixture.Evaluate("self:Saved()", entity.GetUUID()).has_value());
				REQUIRE_FALSE(fixture.Errors.empty());
				CHECK(fixture.Errors.back().Line == 3);
				Test::ExpectLog expectedNew(LogLevel::Error, "new body");
				CHECK_FALSE(fixture.Evaluate("self:Fail()", entity.GetUUID()).has_value());
				CHECK(fixture.Errors.back().Line == 5);
			}
			SUBCASE("interned map lifetime and budget do not depend on closure collection")
			{
				Random random(83);
				SandboxSpecification specification{};
				specification.RandomStream = &random;
				specification.MemoryLimitMB = 1;
				specification.ClockSeconds = []
				{
					return 0.0;
				};
				auto sandbox = Sandbox::Create(specification);
				REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
				const auto compilation = ScriptCompiler::Compile({ .Source = "return true", .ChunkName = "=budget" });
				REQUIRE(compilation.has_value());
				ScriptData script;
				script.Bytecode = compilation->Bytecode;
				script.SourceMap = compilation->SourceMap;
				const auto initial = (*sandbox)->GetMemoryState().UsedBytes;
				script.SourceMap.JsonPointer = "/" + std::string(2 * 1024 * 1024, 'x');
				CHECK_FALSE((*sandbox)->ExecuteBytecode(script).has_value());
				CHECK((*sandbox)->GetMemoryState().UsedBytes > initial + 2 * 1024 * 1024);
				CHECK((*sandbox)->GetMemoryState().SoftBreachCount == 1);
				CHECK((*sandbox)->IsStopped()); // retained maps cannot be reclaimed by recovery GC
				const auto error = (*sandbox)->GetLastError();
				REQUIRE(error.has_value());
				CHECK(error->Kind == ScriptErrorKind::Memory);
			}
		}

		TEST_CASE("Sandbox: native JSON budgets reject shared DAGs and repeated string storage without stopping the VM")
		{
			Random random(73);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.MemoryLimitMB = 8;
			specification.ClockSeconds = []
			{
				return 0.0;
			};
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox);
			const auto path = VfsPath::Parse("project://Assets/JsonBudget.luau");
			REQUIRE(path);
			// These inputs remain small even under the old converter: a failure is the new budget, not host OOM.
			for (const std::string_view source : {
					 "local t = {1}; for i = 1,12 do t = {t,t} end; return t",
					 "return table.create(17, string.rep('x', 65536))",
					 "local t = {}; local key = string.rep('k', 65536); for i = 1,20 do t[key .. i] = i end; return t",
				 })
			{
				CAPTURE(source);
				const auto result = (*sandbox)->Evaluate(source, *path);
				REQUIRE_FALSE(result);
				CHECK(result.error().GetCode() == ErrorCode::Script);
				const auto error = (*sandbox)->GetLastError();
				REQUIRE(error);
				CHECK(error->Kind == ScriptErrorKind::Runtime);
				CHECK(error->Script == "Assets/JsonBudget.luau");
				CHECK(error->Message.find("native budget") != std::string::npos);
				CHECK_FALSE((*sandbox)->IsStopped());
				CHECK((*sandbox)->GetMemoryState().SoftBreachCount == 0);
				const auto healthy = (*sandbox)->Evaluate("return {Ready = true}", *path);
				REQUIRE(healthy);
				CHECK(healthy->Value.Get() == Json({ { "Ready", true } }));
				CHECK_FALSE((*sandbox)->GetLastError());
			}
		}

		TEST_CASE("Sandbox: native JSON allowance follows a smaller configured memory limit")
		{
			for (const uint32_t limit : { 1u, 8u })
			{
				CAPTURE(limit);
				Random random(79);
				SandboxSpecification specification{};
				specification.RandomStream = &random;
				specification.MemoryLimitMB = limit;
				specification.ClockSeconds = []
				{
					return 0.0;
				};
				auto sandbox = Sandbox::Create(specification);
				REQUIRE(sandbox);
				const auto result = (*sandbox)->Evaluate("local t = {1}; for i = 1,10 do t = {t,t} end; return t", {});
				if (limit == 1)
				{
					REQUIRE_FALSE(result);
					CHECK(result.error().GetMessageText().find("native budget") != std::string::npos);
				}
				else
				{
					REQUIRE(result);
					CHECK(result->Value.Get().is_array());
					CHECK(result->Value.Get().size() == 2);
				}
				CHECK((*sandbox)->GetMemoryState().SoftBreachCount == 0);
				CHECK_FALSE((*sandbox)->IsStopped());
			}
		}

		TEST_CASE("Sandbox: bounded JSON preserves shared values and the exact nesting boundary")
		{
			Random random(83);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.ClockSeconds = []
			{
				return 0.0;
			};
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox);
			const auto shared = (*sandbox)->Evaluate("local t = {Flag = true, Values = {1,2,3}}; return {Right = t, Empty = {}, Left = t}", {});
			REQUIRE(shared);
			const Json value = { { "Flag", true }, { "Values", Json::array({ 1, 2, 3 }) } };
			CHECK(shared->Value.Get() == Json({ { "Empty", Json::object() }, { "Left", value }, { "Right", value } }));
			for (const std::string_view source : {
					 "local t = {}; t.Self = t; return t",
					 "local a = {}; local b = {a}; a.Child = b; return a",
				 })
			{
				const auto cyclic = (*sandbox)->Evaluate(source, {});
				REQUIRE_FALSE(cyclic);
				CHECK(cyclic.error().GetMessageText().find("cycle") != std::string::npos);
			}
			const auto nested = (*sandbox)->Evaluate(std::format("local t = 1; for i = 1,{} do t = {{t}} end; return t", MaxJsonDepth), {});
			REQUIRE(nested);
			const Json* leaf = &nested->Value.Get();
			for (size_t depth = 0; depth < MaxJsonDepth; ++depth)
			{
				REQUIRE(leaf->is_array());
				REQUIRE(leaf->size() == 1);
				leaf = &(*leaf)[0];
			}
			CHECK(*leaf == Json(1));
			const auto mixed = (*sandbox)->Evaluate(std::format(R"(
local value = 7
for i = 1,{} do
	if i % 2 == 1 then value = {{Child = value}} else value = {{value}} end
end
return {{Right = value, Left = value, Tail = 9}}
)",
														MaxJsonDepth - 1),
				{});
			REQUIRE(mixed);
			CHECK(mixed->Value.Get().begin().key() == "Left");
			CHECK(mixed->Value.Get()["Tail"] == Json(9));
			for (const char* name : { "Left", "Right" })
			{
				const Json* mixedLeaf = &mixed->Value.Get()[name];
				for (size_t depth = MaxJsonDepth - 1; depth > 0; --depth)
				{
					if (depth % 2 == 1)
					{
						REQUIRE(mixedLeaf->is_object());
						REQUIRE(mixedLeaf->contains("Child"));
						mixedLeaf = &(*mixedLeaf)["Child"];
					}
					else
					{
						REQUIRE(mixedLeaf->is_array());
						REQUIRE(mixedLeaf->size() == 1);
						mixedLeaf = &(*mixedLeaf)[0];
					}
				}
				CHECK(*mixedLeaf == Json(7));
			}
			const auto tooDeep = (*sandbox)->Evaluate(std::format("local t = 1; for i = 1,{} do t = {{t}} end; return t", MaxJsonDepth + 1), {});
			REQUIRE_FALSE(tooDeep);
			CHECK(tooDeep.error().GetMessageText().find("nesting limit") != std::string::npos);
			CHECK((*sandbox)->Evaluate("return true", {}).has_value());
		}

		TEST_CASE("Sandbox: native JSON traversal observes the inherited deadline after script return")
		{
			Random random(89);
			double now = 0.0;
			bool armed = false;
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			specification.IsTestRun = true;
			specification.ClockSeconds = [&now, &armed]
			{
				if (armed)
					now += 0.01;
				return now;
			};
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox);
			const auto path = VfsPath::Parse("project://Assets/JsonDeadline.luau");
			REQUIRE(path);
			// table.create has no Luau loop; the old reader only sampled at entry/exit and completed this input.
			armed = true;
			const auto timedOut = (*sandbox)->Evaluate("return table.create(512, 1)", *path);
			armed = false;
			REQUIRE_FALSE(timedOut);
			CHECK(timedOut.error().GetCode() == ErrorCode::Timeout);
			const auto error = (*sandbox)->GetLastError();
			REQUIRE(error);
			CHECK(error->Kind == ScriptErrorKind::Timeout);
			CHECK(error->Script == "Assets/JsonDeadline.luau");
			CHECK_FALSE((*sandbox)->IsStopped());
			const auto healthy = (*sandbox)->Evaluate("return table.create(512, 1)", *path);
			REQUIRE(healthy);
			CHECK(healthy->Value.Get() == Json(Json::array_t(512, Json(1))));
			CHECK_FALSE((*sandbox)->GetLastError());
		}

		TEST_CASE("Sandbox: evaluation rejects non-finite cyclic and non-JSON results with owned diagnostics")
		{
			Random random(41);
			SandboxSpecification specification{};
			specification.RandomStream = &random;
			auto sandbox = Sandbox::Create(specification);
			REQUIRE_MESSAGE(sandbox.has_value(), (sandbox ? "" : sandbox.error().ToString()));
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
