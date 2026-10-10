#include "TestsPCH.h"
#include "Engine/Scripting/LuaHelpers.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Core/Json/Json.h"
#include "Engine/Reflection/TypeRegistry.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptCompiler.h"
#include "Engine/Scripting/ScriptError.h"
#include "Engine/Scripting/TaskScheduler.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <format>
#include <map>
#include <string>
#include <type_traits>
#include <vector>

namespace Engine {

	static_assert(std::is_same_v<ScriptNativeFunction, int (*)(ScriptCall&)>);
	static_assert(std::is_same_v<decltype(&Lua::Check<float>), float (*)(ScriptCall&, int)>);
	static_assert(std::is_same_v<decltype(&Lua::Push<glm::vec3>), void (*)(ScriptCall&, const glm::vec3&)>);

	template<typename T>
	static int HelperEcho(ScriptCall& call)
	{
		Lua::Push(call, Lua::Check<T>(call, 1));
		return 1;
	}
	static int HelperAsset(ScriptCall& call)
	{
		Lua::Push(call, AssetHandle(0x1234));
		return 1;
	}
	static int HelperAssetText(ScriptCall& call)
	{
		Lua::PushString(call, Lua::Check<AssetHandle>(call, 1).ToString());
		return 1;
	}
	static int HelperNil(ScriptCall& call)
	{
		Lua::Push(call, AssetHandle{});
		return 1;
	}
	static int HelperHasEngine(ScriptCall& call)
	{
		Lua::Push(call, Lua::GetEngine(call) != nullptr);
		return 1;
	}
	static int HelperMemory(ScriptCall& call)
	{
		Lua::PushString(call, std::string(32 * 1024 * 1024, 'x'));
		return 1;
	}
	static int HelperForwarded(ScriptCall& call)
	{
		const ScriptError error{
			.Kind = ScriptErrorKind::Type,
			.Script = "Assets/Child.luau",
			.Line = 71,
			.Column = 9,
			.Message = "child diagnostic",
			.Callback = "OnUpdate",
			.Entity = UUID(65),
			.EntityName = "ChildOwner",
			.Tick = 9007199254740993ull,
			.Traceback = { { "Assets/Child.luau", 71, "ChildFunction" }, { "Assets/Dependency.luau", 12, "Dependency" } },
			.JsonPointer = "/Expect/3/Luau"
		};
		return Lua::RaiseError(call, error);
	}
	struct HelperDeepValue
	{
		std::vector<HelperDeepValue> Children{};
	};
	struct HelperContainers
	{
		std::map<std::string, VariantValue> Variants{};
		std::map<std::string, std::vector<std::string>> Strings{};
		HelperDeepValue Deep{};
		std::vector<HelperDeepValue> DeepList{};
	};
	static int HelperContainer(ScriptCall& call)
	{
		TypeRegistry types;
		types.Struct<HelperDeepValue>("HelperDeepValue", "Recursive schema for the reflected nesting limit.")
			.Field("Children", &HelperDeepValue::Children, "Nested child values.");
		types.Struct<HelperContainers>("HelperContainers", "Native conversion test schemas.")
			.Field("Variants", &HelperContainers::Variants, "Unresolved values sharing one conversion allowance.")
			.Field("Strings", &HelperContainers::Strings, "Typed maps of arrays of strings.")
			.Field("Deep", &HelperContainers::Deep, "A recursive reflected struct.")
			.Field("DeepList", &HelperContainers::DeepList, "An array of recursive reflected structs.");
		types.Freeze();
		const auto* field = types.FindStruct<HelperContainers>()->FindField(Lua::Check<std::string>(call, 1));
		if (!field)
			return Lua::RaiseError(call, "unknown container probe");
		const auto value = Lua::CheckValue(call, 2, *field);
		Lua::PushValue(call, value, *field);
		return 1;
	}
	static Status HelperProbes(ScriptApiRegistry& api)
	{
		const ScriptMemberOptions pure{ .Environments = ScriptApiEnvironment::All, .Mutates = false };
		api.Module("Probe", "Typed marshalling probes.")
			.Function("Bool", HelperEcho<bool>, "(value: boolean) -> boolean", "Round-trip a boolean.", pure)
			.Function("Signed", HelperEcho<int32_t>, "(value: number) -> number", "Round-trip an exact signed integer.", pure)
			.Function("Unsigned", HelperEcho<uint32_t>, "(value: number) -> number", "Round-trip an exact unsigned integer.", pure)
			.Function("Float", HelperEcho<float>, "(value: number) -> number", "Round-trip a finite f32.", pure)
			.Function("Double", HelperEcho<double>, "(value: number) -> number", "Round-trip a finite f64.", pure)
			.Function("String", HelperEcho<std::string>, "(value: string) -> string", "Round-trip an owned byte string.", pure)
			.Function("Vec2", HelperEcho<glm::vec2>, "(value: vector) -> vector", "Round-trip XY with zero Z.", pure)
			.Function("Vec3", HelperEcho<glm::vec3>, "(value: vector) -> vector", "Round-trip XYZ.", pure)
			.Function("Color", HelperEcho<glm::vec4>, "(value: Color) -> Color", "Round-trip tagged RGBA.", pure)
			.Function("Quat", HelperEcho<glm::quat>, "(value: Quat) -> Quat", "Round-trip tagged XYZW.", pure)
			.Function("Entity", HelperEcho<ScriptEntityIdentity>, "(value: Entity) -> Entity", "Round-trip an entity identity.", pure)
			.Function("Proxy", HelperEcho<ScriptProxyIdentity>, "(value: Transform) -> Transform", "Round-trip a component identity.", { .Mutates = false })
			.Function("Asset", HelperAsset, "() -> AssetRef", "Push a value-only asset handle.", pure)
			.Function("AssetText", HelperAssetText, "(value: AssetRef) -> string", "Read an asset handle.", pure)
			.Function("Nil", HelperNil, "() -> AssetRef?", "Push an invalid reference as nil.", pure)
			.Function("HasEngine", HelperHasEngine, "() -> boolean", "Whether this call borrows a runtime engine.", pure)
			.Function("Memory", HelperMemory, "() -> string", "Allocate beyond the test VM's hard limit.", pure);
		api.Module("Probe", "Typed marshalling probes.").Function("Forwarded", HelperForwarded, "() -> ()", "Forward a located child error.", pure);
		api.Module("Probe", "Typed marshalling probes.").Function("Container", HelperContainer, "(kind: string, value: any) -> any", "Convert reflected containers with a shared native allowance.", pure);
		return {};
	}
	static void CheckHelperScript(Test::ScriptTestFixture& fixture, std::string_view source, std::optional<UUID> entity = {})
	{
		const auto result = fixture.Evaluate(source, entity);
		REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
		CHECK(result->Value.Get() == true);
	}
	static UUID HelperBehaviour(Test::ScriptTestFixture& fixture, AssetHandle handle, std::string path, std::string source, std::string_view name = "Owner")
	{
		const auto asset = fixture.AddScript(handle, std::move(path), std::move(source));
		REQUIRE_MESSAGE(asset.has_value(), (asset ? "" : asset.error().ToString()));
		const Entity entity = fixture.GetScene().CreateEntity(name);
		ScriptComponent script;
		script.Script.SetHandle(handle);
		entity.AddComponent<ScriptComponent>(script);
		return entity.GetUUID();
	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("LuaHelpers: public engine access is borrowed and absent in hostless environments")
		{
			Test::ScriptTestFixture fixture({ .ConfigureApi = HelperProbes });
			REQUIRE(fixture.Start());
			CheckHelperScript(fixture, "return Probe.HasEngine()");
			Random random(37);
			for (const auto mode : { SandboxMode::LoadTime, SandboxMode::Runtime })
			{
				auto sandbox = Sandbox::Create({ .Mode = mode, .Api = &fixture.GetApi(), .RandomStream = &random });
				REQUIRE(sandbox);
				const auto evaluated = (*sandbox)->Evaluate("return Probe.HasEngine()", {});
				REQUIRE(evaluated);
				CHECK(evaluated->Value.Get() == false);
			}
		}
		TEST_CASE("LuaHelpers: a caught child timeout retains its source through the outer post-return guard")
		{
			const auto childPath = VfsPath::Create("project", "Assets/TimeoutChild.luau");
			const auto rootPath = VfsPath::Create("project", "Assets/TimeoutRoot.luau");
			REQUIRE(childPath);
			REQUIRE(rootPath);
			const auto child = ScriptCompiler::Compile({ .Path = *childPath, .Source = "local value = 0\nwhile true do value += 1 end\nreturn value" });
			REQUIRE(child);
			auto childData = CreateRef<ScriptData>();
			childData->Bytecode = child->Bytecode;
			childData->SourceMap = child->SourceMap;
			Random random(47);
			double now = 0;
			bool armed = false;
			SandboxSpecification specification;
			specification.RandomStream = &random;
			specification.IsTestRun = true;
			specification.ClockSeconds = [&now, &armed]
			{
				if (armed)
					now += 0.02;
				return now;
			};
			specification.CookedModules = [childData, &armed](const VfsPath&) -> Result<Ref<const ScriptData>>
			{
				armed = true;
				return childData;
			};
			auto sandbox = Sandbox::Create(specification);
			REQUIRE(sandbox);
			CHECK_FALSE((*sandbox)->Evaluate("local ok = pcall(require, \"./TimeoutChild\")\nreturn ok", *rootPath));
			const auto timeout = (*sandbox)->GetLastError();
			REQUIRE(timeout);
			CHECK(timeout->Kind == ScriptErrorKind::Timeout);
			CHECK(timeout->Script == "Assets/TimeoutChild.luau");
			CHECK(timeout->Line == 2);
			CHECK_FALSE(timeout->Traceback.empty());
			armed = false;
			CHECK_FALSE((*sandbox)->Evaluate("local value = 1\nerror(\"unrelated after timeout\")", *rootPath));
			const auto unrelated = (*sandbox)->GetLastError();
			REQUIRE(unrelated);
			CHECK(unrelated->Kind == ScriptErrorKind::Runtime);
			CHECK(unrelated->Script == "Assets/TimeoutRoot.luau");
			CHECK(unrelated->Line == 2);
		}

		TEST_CASE("LuaHelpers: forwarded child errors preserve owned diagnostics without tainting later failures")
		{
			Test::ScriptTestFixture fixture({ .ConfigureApi = HelperProbes });
			fixture.Frame.Tick = 9007199254740993ull;
			REQUIRE(fixture.Start());
			for (const auto source : { "Probe.Forwarded()", "local ok, caught = pcall(Probe.Forwarded); assert(not ok and tostring(caught) == \"child diagnostic\"); error(caught)" })
			{
				Test::ExpectLog expected(LogLevel::Error, "child diagnostic");
				CHECK_FALSE(fixture.Evaluate(source));
				CHECK(expected.GetMatchCount() == 1);
				REQUIRE_FALSE(fixture.Errors.empty());
				const auto& error = fixture.Errors.back();
				CHECK(error.Kind == ScriptErrorKind::Type);
				CHECK(error.Script == "Assets/Child.luau");
				CHECK(error.Line == 71);
				CHECK(error.Column == 9);
				CHECK(error.JsonPointer == "/Expect/3/Luau");
				CHECK(error.Callback == "OnUpdate");
				CHECK(error.Entity == UUID(65));
				CHECK(error.EntityName == "ChildOwner");
				CHECK(error.Tick == 9007199254740993ull);
				REQUIRE(error.Traceback.size() == 2);
				CHECK(error.Traceback[0].Function == "ChildFunction");
				CHECK(error.Traceback[1].Script == "Assets/Dependency.luau");
			}
			{
				Test::ExpectLog unrelatedLog(LogLevel::Error, "unrelated");
				CHECK_FALSE(fixture.Evaluate("pcall(Probe.Forwarded)\nerror(\"unrelated\")"));
				CHECK(unrelatedLog.GetMatchCount() == 1);
				REQUIRE_FALSE(fixture.Errors.empty());
				const auto& unrelated = fixture.Errors.back();
				CHECK(unrelated.Kind == ScriptErrorKind::Runtime);
				CHECK(unrelated.Script != "Assets/Child.luau");
				CHECK(unrelated.Line == 2);
				CHECK(unrelated.JsonPointer.empty());
				CHECK(unrelated.Message.find("unrelated") != std::string::npos);
			}
			{
				Test::ExpectLog nonstringLog(LogLevel::Error, "script raised a non-string error");
				CHECK_FALSE(fixture.Evaluate("error({Kind = \"type\", Script = \"Assets/Child.luau\", Line = 71})"));
				CHECK(nonstringLog.GetMatchCount() == 1);
				CHECK(fixture.Errors.back().Kind == ScriptErrorKind::Runtime);
				CHECK(fixture.Errors.back().Script != "Assets/Child.luau");
			}
		}

		TEST_CASE("LuaHelpers: scene parameters deeply copy JSON and reject nonserializable tables")
		{
			Test::ScriptTestFixture fixture;
			auto scene = CreateRef<SceneData>();
			scene->Document = CreateRef<const Json>(Json::object());
			fixture.GetAssetManager().Publish(AssetHandle(77), scene, "Assets/Next.scene");
			Json nested = { { "Value", 9 } };
			for (int i = 0; i < 100; ++i)
				nested = Json{ { "Child", std::move(nested) } };
			fixture.Parameters = nested;
			REQUIRE(fixture.Start());
			CheckHelperScript(fixture, R"(
local params = Scene.GetLoadParameters()
local leaf = params
for _ = 1,100 do leaf = leaf.Child end
assert(leaf.Value == 9)
leaf.Value = 12
params["a\0b"] = "c\0d"
local nextScene = Assets.Load("Assets/Next.scene")
Scene.Load(nextScene, params)
local fresh = Scene.GetLoadParameters()
for _ = 1,100 do fresh = fresh.Child end
assert(fresh.Value == 9)
local cycle = {}; cycle.Self = cycle
for _, value in {
	cycle, {function() end}, {[2] = true}, {[1] = true, Named = false},
	{Bad = math.huge}, {Bad = vector.one}, {Bad = Color.New(1,1,1)},
	{Bad = string.char(255)}, {[string.char(255)] = true},
} do assert(not pcall(function() Scene.Load(nextScene, value) end)) end
return true
)");
			Json expected = { { "Value", 12 } };
			for (int i = 0; i < 100; ++i)
				expected = Json{ { "Child", std::move(expected) } };
			expected[std::string("a\0b", 3)] = std::string("c\0d", 3);
			CHECK(fixture.RequestedParameters == expected);
			CHECK(fixture.ExternalMutations.size() == 1);
		}
		TEST_CASE("LuaHelpers: scene JSON budgets reject expansion before host mutation and recover for valid aliases")
		{
			Test::ScriptTestFixtureSpecification specification{};
			specification.ClockSeconds = []
			{
				return 0.0;
			};
			Test::ScriptTestFixture fixture(specification);
			auto scene = CreateRef<SceneData>();
			scene->Document = CreateRef<const Json>(Json::object());
			fixture.GetAssetManager().Publish(AssetHandle(77), scene, "Assets/Next.scene");
			REQUIRE(fixture.Start());
			for (const std::string_view source : {
					 "local t = {1}; for i = 1,12 do t = {t,t} end; Scene.Load(Assets.Load('Assets/Next.scene'), {Payload = t})",
					 "Scene.Load(Assets.Load('Assets/Next.scene'), {Payload = table.create(17, string.rep('x', 65536))})",
					 "local t = {}; local key = string.rep('k', 65536); for i = 1,20 do t[key .. i] = i end; Scene.Load(Assets.Load('Assets/Next.scene'), t)",
				 })
			{
				CAPTURE(source);
				Test::ExpectLog expected(LogLevel::Error, "native budget");
				CHECK_FALSE(fixture.Evaluate(source));
				CHECK(expected.GetMatchCount() == 1);
				REQUIRE_FALSE(fixture.Errors.empty());
				CHECK(fixture.Errors.back().Kind == ScriptErrorKind::Runtime);
				CHECK(fixture.Errors.back().Line > 0);
				CHECK_FALSE(fixture.RequestedScene.IsValid());
				CHECK(fixture.RequestedParameters.is_null());
				CHECK(fixture.ExternalMutations.empty());
				CHECK_FALSE(fixture.GetEngine()->IsStopped());
			}
			CheckHelperScript(fixture, R"(
local shared = {Flag = true, Values = {1,2,3}}
Scene.Load(Assets.Load("Assets/Next.scene"), {Right = shared, Empty = {}, Left = shared})
shared.Values[1] = 99
return true
)");
			const Json value = { { "Flag", true }, { "Values", Json::array({ 1, 2, 3 }) } };
			CHECK(fixture.RequestedScene == AssetHandle(77));
			CHECK(fixture.RequestedParameters == Json({ { "Empty", Json::object() }, { "Left", value }, { "Right", value } }));
			CHECK(fixture.ExternalMutations.size() == 1);
		}

		TEST_CASE("LuaHelpers: scene JSON rejects cycles and excess depth while preserving the nesting boundary")
		{
			Test::ScriptTestFixtureSpecification specification{};
			specification.ClockSeconds = []
			{
				return 0.0;
			};
			Test::ScriptTestFixture fixture(specification);
			auto scene = CreateRef<SceneData>();
			scene->Document = CreateRef<const Json>(Json::object());
			fixture.GetAssetManager().Publish(AssetHandle(77), scene, "Assets/Next.scene");
			REQUIRE(fixture.Start());
			CheckHelperScript(fixture, R"(
local scene = Assets.Load("Assets/Next.scene")
local direct = {}; direct.Self = direct
local a = {}; local b = {a}; a.Child = b
for _, value in {direct, a} do
	local ok, message = pcall(Scene.Load, scene, value)
	assert(not ok and string.find(tostring(message), "cycle", 1, true))
end
return true
)");
			CheckHelperScript(fixture, std::format(R"(
local value = 1
for i = 1,{} do value = {{value}} end
local ok, message = pcall(Scene.Load, Assets.Load("Assets/Next.scene"), {{Payload = value}})
assert(not ok and string.find(tostring(message), "nesting limit", 1, true))
return true
)",
										   MaxJsonDepth));
			CHECK_FALSE(fixture.RequestedScene.IsValid());
			CHECK(fixture.RequestedParameters.is_null());
			CHECK(fixture.ExternalMutations.empty());
			CheckHelperScript(fixture, std::format(R"(
local value = 1
for i = 1,{} do value = {{value}} end
Scene.Load(Assets.Load("Assets/Next.scene"), {{Payload = value}})
return true
)",
										   MaxJsonDepth - 1));
			CHECK(fixture.RequestedScene == AssetHandle(77));
			const Json* leaf = &fixture.RequestedParameters["Payload"];
			for (size_t depth = 1; depth < MaxJsonDepth; ++depth)
			{
				REQUIRE(leaf->is_array());
				REQUIRE(leaf->size() == 1);
				leaf = &(*leaf)[0];
			}
			CHECK(*leaf == Json(1));
			CHECK(fixture.ExternalMutations.size() == 1);
			CHECK(fixture.Errors.empty());
		}

		TEST_CASE("LuaHelpers: scene JSON deadline interrupts native traversal before a host request even through pcall")
		{
			double now = 0.0;
			bool armed = false;
			Test::ScriptTestFixtureSpecification specification{};
			specification.TestMode = true;
			specification.ClockSeconds = [&now, &armed]
			{
				if (armed)
					now += 0.01;
				return now;
			};
			Test::ScriptTestFixture fixture(specification);
			auto scene = CreateRef<SceneData>();
			scene->Document = CreateRef<const Json>(Json::object());
			fixture.GetAssetManager().Publish(AssetHandle(77), scene, "Assets/Next.scene");
			REQUIRE(fixture.Start());
			armed = true;
			{
				Test::ExpectLog expected(LogLevel::Error, "script exceeded");
				CHECK_FALSE(fixture.Evaluate(R"(
local scene = Assets.Load("Assets/Next.scene")
local params = {Values = table.create(512, 1)}
local ok = pcall(Scene.Load, scene, params)
return ok
)"));
				CHECK(expected.GetMatchCount() == 1);
			}
			armed = false;
			REQUIRE_FALSE(fixture.Errors.empty());
			CHECK(fixture.Errors.back().Kind == ScriptErrorKind::Timeout);
			CHECK(fixture.Errors.back().Line > 0);
			CHECK_FALSE(fixture.Errors.back().Script.empty());
			CHECK_FALSE(fixture.RequestedScene.IsValid());
			CHECK(fixture.RequestedParameters.is_null());
			CHECK(fixture.ExternalMutations.empty());
			CHECK_FALSE(fixture.GetEngine()->IsStopped());
			CheckHelperScript(fixture, "Scene.Load(Assets.Load('Assets/Next.scene'), {Values = table.create(512, 1)}); return true");
			CHECK(fixture.RequestedScene == AssetHandle(77));
			CHECK(fixture.RequestedParameters == Json({ { "Values", Json::array_t(512, Json(1)) } }));
			CHECK(fixture.ExternalMutations.size() == 1);
		}

		TEST_CASE("Bindings: NaN and Inf are rejected with located errors")
		{
			Test::ScriptTestFixture fixture({ .ConfigureApi = HelperProbes });
			REQUIRE(fixture.Start());
			CheckHelperScript(fixture, R"(
for _, value in {0/0, math.huge, -math.huge} do
	for _, fn in {Probe.Float, Probe.Double, function(v) return Probe.Vec3(vector.create(v,0,0)) end, function(v) return Color.New(v,0,0) end, function(v) return Quat.New(v,0,0,1) end} do
		local ok, message = pcall(fn, value)
		assert(not ok and string.find(message, "argument", 1, true))
	end
end
local e = Scene.CreateEntity()
assert(not pcall(function() e.Transform.Translation = vector.create(math.huge,0,0) end))
assert(e.Transform.Translation == vector.zero)
return true
)");
			Test::ExpectLog expected(LogLevel::Error, "Probe.Float");
			const auto failure = fixture.Evaluate("local function inner()\n return Probe.Float(math.huge)\nend\nreturn inner()");
			CHECK_FALSE(failure);
			CHECK(expected.GetMatchCount() == 1);
			REQUIRE_FALSE(fixture.Errors.empty());
			CHECK(fixture.Errors.back().Line == 2);
			CHECK(fixture.Errors.back().Message.find("Probe.Float") != std::string::npos);
		}
		TEST_CASE("LuaHelpers: checks reject coercions fractional integers and narrowing overflow")
		{
			Test::ScriptTestFixture fixture({ .ConfigureApi = HelperProbes });
			REQUIRE(fixture.Start());
			CheckHelperScript(fixture, R"(
assert(Probe.Signed(-2147483648) == -2147483648 and Probe.Signed(2147483647) == 2147483647)
assert(Probe.Unsigned(4294967295) == 4294967295)
for _, value in {1.5, 2147483648, -2147483649, "1", true} do assert(not pcall(Probe.Signed, value)) end
for _, value in {-1, 4294967296, 1.5} do assert(not pcall(Probe.Unsigned, value)) end
assert(not pcall(Probe.Float, 1e100))
assert(not pcall(Probe.Bool, 0) and not pcall(Probe.String, 5))
assert(not pcall(Probe.Vec2, vector.create(1,2,3)))
assert(not pcall(Quat.New, 0,0,0,0))
assert(Probe.String("a\0b") == "a\0b")
return true
)");
		}
		TEST_CASE("LuaHelpers: pushes and checks preserve supported engine values")
		{
			Test::ScriptTestFixture fixture({ .ConfigureApi = HelperProbes });
			REQUIRE(fixture.Start());
			CheckHelperScript(fixture, R"(
assert(Probe.Bool(true) and not Probe.Bool(false))
assert(Probe.Signed(-17) == -17 and Probe.Unsigned(31) == 31)
assert(Probe.Float(1.25) == 1.25 and Probe.Double(1e100) == 1e100)
assert(Probe.String("owned") == "owned")
assert(Probe.Vec2(vector.create(1,2,0)) == vector.create(1,2,0))
assert(Probe.Vec3(vector.create(1,2,3)) == vector.create(1,2,3))
local c = Probe.Color(Color.New(0.25,0.5,0.75,1)); assert(c.b == 0.75)
local q = Probe.Quat(Quat.New(1,2,3,4)); assert(q.w == 4 and q.x == 1)
local e = Scene.CreateEntity(); assert(Probe.Entity(e) == e)
assert(Probe.Proxy(e.Transform).Translation == vector.zero)
assert(Probe.AssetText(Probe.Asset()) == "0000000000001234")
assert(Probe.Nil() == nil)
assert(not pcall(Probe.AssetText, e))
assert(not pcall(Probe.Entity, Probe.Asset()))
assert(not pcall(Probe.Color, vector.one))
return true
)");
		}
		TEST_CASE("LuaHelpers: reflected marshalling traverses fields without losing type identity")
		{
			Test::ScriptTestFixture fixture({ .ConfigureApi = HelperProbes });
			const auto id = HelperBehaviour(fixture, AssetHandle(91), "Assets/Fields.luau", R"(
local T = {Fields = {
	Number = Field.Number(2), Mode = Field.Enum({"One","Two"}), Tint = Field.Color(), Rotation = Field.Quat(),
	Points = Field.Array(Field.Vector()), Values = Field.Array(Field.Array(Field.Integer())), Target = Field.Entity(), Clip = Field.Asset("AudioClip")
}}
return Script.Define("Fields", T)
)");
			REQUIRE(fixture.Start());
			CheckHelperScript(fixture, R"(
local script = self.Entity:GetComponent("Script")
script.Fields = {Number = 3, Mode = "Two", Tint = Color.New(0.25,0.5,0.75), Rotation = Quat.Identity(), Points = {vector.one}, Values = {{1,2},{3}}, Target = self.Entity, Clip = Probe.Asset()}
local fields = script.Fields
assert(fields.Number == 3 and fields.Mode == "Two" and fields.Tint.r == 0.25)
assert(fields.Rotation.w == 1 and fields.Points[1] == vector.one and fields.Values[1][2] == 2)
assert(fields.Target == self.Entity and Probe.AssetText(fields.Clip) == "0000000000001234")
local cycle = {}; cycle[1] = cycle
assert(not pcall(function() script.Fields = {Values = cycle} end))
assert(not pcall(function() script.Fields = {Number = function() end} end))
assert(script.Fields.Number == 3)
return true
)",
				id);
		}
		TEST_CASE("LuaHelpers: reflected proxy budgets reject nested aliases before any host write")
		{
			Test::ScriptTestFixtureSpecification specification{};
			specification.ClockSeconds = []
			{
				return 0.0;
			};
			Test::ScriptTestFixture fixture(specification);
			const auto id = HelperBehaviour(fixture, AssetHandle(92), "Assets/BoundedFields.luau", R"(
local deep, shared = Field.Integer(), Field.Integer()
for i = 1,13 do deep = Field.Array(deep) end
for i = 1,11 do shared = Field.Array(shared) end
return Script.Define("BoundedFields", {Fields = {
	Data = deep, A = shared, B = shared, C = shared, Text = Field.Array(Field.String())
}})
)");
			REQUIRE(fixture.Start());
			const auto before = fixture.GetScene().FindEntityByID(id).GetComponent<ScriptComponent>().Fields;
			CheckHelperScript(fixture, R"(
local script = self.Entity:GetComponent("Script")
local function refused(fields, reason)
	local ok, message = pcall(function() script.Fields = fields end)
	assert(not ok and string.find(tostring(message), reason, 1, true), tostring(message))
end
local value = 1
for i = 1,13 do value = {value,value} end
refused({Data = value}, "native budget")
value = 1
for i = 1,11 do value = {value,value} end
-- Each resolved Variant fits alone, but the whole Fields assignment must use one allowance.
refused({A = value, B = value, C = value}, "native budget")
refused({Text = table.create(17, string.rep("x", 65536))}, "native budget")
local direct = {}; direct[1] = direct
local left, right = {}, {}; left[1] = right; right[1] = left
refused({Data = direct}, "cyclic")
refused({Data = left}, "cyclic")
return true
)",
				id);
			CHECK(fixture.GetScene().FindEntityByID(id).GetComponent<ScriptComponent>().Fields == before);
			CHECK(fixture.ExternalMutations.empty());
			CHECK(fixture.Errors.empty());
			CheckHelperScript(fixture, R"(
local script = self.Entity:GetComponent("Script")
local dag = 1
for i = 1,11 do dag = {dag,dag} end
script.Fields = {A = dag}
assert(script.Fields.A[1] ~= nil)
local value = {7,8}
for i = 1,11 do value = {value} end
script.Fields = {Data = {value,value}, Text = {"a\0b", "normal"}}
value[1] = nil
local copy = script.Fields.Data
assert(copy[1] ~= copy[2])
local leaf = copy[2]
for i = 1,11 do leaf = leaf[1] end
assert(leaf[1] == 7 and leaf[2] == 8)
assert(script.Fields.Text[1] == "a\0b")
return true
)",
				id);
			CHECK(fixture.ExternalMutations.size() == 1);
			CHECK_FALSE(fixture.GetEngine()->IsStopped());
		}
		TEST_CASE("LuaHelpers: typed maps and unresolved Variants share string key and expansion budgets")
		{
			Test::ScriptTestFixtureSpecification specification{};
			specification.ConfigureApi = HelperProbes;
			specification.ClockSeconds = []
			{
				return 0.0;
			};
			Test::ScriptTestFixture fixture(specification);
			REQUIRE(fixture.Start());
			CheckHelperScript(fixture, R"(
local function refused(kind, value)
	local ok, message = pcall(Probe.Container, kind, value)
	assert(not ok and string.find(tostring(message), "native budget", 1, true), tostring(message))
end
local dag = 1
for i = 1,11 do dag = {dag,dag} end
assert(Probe.Container("Variants", {A = dag}).A ~= nil)
refused("Variants", {A = dag, B = dag, C = dag})
local strings, keys = {}, {}
local text = string.rep("x", 65536)
for i = 1,17 do strings[tostring(i)] = {text}; keys[text .. i] = {"ok"} end
refused("Strings", strings)
refused("Strings", keys)
local shared = {"a\0b", "normal"}
local copy = Probe.Container("Strings", {A = shared, B = shared})
shared[1] = "changed"
assert(copy.A[1] == "a\0b" and copy.B[1] == "a\0b" and copy.A ~= copy.B)
local cycle = {}; cycle.A = cycle
assert(not pcall(Probe.Container, "Variants", cycle))
return true
)");
			// The enclosing typed map consumes one ancestor level, including across an unresolved Variant.
			CheckHelperScript(fixture, std::format(R"(
local value = 1
for i = 1,{} do value = {{value}} end
assert(Probe.Container("Variants", {{A = value}}).A ~= nil)
local ok, message = pcall(Probe.Container, "Variants", {{A = {{value}}}})
assert(not ok and string.find(tostring(message), "nesting limit", 1, true))
return true
)",
										   MaxJsonDepth - 1));
			CheckHelperScript(fixture, std::format(R"(
local value = 7
for i = 1,{0} do
	if i % 2 == 1 then value = {{Child = value}} else value = {{value}} end
end
local copy = Probe.Container("Variants", {{Right = value, Left = value, Tail = 9}})
assert(copy.Left ~= copy.Right and copy.Tail == 9)
for _, branch in {{copy.Left, copy.Right}} do
	for i = {0},1,-1 do
		if i % 2 == 1 then branch = branch.Child else branch = branch[1] end
	end
	assert(branch == 7)
end
return true
)",
										   MaxJsonDepth - 1));
			CHECK(fixture.ExternalMutations.empty());
			CHECK(fixture.Errors.empty());
		}
		TEST_CASE("LuaHelpers: reflected traversal preserves the full depth limit without native recursion")
		{
			Test::ScriptTestFixtureSpecification specification{};
			specification.ConfigureApi = HelperProbes;
			specification.ClockSeconds = []
			{
				return 0.0;
			};
			Test::ScriptTestFixture fixture(specification);
			const auto id = HelperBehaviour(fixture, AssetHandle(94), "Assets/DeepFields.luau", R"(
local field = Field.Integer(7)
for i = 1,63 do field = Field.Array(field) end
return Script.Define("DeepFields", {Fields = {Data = field}})
)");
			REQUIRE(fixture.Start());
			CheckHelperScript(fixture, std::format(R"(
local value = {{Children = {{}}}}
for i = 2,{0} do value = {{Children = {{value}}}} end
local copy = Probe.Container("Deep", value)
for i = 2,{0} do copy = copy.Children[1] end
assert(#copy.Children == 0)
local ok, message = pcall(Probe.Container, "DeepList", {{value}})
assert(not ok and string.find(tostring(message), "excessively nested", 1, true))
local cycle = {{}}; cycle.Children = {{cycle}}
assert(not pcall(Probe.Container, "Deep", cycle))
assert(#Probe.Container("Deep", {{Children = {{}}}}).Children == 0)
return true
)",
										   MaxJsonDepth / 2));
			CHECK(fixture.ExternalMutations.empty());
			CheckHelperScript(fixture, R"(
local value = 9
for i = 1,63 do value = {value} end
local script = self.Entity:GetComponent("Script")
script.Fields = {Data = value}
local copy = script.Fields.Data
for i = 1,63 do copy = copy[1] end
assert(copy == 9)
return true
)",
				id);
			CHECK(fixture.ExternalMutations.size() == 1);
			CHECK(fixture.Errors.empty());
		}
		TEST_CASE("LuaHelpers: reflected native deadlines survive pcall without writing a component")
		{
			double now = 0.0;
			bool armed = false;
			Test::ScriptTestFixtureSpecification specification{};
			specification.TestMode = true;
			specification.ClockSeconds = [&now, &armed]
			{
				if (armed)
					now += 0.01;
				return now;
			};
			Test::ScriptTestFixture fixture(specification);
			const auto id = HelperBehaviour(fixture, AssetHandle(93), "Assets/DeadlineFields.luau", R"(
return Script.Define("DeadlineFields", {Fields = {Data = Field.Array(Field.Integer())}})
)");
			REQUIRE(fixture.Start());
			const auto before = fixture.GetScene().FindEntityByID(id).GetComponent<ScriptComponent>().Fields;
			armed = true;
			{
				Test::ExpectLog expected(LogLevel::Error, "script exceeded");
				CHECK_FALSE(fixture.Evaluate(R"(
local script = self.Entity:GetComponent("Script")
local data = table.create(512, 1)
return pcall(function() script.Fields = {Data = data} end)
)",
					id));
				CHECK(expected.GetMatchCount() == 1);
			}
			armed = false;
			CHECK(fixture.GetScene().FindEntityByID(id).GetComponent<ScriptComponent>().Fields == before);
			CHECK(fixture.ExternalMutations.empty());
			REQUIRE_FALSE(fixture.Errors.empty());
			CHECK(fixture.Errors.back().Kind == ScriptErrorKind::Timeout);
			CHECK(fixture.Errors.back().Line > 0);
			CHECK_FALSE(fixture.Errors.back().Script.empty());
			CheckHelperScript(fixture, R"(
local script = self.Entity:GetComponent("Script")
script.Fields = {Data = table.create(512, 1)}
assert(#script.Fields.Data == 512 and script.Fields.Data[512] == 1)
return true
)",
				id);
			CHECK(fixture.ExternalMutations.size() == 1);
			CHECK_FALSE(fixture.GetEngine()->IsStopped());
		}
		TEST_CASE("LuaHelpers: protected callbacks capture traceback before unwinding")
		{
			Test::ScriptTestFixture fixture;
			const auto bad = HelperBehaviour(fixture, AssetHandle(101), "Assets/Bad.luau", "local T = {}\nfunction T:OnUpdate()\n local function inner()\n  error('nested fault')\n end\n inner()\nend\nreturn Script.Define('Bad', T)", "Bad");
			const auto good = HelperBehaviour(fixture, AssetHandle(102), "Assets/Good.luau", "local T = {}\nfunction T:OnUpdate() self.Count = (self.Count or 0) + 1 end\nreturn Script.Define('Good', T)", "Good");
			REQUIRE(fixture.Start());
			Test::ExpectLog expected(LogLevel::Error, "nested fault");
			fixture.GetEngine()->Update();
			REQUIRE(fixture.Errors.size() == 1);
			CHECK(fixture.Errors[0].Entity == bad);
			CHECK(fixture.Errors[0].Line == 4);
			CHECK(fixture.Errors[0].Script == "Assets/Bad.luau");
			CHECK_FALSE(fixture.Errors[0].Traceback.empty());
			fixture.GetEngine()->Update();
			CHECK(fixture.Errors.size() == 1);
			CHECK(expected.GetMatchCount() == 1);
			CheckHelperScript(fixture, "return self.Count == 2", good);
		}
		TEST_CASE("LuaHelpers: protected resumes capture the failed thread and distinguish yielding")
		{
			Test::ScriptTestFixture fixture;
			const auto id = HelperBehaviour(fixture, AssetHandle(111), "Assets/Resume.luau", R"(
local T = {}
function T:OnStart()
	Task.Spawn(function()
		self.Step = 1
		Task.WaitTicks(2)
		self.Step = 2
		error("thread fault")
	end)
end
return Script.Define("Resume", T)
)");
			REQUIRE(fixture.Start());
			CheckHelperScript(fixture, "return self.Step == 1", id);
			CHECK(fixture.Errors.empty());
			fixture.Frame.Tick = 1;
			fixture.GetEngine()->ResumeTasks();
			CHECK(fixture.Errors.empty());
			fixture.Frame.Tick = 2;
			Test::ExpectLog expected(LogLevel::Error, "thread fault");
			fixture.GetEngine()->ResumeTasks();
			CHECK(expected.GetMatchCount() == 1);
			REQUIRE(fixture.Errors.size() == 1);
			CHECK(fixture.Errors[0].Message.find("thread fault") != std::string::npos);
			CHECK_FALSE(fixture.Errors[0].Traceback.empty());
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			CheckHelperScript(fixture, "return 2 + 2 == 4");
		}
		TEST_CASE("LuaHelpers: protected native setup contains marshalling and allocation errors")
		{
			Test::ScriptTestFixtureSpecification spec;
			spec.Settings.MemoryLimitMB = 4;
			spec.ConfigureApi = HelperProbes;
			Test::ScriptTestFixture fixture(spec);
			REQUIRE(fixture.Start());
			Test::ExpectLog expected(LogLevel::Error, "script exceeded memory limit 4 MB");
			const auto result = fixture.Evaluate("return Probe.Memory()");
			CHECK_FALSE(result);
			CHECK(expected.GetMatchCount() == 1);
			REQUIRE_FALSE(fixture.Errors.empty());
			CHECK(fixture.Errors.back().Kind == ScriptErrorKind::Memory);
			CHECK(fixture.FatalError);
		}
		TEST_CASE("LuaHelpers: pure load-time calls never require a runtime engine or host")
		{
			Test::ScriptTestFixture fixture;
			const auto behaviour = fixture.AddScript(AssetHandle(121), "Assets/Pure.luau", R"(
local T = {Fields = {Number = Field.Number(), Integer = Field.Integer(), Bool = Field.Bool(), String = Field.String(), Vector = Field.Vector(), Tint = Field.Color(Color.New(1,1,1)), Rotation = Field.Quat(Quat.Identity()), Target = Field.Entity(), Clip = Field.Asset("AudioClip"), Mode = Field.Enum({"First","Second"}), Nested = Field.Array(Field.Array(Field.Integer(2, {Min=0, Max=5, Tooltip="Nested"})))}}
assert(Math.Clamp(3,0,1) == 1)
return Script.Define("Pure", T)
)");
			if (!behaviour)
				INFO(behaviour.error().ToString());
			REQUIRE(behaviour);
			CHECK((*behaviour)->Kind == ScriptKind::Behaviour);
			CHECK((*behaviour)->Fields.size() == 11);
			for (const auto& field : (*behaviour)->Fields)
				if (field.Name == "Nested")
				{
					REQUIRE(field.Element);
					REQUIRE(field.Element->Element);
					CHECK(field.Element->Element->Meta.Min == 0);
					CHECK(field.Element->Element->Tooltip == "Nested");
				}
			const auto suite = fixture.AddScript(AssetHandle(122), "Assets/Suite.luau", "return Test.Suite('PureSuite', function() error('must not run') end, {CaseTimeoutTicks=7})");
			REQUIRE(suite);
			CHECK((*suite)->Kind == ScriptKind::TestSuite);
			const auto fake = fixture.AddScript(AssetHandle(123), "Assets/Fake.luau", "return {Name='Fake', Body=function() end, Fields={Value=Field.Number()}}");
			REQUIRE(fake);
			CHECK((*fake)->Kind == ScriptKind::Module);
			for (const char* source : { "Scene.CreateEntity()", "return Test.Suite('Bad', function() end, {TimeoutTicks=2})", "return Test.Suite('Bad', function() end, {CaseTimeoutTicks=0})", "return Script.Define('Bad', {Fields={Fake={Type='Number'}}})", "return Script.Define('Bad', {Fields={Bad=Field.Number(2, {Max=1})}})" })
				CHECK_FALSE(fixture.AddScript(AssetHandle(124), "Assets/BadMetadata.luau", source));
		}
		TEST_CASE("LuaHelpers: writable checks protect nested and deferred execution")
		{
			Test::ScriptTestFixture fixture({ .ReadOnly = true });
			const auto id = HelperBehaviour(fixture, AssetHandle(131), "Assets/ReadOnly.luau", R"(
local T = {Fields={Count=Field.Integer(4)}}
function T:OnCreate() self.Entity.Name = "unexpected" end
function T:Mutate() self.Entity:SetActive(false) end
return Script.Define("ReadOnly", T)
)");
			REQUIRE(fixture.Start());
			CHECK(fixture.Errors.empty());
			CHECK(fixture.ExternalMutations.empty());
			CheckHelperScript(fixture, R"(
assert(self.Count == 4 and self.Entity.Name == "Owner")
local alias = self.Mutate
assert(not pcall(alias, self))
assert(not pcall(function() Task.Delay(0, function() self.Entity:Destroy() end) end))
assert(not pcall(math.random))
assert(Random.New(2):Number() >= 0)
return true
)",
				id);
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
			CHECK(fixture.ExternalMutations.empty());
		}
		TEST_CASE("LuaHelpers: external writes invalidate recording before side effects and later faults")
		{
			Test::ScriptTestFixture fixture;
			const auto id = HelperBehaviour(fixture, AssetHandle(141), "Assets/Origins.luau", "local T = {}; function T:OnStart() self.Entity.Name = 'Gameplay' end; return Script.Define('Origins', T)");
			REQUIRE(fixture.Start());
			CHECK(fixture.ExternalMutations.empty());
			CheckHelperScript(fixture, "local c = Color.New(1,1,1); c.r = 2; local r = Random.New(9); r:Number(); return true");
			CHECK(fixture.ExternalMutations.empty());
			CheckHelperScript(fixture, "assert(not pcall(function() self.Entity.Transform.Scale = vector.zero end)); return true", id);
			CHECK(fixture.ExternalMutations.empty());
			{
				Test::ExpectLog expected(LogLevel::Error, "later fault");
				const auto failed = fixture.Evaluate("self.Entity.Name = 'External'; Random.Number(); error('later fault')", id);
				CHECK_FALSE(failed);
				CHECK(expected.GetMatchCount() == 1);
				CHECK(fixture.GetScene().FindEntityByID(id).GetName() == "External");
				CHECK(fixture.ExternalMutations.size() == 1);
			}
			CheckHelperScript(fixture, "Random.Number(); return true");
			CHECK(fixture.ExternalMutations.size() == 2);
		}
		TEST_CASE("LuaHelpers: deferred work retains origin and renews mutation notification per resume")
		{
			Test::ScriptTestFixture fixture;
			const auto id = HelperBehaviour(fixture, AssetHandle(151), "Assets/Deferred.luau", "return Script.Define('Deferred', {})");
			REQUIRE(fixture.Start());
			CheckHelperScript(fixture, R"(
Task.Spawn(function()
	self.Entity.Name = "First"
	Task.WaitTicks(1)
	self.Entity.Name = "Second"
	Random.Number()
	Task.WaitTicks(1)
	self.Entity.Name = "Third"
end)
return true
)",
				id);
			REQUIRE(fixture.ExternalMutations.size() == 1);
			fixture.ExternalMutations.clear();
			fixture.Frame.Tick = 1;
			fixture.GetEngine()->ResumeTasks();
			CHECK(fixture.ExternalMutations.size() == 1);
			CHECK(fixture.GetScene().FindEntityByID(id).GetName() == "Second");
			fixture.ExternalMutations.clear();
			fixture.Frame.Tick = 2;
			fixture.GetEngine()->ResumeTasks();
			CHECK(fixture.ExternalMutations.size() == 1);
			CHECK(fixture.GetScene().FindEntityByID(id).GetName() == "Third");
			CHECK(fixture.GetEngine()->GetTaskScheduler()->GetCount() == 0);
		}
	}

}
