#include "TestsPCH.h"

#include "Engine/Asset/StringTableData.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <format>
#include <string>
#include <string_view>

// The Loc module (M22, C22.1, R-UI-L10N; Docs/Decisions/0033-m22-contract.md decision 11): localized strings from string tables
// in the VM's locale. Skipped by the contract; stream L un-skips them.

namespace Engine {

	namespace {

		constexpr AssetHandle MenuTableHandle{ 0x57AB0001 };

		// Two entries sorted by key, as a loaded table's are: "Menu.Quit" in English only, "Menu.Start" in three locales.
		Ref<StringTableData> MakeMenuTable()
		{
			auto table = CreateRef<StringTableData>();
			table->Locales = { "en", "fr", "pt-BR" };
			table->Entries = {
				StringTableEntry{ .Key = "Menu.Quit", .Values = { { "en", "Quit" } } },
				StringTableEntry{ .Key = "Menu.Start", .Values = { { "en", "Start" }, { "fr", "Commencer" }, { "pt-BR", "Iniciar" } } },
			};
			return table;
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("Loc: Get reads the VM's locale, then its language, then the table's fallback" * doctest::skip(true))
		{
			Test::ScriptTestFixture fixture({ .Locale = "fr-CA" });
			fixture.GetAssetManager().Publish(MenuTableHandle, MakeMenuTable(), "Assets/Text/Menu.stringtable");
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
local menu = Assets.Load("Assets/Text/Menu.stringtable")
assert(Loc.GetLocale() == "fr-CA")
assert(Loc.Get(menu, "Menu.Start") == "Commencer")
assert(Loc.Get(menu, "Menu.Quit") == "Quit")
return Loc.Has(menu, "Menu.Start") and not Loc.Has(menu, "Menu.Missing")
)");
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK(result->Value.Get() == Json(true));
		}

		TEST_CASE("Loc: an empty locale reads every table's fallback" * doctest::skip(true))
		{
			Test::ScriptTestFixture fixture;
			fixture.GetAssetManager().Publish(MenuTableHandle, MakeMenuTable(), "Assets/Text/Menu.stringtable");
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
local menu = Assets.Load("Assets/Text/Menu.stringtable")
return Loc.GetLocale() == "" and Loc.Get(menu, "Menu.Start") == "Start"
)");
			REQUIRE_MESSAGE(result.has_value(), result.error().ToString());
			CHECK(result->Value.Get() == Json(true));
		}

		TEST_CASE("Loc: Get of a key the table lacks, and Get or Has of an asset that is not a string table or fails to load, raise a located error"
			* doctest::skip(true))
		{
			Test::ScriptTestFixture fixture;
			fixture.GetAssetManager().Publish(MenuTableHandle, MakeMenuTable(), "Assets/Text/Menu.stringtable");
			REQUIRE(fixture.AddScript(AssetHandle(0x5C1), "Assets/Scripts/Probe.luau", "return Script.Define(\"Probe\", {})").has_value());
			REQUIRE(fixture.Start().has_value());
			// Each refused lookup is a failed evaluation, which the VM logs as a script error of Loc.Get or Loc.Has.
			const Test::ExpectLog raised(LogLevel::Error, "Loc.");
			const auto missing = fixture.Evaluate(R"(return Loc.Get(Assets.Load("Assets/Text/Menu.stringtable"), "Menu.Strat"))");
			REQUIRE_FALSE(missing.has_value());
			CHECK(missing.error().ToString().contains("Menu.Strat"));
			const auto wrongType = fixture.Evaluate(R"(return Loc.Get(Assets.Load("Assets/Scripts/Probe.luau"), "Menu.Start"))");
			REQUIRE_FALSE(wrongType.has_value());
			const auto hasWrongType = fixture.Evaluate(R"(return Loc.Has(Assets.Load("Assets/Scripts/Probe.luau"), "Menu.Start"))");
			REQUIRE_FALSE(hasWrongType.has_value());

			// A table that fails to load raises in Has as in Get: the chunk's entity request removes the table it already holds,
			// so the lookup's load fails. The same chunk succeeds while the table stays.
			for (const std::string_view lookup : { "Loc.Get(menu, \"Menu.Start\")", "Loc.Has(menu, \"Menu.Start\")" })
			{
				CAPTURE(lookup);
				const std::string chunk = std::format(R"(
local menu = Assets.Load("Assets/Text/Menu.stringtable")
Scene.CreateEntity("Probe")
return {}
)",
					lookup);
				fixture.GetAssetManager().Publish(MenuTableHandle, MakeMenuTable(), "Assets/Text/Menu.stringtable");
				const auto kept = fixture.Evaluate(chunk);
				REQUIRE_MESSAGE(kept.has_value(), kept.error().ToString());
				fixture.OnHostRequest = [&fixture](std::string_view /*request*/)
				{
					fixture.GetAssetManager().Remove(MenuTableHandle);
				};
				const auto unloaded = fixture.Evaluate(chunk);
				fixture.OnHostRequest = {};
				CHECK_FALSE(unloaded.has_value());
			}
		}
	}

}
