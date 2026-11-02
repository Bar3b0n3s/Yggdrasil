#include "TestsPCH.h"

#include "Engine/Asset/StringTableData.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <format>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// The Loc module (M22, C22.1, R-UI-L10N; Docs/Decisions/0033-m22-contract.md decision 11): localized strings from string tables
// in the VM's locale.

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

	namespace Test {

		// The Loc module's coverage scenario ("ScriptApi: every registered function, method, property, operator and constructor
		// has a test"): every function called with real dispatcher observations, in `mode`, in a read-only test-mode
		// evaluation as a suite runs.
		void RunLocBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true, .ReadOnly = true, .Locale = "pt-BR" });
			fixture.GetAssetManager().Publish(MenuTableHandle, MakeMenuTable(), "Assets/Text/Menu.stringtable");
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
local menu = Assets.Load("Assets/Text/Menu.stringtable")
assert(Loc.GetLocale() == "pt-BR")
assert(Loc.Get(menu, "Menu.Start") == "Iniciar")
return Loc.Has(menu, "Menu.Quit") and not Loc.Has(menu, "Menu.Missing")
)");
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
			CHECK(fixture.ExternalMutations.empty());
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage.has_value());
			for (const auto& counter : coverage->Members)
			{
				if (counter.Owner == "Loc")
				{
					CAPTURE(counter.Member);
					CHECK(counter.Calls > 0);
				}
			}
			snapshots.push_back(*coverage);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("Loc: Get reads the VM's locale, then its language, then the table's fallback")
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

		TEST_CASE("Loc: an empty locale reads every table's fallback")
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

		TEST_CASE("Loc: Get of a key the table lacks, and Get or Has of an asset that is not a string table or fails to load, raise a located error")
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
			// The closest key is named as a hint (decision 11), and only a close one: a key like none of the table's names none.
			CHECK(missing.error().ToString().contains("'Menu.Start'"));
			CHECK_FALSE(missing.error().ToString().contains("'Menu.Quit'"));
			const auto unlike = fixture.Evaluate(R"(return Loc.Get(Assets.Load("Assets/Text/Menu.stringtable"), "Options.Volume"))");
			REQUIRE_FALSE(unlike.has_value());
			CHECK(unlike.error().ToString().contains("Options.Volume"));
			CHECK_FALSE(unlike.error().ToString().contains("'Menu.Start'"));
			CHECK_FALSE(unlike.error().ToString().contains("'Menu.Quit'"));
			// A key no table can hold, longer than a key may be or not UTF-8, is named by its byte count, so the error stays
			// short, valid text.
			for (const auto& [key, bytes] : { std::pair<std::string_view, size_t>{ R"(string.rep("k", 257))", 257 }, { R"("Menu\255")", 5 } })
			{
				CAPTURE(key);
				const auto unnamed = fixture.Evaluate(std::format(R"(return Loc.Get(Assets.Load("Assets/Text/Menu.stringtable"), {}))", key));
				REQUIRE_FALSE(unnamed.has_value());
				const std::string text = unnamed.error().ToString();
				CHECK(text.contains(std::format("a key of {} bytes", bytes)));
				CHECK(IsValidUtf8(text));
				CHECK_FALSE(text.contains(std::string(16, 'k')));
			}
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
