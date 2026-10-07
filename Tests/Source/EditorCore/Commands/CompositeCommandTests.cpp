#include "TestsPCH.h"

#include "EditorCore/Commands/CompositeCommand.h"

#include "EditorCore/EditorContext.h"
#include "Engine/Core/Log.h"
#include "Engine/Scene/Scene.h"
#include "Support/EditorTestFixture.h"
#include "Support/ExpectLog.h"

namespace Engine {

	namespace {

		// Appends its name to a log the test owns on Execute and "undo <name>" on Undo; fails when told to.
		class LoggingCommand final : public Command
		{
		public:
			LoggingCommand(std::vector<std::string>& log, std::string name, bool fail = false, bool changesScene = true, bool failUndo = false)
				: m_Log(&log), m_Name(std::move(name)), m_Fail(fail), m_ChangesScene(changesScene), m_FailUndo(failUndo)
			{
			}

			Status Execute(EditorContext& /*context*/) override
			{
				if (m_Fail)
					return MakeError(ErrorCode::Validation, "'{}' fails", m_Name);
				m_Log->push_back(m_Name);
				return {};
			}

			Status Undo(EditorContext& /*context*/) override
			{
				if (m_FailUndo)
					return MakeError(ErrorCode::Io, "undoing '{}' fails", m_Name);
				m_Log->push_back("undo " + m_Name);
				return {};
			}

			Status ReplayOnSceneCopy(Scene& /*scene*/, bool after) const override
			{
				m_Log->push_back((after ? "replay " : "revert ") + m_Name);
				return {};
			}

			std::string_view GetLabel() const override { return m_Name; }
			bool ChangesScene() const override { return m_ChangesScene; }
			size_t GetMemorySize() const override { return 16; }
		private:
			std::vector<std::string>* m_Log = nullptr;
			std::string m_Name;
			bool m_Fail = false;
			bool m_ChangesScene = true;
			bool m_FailUndo = false;
		};

	}

	TEST_SUITE("EditorCore")
	{
		TEST_CASE("CompositeCommand: a failing child undoes executed children")
		{
			Test::EditorTestFixture fixture("CompositeFailure");
			std::vector<std::string> log;
			CompositeCommand composite("Batch");
			composite.Add(CreateScope<LoggingCommand>(log, "a"));
			composite.Add(CreateScope<LoggingCommand>(log, "b"));
			composite.Add(CreateScope<LoggingCommand>(log, "c", true));
			composite.Add(CreateScope<LoggingCommand>(log, "d"));

			const Status status = composite.Execute(fixture.GetEditor());
			REQUIRE_FALSE(status.has_value());
			CHECK(status.error().GetCode() == ErrorCode::Validation);
			CHECK(status.error().ToString().contains("step 2"));
			CHECK(log == std::vector<std::string>{ "a", "b", "undo b", "undo a" });
		}

		TEST_CASE("CompositeCommand: undo runs the children in reverse order and redo in order")
		{
			Test::EditorTestFixture fixture("CompositeUndo");
			std::vector<std::string> log;
			CompositeCommand composite("Batch");
			composite.Add(CreateScope<LoggingCommand>(log, "a"));
			composite.Add(CreateScope<LoggingCommand>(log, "b"));
			REQUIRE(composite.Execute(fixture.GetEditor()).has_value());
			REQUIRE(composite.Undo(fixture.GetEditor()).has_value());
			CHECK(composite.GetAppliedCount() == 0);
			REQUIRE(composite.Execute(fixture.GetEditor()).has_value());
			CHECK(composite.GetAppliedCount() == 2);
			CHECK(log == std::vector<std::string>{ "a", "b", "undo b", "undo a", "a", "b" });
			CHECK(composite.GetChildCount() == 2);
			CHECK(composite.GetChild(1).GetLabel() == "b");
			CHECK(composite.GetMemorySize() == 32);
		}

		TEST_CASE("CompositeCommand: executed children make an applied composite")
		{
			Test::EditorTestFixture fixture("CompositeExecuted");
			std::vector<std::string> log;
			CompositeCommand composite("Transaction");
			CHECK(composite.IsEmpty());
			composite.AddExecuted(CreateScope<LoggingCommand>(log, "a"));
			composite.AddExecuted(CreateScope<LoggingCommand>(log, "b"));
			CHECK(composite.GetAppliedCount() == 2);
			REQUIRE(composite.Execute(fixture.GetEditor()).has_value()); // already applied: nothing runs
			CHECK(log.empty());
			REQUIRE(composite.Undo(fixture.GetEditor()).has_value());
			CHECK(log == std::vector<std::string>{ "undo b", "undo a" });
		}

		TEST_CASE("CompositeCommand: a failing child Undo executes the undone children again and keeps the composite applied")
		{
			Test::EditorTestFixture fixture("CompositeUndoFailure");
			std::vector<std::string> log;
			CompositeCommand composite("Batch");
			composite.Add(CreateScope<LoggingCommand>(log, "a"));
			composite.Add(CreateScope<LoggingCommand>(log, "b", false, true, true));
			composite.Add(CreateScope<LoggingCommand>(log, "c"));
			REQUIRE(composite.Execute(fixture.GetEditor()).has_value());

			const Status undone = composite.Undo(fixture.GetEditor());
			REQUIRE_FALSE(undone.has_value());
			CHECK(undone.error().GetCode() == ErrorCode::Io);
			CHECK(undone.error().ToString().contains("undoing step 1 'b'"));
			CHECK(log == std::vector<std::string>{ "a", "b", "c", "undo c", "c" });
			CHECK(composite.GetAppliedCount() == 3);
		}

		TEST_CASE("CompositeCommand: replaying on a scene copy runs the children in order and reverting in reverse order")
		{
			Test::EditorTestFixture fixture("CompositeReplay");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			std::vector<std::string> log;
			CompositeCommand composite("Batch");
			composite.Add(CreateScope<LoggingCommand>(log, "a"));
			composite.Add(CreateScope<LoggingCommand>(log, "b"));
			REQUIRE(composite.Execute(fixture.GetEditor()).has_value());
			log.clear();

			Scene& scene = fixture.GetEditor().GetScene();
			REQUIRE(composite.ReplayOnSceneCopy(scene, false).has_value());
			REQUIRE(composite.ReplayOnSceneCopy(scene, true).has_value());
			CHECK(log == std::vector<std::string>{ "revert b", "revert a", "replay a", "replay b" });
		}

		TEST_CASE("CompositeCommand: a partly applied composite refuses to replay")
		{
			Test::EditorTestFixture fixture("CompositePartialReplay");
			fixture.CreateAndOpenProject();
			fixture.CreateAndOpenScene();
			std::vector<std::string> log;
			CompositeCommand composite("Batch");
			composite.Add(CreateScope<LoggingCommand>(log, "a"));
			composite.Add(CreateScope<LoggingCommand>(log, "b", false, true, true));
			composite.Add(CreateScope<LoggingCommand>(log, "c", true));
			{
				const Test::ExpectLog expected(LogLevel::Error, "could not be rolled back completely");
				REQUIRE_FALSE(composite.Execute(fixture.GetEditor()).has_value());
			}
			REQUIRE(composite.GetAppliedCount() == 2); // a and b stay applied
			log.clear();

			const Status replayed = composite.ReplayOnSceneCopy(fixture.GetEditor().GetScene(), false);
			REQUIRE_FALSE(replayed.has_value());
			CHECK(replayed.error().GetCode() == ErrorCode::InvalidState);
			CHECK(log.empty());
		}

		TEST_CASE("CompositeCommand: ChangesScene is true when any child changes the scene")
		{
			std::vector<std::string> log;
			CompositeCommand settingsOnly("Settings");
			settingsOnly.Add(CreateScope<LoggingCommand>(log, "settings", false, false));
			CHECK_FALSE(settingsOnly.ChangesScene());
			settingsOnly.Add(CreateScope<LoggingCommand>(log, "scene", false, true));
			CHECK(settingsOnly.ChangesScene());
			CHECK(settingsOnly.GetLabel() == "Settings");
		}

		TEST_CASE("CompositeCommand: a rollback that fails too stops there and reports both failures")
		{
			Test::EditorTestFixture fixture("CompositeDoubleFailure");
			std::vector<std::string> log;
			CompositeCommand composite("Batch");
			composite.Add(CreateScope<LoggingCommand>(log, "a"));
			composite.Add(CreateScope<LoggingCommand>(log, "b", false, true, true)); // its Undo fails
			composite.Add(CreateScope<LoggingCommand>(log, "c", true));              // its Execute fails
			const Test::ExpectLog expected(LogLevel::Error, "could not be rolled back completely");
			const Status status = composite.Execute(fixture.GetEditor());
			REQUIRE_FALSE(status.has_value());
			CHECK(status.error().GetCode() == ErrorCode::Validation);
			CHECK(status.error().ToString().contains("in step 2 'c'"));
			CHECK(status.error().ToString().contains("rolling back step 1 'b' failed too"));
			CHECK(composite.GetAppliedCount() == 2); // a and b stay applied, and the composite knows it
			CHECK(log == std::vector<std::string>{ "a", "b" });
		}
	}

}
