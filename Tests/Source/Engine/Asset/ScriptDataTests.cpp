#include "TestsPCH.h"

#include "Engine/Asset/ScriptData.h"

#include "Engine/Reflection/Value.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

namespace Engine {

	TEST_SUITE("Asset")
	{
		TEST_CASE("ScriptData: recursive array schemas enforce element metadata through generic field validation" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			auto scalar = CreateRef<ScriptFieldSchema>();
			scalar->Type = FieldType::Float;
			scalar->DefaultValue = VariantValue(0.0f);
			scalar->Meta.Min = 0.0;
			scalar->Meta.Max = 1.0;
			scalar->Meta.Step = 0.25;
			scalar->Tooltip = "Normalized weight.";
			auto row = CreateRef<ScriptFieldSchema>();
			row->Type = FieldType::Array;
			row->DefaultValue = VariantValue(Json::array());
			row->Element = scalar;
			auto script = CreateRef<ScriptData>();
			script->Kind = ScriptKind::Behaviour;
			script->Name = "Grid";
			script->Fields.push_back({
				.Name = "Rows",
				.Type = FieldType::Array,
				.DefaultValue = VariantValue(Json::array()),
				.Element = row,
			});
			const AssetHandle handle(0x4001);
			const auto source = ScriptFieldSchemaSource::Create({ { handle, script } });
			REQUIRE(source);
			const auto found = (*source)->FindField(handle, "Rows");
			REQUIRE(found);
			const auto* field = *found;
			CHECK_FALSE(field->IsStored());
			CHECK_FALSE(field->GetType().HasOps());
			const auto* rowSchema = field->GetType().GetElementSchema();
			REQUIRE(rowSchema != nullptr);
			CHECK(&rowSchema->GetType() == field->GetType().GetElement());
			const auto* elementSchema = rowSchema->GetType().GetElementSchema();
			REQUIRE(elementSchema != nullptr);
			CHECK(&elementSchema->GetType() == rowSchema->GetType().GetElement());
			CHECK(elementSchema->GetMeta().Min == 0.0);
			CHECK(elementSchema->GetMeta().Max == 1.0);
			CHECK(elementSchema->GetMeta().Step == 0.25);
			CHECK(elementSchema->GetDescription() == "Normalized weight.");
			ResolveContext resolve{};
			resolve.Schemas = source->get();

			const Json valid = Json::array({ Json::array({ 0.0f, 0.5f, 1.0f }) });
			ValidationContext validResult;
			field->ValidateJson(JsonReader(valid), resolve, validResult);
			CHECK_FALSE(validResult.HasErrors());

			const Json invalid = Json::array({ Json::array({ 0.5f }), Json::array({ -0.25f }) });
			ValidationContext jsonResult;
			field->ValidateJson(JsonReader(invalid), resolve, jsonResult);
			REQUIRE(jsonResult.GetErrorCount() == 1);
			REQUIRE(jsonResult.GetIssues().size() == 1);
			CHECK(jsonResult.GetIssues()[0].JsonPointer == "/Rows/1/0");

			const auto invalidValue = Value::FromArray({ Value::FromArray({ Value::FromFloat(0.5f) }),
				Value::FromArray({ Value::FromFloat(-0.25f) }) });
			ValidationContext valueResult;
			field->ValidateValue(invalidValue, resolve, valueResult);
			REQUIRE(valueResult.GetErrorCount() == 1);
			REQUIRE(valueResult.GetIssues().size() == 1);
			CHECK(valueResult.GetIssues()[0].JsonPointer == "/Rows/1/0");
		}

		TEST_CASE("ScriptData: cooked schemas retain every kind default option and source map" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Round-trip engine-compiled Behaviour, Module and TestSuite artifacts; cover Number f32 zero, Integer zero, "
				 "Bool false, String empty, Vector zero, Color white, Quat identity, null Entity/Asset, first authored Enum "
				 "and empty Array defaults; explicit defaults, nested element metadata, asset filters, require handles and line offsets");
		}

		TEST_CASE("ScriptData: schema snapshots pin descriptors and preserve canonical field order" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Destroy input map/temporary builders, then read stable FieldInfo/TypeInfo/EnumInfo addresses; retain an old "
				 "snapshot across replacement; enum names keep authored order; unknown handle/field is NotFound with suggestions");
		}

		TEST_CASE("ScriptData: schema creation rejects malformed descriptors before reflection construction" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Reject duplicate/reserved names, invalid defaults, nonfinite or inverted bounds, unsupported types/options, "
				 "bad enum/default combinations, missing or cyclic Array elements and excessive depth without asserts; "
				 "Module/TestSuite cannot carry fields; reject null asset entries");
		}

		TEST_CASE("ScriptData: generic recursive validation rejects enum asset and quaternion violations" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Run nested Array(Enum), Array(Asset) and Array(Quat) through generic ValidateJson and ValidateValue; "
				 "check exact index pointers, enum suggestions, UUID representation, preserved asset filter and unit-quaternion rule");
		}

		TEST_CASE("ScriptData: cooked input rejects truncation corruption unsupported ABI and noncanonical records" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Seeded mutations exercise every length/count/tag, hash, type/version/compiler ABI, canonical order, UTF-8, "
				 "source offsets, null require handles, nesting and trailing bytes; errors are Results with no VM execution");
		}

		TEST_CASE("ScriptData: identical inputs cook identically across configurations" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Compare complete ECKD bytes for repeat imports and recorded Debug/Release/Dist loads, including nested "
				 "schemas and require ordering; Dist loads without Analysis or Compiler references");
		}

		TEST_CASE("ScriptData: cooked source maps retain replay origins and authored expression offsets" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Round-trip trusted compiled Module chunks with ChunkName =replay/<handle>/Expect/2, real .replay Path, "
				 "JsonPointer /Expect/2/Luau and GeneratedPrefixLines 1; hash/count/line offsets describe exact authored "
				 "UTF-8/CRLF source before wrapping. Preserve pointer and mapping in a nested Replay artifact loaded in Dist");
		}

		TEST_CASE("ScriptData: unchanged chunks and fileless evaluation retain distinct source identities" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Round-trip ordinary .luau chunks with empty pointer and zero prefix; already-returning replay chunks have "
				 "zero prefix and their real replay path/pointer; fileless eval has empty Path, explicit stable = chunk label "
				 "and /code. Different same-tick replay indices remain distinct; no label becomes a require base");
		}

		TEST_CASE("ScriptData: malformed source attribution fails checked cooking and decoding" * doctest::skip())
		{
			ENGINE_CONTRACT_STUB();
			FAIL("Reject malformed RFC 6901 escapes, prefix greater than one, empty/invalid chunk labels, empty Path with "
				 "a file chunk name, overflowing or out-of-bounds authored offsets, truncated pointer/prefix payload; retain "
				 "valid ~0/~1 escapes and final empty line without invented physical JSON locations or runtime columns");
		}
	}

}
