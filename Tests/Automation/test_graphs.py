"""Visual-scripting graphs through automation: graph.create, graph.read, graph.nodes and the graph edits, a graph
behaviour in play, and its live values (Docs/Decisions/0035-m24-contract.md decisions 12 to 14; Roadmap M24, C-VS)."""

from __future__ import annotations

import json
import sys
import unittest
from pathlib import Path
from typing import Any

from harness import REPOSITORY_ROOT, AutomationTestCase, engine_client

sys.path.insert(0, str(REPOSITORY_ROOT / "Scripts"))

from Lib import xxh64  # noqa: E402  (Scripts/ is on sys.path above)

GRAPH = "Assets/Scripts/Counter.graph"
REPLAY = "Assets/Tests/Replays/Counter.replay"
# Past the first state-hash checkpoint (ReplayCheckpointInterval, 60 ticks), so the replay compares one on the way.
LIVE_TICKS = 61


def provenance_entry(root: Path, path: str) -> dict[str, Any]:
    """The project's provenance entry of `path`."""
    provenance = json.loads((root / "Automation" / "Provenance.json").read_text(encoding="utf-8"))
    return {entry["Path"]: entry for entry in provenance["Entries"]}[path]


def pin(node: int, name: str) -> dict[str, Any]:
    return {"Node": node, "Pin": name}


class GraphTests(AutomationTestCase):
    def build_counter(self, client: engine_client.EngineClient, moves: bool = False) -> dict[str, int]:
        """The Counter behaviour, built through the graph methods alone: each fixed step adds its Step field to its
        Count variable and, with `moves`, then moves its entity to x = Count, so the state hash (which covers the
        Transform but no script table) sees every Count. Returns the node ids by role."""
        created = client.call("graph.create", {"path": GRAPH})
        self.assertEqual(created["name"], "Counter")
        self.assertEqual(created["graph"]["type"], "Script")
        number = {"Kind": "Number", "Element": "Number", "AssetType": "", "EnumValues": []}
        client.call("graph.setMembers", {"graph": GRAPH,
                                          "fields": [{"Name": "Step", "Default": 1, "Options": {"Min": 0}, **number}],
                                          "variables": [{"Name": "Count", "Default": 0, **number}]})
        nodes = {}
        for role, kind in (("step", "Event.OnFixedUpdate"), ("count", "Self.Get.Count"), ("increment", "Self.Get.Step"),
                           ("add", "Number.Add"), ("store", "Self.Set.Count")):
            nodes[role] = client.call("graph.addNode", {"graph": GRAPH, "kind": kind})["node"]
        for source, target in (((nodes["step"], "Then"), (nodes["store"], "In")),
                               ((nodes["count"], "Value"), (nodes["add"], "A")),
                               ((nodes["increment"], "Value"), (nodes["add"], "B")),
                               ((nodes["add"], "Result"), (nodes["store"], "Value"))):
            client.call("graph.connect", {"graph": GRAPH, "from": pin(*source), "to": pin(*target)})
        if not moves:
            return nodes
        for role, kind in (("self", "Self.Entity"), ("transform", "Get.Entity.Transform"),
                           ("counted", "Self.Get.Count"), ("translate", "Set.Transform.Translation")):
            nodes[role] = client.call("graph.addNode", {"graph": GRAPH, "kind": kind})["node"]
        nodes["place"] = client.call("graph.addNode", {"graph": GRAPH, "kind": "Vector.Make",
                                                       "values": {"Y": 0, "Z": 0}})["node"]
        for source, target in (((nodes["store"], "Then"), (nodes["translate"], "In")),
                               ((nodes["self"], "Value"), (nodes["transform"], "self")),
                               ((nodes["transform"], "Value"), (nodes["translate"], "self")),
                               ((nodes["counted"], "Value"), (nodes["place"], "X")),
                               ((nodes["place"], "Result"), (nodes["translate"], "Value"))):
            client.call("graph.connect", {"graph": GRAPH, "from": pin(*source), "to": pin(*target)})
        return nodes

    def test_an_agent_builds_a_graph_behaviour_with_the_graph_methods_and_it_runs_in_play(self) -> None:
        # Roadmap M24: "an agent builds a graph through MCP only" (the bridge's tools are these methods; the MCP half is
        # Tools/MCP/tests/test_conformance.py's) and "every editor graph action has its method".
        client, _ = self.open_editor_with_scene()
        self.build_counter(client)
        read = client.call("graph.read", {"graph": GRAPH, "luau": True})
        self.assertEqual(read["diagnostics"], [])
        self.assertEqual(len(read["document"]["Nodes"]), 5)
        self.assertIn('return Script.Define("Counter", Graph)', read["luau"])
        self.assertEqual(len(read["lineNodes"]), read["luau"].count("\n"))
        flow = client.call("graph.nodes", {"prefix": "Flow."})["nodes"]
        self.assertEqual([node["kind"] for node in flow], ["Flow.Branch", "Flow.Delay", "Flow.ForEach", "Flow.ForRange",
                                                           "Flow.IfNotNil", "Flow.Sequence"])

        # The graph is a Script behaviour: a script slot holds it, and it runs in play.
        client.call("entity.create", {"name": "Counter", "components": {
            "Script": {"Scripts": [{"Script": GRAPH, "Fields": {"Step": 2}, "ExecutionOrder": 0}]}}})
        client.call("play.start", {"lockstep": True})
        client.call("play.step", {"ticks": 3})
        count = client.call("script.eval", {"context": "play", "entity": "/Counter", "code": "return self.Count"})
        self.assertEqual(count["value"], 6)
        client.call("play.stop")

        # A refused edit writes nothing.
        before = client.call("graph.read", {"graph": GRAPH})["document"]
        with self.assertRaises(engine_client.EngineError) as unknown:
            client.call("graph.addNode", {"graph": GRAPH, "kind": "Flow.Nope"})
        self.assertEqual(unknown.exception.error_code, "InvalidArgument")
        self.assertEqual(client.call("graph.read", {"graph": GRAPH})["document"], before)

    def test_graph_edits_are_undoable_atomic_in_a_batch_and_recorded_in_provenance(self) -> None:
        client, root = self.open_editor_with_scene()
        nodes = self.build_counter(client)
        path = root / "Assets" / "Scripts" / "Counter.graph"
        built = path.read_bytes()
        moved = client.call("graph.moveNodes", {"graph": GRAPH, "positions": [
            {"node": nodes["add"], "position": [320, 64]}]})
        self.assertGreater(moved["undoIndex"], 0)
        self.assertNotEqual(path.read_bytes(), built)
        client.call("edit.undo")
        self.assertEqual(path.read_bytes(), built)

        # An edit.batch builds atomically, naming an earlier op's node; one undo takes it back.
        client.call("edit.batch", {"label": "Log the count", "ops": [
            {"method": "graph.addNode", "name": "log", "params": {"graph": GRAPH, "kind": "Call.Log.Info"}},
            {"method": "graph.connect", "params": {"graph": GRAPH, "from": pin(nodes["store"], "Then"),
                                                   "to": {"Node": {"$ref": "log.node"}, "Pin": "In"}}}]})
        self.assertEqual(len(client.call("graph.read", {"graph": GRAPH})["document"]["Nodes"]), 6)
        client.call("edit.undo")
        self.assertEqual(path.read_bytes(), built)

        # The other edits (gate 5 counts every graph method here): the Add's B input loses its link and takes a literal
        # instead, and the Step node, used by nothing then, is removed; three undo steps take it all back.
        client.call("graph.disconnect", {"graph": GRAPH, "from": pin(nodes["increment"], "Value"),
                                         "to": pin(nodes["add"], "B")})
        client.call("graph.setValues", {"graph": GRAPH, "values": [{"node": nodes["add"], "pin": "B", "value": 5}]})
        # Every write of the graph is recorded with the method that made it, like every write.
        entry = provenance_entry(root, GRAPH)
        self.assertEqual(entry["Method"], "graph.setValues")
        self.assertEqual(entry["XXH64"].lower(), xxh64.xxh64_hex(path.read_bytes()))
        removed = client.call("graph.removeNodes", {"graph": GRAPH, "nodes": [nodes["increment"]]})
        self.assertEqual(removed["removedLinks"], [])
        self.assertEqual(client.call("graph.read", {"graph": GRAPH})["diagnostics"], [])
        client.call("edit.undo", {"steps": 3})
        self.assertEqual(path.read_bytes(), built)
        # An undo writes the graph too, so the last write's entry is edit.undo's, with the restored bytes' hash.
        entry = provenance_entry(root, GRAPH)
        self.assertEqual(entry["Method"], "edit.undo")
        self.assertEqual(entry["XXH64"].lower(), xxh64.xxh64_hex(built))

    def test_live_values_are_read_in_play_without_changing_the_state_hash(self) -> None:
        # Roadmap M24: "live values show in play without changing the state hash" (Docs/Decisions/0035-m24-contract.md
        # decision 14). A recorded lockstep play reads the graph's live values after every tick; its replay without the
        # reads reproduces every checkpoint and the final hash (input.record stop's verification, then input.replay),
        # and a plain play from the same seed gives the same hash at every tick. The Counter moves its entity by its
        # Count, so a read that changed the instance would change the hashes; they are compared before the values read.
        client, _ = self.open_editor_with_scene()
        nodes = self.build_counter(client, moves=True)
        client.call("entity.create", {"name": "Counter", "components": {
            "Script": {"Scripts": [{"Script": GRAPH, "Fields": {}, "ExecutionOrder": 0}]}}})
        client.call("scene.save")

        def run(read_live_values: bool) -> tuple[list[str], list[dict[str, Any]]]:
            """The state hash after each of LIVE_TICKS ticks, and the live values read after each tick when asked."""
            hashes = []
            reads = []
            for _ in range(LIVE_TICKS):
                hashes.append(client.call("play.step", {"ticks": 1})["stateHash"])
                if read_live_values:
                    reads.append(client.call("graph.liveValues", {"entity": "/Counter"}))
            return hashes, reads

        client.call("input.record", {"action": "start", "seed": 11})
        shown, reads = run(True)
        saved = client.call("input.record", {"action": "stop", "path": REPLAY})
        self.assertEqual(saved["stateHash"], shown[-1])
        self.assertGreater(saved["checkpoints"], 0)
        client.call("play.stop")
        replayed = client.call("input.replay", {"path": REPLAY, "verify": True, "strictHash": True})
        self.assertTrue(replayed["passed"], replayed)
        self.assertEqual(replayed["stateHash"], shown[-1])
        self.assertEqual(replayed["checkpointsChecked"], saved["checkpoints"])
        self.assertFalse(replayed["checkpointFailed"], replayed)
        client.call("play.stop")

        client.call("play.start", {"lockstep": True, "seed": 11})
        hidden, _ = run(False)
        self.assertEqual(hidden, shown)
        client.call("play.stop")

        for tick, live in enumerate(reads, start=1):
            self.assertEqual(live["graph"]["path"], GRAPH)
            self.assertEqual((live["tick"], live["slot"]), (tick, 0))
            values = {(value["node"], value["pin"]): value["value"] for value in live["values"]}
            # Tick t read a Count of t - 1 and a Step of 1, stored their sum and moved the entity to x = t.
            self.assertEqual(values[(nodes["count"], "Value")], tick - 1)
            self.assertEqual(values[(nodes["increment"], "Value")], 1)
            self.assertEqual(values[(nodes["add"], "Result")], tick)
            self.assertEqual(values[(nodes["counted"], "Value")], tick)
            self.assertEqual(values[(nodes["place"], "Result")], [tick, 0, 0])
            members = {member["name"]: member["value"] for member in live["members"]}
            self.assertEqual(members, {"Count": tick, "Step": 1})
            self.assertEqual(live["errors"], [])
        with self.assertRaises(engine_client.EngineError) as stopped:
            client.call("graph.liveValues", {"entity": "/Counter"})
        self.assertEqual(stopped.exception.error_code, "InvalidState")


if __name__ == "__main__":
    unittest.main()
