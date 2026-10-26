--!strict
-- Several scripts on one entity (Docs/Decisions/0031-m20-contract.md decisions 17 to 19; Architecture §11.2 to §11.4):
-- every slot of a Script component that names a script is its own instance, which Entity:GetScript(name) and
-- GetScripts() reach; callbacks run in (ExecutionOrder, canonical entity order, slot) order, an entity's own callbacks
-- reaching its slots in slot order and OnDestroy in reverse; a fault disables only its slot's instance; a write of the
-- list keeps the instance of a slot that keeps its script; and each prefab instance keeps its overrides per slot. The
-- suite runs with Isolation "Case", so every case starts from a fresh session of MultiScript.scene.
local CounterScript = require("../../Scripts/MultiScript/Counter")
local FragileScript = require("../../Scripts/MultiScript/Fragile")
local Journal = require("../../Scripts/MultiScript/Journal")
local TracerScript = require("../../Scripts/MultiScript/Tracer")
local TrailScript = require("../../Scripts/MultiScript/Trail")

-- The labels of Alpha's and Beta's Tracer slots in (ExecutionOrder, canonical entity order, slot) order: Alpha's slot 0
-- has ExecutionOrder 5 and every other slot 0, and Alpha comes before Beta in the scene.
local ORDER = { "Order.Alpha.1", "Order.Beta.0", "Order.Beta.1", "Order.Alpha.0" }

local function Find(name: string): Entity
	local entity = Scene.FindByName(name)
	assert(entity ~= nil, name .. " exists")
	return entity
end

local function Slots(entity: Entity): { any }
	return (entity:GetComponent("Script") :: Script).Scripts
end

-- Keeps the journal entries whose label starts with `prefix` and whose event is `event`.
local function Matching(prefix: string, event: string): (string) -> boolean
	return function(entry: string): boolean
		return string.sub(entry, 1, #prefix) == prefix and string.sub(entry, -#event - 1) == ":" .. event
	end
end

local function Expand(event: string): { string }
	local result: { string } = {}
	for _, label in ORDER do
		table.insert(result, label .. ":" .. event)
	end
	return result
end

local function ExpectSequence(actual: { string }, expected: { string }, what: string)
	Test.ExpectEqual(#actual, #expected, what .. ": entry count")
	for index = 1, math.max(#actual, #expected) do
		Test.ExpectEqual(actual[index], expected[index], what .. ": entry " .. index)
	end
end

return Test.Suite("MultiScript", function()
	Test.Case("GetScript(name) and GetScripts() return the instances of an entity's slots", function()
		local hero = Find("Hero")
		local scripts = hero:GetScripts()
		Test.ExpectEqual(#scripts, 4, "one instance per slot")
		local first = hero:GetScript() :: CounterScript.Counter
		Test.ExpectEqual(first, scripts[1], "GetScript() returns the first slot's instance")
		Test.ExpectEqual(first.Label, "A")
		Test.ExpectEqual(hero:GetScript("Counter"), first, "GetScript(name) returns the first slot whose behaviour has the name")
		local second = scripts[2] :: CounterScript.Counter
		Test.Expect(second ~= first, "the same script fills two slots, each its own instance")
		Test.ExpectEqual(second.Label, "B")
		Test.ExpectEqual(hero:GetScript("Fragile"), scripts[3])
		Test.ExpectEqual(hero:GetScript("Tracer"), scripts[4])
		Test.ExpectEqual(hero:GetScript("Missing"), nil, "no slot holds that behaviour")
		local total, otherTotal = first.Total, second.Total
		Test.WaitTicks(2)
		Test.ExpectEqual(first.Total, total + 2, "each instance keeps its own fields and state")
		Test.ExpectEqual(second.Total, otherTotal + 20)

		local hollow = Find("Hollow")
		Test.ExpectEqual(#Slots(hollow), 2)
		Test.ExpectEqual(hollow:GetScript(), nil, "a first slot without a script has no instance")
		local counter = hollow:GetScript("Counter") :: CounterScript.Counter
		Test.ExpectEqual(counter.Label, "Hollow")
		local live = hollow:GetScripts()
		Test.ExpectEqual(#live, 1, "GetScripts() leaves out the slot without an instance")
		Test.ExpectEqual(live[1], counter)
		Test.ExpectEqual(#Find("Camera"):GetScripts(), 0, "an entity without a Script component has no instances")
	end)

	Test.Case("callbacks run in (ExecutionOrder, canonical entity order, slot) order", function()
		ExpectSequence(Journal.Since(0, Matching("Order.", "create")), Expand("create"), "OnCreate at the session's start")
		local cursor = Journal.Count()
		Test.WaitTicks(1)
		ExpectSequence(Journal.Since(cursor, Matching("Order.", "fixed")), Expand("fixed"), "one fixed step")
	end)

	Test.Case("an entity's callbacks reach its slots in slot order, and OnDestroy in reverse slot order", function()
		local beta = Find("Beta")
		local cursor = Journal.Count()
		beta:SetActive(false)
		ExpectSequence(Journal.Since(cursor, Matching("Order.Beta.", "disable")), { "Order.Beta.0:disable", "Order.Beta.1:disable" }, "OnDisable")
		Test.WaitTicks(1)
		ExpectSequence(Journal.Since(cursor, Matching("Order.Beta.", "destroy")), { "Order.Beta.1:destroy", "Order.Beta.0:destroy" }, "the teardown")
		cursor = Journal.Count()
		beta:SetActive(true)
		ExpectSequence(Journal.Since(cursor, Matching("Order.Beta.", "create")), { "Order.Beta.0:create", "Order.Beta.1:create" }, "the new instances")
		ExpectSequence(Journal.Since(cursor, Matching("Order.Beta.", "enable")), { "Order.Beta.0:enable", "Order.Beta.1:enable" }, "OnEnable")

		cursor = Journal.Count()
		Test.WaitUntil(function(): boolean
			return #Journal.Since(cursor, Matching("Faller.", "hit:Ground")) == 2
		end, 120)
		ExpectSequence(Journal.Since(cursor, Matching("Faller.", "hit:Ground")), { "Faller.0:hit:Ground", "Faller.1:hit:Ground" }, "OnCollisionEnter")

		cursor = Journal.Count()
		Find("Alpha"):Destroy()
		Test.WaitTicks(1)
		ExpectSequence(Journal.Since(cursor, Matching("Order.Alpha.", "destroy")), { "Order.Alpha.1:destroy", "Order.Alpha.0:destroy" }, "OnDestroy")
	end)

	Test.Case("a fault disables only its own slot's instance", function()
		local hero = Find("Hero")
		local counter = hero:GetScript("Counter") :: CounterScript.Counter
		local fragile = hero:GetScript("Fragile") :: FragileScript.Fragile
		local tracer = hero:GetScript("Tracer") :: TracerScript.Tracer
		fragile.Armed = true
		Test.ExpectScriptError("deliberate slot fault", 3)
		local errors = Test.GetScriptErrors()
		Test.ExpectEqual(#errors, 1, "one error was reported")
		Test.ExpectEqual(errors[1].script, "Assets/Scripts/MultiScript/Fragile.luau")
		Test.ExpectEqual(errors[1].entity, hero.ID)
		local steps, total, traced = fragile.Steps, counter.Total, tracer.FixedSteps
		Test.WaitTicks(3)
		Test.ExpectEqual(fragile.Steps, steps, "the faulting slot's instance gets no further callbacks")
		Test.ExpectEqual(counter.Total, total + 3, "the slot before it keeps running")
		Test.ExpectEqual(tracer.FixedSteps, traced + 3, "the slot after it keeps running")
		Test.ExpectEqual(hero:GetScript("Fragile"), fragile, "the disabled instance is still its slot's instance")
	end)

	Test.Case("a write of the Script list keeps the instance of a slot that keeps its script", function()
		local swapper = Find("Swapper")
		local component = swapper:GetComponent("Script") :: Script
		local slots = component.Scripts
		Test.ExpectEqual(#slots, 2)
		local counter = swapper:GetScript("Counter") :: CounterScript.Counter
		local cursor = Journal.Count()
		component.Scripts = {
			slots[1],
			{ Script = Assets.Load("Assets/Scripts/MultiScript/Fragile.luau"), Fields = {}, ExecutionOrder = 0 },
		}
		Test.ExpectEqual(swapper:GetScript("Counter"), counter, "slot 0 keeps its script and its instance")
		Test.WaitTicks(2)
		Test.ExpectEqual(swapper:GetScript("Counter"), counter)
		Test.ExpectEqual(swapper:GetScript("Tracer"), nil, "slot 1's Tracer is gone")
		ExpectSequence(Journal.Since(cursor, Matching("Swapper", "destroy")), { "Swapper:destroy" }, "its instance was destroyed")
		local fragile = swapper:GetScript("Fragile") :: FragileScript.Fragile
		Test.Expect(fragile ~= nil and fragile.Steps >= 1, "slot 1's new script runs its own instance")
		Test.ExpectEqual(#swapper:GetScripts(), 2)
	end)

	Test.Case("a slot runs after the slots before it in the same fixed step", function()
		local pilot = Find("Pilot")
		local trail = pilot:GetScript("Trail") :: TrailScript.Trail
		Test.InjectAction("MoveX", nil, 1)
		Test.WaitTicks(10)
		-- The pilot moved in the step that just ran, so a Trail that ran before the Mover would lag a step behind.
		local x = pilot.Transform.Translation.x
		Test.Expect(x > 0.3, "the Mover of slot 0 moved the pilot")
		Test.ExpectNear(pilot.Transform.Scale.x, 1 + math.abs(x) * trail.Gain, 0.0001, "the Trail of slot 1 saw the same step's position")
		Test.InjectAction("MoveX", nil, 0)
	end)

	Test.Case("each prefab instance keeps the overrides of its own slots", function()
		local walker = Find("Walker"):GetScript("Counter") :: CounterScript.Counter
		local a = Find("WalkerA"):GetScript("Counter") :: CounterScript.Counter
		local b = Find("WalkerB"):GetScript("Counter") :: CounterScript.Counter
		Test.ExpectEqual(walker.Step, 5, "the value applied to Walker.prefab")
		Test.ExpectEqual(a.Step, 3, "WalkerA's override of slot 0's Step outlives the prefab's new Step")
		Test.ExpectEqual(a.Label, "Walker", "WalkerA keeps the prefab's Label of slot 0")
		Test.ExpectEqual(b.Step, 5, "WalkerB overrides only slot 0's Label, so it takes the prefab's new Step")
		Test.ExpectEqual(b.Label, "WalkerB")
		local slotsB = Slots(Find("WalkerB"))
		Test.ExpectEqual(slotsB[2].Fields.Label, "WalkerB.T", "WalkerB's override of slot 1's Fields")
		Test.ExpectEqual(slotsB[2].ExecutionOrder, 4, "WalkerB's override of slot 1's ExecutionOrder")
		local slotsA = Slots(Find("WalkerA"))
		Test.ExpectEqual(slotsA[2].Fields.Label, "Walker.T")
		Test.ExpectEqual(slotsA[2].ExecutionOrder, 0)
	end)
end)
