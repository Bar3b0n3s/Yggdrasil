#include "TestsPCH.h"

#include "Engine/Asset/CookedFormat.h"
#include "Engine/Asset/ScriptData.h"
#include "Engine/Core/BinaryReader.h"
#include "Engine/Core/Hash.h"
#include "Engine/Core/Random.h"
#include "Engine/Reflection/EnumInfo.h"
#include "Engine/Scripting/ScriptCompiler.h"
#include "Support/AssetTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <limits>

namespace Engine {

	namespace Test {

		static ScriptFieldSchema ScriptField(std::string name, FieldType kind, Json value)
		{
			ScriptFieldSchema result;
			result.Name = std::move(name);
			result.Type = kind;
			result.DefaultValue = VariantValue(std::move(value));
			return result;
		}

		static ScriptData CompiledAsset(std::string_view source = "return {}", ScriptCompileMode mode = ScriptCompileMode::Chunk,
			std::string_view path = "project://Assets/Probe.luau", std::string_view label = {}, std::string_view pointer = {})
		{
			VfsPath origin;
			if (!path.empty())
				origin = ParseVfsPath(path);
			const auto compiled = ScriptCompiler::Compile({ origin, source, mode, label, pointer });
			REQUIRE(compiled);
			ScriptData result;
			result.Bytecode = compiled->Bytecode;
			result.SourceMap = compiled->SourceMap;
			return result;
		}

		static Ref<ScriptData> SchemaScript(std::vector<ScriptFieldSchema> fields)
		{
			auto script = CreateRef<ScriptData>();
			script->Kind = ScriptKind::Behaviour;
			script->Name = "Schema";
			std::sort(fields.begin(), fields.end(), [](const auto& left, const auto& right)
			{
				return left.Name < right.Name;
			});
			script->Fields = std::move(fields);
			return script;
		}

		static std::vector<ScriptFieldSchema> AllScriptFields()
		{
			std::vector<ScriptFieldSchema> fields;
			fields.push_back(ScriptField("Number", FieldType::Float, 0.0f));
			fields.back().Meta.Min = -1.0000000001;
			fields.back().Meta.Max = 2.0000000001;
			fields.back().Meta.Step = 0.25;
			fields.back().Tooltip = "Precise bounds";
			fields.push_back(ScriptField("Integer", FieldType::Int32, 0));
			fields.push_back(ScriptField("Boolean", FieldType::Bool, false));
			fields.push_back(ScriptField("Text", FieldType::String, ""));
			fields.push_back(ScriptField("Vector", FieldType::Vec3, Json::array({ 0, 0, 0 })));
			fields.push_back(ScriptField("Color", FieldType::Color4, Json::array({ 1, 1, 1, 1 })));
			fields.push_back(ScriptField("Rotation", FieldType::Quat, Json::array({ 0, 0, 0, 1 })));
			fields.push_back(ScriptField("Target", FieldType::EntityRef, nullptr));
			fields.push_back(ScriptField("Clip", FieldType::AssetRef, nullptr));
			fields.back().Meta.AssetFilter = "AudioClip";
			fields.push_back(ScriptField("Choice", FieldType::Enum, "Second"));
			fields.back().EnumValues = { "Second", "First" };
			fields.push_back(ScriptField("Rows", FieldType::Array, Json::array()));
			auto row = CreateRef<ScriptFieldSchema>(ScriptField("", FieldType::Array, Json::array()));
			auto value = CreateRef<ScriptFieldSchema>(ScriptField("", FieldType::Float, 0.0f));
			value->Meta.Min = 0;
			value->Meta.Max = 1;
			value->Meta.Step = 0.25;
			value->Tooltip = "Normalized weight.";
			row->Element = value;
			fields.back().Element = row;
			std::sort(fields.begin(), fields.end(), [](const auto& left, const auto& right)
			{
				return left.Name < right.Name;
			});
			return fields;
		}

		static void CheckSchemaEqual(const ScriptFieldSchema& left, const ScriptFieldSchema& right)
		{
			CHECK(left.Name == right.Name);
			CHECK(left.Type == right.Type);
			CHECK(left.DefaultValue == right.DefaultValue);
			CHECK(left.Meta.Min == right.Meta.Min);
			CHECK(left.Meta.Max == right.Meta.Max);
			CHECK(left.Meta.Step == right.Meta.Step);
			CHECK(left.Meta.AssetFilter == right.Meta.AssetFilter);
			CHECK(left.Tooltip == right.Tooltip);
			CHECK(left.EnumValues == right.EnumValues);
			REQUIRE(bool(left.Element) == bool(right.Element));
			if (left.Element)
				CheckSchemaEqual(*left.Element, *right.Element);
		}

	}

	TEST_SUITE("Asset")
	{
		TEST_CASE("ScriptData: recursive array schemas enforce element metadata through generic field validation")
		{
			const auto script = Test::SchemaScript(Test::AllScriptFields());
			const auto source = ScriptFieldSchemaSource::Create({ { AssetHandle(0x4001), script } });
			REQUIRE(source);
			const auto found = (*source)->FindField(AssetHandle(0x4001), "Rows");
			REQUIRE(found);
			const FieldInfo* field = *found;
			CHECK_FALSE(field->IsStored());
			CHECK_FALSE(field->GetType().HasOps());
			const auto* row = field->GetType().GetElementSchema();
			REQUIRE(row);
			CHECK(&row->GetType() == field->GetType().GetElement());
			const auto* element = row->GetType().GetElementSchema();
			REQUIRE(element);
			CHECK(&element->GetType() == row->GetType().GetElement());
			CHECK(element->GetMeta().Min == 0.0);
			CHECK(element->GetMeta().Max == 1.0);
			CHECK(element->GetMeta().Step == 0.25);
			CHECK(element->GetDescription() == "Normalized weight.");
			ResolveContext resolve{};
			resolve.Schemas = source->get();
			const Json valid = Json::array({ Json::array({ 0.0f, 0.5f, 1.0f }) });
			ValidationContext validResult;
			field->ValidateJson(JsonReader(valid), resolve, validResult);
			CHECK_FALSE(validResult.HasErrors());
			const Json invalid = Json::array({ Json::array({ 0.5f }), Json::array({ -0.25f }) });
			ValidationContext jsonResult;
			field->ValidateJson(JsonReader(invalid), resolve, jsonResult);
			REQUIRE(jsonResult.GetIssues().size() == 1);
			CHECK(jsonResult.GetIssues()[0].JsonPointer == "/Rows/1/0");
			const auto invalidValue = Value::FromArray({ Value::FromArray({ Value::FromFloat(0.5f) }),
				Value::FromArray({ Value::FromFloat(-0.25f) }) });
			ValidationContext valueResult;
			field->ValidateValue(invalidValue, resolve, valueResult);
			REQUIRE(valueResult.GetIssues().size() == 1);
			CHECK(valueResult.GetIssues()[0].JsonPointer == "/Rows/1/0");
		}

		TEST_CASE("ScriptData: cooked schemas retain every kind default option and source map")
		{
			for (const auto kind : { ScriptKind::Behaviour, ScriptKind::Module, ScriptKind::TestSuite })
			{
				auto script = Test::CompiledAsset("local x = 1\nreturn x\n");
				script.Kind = kind;
				script.Name = kind == ScriptKind::Module ? "" : "Probe";
				if (kind == ScriptKind::Behaviour)
					script.Fields = Test::AllScriptFields();
				script.Requires = { { "Assets/Probe.luau", "./Required", "Assets/Required.luau", AssetHandle(0x4567) } };
				const auto cooked = CookScript(script, 7);
				REQUIRE(cooked);
				const auto loaded = LoadCookedScript(*cooked);
				REQUIRE(loaded);
				CHECK((*loaded)->Kind == kind);
				CHECK((*loaded)->Name == script.Name);
				CHECK((*loaded)->Bytecode == script.Bytecode);
				CHECK((*loaded)->Requires == script.Requires);
				CHECK((*loaded)->SourceMap.LineOffsets == script.SourceMap.LineOffsets);
				CHECK((*loaded)->SourceMap.SourceHash == script.SourceMap.SourceHash);
				REQUIRE((*loaded)->Fields.size() == script.Fields.size());
				for (size_t i = 0; i < script.Fields.size(); ++i)
					Test::CheckSchemaEqual(script.Fields[i], (*loaded)->Fields[i]);
			}
		}

		TEST_CASE("ScriptData: schema snapshots pin descriptors and preserve canonical field order")
		{
			const AssetHandle handle(0x4001);
			auto script = Test::SchemaScript(Test::AllScriptFields());
			const auto source = ScriptFieldSchemaSource::Create({ { handle, script } });
			REQUIRE(source);
			const auto field = (*source)->FindField(handle, "Choice");
			const auto schema = (*source)->FindSchema(handle, "Choice");
			REQUIRE(field);
			REQUIRE(schema);
			const auto* type = &(*field)->GetType();
			const auto* enumeration = type->GetEnum();
			REQUIRE(enumeration);
			script.reset();
			const auto replacement = ScriptFieldSchemaSource::Create({ { handle, Test::SchemaScript({ Test::ScriptField("Other", FieldType::Bool, true) }) } });
			REQUIRE(replacement);
			CHECK((*schema)->DefaultValue.Get() == "Second");
			CHECK(enumeration->GetEntries()[0].Name == "Second");
			CHECK(enumeration->GetEntries()[1].Name == "First");
			CHECK(&(**(*source)->FindField(handle, "Choice")).GetType() == type);
			const auto names = (*source)->GetFieldNames(handle);
			CHECK(std::is_sorted(names.begin(), names.end()));
			const auto missing = (*source)->FindField(handle, "Chioce");
			REQUIRE_FALSE(missing);
			CHECK(missing.error().GetCode() == ErrorCode::NotFound);
			CHECK(missing.error().GetHint().find("Choice") != std::string::npos);
			CHECK_FALSE((*source)->FindField(AssetHandle(0x9999), "Choice"));
		}

		TEST_CASE("ScriptData: schema creation rejects malformed descriptors before reflection construction")
		{
			const auto rejects = [](std::vector<ScriptFieldSchema> fields)
			{
				const auto result = ScriptFieldSchemaSource::Create({ { AssetHandle(0x4001), Test::SchemaScript(std::move(fields)) } });
				REQUIRE_FALSE(result);
				CHECK(result.error().GetCode() == ErrorCode::Validation);
			};
			rejects({ Test::ScriptField("Entity", FieldType::Float, 0) });
			rejects({ Test::ScriptField("OnStart", FieldType::Bool, false) });
			rejects({ Test::ScriptField("Value", FieldType::Float, "wrong") });
			rejects({ Test::ScriptField("Value", FieldType::Map, Json::object()) });
			rejects({ Test::ScriptField("Value", FieldType::Array, Json::array()) });
			rejects({ Test::ScriptField("Same", FieldType::Bool, false), Test::ScriptField("Same", FieldType::Bool, true) });
			auto field = Test::ScriptField("Value", FieldType::Float, 0);
			field.Meta.Min = std::numeric_limits<double>::infinity();
			rejects({ field });
			field.Meta.Min = 2;
			field.Meta.Max = 1;
			rejects({ field });
			auto cyclic = CreateRef<ScriptFieldSchema>(Test::ScriptField("", FieldType::Array, Json::array()));
			cyclic->Element = cyclic;
			auto array = Test::ScriptField("Cycle", FieldType::Array, Json::array());
			array.Element = cyclic;
			rejects({ array });
			cyclic->Element.reset();
			auto leaf = CreateRef<ScriptFieldSchema>(Test::ScriptField("", FieldType::Bool, false));
			for (size_t i = 0; i < 70; ++i)
			{
				auto parent = CreateRef<ScriptFieldSchema>(Test::ScriptField("", FieldType::Array, Json::array()));
				parent->Element = leaf;
				leaf = std::move(parent);
			}
			array.Element = leaf;
			rejects({ array });
			CHECK_FALSE(ScriptFieldSchemaSource::Create({ { AssetHandle(0x4001), nullptr } }));
			auto module = CreateRef<ScriptData>();
			module->Fields = { Test::ScriptField("Value", FieldType::Float, 0) };
			CHECK_FALSE(ScriptFieldSchemaSource::Create({ { AssetHandle(0x4001), module } }));
		}

		TEST_CASE("ScriptData: generic recursive validation rejects enum asset and quaternion violations")
		{
			for (const auto kind : { FieldType::Enum, FieldType::AssetRef, FieldType::Quat })
			{
				auto element = CreateRef<ScriptFieldSchema>();
				element->Type = kind;
				Json bad;
				Value invalid;
				if (kind == FieldType::Enum)
				{
					element->DefaultValue = VariantValue("First");
					element->EnumValues = { "First", "Second" };
					bad = "Frist";
					invalid = Value::FromEnum(50);
				}
				else if (kind == FieldType::AssetRef)
				{
					element->Meta.AssetFilter = "AudioClip";
					bad = 42;
					invalid = Value::FromFloat(42);
				}
				else
				{
					element->DefaultValue = VariantValue(Json::array({ 0, 0, 0, 1 }));
					bad = Json::array({ 0, 0, 0, 0 });
					invalid = Value::FromQuat(glm::quat(0, 0, 0, 0));
				}
				auto array = Test::ScriptField("Items", FieldType::Array, Json::array());
				array.Element = element;
				const auto snapshot = ScriptFieldSchemaSource::Create({ { AssetHandle(0x4001), Test::SchemaScript({ array }) } });
				REQUIRE(snapshot);
				const auto field = (*snapshot)->FindField(AssetHandle(0x4001), "Items");
				REQUIRE(field);
				ValidationContext jsonValidation;
				const Json badArray = Json::array({ bad });
				(*field)->ValidateJson(JsonReader(badArray), ResolveContext{}, jsonValidation);
				REQUIRE(jsonValidation.HasErrors());
				CHECK(jsonValidation.GetIssues()[0].JsonPointer.starts_with("/Items/0"));
				ValidationContext valueValidation;
				(*field)->ValidateValue(Value::FromArray({ invalid }), ResolveContext{}, valueValidation);
				REQUIRE(valueValidation.HasErrors());
				CHECK(valueValidation.GetIssues()[0].JsonPointer.starts_with("/Items/0"));
				if (kind == FieldType::AssetRef)
					CHECK((*field)->GetType().GetElementSchema()->GetMeta().AssetFilter == "AudioClip");
			}
		}

		TEST_CASE("ScriptData: cooked input rejects truncation corruption unsupported ABI and noncanonical records")
		{
			auto script = Test::CompiledAsset();
			script.Kind = ScriptKind::Behaviour;
			script.Name = "Probe";
			script.Fields = Test::AllScriptFields();
			const auto cooked = CookScript(script, 1);
			REQUIRE(cooked);
			for (size_t i = 0; i < cooked->size(); ++i)
				CHECK_FALSE(LoadCookedScript(std::span<const std::byte>(*cooked).first(i)));
			const auto view = ReadCookedArtifact(*cooked);
			REQUIRE(view);
			Buffer payload(view->Payload.begin(), view->Payload.end());
			payload[0] = std::byte{ 255 };
			const auto wrongAbi = LoadCookedScript(WriteCookedArtifact(AssetType::Script, 1, 1, payload));
			REQUIRE_FALSE(wrongAbi);
			CHECK(wrongAbi.error().GetCode() == ErrorCode::UnsupportedVersion);
			Random random(314159);
			for (size_t i = 0; i < 10000; ++i)
			{
				Buffer mutated = *cooked;
				const size_t offset = CookedHeader::Size + static_cast<size_t>(random.NextU64() % (mutated.size() - CookedHeader::Size));
				mutated[offset] ^= std::byte{ 1 };
				CHECK_FALSE(LoadCookedScript(mutated));
				Buffer inner(view->Payload.begin(), view->Payload.end());
				inner[static_cast<size_t>(random.NextU64() % inner.size())] ^= std::byte{ 1 };
				// Rehashing reaches the payload decoder. A mutation may describe another valid asset.
				const auto decoded = LoadCookedScript(WriteCookedArtifact(AssetType::Script, 1, 1, inner));
				if (decoded)
					CHECK(CookScript(**decoded, 1).has_value());
			}
			std::reverse(script.Fields.begin(), script.Fields.end());
			CHECK_FALSE(CookScript(script, 1));
		}

		TEST_CASE("ScriptData: identical inputs cook identically across configurations")
		{
			auto first = Test::CompiledAsset("return 1 + 2");
			auto second = Test::CompiledAsset("return 1 + 2");
			first.Kind = second.Kind = ScriptKind::Behaviour;
			first.Name = second.Name = "Canonical";
			first.Fields = second.Fields = Test::AllScriptFields();
			const auto a = CookScript(first, 19);
			const auto b = CookScript(second, 19);
			REQUIRE(a);
			REQUIRE(b);
			CHECK(*a == *b);
			const auto view = ReadCookedArtifact(*a);
			REQUIRE(view);
			BinaryReader bytes(view->Payload);
			CHECK(bytes.ReadU32() == ScriptData::CompilerAbiVersion);
			CHECK(bytes.ReadU8() == 0);
			const auto loaded = LoadCookedScript(*a);
			REQUIRE(loaded);
			const auto recooked = CookScript(**loaded, 19);
			REQUIRE(recooked);
			CHECK(*a == *recooked);
			CHECK(XXH64(*a) == XXH64(*b));
		}

		TEST_CASE("ScriptData: cooked source maps retain replay origins and authored expression offsets")
		{
			const std::string source = "-- UTF-8: café\r\n1 + 2\r\n";
			const auto script = Test::CompiledAsset(source, ScriptCompileMode::ExpressionOrChunk,
				"project://Assets/Probe.replay", "=replay/0000000000004001/Expect/2", "/Expect/2/Luau");
			const auto cooked = CookScript(script, 1);
			REQUIRE(cooked);
			const auto loaded = LoadCookedScript(*cooked);
			REQUIRE(loaded);
			const auto& map = (*loaded)->SourceMap;
			CHECK(map.Path == "Assets/Probe.replay");
			CHECK(map.JsonPointer == "/Expect/2/Luau");
			CHECK(map.ChunkName == "=replay/0000000000004001/Expect/2");
			CHECK(map.GeneratedPrefixLines == 1);
			CHECK(map.SourceHash == XXH64(source));
			CHECK(map.SourceByteCount == source.size());
			CHECK(map.LineOffsets.back() == source.size());
		}

		TEST_CASE("ScriptData: unchanged chunks and fileless evaluation retain distinct source identities")
		{
			for (const bool fileless : { false, true })
			{
				const auto script = Test::CompiledAsset("return true", ScriptCompileMode::ExpressionOrChunk,
					fileless ? "" : "project://Assets/Probe.replay", fileless ? "=eval" : "=replay/0000000000004001/Expect/1",
					fileless ? "/code" : "/Expect/1/Luau");
				const auto cooked = CookScript(script, 1);
				REQUIRE(cooked);
				const auto loaded = LoadCookedScript(*cooked);
				REQUIRE(loaded);
				CHECK((*loaded)->SourceMap.Path == script.SourceMap.Path);
				CHECK((*loaded)->SourceMap.JsonPointer == script.SourceMap.JsonPointer);
				CHECK((*loaded)->SourceMap.GeneratedPrefixLines == 0);
			}
		}

		TEST_CASE("ScriptData: malformed source attribution fails checked cooking and decoding")
		{
			const auto original = Test::CompiledAsset();
			for (const std::string pointer : { "wrong", "/~", "/~2" })
			{
				auto script = original;
				script.SourceMap.JsonPointer = pointer;
				CHECK_FALSE(CookScript(script, 1));
			}
			auto script = original;
			script.SourceMap.JsonPointer = "/valid~0/~1";
			CHECK(CookScript(script, 1).has_value());
			script.SourceMap.GeneratedPrefixLines = 2;
			CHECK_FALSE(CookScript(script, 1));
			script = original;
			script.SourceMap.LineOffsets.push_back(script.SourceMap.SourceByteCount + 1);
			CHECK_FALSE(CookScript(script, 1));
			script = original;
			script.SourceMap.Path.clear();
			CHECK_FALSE(CookScript(script, 1));
			script.SourceMap.ChunkName = "=eval";
			CHECK(CookScript(script, 1).has_value());
			script.SourceMap.ChunkName.clear();
			CHECK_FALSE(CookScript(script, 1));
		}
	}

}
