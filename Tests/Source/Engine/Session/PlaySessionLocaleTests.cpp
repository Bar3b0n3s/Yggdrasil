#include "TestsPCH.h"
#include "Engine/Session/PlaySession.h"

#include "Engine/Asset/DocumentData.h"
#include "Engine/Physics/PhysicsTypes.h"
#include "Engine/Scene/Components/ScriptComponent.h"
#include "Engine/Scene/Entity.h"
#include "Engine/Scene/Scene.h"
#include "Engine/Scene/SceneSerializer.h"
#include "Engine/Scripting/ScriptEngine.h"
#include "Support/PhysicsTestScene.h"
#include "Support/ScriptTestFixture.h"

#include <doctest/doctest.h>
#include <glm/glm.hpp>

#include <cstdint>
#include <string>
#include <string_view>
#include <utility>

// The session's locale (M38, Docs/Decisions/0055-m38-runtime-locale.md decisions 1, 3 and 4): session state that
// Loc.SetLocale changes and Scene.Load and Scene.Reload keep, and the state hash's locale block.

namespace Engine {

	namespace {

		void AttachScript(Entity entity, AssetHandle script)
		{
			ScriptSlot slot{};
			slot.Script.SetHandle(script);
			entity.AddComponent<ScriptComponent>(ScriptComponent{ { slot } });
		}

		PlaySessionSpecification MakeLocaleSpecification(Test::ScriptTestFixture& fixture, std::string locale)
		{
			PlaySessionSpecification specification{};
			specification.Registry = &fixture.GetTypes();
			specification.Assets = &fixture.GetAssetManager();
			specification.ScriptSchemas = fixture.GetSchemaSnapshot();
			specification.Seed = 38;
			specification.Serial = 38;
			specification.Project.Localization.Locale = std::move(locale);
			return specification;
		}

		// The JSON value a script evaluation on the entity at `path` returns.
		Json EvaluateOn(PlaySession& session, std::string_view source, std::string_view path)
		{
			const UUID entity = Test::GetEntityId(session.GetScene(), path);
			REQUIRE(entity.IsValid());
			const Result<ScriptEvaluation> evaluated = session.GetScripts()->Evaluate(source, {}, entity);
			REQUIRE_MESSAGE(evaluated.has_value(), (evaluated ? "" : evaluated.error().ToString()));
			return evaluated->Value.Get();
		}

	}

	TEST_SUITE("Session")
	{
		// Roadmap M38's acceptance: the options menu's choice lasts through the realm's scene changes.
		TEST_CASE("PlaySession: the session's locale survives Scene.Load and Scene.Reload")
		{
			Test::ScriptTestFixture fixture;
			const AssetHandle menu(0x38B0001), level(0x38B0002), next(0x38B0003);
			REQUIRE(fixture.AddScript(menu, "Assets/Scripts/Menu.luau", R"(
local M = {}
function M:OnFixedUpdate()
	local tick = Time.GetTick()
	if tick == 3 then Loc.SetLocale("de") end
	if tick == 5 then Scene.Load(Assets.Load("Assets/Scenes/Next.scene")) end
end
return Script.Define("Menu", M)
)"));
			// The loaded scene's behaviour records the locale its VM starts in, and counts OnLocaleChanged, which a new scene's
			// behaviours never receive for the locale they start in.
			REQUIRE(fixture.AddScript(level, "Assets/Scripts/Level.luau", R"(
local L = {}
function L:OnCreate()
	self.CreatedIn = Loc.GetLocale()
	self.Changes = 0
end
function L:OnStart() self.StartedIn = Loc.GetLocale() end
function L:OnLocaleChanged() self.Changes += 1 end
return Script.Define("Level", L)
)"));
			Scene& scene = fixture.GetScene();
			AttachScript(scene.CreateEntity("Level"), level);
			Result<Json> document = SceneSerializer::ToJson(scene);
			REQUIRE(document.has_value());
			auto data = CreateRef<SceneData>();
			data->Document = CreateRef<const Json>(std::move(*document));
			fixture.GetAssetManager().Publish(next, data, "Assets/Scenes/Next.scene");
			scene.DestroyEntity(scene.FindEntityByPath("/Level"));
			AttachScript(scene.CreateEntity("Menu"), menu);

			Result<Scope<PlaySession>> started = PlaySession::CreateFromScene(MakeLocaleSpecification(fixture, "fr"), scene);
			REQUIRE_MESSAGE(started.has_value(), (started ? "" : started.error().ToString()));
			PlaySession& session = **started;
			session.SetExtractionEnabled(false);
			CHECK(session.GetLocale() == "fr");
			CHECK(session.GetStartLocale() == "fr");
			Test::RunTicks(session, 8);
			REQUIRE(session.GetSceneGeneration() == 2);
			REQUIRE(session.GetScriptErrors().GetErrors().empty());
			CHECK(session.GetLocale() == "de");
			CHECK(session.GetStartLocale() == "fr");
			CHECK(session.GetScripts()->GetLocale() == "de");
			CHECK(EvaluateOn(session, "return {self.CreatedIn, self.StartedIn, self.Changes}", "/Level") == Json::array({ "de", "de", 0 }));

			// A reload's VM starts in the session's locale too.
			REQUIRE(session.GetScripts()->Evaluate("Scene.Reload()", {}));
			Test::RunTicks(session, 2);
			REQUIRE(session.GetSceneGeneration() == 3);
			CHECK(session.GetLocale() == "de");
			CHECK(EvaluateOn(session, "return {self.CreatedIn, self.StartedIn, self.Changes}", "/Level") == Json::array({ "de", "de", 0 }));

			// The new scene's behaviours hear a later change.
			REQUIRE(session.GetScripts()->Evaluate("Loc.SetLocale('ar')", {}));
			CHECK(session.GetLocale() == "ar");
			CHECK(EvaluateOn(session, "return {Loc.GetLocale(), self.Changes}", "/Level") == Json::array({ "ar", 1 }));
		}

		// Roadmap M38's acceptance: decision 4's block, after the world-pause block. The start locale is an input, not hashed, so
		// sessions that keep their start locales hash alike whatever they are, and a change hashes only until it is undone.
		TEST_CASE("PlaySession: the locale block is empty while the locale is the start locale")
		{
			Test::ScriptTestFixture fixture;
			Scene& scene = fixture.GetScene();
			static_cast<void>(Test::AddGround(scene));
			static_cast<void>(Test::AddBoxBody(scene, "Box", glm::vec3(0.0f, 2.0f, 0.0f), glm::vec3(0.5f), BodyType::Dynamic));
			const auto start = [&](std::string locale)
			{
				Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(MakeLocaleSpecification(fixture, std::move(locale)), scene);
				REQUIRE_MESSAGE(created.has_value(), (created ? "" : created.error().ToString()));
				(*created)->SetExtractionEnabled(false);
				return std::move(*created);
			};
			Scope<PlaySession> french = start("fr");
			Scope<PlaySession> fallback = start("");
			Scope<PlaySession> changed = start("fr");
			const auto tick = [&]()
			{
				for (PlaySession* session : { french.get(), fallback.get(), changed.get() })
					session->Tick();
			};
			tick();
			// Different start locales, kept: equal hashes.
			CHECK(french->ComputeStateHash() == fallback->ComputeStateHash());
			CHECK(french->ComputeStateHash() == changed->ComputeStateHash());

			// A change to another locale is in the hash at once and at every later tick.
			REQUIRE(changed->GetScripts()->Evaluate("Loc.SetLocale('de')", {}));
			CHECK(changed->ComputeStateHash() != french->ComputeStateHash());
			tick();
			CHECK(changed->ComputeStateHash() != french->ComputeStateHash());
			const uint64_t german = changed->ComputeStateHash();
			REQUIRE(changed->GetScripts()->Evaluate("Loc.SetLocale('ar')", {}));
			CHECK(changed->ComputeStateHash() != german);

			// Back at the start locale, the block is empty again (not sticky).
			REQUIRE(changed->GetScripts()->Evaluate("Loc.SetLocale('fr')", {}));
			CHECK(changed->ComputeStateHash() == french->ComputeStateHash());
			tick();
			CHECK(changed->ComputeStateHash() == french->ComputeStateHash());
			CHECK(changed->ComputeStateHash() == fallback->ComputeStateHash());
		}

		TEST_CASE("PlaySession: a session starts in its project's locale and refuses a locale that is neither empty nor a tag")
		{
			Test::ScriptTestFixture fixture;
			Scene& scene = fixture.GetScene();
			{
				const Result<Scope<PlaySession>> refused = PlaySession::CreateFromScene(MakeLocaleSpecification(fixture, "pt_BR"), scene);
				REQUIRE_FALSE(refused.has_value());
				CHECK(refused.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(refused.error().ToString().contains("Project.Localization.Locale"));
			}
			Result<Scope<PlaySession>> created = PlaySession::CreateFromScene(MakeLocaleSpecification(fixture, "pt-BR"), scene);
			REQUIRE_MESSAGE(created.has_value(), (created ? "" : created.error().ToString()));
			PlaySession& session = **created;
			CHECK(session.GetLocale() == "pt-BR");
			CHECK(session.GetScripts()->GetLocale() == "pt-BR");
			for (const std::string_view invalid : { "pt_BR", "-pt", "1pt", "abcdefghijklmnopqrstuvwxyz0123456789" })
			{
				CAPTURE(invalid);
				const Status status = session.SetLocale(invalid);
				REQUIRE_FALSE(status.has_value());
				CHECK(status.error().GetCode() == ErrorCode::InvalidArgument);
				CHECK(session.GetLocale() == "pt-BR");
			}
			// SetLocale sets the session's and its VM's locale and dispatches nothing (the binding does).
			REQUIRE(session.SetLocale(""));
			CHECK(session.GetLocale() == "");
			CHECK(session.GetStartLocale() == "pt-BR");
			CHECK(session.GetScripts()->GetLocale() == "");
		}
	}

}
