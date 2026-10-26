"""M19 native gameplay modules (Docs/Decisions/0030-m19-contract.md decisions 26 to 35, C-NATIVE-STATIC and
C-NATIVE-DLL): the native.* methods against real builds of a small module through Scripts/BuildNative.py, on
Windows. The user's "Allow native code" preference is written into each test's own user-data directory before its
editor starts, as the games suite does: the test harness's own file, never a method (decision 29).
"""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path

from harness import EXIT_SUCCESS, REPOSITORY_ROOT, AutomationTestCase, app_name, automation_config, engine_client

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

from Lib import native_build  # noqa: E402  (Scripts/ is on sys.path above)

SPINNER_SOURCE = """#include "Engine/Native/NativeApi.h"

namespace Spinners {

	struct Spinner
	{
		static constexpr std::string_view NativeTypeName = "Spinner";
		float DegreesPerSecond = 90.0f;
		glm::vec3 Axis = glm::vec3(0.0f, 1.0f, 0.0f);
	};

	static void Spin(Engine::INativeStepContext& context)
	{
		for (const Engine::NativeEntity entity : context.Query<Spinner>())
		{
			const Spinner* spinner = context.Get<Spinner>(entity);
			Engine::NativeTransform transform{};
			if (spinner == nullptr || !context.GetTransform(entity, transform))
				continue;
			const float seconds = static_cast<float>(context.GetFixedDelta());
			const float radians = spinner->DegreesPerSecond * 0.017453292f * seconds;
			transform.Rotation = context.AngleAxis(radians, spinner->Axis) * transform.Rotation;
			static_cast<void>(context.SetTransform(entity, transform));
		}
	}

	static void Register(Engine::INativeRegistrar& registrar)
	{
		registrar.Add(Engine::NativeComponent<Spinner>("Turns its entity about an axis.")
				.Field("DegreesPerSecond", &Spinner::DegreesPerSecond, "How fast it turns.", { .Unit = "deg/s" })
				.Field("Axis", &Spinner::Axis, "The local axis it turns about."));
		registrar.AddSystem({ .Name = "Spin", .Description = "Turns every Spinner.", .Run = &Spin });
	}

}

ENGINE_NATIVE_MODULE(Spinners::Register)
"""

# The same module after a change of layout: DegreesPerSecond renamed Speed, which RenamedFrom carries.
RENAMED_SOURCE = SPINNER_SOURCE.replace(
    "float DegreesPerSecond = 90.0f;", "float Speed = 90.0f;").replace(
    "spinner->DegreesPerSecond", "spinner->Speed").replace(
    '.Field("DegreesPerSecond", &Spinner::DegreesPerSecond, "How fast it turns.", { .Unit = "deg/s" })',
    '.Field("Speed", &Spinner::Speed, "How fast it turns.", { .Unit = "deg/s", .RenamedFrom = "DegreesPerSecond" })')


class NativeModuleTests(AutomationTestCase):
    def allow_native_code(self) -> None:
        """The user's preference, written into this test's user data before the editor starts (the harness's file, in
        the format of user://Editor.json: EditorCore/EditorPreferences.h)."""
        path = self.user_data / app_name() / "Editor.json"
        path.parent.mkdir(parents=True, exist_ok=True)
        document = {"Format": "EditorPreferences", "Version": 1, "AllowNativeCode": True}
        path.write_text(json.dumps(document, indent="\t") + "\n", encoding="utf-8")

    def build(self, client: engine_client.EngineClient) -> dict:
        """native.build, which must succeed; its result."""
        built = client.call("native.build")
        self.assertTrue(built["succeeded"], built["diagnostics"])
        return built

    def open_native_project(self) -> tuple[engine_client.EngineClient, Path]:
        client, root = self.open_editor_with_scene("NativeGame")
        client.call("native.write", {"path": "Native/Spinners.cpp", "source": SPINNER_SOURCE})
        return client, root

    def test_build_and_load_are_refused_while_the_preference_is_off_and_no_method_sets_it(self) -> None:
        # Decision 29: native code runs only with the user's permission, which automation can neither give nor reach.
        client, _ = self.open_native_project()
        self.assertFalse(client.call("native.status")["allowed"])
        for method in ("native.build", "native.load"):
            with self.assertRaises(engine_client.EngineError) as refused:
                client.call(method)
            self.assertEqual(refused.exception.error_code, "PermissionDenied", method)
        discovered = json.dumps(client.call("rpc.discover", {}))
        self.assertNotIn("allowNativeCode", discovered)
        self.assertNotIn("AllowNativeCode", discovered)
        # The .eproj has no such member: a non-strict editor drops it as an unknown member (a --strict one refuses it),
        # and the project's settings never hold it.
        try:
            client.call("project.setSettings", {"patch": {"AllowNativeCode": True}})
        except engine_client.EngineError:
            pass
        self.assertNotIn("AllowNativeCode", json.dumps(client.call("project.getSettings")))
        self.assertFalse(client.call("native.status")["allowed"])
        # The editor's own writes of user://Editor.json (the recent projects) never add the preference.
        preferences = self.user_data / app_name() / "Editor.json"
        if preferences.exists():
            self.assertNotIn("AllowNativeCode", preferences.read_text(encoding="utf-8"))

    def test_module_builds_loads_and_its_systems_run_in_play(self) -> None:
        # Decisions 30 to 32: with the preference on, native.build compiles the module with the first-party settings,
        # native.load registers it, native.setComponents puts its component on an entity, and in play its system turns
        # that entity every tick; the source hash the editor reports is the one Scripts/Lib/native_build.py computes.
        self.allow_native_code()
        client, root = self.open_native_project()
        status = client.call("native.status")
        self.assertTrue(status["allowed"])
        self.assertEqual(status["sources"], ["Native/Spinners.cpp"])
        api_header = REPOSITORY_ROOT / native_build.API_HEADER
        self.assertEqual(status["sourceHash"], native_build.source_hash(root, automation_config(), api_header))
        built = client.call("native.build")
        self.assertTrue(built["succeeded"], built["diagnostics"])
        self.assertTrue((root / built["library"]).is_file())
        loaded = client.call("native.load")
        self.assertEqual([component["registryName"] for component in loaded["components"]], ["Native.Spinner"])
        self.assertEqual([system["name"] for system in loaded["systems"]], ["Spin"])
        client.call("entity.create", {"name": "Wheel"})
        client.call("native.setComponents",
                    {"entity": "/Wheel", "components": {"Native.Spinner": {"DegreesPerSecond": 180}}})
        client.call("play.start", {"mode": "Play", "lockstep": True})
        before = client.call("entity.get", {"entity": "/Wheel"})["entity"]["components"]["Transform"]["Rotation"]
        client.call("play.step", {"ticks": 30})
        after = client.call("entity.get", {"entity": "/Wheel"})["entity"]["components"]["Transform"]["Rotation"]
        self.assertNotEqual(before, after)
        client.call("play.stop")

    def test_reload_keeps_an_unchanged_scene_and_migrates_a_changed_layout(self) -> None:
        # Decision 31 (the milestone's acceptance): reloading an unchanged module leaves the scene byte-identical, and a
        # reload with a changed layout migrates its native components by field name.
        self.allow_native_code()
        client, root = self.open_native_project()
        self.build(client)
        client.call("native.load")
        client.call("entity.create", {"name": "Wheel"})
        client.call("native.setComponents",
                    {"entity": "/Wheel", "components": {"Native.Spinner": {"DegreesPerSecond": 30}}})
        client.call("scene.save")
        saved = (root / "Assets/Scenes/Main.scene").read_bytes()
        self.build(client)
        unchanged = client.call("native.load")
        self.assertTrue(unchanged["reloaded"])
        self.assertFalse(unchanged["sceneChanged"])
        client.call("scene.save")
        self.assertEqual((root / "Assets/Scenes/Main.scene").read_bytes(), saved)

        client.call("native.write", {"path": "Native/Spinners.cpp", "source": RENAMED_SOURCE})
        self.assertFalse(client.call("native.status")["loadedCurrent"])
        self.build(client)
        changed = client.call("native.load")
        self.assertTrue(changed["layoutChanged"])
        self.assertTrue(changed["sceneChanged"])
        components = client.call("native.getComponents", {"entity": "/Wheel"})["components"]
        self.assertEqual(components["Native.Spinner"]["Speed"], 30)

    def test_native_sources_are_written_with_provenance_and_read_back(self) -> None:
        # Decisions 32 and 35: a builder writes the module's sources through automation, so the provenance audit covers
        # them like every authored file.
        client, root = self.open_native_project()
        self.assertEqual(client.call("native.read", {"path": "Native/Spinners.cpp"})["source"], SPINNER_SOURCE)
        provenance = json.loads((root / "Automation" / "Provenance.json").read_text(encoding="utf-8"))
        entries = {entry["Path"]: entry for entry in provenance["Entries"]}
        self.assertEqual(entries["Native/Spinners.cpp"]["Method"], "native.write")
        for refused in ("Assets/Spinners.cpp", "Native/Notes.txt", "Native/../Assets/Spinners.cpp"):
            with self.assertRaises(engine_client.EngineError) as invalid:
                client.call("native.write", {"path": refused, "source": ""})
            self.assertEqual(invalid.exception.error_code, "InvalidArgument", refused)

    def test_export_of_a_native_game_links_its_module_and_runs_it(self) -> None:
        # Decision 33: the export of a game with Native/ builds its Runtime with the module linked; the smoke test then
        # runs the exported game, and in the exported game's own session the module's system turns the Wheel, with no
        # library loaded (the Runtime's automation subset reads its rotation, as test_runtime.py reads an exported
        # game).
        self.allow_native_code()
        client, _ = self.open_native_project()
        client.call("native.build")
        client.call("native.load")
        client.call("entity.create", {"name": "Wheel"})
        client.call("native.setComponents", {"entity": "/Wheel", "components": {"Native.Spinner": {}}})
        client.call("scene.save")
        client.call("project.setSettings", {"patch": {"StartScene": "Assets/Scenes/Main.scene",
                                                      "Export": {"BuildScenes": ["Assets/Scenes/Main.scene"]}}})
        exported = client.call("project.export", {"config": "Release", "smokeTest": True})
        self.assertTrue(exported["nativeModule"])
        self.assertIn("NativeBuild", [timing["phase"] for timing in exported["phaseTimings"]])
        self.assertEqual(exported["smokeTestExitCode"], 0)

        arguments = ["--headless", "--automation", "--paused", "--renderer", "none"]
        game = engine_client.launch_editor(Path(exported["executable"]), arguments, self.directory / "ExportedGame",
                                           "NativeGame")
        self.editors.append(game)
        played = self.connect(game)

        def wheel_rotation() -> list[float]:
            entity = played.call("entity.get", {"entity": "/Wheel", "components": ["Transform"]})["entity"]
            return entity["components"]["Transform"]["Rotation"]

        before = wheel_rotation()
        played.call("play.step", {"ticks": 30, "render": "none"})
        self.assertNotEqual(wheel_rotation(), before)
        played.call("session.shutdown")
        self.assertEqual(game.wait(), EXIT_SUCCESS, game.output())


if __name__ == "__main__":
    unittest.main()
