#include "TestsPCH.h"

#include "EditorCore/Automation/EditorMethodContext.h"

#include "EditorCore/Commands/SceneEdit.h"
#include "Engine/Automation/Protocol/MethodRegistry.h"
#include "Engine/Scene/Entity.h"
#include "Support/AutomationTestClient.h"

namespace Engine {

	// A context for session.info, the simplest method, over the fixture's editor and server.
	static Scope<EditorMethodContext> MakeEditorMethodContext(Test::AutomationFixture& setup)
	{
		const MethodRegistry& methods = setup.GetClient().GetServer().GetMethods();
		const MethodDescriptor* info = methods.Find("session.info");
		REQUIRE(info != nullptr);
		return CreateScope<EditorMethodContext>(setup.GetEditor(), setup.GetClient().GetServer(),
			MethodRequest{ .Info = { .Client = 1, .ClientName = "test", .Id = Json(1), .Method = "session.info", .TranscriptLine = std::nullopt },
				.Options = {},
				.Method = info,
				.Params = Json::object(),
				.Registry = &methods,
				.PhaseMarker = nullptr,
				.NestingDepth = 0 });
	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("EditorMethodContext: entity references resolve by id, unique prefix and path")
		{
			Test::AutomationFixture setup("ContextEntities");
			Scene& scene = setup.GetEditor().GetScene();
			UUID gameId;
			{
				SceneEdit edit(setup.GetEditor(), "Create");
				const Entity game = scene.CreateEntity("Game");
				static_cast<void>(scene.CreateEntity("Board", game));
				static_cast<void>(scene.CreateEntity("Piece", game));
				static_cast<void>(scene.CreateEntity("Piece", game));
				gameId = game.GetUUID();
				REQUIRE(edit.Commit().has_value());
			}
			Scope<EditorMethodContext> context = MakeEditorMethodContext(setup);
			const std::string id = gameId.ToString();

			std::string upper = id;
			std::transform(upper.begin(), upper.end(), upper.begin(), [](char character)
			{
				return static_cast<char>(std::toupper(static_cast<unsigned char>(character)));
			});
			for (const std::string& reference : { id, upper, id.substr(0, 6) })
			{
				INFO(reference);
				const Result<Entity> resolved = context->ResolveEntity(scene, reference, "/entity");
				REQUIRE_MESSAGE(resolved.has_value(), resolved.error().ToString());
				CHECK(resolved->GetUUID() == gameId);
			}
			const Result<Entity> board = context->ResolveEntity(scene, "/Game/Board", "/entity");
			REQUIRE_MESSAGE(board.has_value(), board.error().ToString());
			CHECK(board->GetName() == "Board");
			CHECK(context->ResolveEntity(scene, "/Game/Piece[1]", "/entity").has_value());

			const Result<Entity> ambiguous = context->ResolveEntity(scene, "/Game/Piece", "/entities/1");
			REQUIRE_FALSE(ambiguous.has_value());
			CHECK(ambiguous.error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(ambiguous.error().GetIssues().size() == 2);
			CHECK(ambiguous.error().ToString().contains("/entities/1"));
			CHECK(context->ResolveEntity(scene, "/Gmae", "/entity").error().GetCode() == ErrorCode::NotFound);
			CHECK(context->ResolveEntity(scene, "00000000000000aa", "/entity").error().GetCode() == ErrorCode::NotFound);
			CHECK(context->ResolveEntity(scene, "abc", "/entity").error().GetCode() == ErrorCode::InvalidArgument);
			CHECK(context->ResolveEntity(scene, "Game", "/entity").error().GetCode() == ErrorCode::InvalidArgument);
		}

		TEST_CASE("EditorMethodContext: project paths are confined to project://")
		{
			Test::AutomationFixture setup("ContextPaths", false);
			Scope<EditorMethodContext> context = MakeEditorMethodContext(setup);
			const Result<VfsPath> relative = context->ResolveProjectPath("Assets/Scenes/Main.scene", "/path", ".scene");
			REQUIRE_MESSAGE(relative.has_value(), relative.error().ToString());
			CHECK(relative->ToString() == "project://Assets/Scenes/Main.scene");
			CHECK(context->ResolveProjectPath("project://Assets/Scenes/Main.scene", "/path", ".scene").has_value());
			for (const std::string_view bad : { "", "../Main.scene", "/abs/Main.scene", "C:/Main.scene", "user://Main.scene",
					 "Assets/Scenes/Main.prefab", "Assets/CON.scene" })
			{
				INFO(std::string(bad));
				const Result<VfsPath> path = context->ResolveProjectPath(bad, "/path", ".scene");
				REQUIRE_FALSE(path.has_value());
				CHECK(path.error().GetCode() == ErrorCode::InvalidArgument);
			}
		}

		TEST_CASE("EditorMethodContext: the target scene is the edit scene until play sessions exist")
		{
			Test::AutomationFixture setup("ContextTarget");
			Scope<EditorMethodContext> context = MakeEditorMethodContext(setup);
			const Result<Scene*> implicitTarget = context->ResolveTargetScene(SceneTarget::Edit, false, false);
			REQUIRE_MESSAGE(implicitTarget.has_value(), implicitTarget.error().ToString());
			CHECK(*implicitTarget == &setup.GetEditor().GetScene());
			const Result<Scene*> explicitTarget = context->ResolveTargetScene(SceneTarget::Edit, true, true);
			REQUIRE_MESSAGE(explicitTarget.has_value(), explicitTarget.error().ToString());
			CHECK(*explicitTarget == &setup.GetEditor().GetScene());
			CHECK(context->ResolveTargetScene(SceneTarget::Play, true, false).error().GetCode() == ErrorCode::InvalidState);
			setup.GetEditor().CloseScene();
			CHECK(context->ResolveTargetScene(SceneTarget::Edit, false, false).error().GetCode() == ErrorCode::InvalidState);
		}

		TEST_CASE("EditorMethodContext: entity summaries carry the id, name and path")
		{
			Test::AutomationFixture setup("ContextSummary");
			Entity entity;
			{
				SceneEdit edit(setup.GetEditor(), "Create");
				entity = setup.GetEditor().GetScene().CreateEntity("A/B");
				REQUIRE(edit.Commit().has_value());
			}
			Scope<EditorMethodContext> context = MakeEditorMethodContext(setup);
			const EntitySummary summary = context->MakeEntitySummary(entity);
			CHECK(summary.Id == entity.GetUUID().ToString());
			CHECK(summary.Name == "A/B");
			CHECK(summary.Path == "/A\\/B");
			CHECK(&context->GetEditor() == &setup.GetEditor());
			CHECK(context->GetHostKey() == TypeKeyOf<EditorMethodContext>());
		}
	}

}
