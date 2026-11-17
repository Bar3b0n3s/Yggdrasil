--!strict
-- GraphMembers.scene (M39, Docs/Decisions/0056-m39-graphs-behaviours.md decision 12; World Tree's brief 18): visual-scripting
-- graphs that talk to Luau behaviours, built by Scaffold/GraphMembers.jsonl through the graph.* methods alone and run in
-- every mode as their cooked Luau. The Valve graph finds the Press behaviour through the instance chain (Scene.FindByPath,
-- Flow.IfNotNil, Entity:GetScript), writes its Running, calls its Toggle with the graph's own instance and reads the table
-- it returns; the Rune graph's methods answer the Crystal's Luau Health (Blocks and Damaged, as the game's Health calls
-- them) and a Luau caller; the Clicker graphs consume a UI click or let it bubble; the Ticker graph runs while the world is
-- paused. Also driven by Assets/Tests/Replays/GraphMembers.replay (Scaffold/GraphMembersReplay.jsonl), in which input
-- strikes the Rune through its Health.
local Support = require("../Api/Support")

-- The behaviour `behaviour` of the entity named `entity`.
local function Instance(entity: string, behaviour: string): any
	local instance = Support.Find(entity):GetScript(behaviour)
	Test.Expect(instance ~= nil, entity .. " runs the behaviour " .. behaviour)
	return instance
end

-- The centre of a laid-out element, in view coordinates (the session's view is the scene's resolution here).
local function Centre(element: Entity): vector
	local rect = Ui.GetRect(element)
	assert(rect, `{element.Name} is laid out`)
	return (rect.Min + rect.Max) * 0.5
end

return Test.Suite("GraphMembers", function()
	Test.Case("a graph reads, writes and calls a behaviour's members found through the instance chain, and reads a table", function()
		Test.WaitTicks(2)
		local press = Instance("Press", "Press")
		local valve = Instance("Valve", "Valve")
		Test.ExpectEqual(press.Running, false, "Script.Set wrote the press's Running")
		Test.Expect(press.By == valve, "Script.Call passed the graph's own instance (Self.Instance) to Toggle")
		Test.ExpectEqual(valve.Pressure, 20, "Script.Get read Pressure from the table Toggle returned")
		Test.ExpectEqual(valve.SawNil, true, "a member the table lacks read nil")
		local strokes = press.Strokes
		Test.WaitTicks(3)
		Test.ExpectEqual(press.Strokes, strokes, "the press stopped running")
	end)

	Test.Case("a Luau behaviour strikes a graph through its methods, which return their results", function()
		Test.WaitTicks(1)
		local crystal = Support.Find("Crystal")
		local health = Instance("Crystal", "Health")
		local rune = Instance("Crystal", "Rune")
		Test.ExpectEqual(health:Damage({ Kind = "Ice", Amount = 3 }), true, "Blocks returned true for ice, through its merge")
		Test.ExpectEqual(rune.LastKind, "Ice", "the merge kept the blocked hit's Kind")
		Test.ExpectEqual(rune.Blows, 0, "a blocked hit is no blow")
		Test.ExpectEqual(health:Damage({ Kind = "Stone", Amount = 3 }), true, "and for stone, through the same merge")
		Test.ExpectEqual(health:Damage({ Kind = "Fire", Amount = 3 }), false, "Blocks returned nil for fire")
		Test.ExpectEqual(rune.Blows, 1, "Damaged counted the blow")
		Test.ExpectEqual(rune.LastKind, "Fire", "and read the hit's Kind")
		Test.ExpectEqual(rune.Glyphs, "", "only a bolt lights a glyph")
		Test.ExpectEqual(health.Health, 7, "the Health took the Amount")
		health:Damage({ Kind = "Bolt", Amount = 2 })
		Test.ExpectEqual(rune.Glyphs, "*", "a bolt lit a glyph")
		health:Damage({ Kind = "Bolt", Amount = 0.5 })
		Test.ExpectEqual(rune.Blows, 2, "a hit below 1 returned early, counting nothing")
		Test.ExpectEqual(rune.Glyphs, "*")
		Test.ExpectEqual(crystal.Transform.Translation.x, 2, "the rune moved its entity to x = Blows")
		Test.ExpectEqual(rune:Describe("Rune of ", 0.5, crystal), "Rune of Crystal2.5", "Describe returned its String result")
		Test.Expect(rune:Count() == nil, "a method without an event node returns nil")
	end)

	Test.Case("a graph's OnUiClick Return of true consumes the click, and of false lets it bubble", function()
		Test.WaitTicks(2)
		local menu = Instance("Menu", "Bubbles")
		local consuming = Support.Find("Consuming")
		local bubbling = Support.Find("Bubbling")
		Test.InjectMouseMove(Centre(consuming))
		Test.InjectMouse("Left", "Tap", Centre(consuming))
		Test.WaitTicks(2)
		Test.ExpectEqual(Instance("Consuming", "Clicker").Clicks, 1, "the Consuming button's graph heard the click")
		Test.ExpectEqual(menu.Heard, 0, "and consumed it")
		Test.InjectMouseMove(Centre(bubbling))
		Test.InjectMouse("Left", "Tap", Centre(bubbling))
		Test.WaitTicks(2)
		Test.ExpectEqual(Instance("Bubbling", "Clicker").Clicks, 1, "the Bubbling button's graph heard the click")
		Test.ExpectEqual(menu.Heard, 1, "and returned false, so the click bubbled to the menu")
	end)

	Test.Case("a graph that opts in runs while the world is paused, and a Luau behaviour that does not holds", function()
		Test.WaitTicks(2)
		local ticker = Instance("Ticker", "Ticker")
		local press = Instance("Press", "Press")
		Time.PauseWorld()
		Test.WaitTicks(2)
		local ticks, pressTicks = ticker.Ticks, press.Ticks
		Test.WaitTicks(10)
		Test.ExpectEqual(ticker.Ticks, ticks + 10, "the Ticker graph's OnFixedUpdate ran every paused tick")
		Test.ExpectEqual(press.Ticks, pressTicks, "the press held")
		Time.ResumeWorld()
		Test.WaitTicks(3)
		Test.Expect(press.Ticks > pressTicks, "the press runs again after the resume")
	end)
end)
