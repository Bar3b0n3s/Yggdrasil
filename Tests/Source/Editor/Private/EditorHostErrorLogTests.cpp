#include "TestsPCH.h"
#include "Editor/Private/EditorHostErrorLog.h"

#include "Support/ExpectLog.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorHostErrorLog: a failure repeated every frame is logged once and again after a success")
		{
			EditorHostErrorLog channel;
			const Test::ExpectLog logged(LogLevel::Error, "Cannot draw editor UI: ");
			// A selected material whose file no longer parses fails the inspector's Draw on every frame.
			const Status broken = MakeError(ErrorCode::Parse, "material 'Red.mat' is not JSON");
			CHECK(channel.Report("Cannot draw editor UI", broken));
			for (int frame = 1; frame < 60; ++frame)
				CHECK_FALSE(channel.Report("Cannot draw editor UI", broken));
			CHECK(logged.GetMatchCount() == 1);
			CHECK(channel.GetLastError() == broken.error().ToString());
			// Another error is news.
			CHECK(channel.Report("Cannot draw editor UI", MakeError(ErrorCode::Io, "the material could not be read")));
			CHECK(logged.GetMatchCount() == 2);
			// A success re-arms the channel, so the first failure returning later is logged again.
			CHECK_FALSE(channel.Report("Cannot draw editor UI", Status{}));
			CHECK(channel.GetLastError().empty());
			CHECK_FALSE(channel.Report("Cannot draw editor UI", Status{}));
			CHECK(channel.Report("Cannot draw editor UI", broken));
			CHECK(logged.GetMatchCount() == 3);
		}

		TEST_CASE("EditorHostErrorLog: channels are independent and warnings use the warning level")
		{
			EditorHostErrorLog autosave;
			EditorHostErrorLog actions;
			const Test::ExpectLog waits(LogLevel::Warn, "Autosave waits for the source conflict to be resolved: ");
			const Test::ExpectLog action(LogLevel::Error, "Editor UI action: ");
			const Status conflict = MakeError(ErrorCode::Conflict, "scene 'Assets/Scenes/Main.scene' changed after the autosave snapshot was captured");
			for (int frame = 0; frame < 10; ++frame)
			{
				autosave.Report("Autosave waits for the source conflict to be resolved", conflict, EditorHostErrorLevel::Warning);
				actions.Report("Editor UI action", conflict);
			}
			CHECK(waits.GetMatchCount() == 1);
			CHECK(action.GetMatchCount() == 1);
		}
	}

}
