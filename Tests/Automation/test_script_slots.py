"""Multiple scripts per entity through automation (Docs/Decisions/0031-m20-contract.md decisions 17 to 19, S-MULTI)."""

from __future__ import annotations

import unittest

from harness import AutomationTestCase, engine_client

SKIP_SCRIPT_SLOTS = ("contract stub: the Script component as an ordered list of slots "
                     "(ADR 0031 decisions 17 to 19, streams M-A, M-B and M-C)")


def behaviour(name: str) -> str:
    """A behaviour named `name` that records each fixed step as an entity "Trace:<entity>:<name>" and keeps Kind."""
    return f"""local T = {{}}
function T:OnCreate() self.Kind = "{name}" end
function T:OnFixedUpdate() Scene.CreateEntity("Trace:" .. self.Entity.Name .. ":{name}") end
return Script.Define("{name}", T)
"""


class ScriptSlotTests(AutomationTestCase):
    def write_behaviours(self, client: engine_client.EngineClient, *names: str) -> None:
        for name in names:
            client.call("script.write", {"path": f"Assets/Scripts/{name}.luau", "source": behaviour(name)})

    @unittest.skip(SKIP_SCRIPT_SLOTS)
    def test_an_entity_runs_its_scripts_in_slot_order(self) -> None:
        # Decision 17 (Roadmap M20 acceptance "script callback order, slot included"): one entity holds several
        # scripts, each slot its own instance, called in (ExecutionOrder, canonical entity, slot) order; entity.get
        # reports the list.
        client, _ = self.open_editor_with_scene()
        self.write_behaviours(client, "Aim", "Fire")
        client.call("entity.create", {"name": "Turret", "components": {"Script": {"Scripts": [
            {"Script": "Assets/Scripts/Aim.luau"},
            {"Script": "Assets/Scripts/Fire.luau"},
        ]}}})
        client.call("entity.create", {"name": "Early", "components": {"Script": {"Scripts": [
            {"Script": "Assets/Scripts/Fire.luau", "ExecutionOrder": -1},
        ]}}})
        slots = client.call("entity.get", {"entity": "/Turret"})["entity"]["components"]["Script"]["Scripts"]
        self.assertEqual(len(slots), 2)

        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 1})
        traces = client.call("scene.query", {"where": {"name": "Trace:*"}, "target": "play"})["entities"]
        self.assertEqual([entity["name"] for entity in traces],
                         ["Trace:Early:Fire", "Trace:Turret:Aim", "Trace:Turret:Fire"])
        kinds = client.call("script.eval", {"context": "play", "code":
                            "local all = Scene.FindByName('Turret'):GetScripts() return {all[1].Kind, all[2].Kind}"})
        self.assertEqual(kinds["value"], ["Aim", "Fire"])
        client.call("play.stop")

    @unittest.skip(SKIP_SCRIPT_SLOTS)
    def test_a_fault_names_its_slot_and_eval_addresses_a_slot(self) -> None:
        # Decisions 17 and 18 (Roadmap M20 acceptance "a fault stays in its instance"): a faulting slot is disabled
        # alone, script.errors names its slot, and script.eval {entity, script} binds self to the named behaviour's
        # instance. The faulting behaviour fills slot 1, so a slot left at its default (0) fails.
        client, _ = self.open_editor_with_scene()
        self.write_behaviours(client, "Aim")
        client.call("script.write", {"path": "Assets/Scripts/Faulty.luau", "source": (
            'local T = {}\n'
            'function T:OnFixedUpdate() error("slot fault") end\n'
            'return Script.Define("Faulty", T)\n')})
        client.call("entity.create", {"name": "Both", "components": {"Script": {"Scripts": [
            {"Script": "Assets/Scripts/Aim.luau"},
            {"Script": "Assets/Scripts/Faulty.luau"},
        ]}}})
        cursor = client.call("script.errors", {"since": "end"})["nextCursor"]
        client.call("play.start", {"lockstep": True, "pauseOnError": False})
        client.call("play.step", {"ticks": 2})
        errors = client.call("script.errors", {"since": cursor})["errors"]
        self.assertEqual(len(errors), 1, errors)
        self.assertEqual(errors[0]["slot"], 1)
        traces = client.call("scene.query", {"where": {"name": "Trace:Both:Aim"}, "target": "play"})["total"]
        self.assertEqual(traces, 2)
        kind = client.call("script.eval", {"context": "play", "entity": "/Both", "script": "Aim",
                                           "code": "return self.Kind"})
        self.assertEqual(kind["value"], "Aim")
        with self.assertRaises(engine_client.EngineError) as missing:
            client.call("script.eval", {"context": "play", "entity": "/Both", "script": "Missing", "code": "return 1"})
        self.assert_engine_error(missing.exception, engine_client.NOT_FOUND, "NotFound")
        client.call("play.stop")


if __name__ == "__main__":
    unittest.main()
