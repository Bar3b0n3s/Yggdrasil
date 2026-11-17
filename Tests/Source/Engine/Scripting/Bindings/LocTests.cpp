#include "TestsPCH.h"

#include "Engine/Asset/StringTableData.h"
#include "Engine/Core/Utf8.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scripting/ScriptApiRegistry.h"
#include "Engine/Scripting/ScriptEngine.h"
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
// in the VM's locale, which Loc.SetLocale changes (M38, Docs/Decisions/0055-m38-runtime-locale.md decisions 1 and 2).

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

		// A behaviour that traces its OnLocaleChanged as "<entity>:<locale>:<previous>:<Loc.GetLocale()>" (Scene.CreateEntity of a
		// "Trace:" entity, read back by LocaleTrace); "Faulty" then raises, and "Nested" tries a change of its own.
		constexpr std::string_view LocaleListenerSource = R"(
local L = {}
function L:OnCreate() self.Label = self.Entity.Name end
function L:OnLocaleChanged(locale, previous)
	Scene.CreateEntity("Trace:" .. self.Label .. ":" .. locale .. ":" .. previous .. ":" .. Loc.GetLocale())
	if self.Label == "Faulty" then error("the redraw failed") end
	if self.Label == "Nested" then
		local ok, message = pcall(Loc.SetLocale, "ar")
		Scene.CreateEntity("Trace:Nested:refused:" .. tostring(not ok and string.find(message, "OnLocaleChanged", 1, true) ~= nil))
	end
end
function L:OnDestroy()
	if self.Label == "Teardown" then
		local ok, message = pcall(Loc.SetLocale, "ar")
		Scene.CreateEntity("Trace:Teardown:refused:" .. tostring(not ok and string.find(message, "stops", 1, true) ~= nil))
	end
end
return Script.Define("Listener", L)
)";
		constexpr AssetHandle LocaleListenerHandle{ 0x38A0001 };

		void AttachListener(Entity entity, int32_t order = 0)
		{
			ScriptSlot slot{};
			slot.Script.SetHandle(LocaleListenerHandle);
			slot.ExecutionOrder = order;
			entity.AddComponent<ScriptComponent>(ScriptComponent{ { slot } });
		}

		// The "Trace:" entities' names without the prefix, in canonical order (their creation order: roots).
		std::vector<std::string> LocaleTrace(Scene& scene)
		{
			std::vector<std::string> trace;
			for (const UUID id : scene.GetCanonicalOrder())
			{
				const Entity entity = scene.FindEntityByID(id);
				if (entity && entity.GetName().starts_with("Trace:"))
					trace.push_back(entity.GetName().substr(6));
			}
			return trace;
		}

	}

	namespace Test {

		// The Loc module's coverage scenario ("ScriptApi: every registered function, method, property, operator and constructor
		// has a test"): every function called with real dispatcher observations, in `mode`, in a read-only test-mode
		// evaluation as a suite runs.
		void RunLocBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			const size_t firstSnapshot = snapshots.size();
			{
				// The lookups, in a read-only test-mode evaluation.
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
				snapshots.push_back(*coverage);
			}
			{
				// M38 (decision 2): SetLocale writes the session's locale, so it runs where the host has one.
				Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true, .Locale = "pt-BR" });
				fixture.SessionLocale = "pt-BR";
				fixture.GetAssetManager().Publish(MenuTableHandle, MakeMenuTable(), "Assets/Text/Menu.stringtable");
				REQUIRE(fixture.Start().has_value());
				const auto result = fixture.Evaluate(R"(
local menu = Assets.Load("Assets/Text/Menu.stringtable")
Loc.SetLocale("fr")
return Loc.GetLocale() == "fr" and Loc.Get(menu, "Menu.Start") == "Commencer"
)");
				REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
				CHECK(result->Value.Get() == Json(true));
				CHECK(fixture.SessionLocale == std::optional<std::string>("fr"));
				const auto coverage = fixture.GetApi().GetCoverage(mode);
				REQUIRE(coverage.has_value());
				snapshots.push_back(*coverage);
			}
			for (const auto& member : snapshots[firstSnapshot].Members)
			{
				if (member.Owner != "Loc")
					continue;
				CAPTURE(member.Member);
				uint64_t calls = 0;
				for (size_t index = firstSnapshot; index < snapshots.size(); ++index)
					for (const auto& counter : snapshots[index].Members)
						if (counter.Owner == member.Owner && counter.Member == member.Member)
							calls += counter.Calls;
				CHECK(calls > 0);
			}
		}

		// M38 (decision 2): OnLocaleChanged in `mode`, delivered by a Loc.SetLocale, for the behaviour callbacks' coverage
		// (RunBehaviourCallbacksCoverage, ScriptInstanceLifecycleTests.cpp).
		void RunLocaleCallbackCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true, .Locale = "en" });
			fixture.SessionLocale = "en";
			REQUIRE(fixture.AddScript(LocaleListenerHandle, "Assets/Scripts/Listener.luau", std::string(LocaleListenerSource)));
			AttachListener(fixture.GetScene().CreateEntity("Title"));
			REQUIRE(fixture.Start());
			REQUIRE(fixture.Evaluate("Loc.SetLocale(\"de\")"));
			CHECK(LocaleTrace(fixture.GetScene()) == std::vector<std::string>{ "Title:de:en:de" });
			CHECK(fixture.Errors.empty());
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage);
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

		// M38 (Docs/Decisions/0055-m38-runtime-locale.md decisions 1 and 2; Roadmap M38's acceptance).
		TEST_CASE("Loc: SetLocale changes what Get and GetLocale return from the next call")
		{
			Test::ScriptTestFixture fixture({ .Locale = "en" });
			fixture.SessionLocale = "en";
			fixture.GetAssetManager().Publish(MenuTableHandle, MakeMenuTable(), "Assets/Text/Menu.stringtable");
			REQUIRE(fixture.Start().has_value());
			const auto result = fixture.Evaluate(R"(
local menu = Assets.Load("Assets/Text/Menu.stringtable")
local before = Loc.Get(menu, "Menu.Start")
Loc.SetLocale("pt-BR")
local brazilian = Loc.GetLocale() .. "=" .. Loc.Get(menu, "Menu.Start")
Loc.SetLocale("fr-CA")
local canadian = Loc.GetLocale() .. "=" .. Loc.Get(menu, "Menu.Start") .. "," .. Loc.Get(menu, "Menu.Quit")
Loc.SetLocale("")
local fallback = Loc.GetLocale() .. "=" .. Loc.Get(menu, "Menu.Start")
-- Has asks only whether the table holds the key, in every locale.
return { before, brazilian, canadian, fallback, Loc.Has(menu, "Menu.Quit") }
)");
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			CHECK(result->Value.Get() == Json::array({ "Start", "pt-BR=Iniciar", "fr-CA=Commencer,Quit", "=Start", true }));
			// The session's locale is the host's (a play session's), and the engine's copy is what the next lookup reads.
			CHECK(fixture.SessionLocale == std::optional<std::string>(""));
			CHECK(fixture.GetEngine()->GetLocale() == "");
			// One evaluation notifies its write once (PrepareHostMutation), so an input-only recording is invalidated.
			CHECK(fixture.ExternalMutations.size() == 1);

			// The locale in force changes nothing: no mutation notice.
			fixture.ExternalMutations.clear();
			REQUIRE(fixture.Evaluate("Loc.SetLocale(\"\")"));
			CHECK(fixture.ExternalMutations.empty());
		}

		TEST_CASE("Loc: SetLocale refuses a malformed tag and a change inside OnLocaleChanged")
		{
			Test::ScriptTestFixture fixture({ .Locale = "en" });
			fixture.SessionLocale = "en";
			REQUIRE(fixture.AddScript(LocaleListenerHandle, "Assets/Scripts/Listener.luau", std::string(LocaleListenerSource)));
			AttachListener(fixture.GetScene().CreateEntity("Nested"));
			AttachListener(fixture.GetScene().CreateEntity("Teardown"));
			REQUIRE(fixture.Start());
			{
				const Test::ExpectLog refused(LogLevel::Error, "not a locale");
				for (const std::string_view argument : { "\"de_DE\"", "\"-de\"", "\"d e\"", "string.rep(\"a\", 36)", "\"de\\255\"" })
				{
					CAPTURE(argument);
					const auto result = fixture.Evaluate(std::format("Loc.SetLocale({})", argument));
					REQUIRE_FALSE(result.has_value());
					CHECK(result.error().ToString().contains("not a locale"));
					CHECK(IsValidUtf8(result.error().ToString()));
				}
				CHECK(fixture.GetEngine()->GetLocale() == "en");
				CHECK(fixture.SessionLocale == std::optional<std::string>("en"));
				CHECK(LocaleTrace(fixture.GetScene()).empty());
			}

			// A change inside OnLocaleChanged raises there; the first change stands.
			REQUIRE(fixture.Evaluate("Loc.SetLocale(\"de\")"));
			CHECK(LocaleTrace(fixture.GetScene()) == std::vector<std::string>{ "Nested:de:en:de", "Nested:refused:true", "Teardown:de:en:de" });
			CHECK(fixture.GetEngine()->GetLocale() == "de");
			CHECK(fixture.SessionLocale == std::optional<std::string>("de"));
			CHECK(fixture.Errors.empty());

			// A change at teardown (an OnDestroy of Stop) raises too.
			fixture.GetEngine()->Stop();
			CHECK(LocaleTrace(fixture.GetScene()).back() == "Teardown:refused:true");
			CHECK(fixture.SessionLocale == std::optional<std::string>("de"));
		}

		TEST_CASE("Loc: SetLocale needs a host with a session locale and a writable VM")
		{
			{
				// The edit evaluation's host and every host without a session keep IScriptHost's default.
				Test::ScriptTestFixture fixture({ .Locale = "en" });
				REQUIRE(fixture.Start());
				const Test::ExpectLog refused(LogLevel::Error, "play session");
				const auto result = fixture.Evaluate("Loc.SetLocale(\"de\")");
				REQUIRE_FALSE(result.has_value());
				CHECK(result.error().ToString().contains("Loc.SetLocale works in a play session"));
				CHECK(fixture.GetEngine()->GetLocale() == "en");
			}
			{
				// A read-only evaluation (the edit context's) is refused before anything changes.
				Test::ScriptTestFixture fixture({ .ReadOnly = true, .Locale = "en" });
				fixture.SessionLocale = "en";
				REQUIRE(fixture.Start());
				const Test::ExpectLog refused(LogLevel::Error, "");
				CHECK_FALSE(fixture.Evaluate("Loc.SetLocale(\"de\")").has_value());
				CHECK(fixture.SessionLocale == std::optional<std::string>("en"));
				CHECK(fixture.ExternalMutations.empty());
			}
		}
	}

}
