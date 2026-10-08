#include "TestsPCH.h"

#include "Engine/Renderer/StaleMirrorSchedule.h"

// When the hosts' stale-mirror collections release what the last frame did not use (Renderer/StaleMirrorSchedule.h;
// Docs/Decisions/0013-m8-decisions.md decisions 7 and 27). Each frame of a host: the collection (TakeReleaseUnused), then
// the render (NoteRendered); a scene change is noted before the frame's collection.

namespace Engine {

	TEST_SUITE("Renderer")
	{
		TEST_CASE("StaleMirrorSchedule: without a scene change no collection releases unused mirrors")
		{
			StaleMirrorSchedule schedule;
			CHECK_FALSE(schedule.IsReleasePending());
			for (int frame = 0; frame < 3; ++frame)
			{
				CHECK_FALSE(schedule.TakeReleaseUnused());
				schedule.NoteRendered();
			}
			CHECK_FALSE(schedule.IsReleasePending());
		}

		TEST_CASE("StaleMirrorSchedule: a scene change releases exactly once, at the first collection after a rendered frame")
		{
			StaleMirrorSchedule schedule;
			schedule.NoteSceneChange();
			CHECK(schedule.IsReleasePending());
			// The frame of the change: its collection runs before its render, so nothing the new scene uses was rendered yet.
			CHECK_FALSE(schedule.TakeReleaseUnused());
			schedule.NoteRendered();
			// The next frame's collection follows the first render of the new scene: the one release.
			CHECK(schedule.TakeReleaseUnused());
			CHECK_FALSE(schedule.IsReleasePending());
			schedule.NoteRendered();
			CHECK_FALSE(schedule.TakeReleaseUnused());
			schedule.NoteRendered();
			CHECK_FALSE(schedule.TakeReleaseUnused());
		}

		TEST_CASE("StaleMirrorSchedule: frames that render nothing keep the release waiting")
		{
			// A minimized window or a failed frame renders nothing: no collection releases until a render happened.
			StaleMirrorSchedule schedule;
			schedule.NoteSceneChange();
			CHECK_FALSE(schedule.TakeReleaseUnused());
			CHECK_FALSE(schedule.TakeReleaseUnused());
			CHECK(schedule.IsReleasePending());
			schedule.NoteRendered();
			CHECK(schedule.TakeReleaseUnused());
		}

		TEST_CASE("StaleMirrorSchedule: a change after a render waits for a render of the newer scene")
		{
			// A render before the change showed the previous scene; it does not count towards the newer scene's release.
			StaleMirrorSchedule schedule;
			schedule.NoteSceneChange();
			schedule.NoteRendered();
			schedule.NoteSceneChange();
			CHECK_FALSE(schedule.TakeReleaseUnused());
			schedule.NoteRendered();
			CHECK(schedule.TakeReleaseUnused());
			CHECK_FALSE(schedule.TakeReleaseUnused());
		}

		TEST_CASE("StaleMirrorSchedule: renders before any change do not count")
		{
			StaleMirrorSchedule schedule;
			schedule.NoteRendered();
			schedule.NoteSceneChange();
			CHECK_FALSE(schedule.TakeReleaseUnused());
			schedule.NoteRendered();
			CHECK(schedule.TakeReleaseUnused());
		}
	}

}
