--!strict
-- Every lifecycle callback of Callbacks.scene fires in the order Architecture §5.7 and §11.3 document, including
-- OnEnable/OnDisable, collision and trigger enter/exit with the exits synthesized on destruction and deactivation, and
-- OnDestroy. The cases share one session (Isolation "Suite"), so they run in order on one timeline.
local Journal = require("../../Scripts/Callbacks/Journal")

-- The Recorder labels in (ExecutionOrder, canonical entity order): Early has -5, Late 7, the rest 0 in scene order.
local ORDER = { "Early", "Root", "Child", "Floor", "Dropper", "Faller", "Zone", "Visitor", "Lingerer", "Late" }

local function Expand(event: string, labels: { string }?): { string }
	local result: { string } = {}
	for _, label in labels or ORDER do
		table.insert(result, label .. ":" .. event)
	end
	return result
end

local function Concat(...: { string }): { string }
	local result: { string } = {}
	for _, list in { ... } do
		for _, entry in list do
			table.insert(result, entry)
		end
	end
	return result
end

local function ExpectSequence(actual: { string }, expected: { string }, what: string)
	Test.ExpectEqual(#actual, #expected, what .. ": entry count")
	for index = 1, math.max(#actual, #expected) do
		Test.ExpectEqual(actual[index], expected[index], what .. ": entry " .. index)
	end
end

local function IsPhase(entry: string): boolean
	return string.find(entry, ":fixed$") ~= nil or string.find(entry, ":update$") ~= nil or string.find(entry, ":late$") ~= nil
end

local function Of(label: string): (string) -> boolean
	return function(entry: string): boolean
		return string.sub(entry, 1, #label + 1) == label .. ":"
	end
end

local function WaitFor(cursor: number, entry: string, ticks: number)
	Test.WaitUntil(function(): boolean
		return Journal.Has(cursor, entry)
	end, ticks)
end

local function Body(name: string): RigidBody
	local entity = Scene.FindByName(name)
	assert(entity ~= nil, name .. " exists")
	return entity.RigidBody :: RigidBody
end

return Test.Suite("Callbacks", function()
	Test.Case("every instance is created before any starts and all start before the first fixed step", function()
		local expected = Concat(Expand("create"), Expand("start"), Expand("fixed"))
		ExpectSequence(Journal.Since(0), expected, "session start")
		Test.ExpectEqual(Time.GetTick(), 0)
	end)

	Test.Case("each tick runs the frame's update and late phases and then the next fixed step in execution order", function()
		local cursor = Journal.Count()
		Test.WaitTicks(1)
		ExpectSequence(Journal.Since(cursor, IsPhase), Concat(Expand("update"), Expand("late"), Expand("fixed")), "one tick")
		cursor = Journal.Count()
		Test.WaitTicks(2)
		local two = Journal.Since(cursor, IsPhase)
		local cycle = Concat(Expand("update"), Expand("late"), Expand("fixed"))
		ExpectSequence(two, Concat(cycle, cycle), "two ticks")
	end)

	Test.Case("a body that starts inside a trigger enters it at the first step", function()
		Test.Expect(Journal.Has(0, "Zone:trigger-enter:Lingerer"), "the zone saw the lingerer enter")
		Test.Expect(Journal.Has(0, "Lingerer:trigger-enter:Zone"), "the lingerer saw itself enter the zone")
		-- Both sides at the first fixed step, tick 0, the step in which the session's instances first ran OnFixedUpdate.
		local first = Journal.Ticks["Lingerer:fixed"]
		Test.ExpectEqual(first, 0, "the first fixed step is tick 0")
		Test.ExpectEqual(Journal.Ticks["Zone:trigger-enter:Lingerer"], first, "the zone's enter arrives at the first step")
		Test.ExpectEqual(Journal.Ticks["Lingerer:trigger-enter:Zone"], first, "the lingerer's enter arrives at the first step")
		Test.ExpectEqual(#Journal.Since(0, Of("Lingerer")) > 0, true)
	end)

	Test.Case("a body falling through a trigger enters and exits it once on both sides", function()
		WaitFor(0, "Visitor:trigger-exit:Zone", 240)
		local visitor = Journal.Since(0, function(entry: string): boolean
			return string.find(entry, "trigger", 1, true) ~= nil and string.find(entry, "Visitor", 1, true) ~= nil
		end)
		ExpectSequence(visitor, { "Zone:trigger-enter:Visitor", "Visitor:trigger-enter:Zone", "Zone:trigger-exit:Visitor", "Visitor:trigger-exit:Zone" }, "visitor")
	end)

	Test.Case("a landing body and the floor receive OnCollisionEnter with mirrored contacts", function()
		WaitFor(0, "Dropper:collision-enter:Floor", 240)
		Test.Expect(Journal.Has(0, "Floor:collision-enter:Dropper"), "the floor saw the dropper land")
		local dropper = Scene.FindByName("Dropper") :: Entity
		local floor = Scene.FindByName("Floor") :: Entity
		local mine = Journal.Contacts["Dropper"]
		local theirs = Journal.Contacts["Floor"]
		Test.Expect(mine ~= nil and theirs ~= nil, "both contacts recorded")
		Test.ExpectEqual(mine.Collider, dropper)
		Test.ExpectEqual(mine.OtherCollider, floor)
		Test.ExpectNear(math.abs(mine.Normal.y), 1, 0.001, "the contact normal is vertical")
		Test.ExpectNear(mine.Normal, -theirs.Normal, 0.0001, "the two sides see opposite normals")
		Test.ExpectNear(mine.Point.y, 0, 0.05, "the contact lies on the floor")
		Test.Expect(mine.RelativeSpeed > 0, "the bodies approached")
		Test.ExpectNear(mine.RelativeSpeed, theirs.RelativeSpeed, 0.0001)
	end)

	Test.Case("a body lifted off the floor reports OnCollisionExit on both sides and lands again", function()
		local cursor = Journal.Count()
		Body("Dropper"):Teleport(vector.create(0, 3, 0))
		WaitFor(cursor, "Dropper:collision-exit:Floor", 8)
		Test.Expect(Journal.Has(cursor, "Floor:collision-exit:Dropper"), "the floor saw the dropper leave")
		WaitFor(cursor, "Dropper:collision-enter:Floor", 240)
		Test.Expect(Journal.Has(cursor, "Floor:collision-enter:Dropper"), "the floor saw the dropper land again")
	end)

	Test.Case("destroying a body runs OnDestroy and then synthesized exits to its surviving partners", function()
		local cursor = Journal.Count()
		Scene.Destroy(Scene.FindByName("Dropper") :: Entity);
		(Scene.FindByName("Lingerer") :: Entity):Destroy()
		Test.WaitTicks(1)
		local destroyedDropper = Journal.Find(cursor, "Dropper:destroy")
		local floorExit = Journal.Find(cursor, "Floor:collision-exit:Dropper")
		local destroyedLingerer = Journal.Find(cursor, "Lingerer:destroy")
		local zoneExit = Journal.Find(cursor, "Zone:trigger-exit:Lingerer")
		Test.Expect(destroyedDropper > 0 and floorExit > destroyedDropper, "OnDestroy precedes the floor's synthesized exit")
		Test.Expect(destroyedLingerer > 0 and zoneExit > destroyedLingerer, "OnDestroy precedes the zone's synthesized exit")
		Test.Expect(not Journal.Has(cursor, "Dropper:collision-exit:Floor"), "a destroyed body gets no exit of its own")
		Test.Expect(not Journal.Has(cursor, "Lingerer:trigger-exit:Zone"), "a destroyed body gets no trigger exit of its own")
		Test.ExpectEqual(Scene.FindByName("Dropper"), nil)
	end)

	Test.Case("deactivating a body runs OnDisable and OnDestroy, synthesizes exits, and reactivation recreates it", function()
		local faller = Scene.FindByName("Faller") :: Entity
		local cursor = Journal.Count()
		faller:SetActive(false)
		ExpectSequence(Journal.Since(cursor, Of("Faller")), { "Faller:disable" }, "deactivation dispatches OnDisable at once")
		Test.WaitTicks(1)
		local disable = Journal.Find(cursor, "Faller:disable")
		local destroy = Journal.Find(cursor, "Faller:destroy")
		local exit = Journal.Find(cursor, "Floor:collision-exit:Faller")
		Test.Expect(disable > 0 and destroy > disable and exit > destroy, "disable, then the instance's teardown, then the floor's exit")
		Test.Expect(not Journal.Has(cursor, "Faller:fixed"), "a deactivated instance gets no updates")
		cursor = Journal.Count()
		faller:SetActive(true)
		ExpectSequence(Journal.Since(cursor, Of("Faller")), { "Faller:create", "Faller:enable" }, "reactivation creates and enables a new instance")
		Test.WaitTicks(1)
		local started = Journal.Find(cursor, "Faller:start")
		local fixed = Journal.Find(cursor, "Faller:fixed")
		Test.Expect(started > 0 and fixed > started, "the new instance starts before its first fixed step")
		WaitFor(cursor, "Faller:collision-enter:Floor", 120)
		Test.Expect(Journal.Has(cursor, "Floor:collision-enter:Faller"), "the re-added body touches the floor again")
	end)

	Test.Case("OnDisable and OnEnable follow the effective active state of a hierarchy", function()
		local root = Scene.FindByName("Root") :: Entity
		local child = root:FindChild("Child") :: Entity
		local cursor = Journal.Count()
		root:SetActive(false)
		Test.Expect(not child:IsActive() and child:IsActiveSelf(), "the child is inactive through its parent")
		local keep = function(entry: string): boolean
			return (Of("Root")(entry) or Of("Child")(entry)) and not IsPhase(entry)
		end
		ExpectSequence(Journal.Since(cursor, keep), { "Root:disable", "Child:disable" }, "deactivation, parents first")
		Test.WaitTicks(1)
		ExpectSequence(Journal.Since(cursor, keep), { "Root:disable", "Child:disable", "Child:destroy", "Root:destroy" }, "teardown, children first")
		cursor = Journal.Count()
		root:SetActive(true)
		ExpectSequence(Journal.Since(cursor, keep), { "Root:create", "Child:create", "Root:enable", "Child:enable" }, "reactivation")
		Test.WaitTicks(1)
		ExpectSequence(Journal.Since(cursor, keep), { "Root:create", "Child:create", "Root:enable", "Child:enable", "Root:start", "Child:start" }, "restart")
		Test.Expect(child:IsActive(), "the child is active again")
	end)

	Test.Case("an instance a script creates runs OnCreate at once and OnStart before its first update", function()
		local recorder = Assets.Load("Assets/Scripts/Callbacks/Recorder.luau")
		Test.Expect(recorder ~= nil, "the Recorder script loads")
		local cursor = Journal.Count()
		local spawned = Scene.CreateEntity("Spawned")
		local component = spawned:AddComponent("Script", { Script = recorder, Fields = { Label = "Spawned" }, ExecutionOrder = 3 })
		Test.Expect(component ~= nil, "the Script component was added")
		ExpectSequence(Journal.Since(cursor, Of("Spawned")), { "Spawned:create" }, "OnCreate runs inside AddComponent")
		Test.WaitTicks(1)
		ExpectSequence(Journal.Since(cursor, Of("Spawned")), { "Spawned:create", "Spawned:start", "Spawned:update", "Spawned:late", "Spawned:fixed" }, "the first tick")
		-- ExecutionOrder 3 runs between the order-0 instances and Late (7).
		local fixed = Journal.Since(cursor, function(entry: string): boolean
			return string.find(entry, ":fixed$") ~= nil
		end)
		Test.ExpectEqual(fixed[#fixed - 1], "Spawned:fixed")
		Test.ExpectEqual(fixed[#fixed], "Late:fixed")
	end)

	Test.Case("deactivating a body inside a trigger synthesizes the zone's exit", function()
		local recorder = Assets.Load("Assets/Scripts/Callbacks/Recorder.luau")
		local napper = Scene.CreateEntity("Napper")
		napper.Transform.Translation = vector.create(-4, 2, 0.5)
		napper:AddComponent("RigidBody", { Type = "Dynamic", GravityFactor = 0, AllowSleeping = false })
		napper:AddComponent("SphereCollider", { Radius = 0.25 })
		local cursor = Journal.Count()
		Test.Expect(napper:AddComponent("Script", { Script = recorder, Fields = { Label = "Napper" } }) ~= nil, "the Recorder was added")
		WaitFor(cursor, "Zone:trigger-enter:Napper", 8)
		Test.Expect(Journal.Has(cursor, "Napper:trigger-enter:Zone"), "the napper saw itself enter the zone")
		cursor = Journal.Count()
		napper:SetActive(false)
		Test.WaitTicks(1)
		local disable = Journal.Find(cursor, "Napper:disable")
		local destroy = Journal.Find(cursor, "Napper:destroy")
		local exit = Journal.Find(cursor, "Zone:trigger-exit:Napper")
		Test.Expect(disable > 0 and destroy > disable and exit > destroy, "disable, then the instance's teardown, then the zone's synthesized exit")
		Test.Expect(not Journal.Has(cursor, "Napper:trigger-exit:Zone"), "a deactivated body gets no trigger exit of its own")
	end)
end)
