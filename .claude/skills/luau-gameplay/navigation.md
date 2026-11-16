# Navigation in scripts

A reference page of the `luau-gameplay` skill (`skills/luau-gameplay`): the `Navigation` module, which queries the play session's baked navigation mesh so enemies path around walls, up ramps and away from hazards. The signatures are in `reference/ScriptAPI` (section `Navigation`); how a scene declares and bakes its mesh is `skills/game-building/navigation`.

**What it reads.** The mesh `navigation_bake` baked for the scene's `Navigation` component, one per agent type (`"Small"`, `"Large"`, ...: the names of its `AgentTypes`). The session builds it when it loads the scene, and again at `Scene.Load` and `Scene.Reload`. A scene without a baked mesh (no component, never baked, or a mesh that does not load: `NAV_MESH_NOT_BAKED`) finds nothing: every query returns nil. The components themselves are not visible to scripts; there are no run-time rebakes, crowds or local avoidance, so scripts steer their agents and keep them apart.

**The queries,** each taking an optional agent type name last (default: the component's first agent type; an unknown name raises):
- `Navigation.FindPath(from, to, agent?)` returns the path's corners, a `{vector}` from `from`'s point on the mesh to `to`'s, and whether it reaches `to`. A goal the agent cannot reach (an island, a platform without a ramp) gives the partial path to the reachable point closest to it and `false`; an end more than 2 m from the mesh gives `nil, false`. At most 256 corners: a longer path is cut there and reported as not reaching its goal.
- `Navigation.ClosestPoint(position, maxDistance?, agent?)` returns the surface point nearest to `position` within `maxDistance` metres (default 2), or nil: snap a spawn or a click to the floor an agent can stand on.
- `Navigation.Raycast(from, to, agent?)` walks straight along the surface from `from`'s point toward `to`: `{Clear, Point, Normal, Fraction}`, `Clear` when nothing blocks the walk, else `Point` where it stops (at the agent's clearance from the wall) and the wall's horizontal `Normal`. Use it to tell whether an agent can walk straight to its target, or to skip corners of a path; it is not a line of sight (`Physics.Raycast` is).
- `Navigation.RandomPoint(center?, radius?, agent?, generator?)` returns a random point of the surface, uniform over its area: anywhere, or within `radius` metres of `center` on floor reachable from it (wandering, spawn spots). It draws from `generator` (a `Random.New` stream) or, without one, the session's `Random` stream, like `Random.Number`.

**Determinism.** The queries read only the baked mesh and their arguments and keep no state, so the same calls give the same results in every configuration: call them from `OnFixedUpdate` and a replay of agents following paths verifies at one hash in Debug, Release and Dist. `RandomPoint` without a generator advances the session's stream, which the state hash covers. Re-plan a few times a second rather than every step: 150 agents each finding a path every 24 fixed steps, with their scripts, took about 1.7 ms per step in a Release editor, a ninth of a 64 Hz step (`Perf.py`'s navigation case).

A chaser that finds a path to its target every 20 fixed steps (three times a second at 60 Hz) and walks its corners:

```luau path=Assets/Scripts/Chaser.luau
--!strict
export type Chaser = ScriptInstance & {
	Target: Entity?,
	Agent: string,
	Speed: number,
	Ticks: number,
	Corners: { vector }?,
	Next: number,
}

local Chaser = {}

Chaser.Fields = {
	Target = Field.Entity(),
	Agent = Field.String("Small"),
	Speed = Field.Number(3, { Min = 0 }),
}

function Chaser.OnCreate(self: Chaser)
	self.Ticks = 0
	self.Corners = nil
	self.Next = 2
end

function Chaser.OnFixedUpdate(self: Chaser, dt: number)
	local target = self.Target
	if target == nil then
		return
	end
	if self.Ticks % 20 == 0 then
		-- corners[1] is where the agent stands on the mesh: walk toward corners[2] first.
		self.Corners = Navigation.FindPath(self.Entity.Transform.Translation, target.Transform.Translation, self.Agent)
		self.Next = 2
	end
	self.Ticks += 1
	local corners = self.Corners
	if corners == nil or self.Next > #corners then
		return
	end
	local position = self.Entity.Transform.Translation
	local goal = corners[self.Next]
	local distance = vector.magnitude(goal - position)
	local step = self.Speed * dt
	if distance <= step then
		self.Entity.Transform.Translation = goal
		self.Next += 1
	else
		self.Entity.Transform.Translation = position + (goal - position) * (step / distance)
	end
end

return Script.Define("Chaser", Chaser)
```

**Seeing it.** `engine_call {method: "viewport.setOptions", params: {navMesh: true}}` draws the baked mesh's polygon edges in the scene view, one colour per agent type, and `viewport_screenshot {annotate: {navMesh: true}}` draws them into a capture, in edit and play alike.
