#include "TestsPCH.h"

#include "EditorCore/Commands/Command.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Scene/Scene.h"
#include "Support/EditorTestFixture.h"

namespace Engine {

	namespace {

		// A command that keeps the default ReplayOnSceneCopy.
		class PlainCommand final : public Command
		{
		public:
			explicit PlainCommand(bool changesScene)
				: m_ChangesScene(changesScene)
			{
			}

			Status Execute(EditorContext& /*context*/) override { return {}; }
			Status Undo(EditorContext& /*context*/) override { return {}; }
			std::string_view GetLabel() const override { return "Plain"; }
			bool ChangesScene() const override { return m_ChangesScene; }
			size_t GetMemorySize() const override { return 0; }
		private:
			bool m_ChangesScene = true;
		};

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("CommandOrigin: names both origins")
		{
			CHECK(CommandOriginToString(CommandOrigin::User) == "User");
			CHECK(CommandOriginToString(CommandOrigin::Agent) == "Agent");
		}

		TEST_CASE("Command: the default replay changes nothing for settings commands and is Unsupported for scene commands")
		{
			Test::EditorTestFixture fixture("CommandReplay");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			Scene& scene = fixture.GetEditor().GetScene();
			const uint64_t revision = scene.GetRevision();

			const PlainCommand settings(false);
			CHECK(settings.ReplayOnSceneCopy(scene, true).has_value());
			CHECK(settings.ReplayOnSceneCopy(scene, false).has_value());

			const PlainCommand edit(true);
			const Status replayed = edit.ReplayOnSceneCopy(scene, false);
			REQUIRE_FALSE(replayed.has_value());
			CHECK(replayed.error().GetCode() == ErrorCode::Unsupported);
			CHECK(replayed.error().ToString().contains("Plain"));
			CHECK(scene.GetRevision() == revision);
		}
	}

}
