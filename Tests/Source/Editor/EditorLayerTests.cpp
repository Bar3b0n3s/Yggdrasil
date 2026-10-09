#include "TestsPCH.h"
#include "Editor/EditorLayer.h"

#include <doctest/doctest.h>

namespace Engine {

	TEST_SUITE("Editor")
	{
		TEST_CASE("EditorLayer: every architecture panel is available in the dockspace" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorLayer: scene changes on disk offer reload without silently discarding edits" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorLayer: the game panel shows the lockstep owner and releases input on blur" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorLayer: editor.screenshot waits for a frame constructed after the request" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorLayer: minimized screenshot requests produce a fresh offscreen UI frame" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorLayer: scene and game targets retire through ImGui texture registration" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorLayer: hierarchy and inspector actions share automation undo behavior" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorLayer: content deletion uses trash and preserves asset handles" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: implement this behavior before removing the skip");
		}

		TEST_CASE("EditorLayer: accepting recovery uses the injected already-open project service" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: assert the modal survives launcher dismissal, queues once, then installs recovery at the safe point without reopening or releasing the lock");
		}

		TEST_CASE("EditorLayer: declining recovery preserves the scene history and durable generation" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: assert only the offer disappears for this open epoch; no file, history, scene or lock change");
		}

		TEST_CASE("EditorLayer: a queued recovery decision cannot overwrite intervening edits or a new project" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: change revision or project epoch after queuing; assert Conflict or cancellation before adoption and keep the user edits");
		}

		TEST_CASE("EditorViewportHost: a superseded click cannot change selection" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: complete older click after newer one; assert only the newest sequence can apply");
		}

		TEST_CASE("EditorViewportHost: a camera change rejects queued and pending pick results" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: change camera between Draw and submission and again after submission; assert neither stale pick mutates selection");
		}

		TEST_CASE("EditorViewportHost: resize rejects an old image while preserving source frame identity" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: resize after a click; assert FrameIndex and SceneRevision came from that displayed image, never current UI serial or replacement image");
		}

		TEST_CASE("EditorViewportHost: scene replacement and deleted UUIDs discard pick results" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: replace the scene or delete the hit entity before polling; assert no selection change even if its old table index is reused");
		}

		TEST_CASE("EditorViewportHost: a stale miss never clears a newer selection" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: complete an old background hit after a newer successful selection; assert the current selection survives");
		}

		TEST_CASE("EditorViewportHost: ImGuizmo hover and active manipulation suppress picking" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: assert neither hovered nor active gizmos queue or submit a pick");
		}

		TEST_CASE("EditorViewportHost: queued clicks drain only against the exact submitted image and table" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: assert no GPU copy during Draw; after OnRenderSubmitted and renderer OnSubmitted, pair the queued frame, revision, table and generation before overwrite");
		}

		TEST_CASE("EditorViewportHost: forced and skipped UI frames never relabel the rendered picking frame" * doctest::skip(true))
		{
			FAIL("M10 contract scaffold: make UI serial differ from source RenderContext frame; assert click and returned result keep image.FrameIndex and delay checks use RenderContext frame identity rather than the UI serial");
		}
	}

}
