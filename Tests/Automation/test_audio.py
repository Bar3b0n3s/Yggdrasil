"""Audio through automation (Docs/Architecture.md §6.6, §10, §13.5 "audio.stats" and asset.create {type: "SoundEffect"},
§13.7; Roadmap M12 acceptance): sound effects created by agents, voices in lockstep sessions, the audio validation codes
and the built-in presets.

Skipped skeletons of the M12 contract (Docs/Decisions/0015-m12-decisions.md): stream C implements and registers
audio.stats and the SoundEffect asset methods over streams A (AudioEngine) and B (SoundSynth, importers, presets) and
removes the skips. The editors are headless, so their AudioEngine has no device and decodes deterministically (§10.1).
"""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client

# A short sound effect: two notes, 0.15 s, plus the default 10 ms release (7,680 frames at 48 kHz).
LOCK_SOUND = {"Layers": [{"Wave": "Square", "Notes": ["A3:0.05", "E4:0.1"]}]}
LOCK_PATH = "Assets/Audio/Lock.sfx"
# Frames per lockstep tick at the default 60 Hz (§10.1).
FRAMES_PER_TICK = 800


class AudioTests(AutomationTestCase):
    """audio.stats, sound effects and the audio validation codes."""

    def create_speaker(self, client: engine_client.EngineClient, name: str, clip: str, **fields: object) -> None:
        """An entity `name` with a non-spatial AudioSource of `clip` that plays on start, plus `fields`."""
        source = {"Clip": clip, "PlayOnStart": True, "Spatial": False, **fields}
        client.call("entity.create", {"name": name, "components": {"AudioSource": source}})

    @unittest.skip("contract stub: un-skipped by M12 stream C")
    def test_sound_effect_create_and_play_in_lockstep(self) -> None:
        client, _ = self.open_editor_with_scene()
        created = client.call("asset.create", {"type": "SoundEffect", "path": LOCK_PATH, "values": LOCK_SOUND})
        self.assertEqual(created["asset"]["type"], "AudioClip")
        self.assertEqual(created["path"], LOCK_PATH)
        clip = created["asset"]["id"]
        self.create_speaker(client, "Speaker", LOCK_PATH)

        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 2})
        stats = client.call("audio.stats")
        self.assertEqual(stats["timeSource"], "Simulation")
        voices = [voice for voice in stats["voices"] if voice["clip"]["id"] == clip]
        self.assertEqual(len(voices), 1, stats)
        voice = voices[0]
        self.assertEqual(voice["clip"]["path"], LOCK_PATH)
        self.assertEqual(voice["entity"]["name"], "Speaker")
        self.assertEqual(voice["group"], "Sfx")
        self.assertFalse(voice["spatial"])
        # The voice advanced with simulation time: exactly two ticks of frames.
        self.assertEqual(voice["cursorFrames"], 2 * FRAMES_PER_TICK)
        self.assertEqual(voice["lengthFrames"], 7680)

        # The non-looping sound ends on its own and its voice is released.
        client.call("play.step", {"ticks": 30})
        self.assertEqual(client.call("audio.stats")["voiceCount"], 0)
        client.call("play.stop")

    @unittest.skip("contract stub: un-skipped by M12 stream C")
    def test_audio_stats_reports_the_device_and_no_voices_in_edit_mode(self) -> None:
        client, _ = self.open_editor_with_scene()
        stats = client.call("audio.stats")
        # A headless editor's engine has no device and pulls frames itself (§10.1).
        self.assertEqual(stats["deviceState"], "None")
        self.assertEqual(stats["timeSource"], "Host")
        self.assertEqual(stats["sampleRate"], 48000)
        self.assertEqual(stats["voiceCount"], 0)
        self.assertEqual(stats["voiceCapacity"], 64)
        self.assertEqual([group["group"] for group in stats["groups"]], ["Music", "Sfx", "Ui"])
        self.assertEqual(stats["voices"], [])

    @unittest.skip("contract stub: un-skipped by M12 stream C")
    def test_pause_holds_session_voices_and_stop_releases_them(self) -> None:
        client, _ = self.open_editor_with_scene()
        self.create_speaker(client, "Loop", "engine://Audio/Coin", Loop=True)
        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 3})
        cursor = client.call("audio.stats")["voices"][0]["cursorFrames"]
        # The owner's play.pause leaves lockstep and pauses (ADR 0012 decision 8); the session's voices pause with it.
        client.call("play.pause")
        paused = client.call("audio.stats")
        self.assertEqual(paused["voiceCount"], 1)
        self.assertTrue(paused["voices"][0]["paused"])
        self.assertEqual(paused["voices"][0]["cursorFrames"], cursor)
        self.assertEqual(paused["timeSource"], "Host")
        client.call("play.stop")
        self.assertEqual(client.call("audio.stats")["voiceCount"], 0)

    @unittest.skip("contract stub: un-skipped by M12 stream C")
    def test_builtin_sound_effect_presets_play(self) -> None:
        client, _ = self.open_editor_with_scene()
        presets = ("Click", "Blip", "Coin", "Jump", "Hit", "Explosion", "PowerUp", "LineClear", "Win", "Lose")
        for preset in presets:
            self.create_speaker(client, preset, f"engine://Audio/{preset}")
        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 1})
        voices = client.call("audio.stats")["voices"]
        self.assertEqual(sorted(voice["clip"]["path"] for voice in voices),
                         sorted(f"engine://Audio/{preset}" for preset in presets))
        for voice in voices:
            self.assertEqual(voice["clip"]["type"], "AudioClip")
        client.call("play.stop")

    @unittest.skip("contract stub: un-skipped by M12 stream C")
    def test_sound_effect_properties_round_trip_and_undo(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("asset.create", {"type": "SoundEffect", "path": LOCK_PATH, "values": LOCK_SOUND})
        properties = client.call("asset.getProperties", {"asset": LOCK_PATH})["values"]
        self.assertEqual(properties["Volume"], 1.0)
        self.assertEqual(properties["Layers"][0]["Notes"], ["A3:0.05", "E4:0.1"])
        patched = client.call("asset.setProperties", {"asset": LOCK_PATH, "values": {"Volume": 0.5, "Seed": 9}})
        self.assertEqual(patched["values"]["Volume"], 0.5)
        self.assertEqual(patched["values"]["Seed"], 9)
        client.call("edit.undo")
        self.assertEqual(client.call("asset.getProperties", {"asset": LOCK_PATH})["values"]["Volume"], 1.0)
        info = client.call("asset.info", {"asset": LOCK_PATH})
        self.assertEqual(info["importer"], "SoundEffect")
        self.assertEqual(info["diagnostics"], [])

    @unittest.skip("contract stub: un-skipped by M12 stream C")
    def test_sound_effect_create_rejects_invalid_values(self) -> None:
        client, _ = self.open_editor_with_scene()
        invalid = (
            ({"Layers": [{"Notes": ["H4:0.1"]}]}, "/values/Layers/0/Notes/0"),
            ({"Volume": 3, "Layers": [{"Duration": 0.1}]}, "/values/Volume"),
            ({"Layers": [{"Wave": "Sawtooth", "Duration": 0.1}]}, "/values/Layers/0/Wave"),
            ({"Layers": []}, "/values/Layers"),
        )
        for values, pointer in invalid:
            with self.subTest(values=values):
                params = {"type": "SoundEffect", "path": "Assets/Audio/Bad.sfx", "values": values}
                with self.assertRaises(engine_client.EngineError) as raised:
                    client.call("asset.create", params)
                self.assert_engine_error(raised.exception, engine_client.INVALID_PARAMS)
                self.assertEqual(raised.exception.issues[0]["pointer"], pointer)
        self.assertEqual(client.call("asset.list", {"dir": "Assets/Audio"})["assets"], [])

    @unittest.skip("contract stub: un-skipped by M12 stream C")
    def test_validate_reports_audio_listener_codes_and_fix_clears_extra_primaries(self) -> None:
        client, _ = self.open_editor_with_scene()
        client.call("entity.create", {"name": "Speaker", "components": {"AudioSource": {"Spatial": True}}})
        unheard = client.call("project.validate", {"scope": "Scene"})
        self.assertIn("AUDIO_NO_LISTENER", {diagnostic["code"] for diagnostic in unheard["diagnostics"]})

        for name in ("EarA", "EarB"):
            client.call("entity.create", {"name": name, "components": {"AudioListener": {"Primary": True}}})
        report = client.call("project.validate", {"scope": "Scene"})
        by_code = {diagnostic["code"]: diagnostic for diagnostic in report["diagnostics"]}
        self.assertNotIn("AUDIO_NO_LISTENER", by_code)
        self.assertIn("AUDIO_MULTIPLE_PRIMARY_LISTENERS", by_code)
        self.assertEqual(by_code["AUDIO_MULTIPLE_PRIMARY_LISTENERS"]["severity"], "Warning")
        self.assertTrue(by_code["AUDIO_MULTIPLE_PRIMARY_LISTENERS"]["autoFixable"])

        fixed = client.call("project.validate", {"scope": "Scene", "fix": ["AUDIO_MULTIPLE_PRIMARY_LISTENERS"]})
        remaining = {diagnostic["code"] for diagnostic in fixed["diagnostics"]}
        self.assertNotIn("AUDIO_MULTIPLE_PRIMARY_LISTENERS", remaining)
        ear_b = client.call("entity.get", {"entity": "/EarB", "components": ["AudioListener"]})
        self.assertFalse(ear_b["entity"]["components"]["AudioListener"]["Primary"])
        ear_a = client.call("entity.get", {"entity": "/EarA", "components": ["AudioListener"]})
        self.assertTrue(ear_a["entity"]["components"]["AudioListener"]["Primary"])


if __name__ == "__main__":
    unittest.main()
