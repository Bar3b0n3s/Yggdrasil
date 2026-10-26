#include "TestsPCH.h"
#include "Engine/Scripting/ScriptApiRegistry.h"

#include "Engine/Asset/IScriptDiagnosticsProvider.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/LoadTimeVm.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <functional>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace Engine {

	namespace {

		class FieldModuleReader final : public IScriptModuleReader
		{
		public:
			[[nodiscard]] Result<std::string> ReadModule(const VfsPath& path) override
			{
				if (OnRead)
					OnRead();
				const auto found = Sources.find(path.ToString());
				if (found == Sources.end())
					return MakeError(ErrorCode::NotFound, "module '{}' was not supplied", path);
				return found->second;
			}

			std::map<std::string, std::string> Sources{};
			std::function<void()> OnRead{};
		};

	}

	static void CheckScriptBindings(Test::ScriptTestFixture& fixture, std::string_view source)
	{
		const auto result = fixture.Evaluate(source);
		REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
		CHECK(result->Value.Get() == Json(true));
	}

	namespace Test {

		void RunScriptBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			REQUIRE(fixture.AddScript(AssetHandle(210), "Assets/Fields.luau", R"(
local T = {Fields = {
	Number = Field.Number(1), Integer = Field.Integer(2), Bool = Field.Bool(true),
	String = Field.String("value"), Vector = Field.Vector(vector.create(1,2,3)),
	Tint = Field.Color(Color.New(1,0,0)), Rotation = Field.Quat(Quat.Identity()),
	Target = Field.Entity(), Clip = Field.Asset("AudioClip"), Mode = Field.Enum({"First","Second"}),
	Array = Field.Array(Field.Integer())
}}
function T:OnCreate()
	assert(self.Number == 1 and self.Integer == 2 and self.Bool and self.String == "value")
	assert(self.Vector == vector.create(1,2,3) and self.Tint.r == 1 and self.Rotation.w == 1)
	assert(self.Target == nil and self.Clip == nil and self.Mode == "First" and #self.Array == 0)
	self.Ready = true
end
return Script.Define("Fields", T)
)"));
			const Entity bound = fixture.GetScene().CreateEntity("Bound");
			ScriptSlot script;
			script.Script.SetHandle(AssetHandle(210));
			bound.AddComponent<ScriptComponent>(ScriptComponent{ { script } });
			REQUIRE(fixture.Start());
			CheckScriptBindings(fixture, R"(
assert(Scene.FindByName("Bound"):GetScript().Ready)
local suite = Test.Suite("Ordinary", function() error("must not execute") end, {CaseTimeoutTicks=5})
assert(suite.Name == "Ordinary" and suite.Options.CaseTimeoutTicks == 5)
local class = {}
assert(Script.Define("Evaluated", class) == class)
assert(not pcall(Script.Define, "Again", class))
return true
)");
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage);
			snapshots.push_back(*coverage);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("ScriptBindings: field constructors declare typed defaults and Define authenticates its class once")
		{
			std::vector<ScriptApiCoverage> snapshots;
			Test::RunScriptBindingsCoverage(RunModes::Editor, snapshots);
		}

		TEST_CASE("ScriptBindings: field defaults metadata and descriptor aliases obey the native conversion allowance")
		{
			FieldModuleReader reader;
			const auto path = VfsPath::Parse("project://Assets/MetadataBudget.luau");
			REQUIRE(path);
			LoadTimeVmSpecification specification{};
			specification.ClockSeconds = []
			{
				return 0.0;
			};
			for (const std::string_view source : {
					 "return Script.Define('Large', {Fields = {Data = Field.String(string.rep('x', 1048576))}})",
					 "return Script.Define('Large', {Fields = {Data = Field.Integer(0, {Tooltip = string.rep('x', 1048576)})}})",
					 "local values = {}; local text = string.rep('x', 65536); for i = 1,17 do values[i] = text .. i end; return Script.Define('Large', {Fields = {Data = Field.Enum(values)}})",
					 "local field = Field.String(string.rep('x', 65536)); local fields = {}; for i = 1,17 do fields['Data' .. i] = field end; return Script.Define('Large', {Fields = fields})" })
			{
				CAPTURE(source);
				const auto result = LoadTimeVm::Extract({ .Path = *path, .Source = std::string(source), .Modules = &reader }, specification);
				REQUIRE_FALSE(result);
				CHECK(result.error().GetCode() == ErrorCode::Script);
				CHECK(result.error().GetMessageText().find("native budget") != std::string::npos);
				CHECK(result.error().GetLocation().File == "Assets/MetadataBudget.luau");
				CHECK(result.error().GetLocation().Line > 0);
			}
			const auto valid = LoadTimeVm::Extract({ .Path = *path,
													   .Source = R"(
local field = Field.String("quote\" slash\\ tab\t", {Tooltip = "ordinary metadata"})
local class = {Fields = {A = field, B = field, Values = Field.Array(Field.Enum({"First", "Second"}, "Second"))}}
return Script.Define("Normal", class)
)",
													   .Modules = &reader },
				specification);
			REQUIRE_MESSAGE(valid, (valid ? "" : valid.error().ToString()));
			REQUIRE((*valid)->Fields.size() == 3);
			CHECK((*valid)->Fields[0].DefaultValue.Get() == Json("quote\" slash\\ tab\t"));
			CHECK((*valid)->Fields[0].DefaultValue == (*valid)->Fields[1].DefaultValue);
			CHECK((*valid)->Fields[0].Tooltip == "ordinary metadata");
			CHECK((*valid)->Fields[2].DefaultValue.Get() == Json::array());
			REQUIRE((*valid)->Fields[2].Element);
			CHECK((*valid)->Fields[2].Element->DefaultValue.Get() == Json("Second"));
			const auto recovered = LoadTimeVm::Extract({ .Path = *path,
														   .Source = R"(
local field = Field.String(string.rep("x", 65536))
local class = {Fields = {}}
for i = 1,17 do class.Fields["Data" .. i] = field end
local ok, message = pcall(Script.Define, "Large", class)
assert(not ok and string.find(tostring(message), "native budget", 1, true))
class.Fields = {Data = Field.String("recovered")}
assert(Script.Define("Recovered", class) == class)
return class
)",
														   .Modules = &reader },
				specification);
			REQUIRE_MESSAGE(recovered, (recovered ? "" : recovered.error().ToString()));
			CHECK((*recovered)->Name == "Recovered");
			REQUIRE((*recovered)->Fields.size() == 1);
			CHECK((*recovered)->Fields.front().DefaultValue.Get() == Json("recovered"));
		}

		TEST_CASE("ScriptBindings: budgeted field descriptors retain the exact schema depth limit")
		{
			FieldModuleReader reader;
			const auto path = VfsPath::Parse("project://Assets/DeepFields.luau");
			REQUIRE(path);
			LoadTimeVmSpecification specification{};
			specification.ClockSeconds = []
			{
				return 0.0;
			};
			const auto result = LoadTimeVm::Extract({ .Path = *path,
														.Source = R"(
local field = Field.Integer(7)
for i = 1,63 do field = Field.Array(field) end
local ok, message = pcall(Field.Array, field)
assert(not ok and string.find(tostring(message), "64 levels", 1, true))
return Script.Define("DeepFields", {Fields = {Data = field}})
)",
														.Modules = &reader },
				specification);
			REQUIRE_MESSAGE(result, (result ? "" : result.error().ToString()));
			REQUIRE((*result)->Fields.size() == 1);
			const ScriptFieldSchema* field = &(*result)->Fields.front();
			for (size_t depth = 0; depth < 63; ++depth)
			{
				CHECK(field->Type == FieldType::Array);
				CHECK(field->DefaultValue.Get() == Json::array());
				REQUIRE(field->Element);
				field = field->Element.get();
			}
			CHECK(field->Type == FieldType::Int32);
			CHECK(field->DefaultValue.Get() == Json(7));
		}

		TEST_CASE("ScriptBindings: field conversion polls the graph deadline inside native enumeration")
		{
			FieldModuleReader reader;
			reader.Sources.emplace("project://Assets/Arm.luau", "return {}");
			const auto path = VfsPath::Parse("project://Assets/MetadataDeadline.luau");
			REQUIRE(path);
			double now = 0.0;
			bool armed = false;
			reader.OnRead = [&armed]
			{
				armed = true;
			};
			LoadTimeVmSpecification specification{};
			specification.ClockSeconds = [&now, &armed]
			{
				if (armed)
					now += 0.001;
				return now;
			};
			const ScriptCheckRequest request{ .Path = *path,
				.Source = R"(
local choices = {}
for i = 1,512 do choices[i] = "Choice" .. i end
require("./Arm")
local ok, field = pcall(Field.Enum, choices)
if not ok then return {} end
return Script.Define("Choices", {Fields = {Choice = field}})
)",
				.Modules = &reader };
			const auto timeout = LoadTimeVm::Extract(request, specification);
			REQUIRE_FALSE(timeout);
			CHECK(timeout.error().GetCode() == ErrorCode::Timeout);
			CHECK(timeout.error().GetLocation().File == "Assets/MetadataDeadline.luau");
			CHECK(timeout.error().GetLocation().Line > 0);
			armed = false;
			reader.OnRead = {};
			const auto valid = LoadTimeVm::Extract(request, specification);
			REQUIRE_MESSAGE(valid, (valid ? "" : valid.error().ToString()));
			REQUIRE((*valid)->Fields.size() == 1);
			CHECK((*valid)->Fields.front().EnumValues.size() == 512);
		}
	}

}
