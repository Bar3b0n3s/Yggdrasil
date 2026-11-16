#include "TestsPCH.h"
#include "Engine/Asset/DocumentData.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Engine/Scripting/ScriptHost.h"
#include "Engine/Session/PlaySession.h"
#include "Engine/Session/SaveDataStore.h"
#include "Support/ExpectLog.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <nlohmann/json.hpp>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

// M28 (Docs/Decisions/0043-m28-game-services.md decision 8): the SaveData module over a play session's store.

namespace Engine {

	namespace {

		// A live session's host: records what SaveData.Commit hands it.
		class SaveDataHost final : public IPlaySessionHost
		{
		public:
			ScriptEnvironment GetScriptEnvironment() const override { return { .IsEditor = false, .IsHeadless = false }; }
			Status SetScriptCursorMode(CursorMode) override { return {}; }
			CursorMode GetScriptCursorMode() const override { return CursorMode::Normal; }
			void OnScriptError(const ScriptError&, bool) override {}
			void OnScriptPhase(std::string_view, std::string_view) override {}
			Status CommitSaveData(const Json& values) override
			{
				Commits.push_back(values);
				if (Refusal.has_value())
					return MakeError(ErrorCode::Unsupported, "{}", *Refusal);
				return {};
			}
			std::vector<Json> Commits{};
			std::optional<std::string> Refusal{};
		};

		struct SaveDataSession
		{
			Test::ScriptTestFixture Fixture{};
			SaveDataHost Host{};
			Scope<PlaySession> Session{};

			// A session of the fixture's scene, seeded with `values`; testMode makes it a test run.
			void Start(const Json& values, bool testMode = false, std::optional<std::string> loadError = std::nullopt)
			{
				PlaySessionSpecification specification{};
				specification.Registry = &Fixture.GetTypes();
				specification.Assets = &Fixture.GetAssetManager();
				specification.ScriptSchemas = Fixture.GetSchemaSnapshot();
				specification.TestMode = testMode;
				specification.SaveData = VariantValue(values);
				specification.SaveDataLoadError = std::move(loadError);
				specification.Host = &Host;
				auto created = PlaySession::CreateFromScene(specification, Fixture.GetScene());
				REQUIRE_MESSAGE(created.has_value(), (created ? "" : created.error().ToString()));
				Session = std::move(*created);
			}

			[[nodiscard]] Json Evaluate(std::string_view source)
			{
				const auto evaluated = Session->GetScripts()->Evaluate(source, {});
				REQUIRE_MESSAGE(evaluated.has_value(), (evaluated ? "" : evaluated.error().ToString()));
				return evaluated->Value.Get();
			}

			// Whether `source` raises; its script error is logged (an expected entry).
			[[nodiscard]] bool Raises(std::string_view source)
			{
				Test::ExpectLog raised(LogLevel::Error, "SaveData.");
				return !Session->GetScripts()->Evaluate(source, {}).has_value();
			}
		};

	}

	namespace Test {

		// The SaveData module and Test.SetSaveData in every run mode, for "ScriptApi: every registered function, method,
		// property, operator and constructor has a test": the fixture is no play session, so the module raises and Commit
		// reports why it wrote nothing; the session's behaviour is the cases below.
		void RunSaveDataBindingsCoverage(RunModes mode, std::vector<ScriptApiCoverage>& snapshots)
		{
			Test::ScriptTestFixture fixture({ .Mode = mode, .TestMode = true });
			REQUIRE(fixture.Start());
			const auto result = fixture.Evaluate(R"(
assert(not pcall(SaveData.Get, "Realm"))
assert(not pcall(SaveData.Set, "Realm", 1))
assert(not pcall(SaveData.Delete, "Realm"))
assert(not pcall(SaveData.GetKeys))
assert(not pcall(SaveData.GetLoadError))
local committed, reason = SaveData.Commit()
assert(committed == false and type(reason) == "string")
assert(not pcall(Test.SetSaveData, { Realm = 2 }))
return true
)");
			REQUIRE_MESSAGE(result.has_value(), (result ? "" : result.error().ToString()));
			CHECK(result->Value.Get() == Json(true));
			const auto coverage = fixture.GetApi().GetCoverage(mode);
			REQUIRE(coverage.has_value());
			snapshots.push_back(*coverage);
		}

	}

	TEST_SUITE("Scripting")
	{
		TEST_CASE("SaveData: a session's store is read, written, listed and deleted, and joins the state hash")
		{
			SaveDataSession play;
			play.Start(Json::parse(R"({"Realm": 2})", nullptr, false));
			CHECK(play.Evaluate(R"(return SaveData.Get("Realm"))") == Json(2.0));
			CHECK(play.Evaluate(R"(return SaveData.Get("Missing") == nil)") == Json(true));
			const auto seeded = play.Session->ComputeStateHash();
			CHECK(play.Evaluate(R"(
SaveData.Set("Options", { Volume = 0.5, Keys = { "W", "A" } })
SaveData.Set("Realm", 3)
return SaveData.GetKeys()
)") == Json::array({ "Options", "Realm" }));
			CHECK(play.Session->ComputeStateHash() != seeded);
			CHECK(play.Evaluate(R"(return SaveData.Get("Options").Keys[2])") == Json("A"));
			CHECK(play.Evaluate(R"(return { SaveData.Delete("Realm"), SaveData.Delete("Realm") })") == Json::array({ true, false }));
			CHECK(play.Evaluate(R"(SaveData.Set("Options", nil) return #SaveData.GetKeys())") == Json(0.0));
			CHECK(play.Session->GetSaveData().IsEmpty());
			CHECK(play.Evaluate(R"(return SaveData.GetLoadError() == nil)") == Json(true));
		}

		TEST_CASE("SaveData: the store lasts across Scene.Load and Scene.Reload")
		{
			SaveDataSession play;
			const AssetHandle scene(0x710031);
			const auto document = SceneSerializer::ToJson(play.Fixture.GetScene());
			REQUIRE(document);
			auto data = CreateRef<SceneData>();
			data->Document = CreateRef<const Json>(*document);
			play.Fixture.GetAssetManager().Publish(scene, data, "Assets/Scenes/Saved.scene");
			play.Start(Json::object());
			play.Session->SetExtractionEnabled(false);
			REQUIRE(play.Evaluate(R"(SaveData.Set("Realm", 2) return true)") == Json(true));
			REQUIRE(play.Session->GetScripts()->GetHost().RequestSceneLoad(scene, Json::object()));
			play.Session->Tick();
			REQUIRE(play.Session->GetSceneGeneration() == 2);
			CHECK(play.Evaluate(R"(return SaveData.Get("Realm"))") == Json(2.0));
			REQUIRE(play.Session->GetScripts()->GetHost().RequestSceneReload(std::nullopt));
			play.Session->Tick();
			REQUIRE(play.Session->GetSceneGeneration() == 3);
			CHECK(play.Evaluate(R"(return SaveData.Get("Realm"))") == Json(2.0));
		}

		TEST_CASE("SaveData: keys, values and sizes the store refuses raise and change nothing")
		{
			SaveDataSession play;
			play.Start(Json::object());
			CHECK(play.Raises(R"(SaveData.Set("", 1))"));
			CHECK(play.Raises(R"(SaveData.Set(string.rep("k", 257), 1))"));
			CHECK(play.Raises(R"(SaveData.Get(string.rep("k", 257)))"));
			CHECK(play.Raises(R"(local t = {} t.self = t SaveData.Set("Cycle", t))"));
			CHECK(play.Raises(R"(SaveData.Set("Function", print))"));
			CHECK(play.Raises(R"(SaveData.Set("Mixed", { 1, Two = 2 }))"));
			// The store's cap counts the whole store: a value that fits alone does not fit beside another.
			CHECK(play.Evaluate(R"(SaveData.Set("A", string.rep("x", 600000)) return true)") == Json(true));
			CHECK(play.Raises(R"(SaveData.Set("B", string.rep("x", 600000)))"));
			CHECK(play.Session->GetSaveData().GetKeys() == std::vector<std::string>{ "A" });
		}

		TEST_CASE("SaveData: Commit writes through a live session's host and in memory in a test run")
		{
			SaveDataSession live;
			live.Start(Json::object());
			CHECK(live.Evaluate(R"(SaveData.Set("Best", 12.5) return { SaveData.Commit() })") == Json::array({ true }));
			REQUIRE(live.Host.Commits.size() == 1);
			CHECK(live.Host.Commits[0] == Json::parse(R"({"Best": 12.5})", nullptr, false));
			live.Host.Refusal = "a headless run reads the game's save data and never writes it";
			CHECK(live.Evaluate(R"(return { SaveData.Commit() })") == Json::array({ false, "a headless run reads the game's save data and never writes it" }));

			SaveDataSession test;
			test.Start(Json::object(), true);
			CHECK(test.Evaluate(R"(SaveData.Set("Best", 1) return { SaveData.Commit() })") == Json::array({ true }));
			CHECK(test.Host.Commits.empty());
		}

		TEST_CASE("SaveData: GetLoadError reports why the store the session started with could not be read")
		{
			SaveDataSession play;
			play.Start(Json::object(), false, "Store.json could not be read, so the game started with no save data; it was kept as Store.corrupt-1.json");
			CHECK(play.Evaluate(R"(return SaveData.GetLoadError())") == Json("Store.json could not be read, so the game started with no save data; it was kept as Store.corrupt-1.json"));
			CHECK(play.Evaluate(R"(return #SaveData.GetKeys())") == Json(0.0));
		}

		TEST_CASE("SaveData: a session that starts with save data the store refuses is not created")
		{
			Test::ScriptTestFixture fixture;
			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.SaveData = VariantValue(Json::array());
			CHECK_FALSE(PlaySession::CreateFromScene(specification, fixture.GetScene()).has_value());
			specification.SaveData = VariantValue(Json::parse(R"({"": 1})", nullptr, false));
			CHECK_FALSE(PlaySession::CreateFromScene(specification, fixture.GetScene()).has_value());
		}

		TEST_CASE("SaveData: a host without a play session raises, and Commit says why it wrote nothing")
		{
			Test::ScriptTestFixture fixture;
			REQUIRE(fixture.Start().has_value());
			for (const std::string_view call : { R"(return SaveData.Get("Realm"))", R"(SaveData.Set("Realm", 1))", R"(return SaveData.GetKeys())",
					 R"(return SaveData.GetLoadError())" })
			{
				CAPTURE(std::string(call));
				Test::ExpectLog raised(LogLevel::Error, "this host has no save data");
				CHECK_FALSE(fixture.Evaluate(call).has_value());
			}
			// Commit reports why it wrote nothing instead of raising.
			const auto committed = fixture.Evaluate(R"(return { SaveData.Commit() })");
			REQUIRE(committed.has_value());
			CHECK(committed->Value.Get() == Json::array({ false, "this host has no save data: SaveData works in a play session" }));
		}
	}

}
