#include "TestsPCH.h"

#include "EditorCore/Commands/CompositeCommand.h"

#include "EditorCore/EditorContext.h"
#include "Support/EditorTestFixture.h"

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
		TEST_CASE("CompositeCommand: a failing child undoes executed children" * doctest::skip(true))
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

		TEST_CASE("CompositeCommand: undo runs the children in reverse order and redo in order" * doctest::skip(true))
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

		TEST_CASE("CompositeCommand: executed children make an applied composite" * doctest::skip(true))
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

		TEST_CASE("CompositeCommand: a failing child Undo executes the undone children again and keeps the composite applied" * doctest::skip(true))
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

		TEST_CASE("CompositeCommand: ChangesScene is true when any child changes the scene" * doctest::skip(true))
		{
			std::vector<std::string> log;
			CompositeCommand settingsOnly("Settings");
			settingsOnly.Add(CreateScope<LoggingCommand>(log, "settings", false, false));
			CHECK_FALSE(settingsOnly.ChangesScene());
			settingsOnly.Add(CreateScope<LoggingCommand>(log, "scene", false, true));
			CHECK(settingsOnly.ChangesScene());
			CHECK(settingsOnly.GetLabel() == "Settings");
		}
	}

}
