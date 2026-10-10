#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Core/Base.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scripting/RegisterBindings.h"

#include <doctest/doctest.h>

#include <string>

namespace Engine {

	static int RegistryContractCallback(ScriptCall& /*call*/)
	{
		ENGINE_CONTRACT_STUB();
		return 0;
	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptApi: every registered function, method, property, operator and constructor has a test" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Implement registry-derived acceptance inventory versus executed test coverage, including proxy read/write and "
				"enum-value gaps in each run mode");
		}

		TEST_CASE("ScriptApiRegistry: one fluent registration supplies binding metadata and generated text" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			TypeRegistry types;
			types.Freeze();
			ScriptApiRegistry api;
			api.Module("Probe", "Contract probe module.")
				.Function("Read", &RegistryContractCallback, "() -> number", "Reads the probe.", { .Mutates = false });
			api.Type("ProbeValue", "Contract probe value.")
				.Method("Read", &RegistryContractCallback, "(self: ProbeValue) -> number", "Reads a value.", { .Mutates = false })
				.Property("Value", &RegistryContractCallback, &RegistryContractCallback, "number", "The stored value.")
				.Operator("__eq", &RegistryContractCallback, "(self: ProbeValue, other: ProbeValue) -> boolean", "Compares values.")
				.Constructor("New", &RegistryContractCallback, "() -> ProbeValue", "Creates a value.");
			REQUIRE(api.Freeze(types));
			CHECK(api.IsFrozen());
			REQUIRE(api.GetModules().size() == 1);
			REQUIRE(api.GetTypes().size() == 1);
			CHECK(api.GetModules()[0].Members.size() == 1);
			CHECK(api.GetTypes()[0].Members.size() == 4);
			const auto definitions = api.GenerateDefinitions();
			const auto documentation = api.GenerateDocumentation();
			REQUIRE(definitions);
			REQUIRE(documentation);
			CHECK(definitions->find("declare extern type ProbeValue") != std::string::npos);
			CHECK(documentation->find("Reads the probe.") != std::string::npos);
		}

		TEST_CASE("ScriptApiRegistry: enum parameters share the authoritative table and retain zero counters" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			EnumInfo space("Space", "The coordinate system.");
			space.AddEntry({ "Local", 0, "Local coordinates." });
			space.AddEntry({ "World", 1, "World coordinates." });
			TypeRegistry types;
			types.Freeze();
			ScriptApiRegistry api;
			REQUIRE(api.RegisterEnum(space));
			ScriptMemberOptions options;
			options.EnumParameters.push_back({ 1, "Space", true, "Local" });
			api.Module("Probe", "Contract enum probe.")
				.Function("Translate", &RegistryContractCallback, "(space: Space?) -> ()", "Uses a coordinate system.", options);
			REQUIRE(api.Freeze(types));
			const auto coverage = api.GetCoverage(RunModes::Dist);
			REQUIRE(coverage);
			REQUIRE(coverage->Members.size() == 1);
			REQUIRE(coverage->Members[0].EnumValues.size() == 2);
			CHECK(coverage->Members[0].EnumValues[0].Count == 0);
			CHECK(coverage->Members[0].EnumValues[1].Count == 0);
		}

		TEST_CASE("ScriptApiRegistry: freeze rejects incomplete metadata and shortcut collisions" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Implement malformed signature/description/enum-slot fixtures and death tests for duplicate members and Entity "
				"shortcut collisions; verify no partial freeze");
		}

		TEST_CASE("ScriptApiRegistry: generated declarations preserve generics overloads constants and reflected fields" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Type-check generated aliases, Script.Define<T>, GetScript:any, GetComponent literal overloads, Quat operators, Color "
				"fields, Math constants and enum unions with the pinned frontend");
		}

		TEST_CASE("ScriptApiRegistry: binding availability is independent of run mode" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Exercise LoadTime with null Engine and no host, hosted Runtime and Test; Test.Suite works in all three, other Test "
				"calls fail in ordinary play, and OnHotReload/ReloadScript are EditorOnly. Standalone Runtime with null Engine "
				"and no host permits only normally eligible members whose metadata also includes LoadTime, including Test.Suite; "
				"other APIs report 'requires an active script engine' before callback or host access. Assert zero callback "
				"side effects for rejected APIs, unchanged coverage counters and the retained Runtime budget, not the LoadTime budget");
		}

		TEST_CASE("ScriptApiRegistry: read-only dispatch rejects host mutations before invoking callbacks" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Use callback side-effect sentinels for entity/scene/physics/audio/time/input writes, Task APIs and session "
				"Random; verify zero invocation and unchanged state while pure values and Random.New local streams remain usable");
		}

		TEST_CASE("ScriptApiRegistry: property getters and setters have independent mutation policies" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Read-only eval permits host property reads and local Color/Quat edits, rejects host setters, and records no "
				"write or enum hit for a rejected setter");
		}

		TEST_CASE("ScriptApiRegistry: coverage keeps separate mode read write and enum counters" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Invoke actual trampolines in test mode; verify independent Editor/Release/Dist buckets, zero targets, successful "
				"access counts, callback counts, ResetCoverage and no production/load-time increments");
		}

		TEST_CASE("ScriptApiRegistry: enum coverage distinguishes parameters defaults and large tables" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Two enum arguments have independent counters; canonicalize case, count optional defaults, reject typos with "
				"hints, count every value through 16 and use table-driven validation for 17-plus values");
		}

		TEST_CASE("ScriptApiRegistry: generated text is deterministic and uses mandatory descriptions" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Register groups in permuted orders and require identical declaration/docs output, including modes and mutation "
				"policies; descriptions and field metadata must come from their single owning registration");
		}

		TEST_CASE("RegisterBindings: the built-in registry covers the complete scripting surface" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL(
				"Register builtin reflected types then RegisterBindings; compare the complete section 11.5 surface and optional "
				"lifecycle callbacks with the acceptance inventory, never a second runtime metadata table");
		}
	}

}
