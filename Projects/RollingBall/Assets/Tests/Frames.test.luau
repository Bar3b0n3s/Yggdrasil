--!strict
-- Rolling Ball's frame-timing suite (ADR 0038 decision 10; the review's G6). It runs on a ScriptedClock (its
-- Testing.Suites entry's Clock) whose frames each run several fixed steps, as live play's frames may. A scene
-- transition requested in a fixed step applies only at the frame's end, and the frame's later steps still run in the
-- current scene, where a second request would raise InvalidState: Game.luau's Leaving flag requests one transition at
-- most. The case during which the transition is requested returns in the frame's last step, and the next case runs in
-- the scene it loads.
local Actions = require("../Scripts/Actions")
local GameScript = require("../Scripts/Game")

local function find(path: string): Entity
	local entity = Scene.FindByPath(path)
	if not entity then
		Test.Fail("the scene has no " .. path)
	end
	return entity :: Entity
end

local function game(): GameScript.Game
	return find("/Game"):GetScript("Game") :: GameScript.Game
end

return Test.Suite("Frames", function()
	Test.Case("Level 1: Restart in the frame its delayed load falls due requests one transition", function()
		local current = game()
		local body = find("/Ball").RigidBody :: RigidBody
		-- The clock's frames: a tick in another frame than the tick before it starts a frame.
		local starts: { number } = {}
		local frame = Time.GetFrameCount()
		while #starts < 3 do
			Test.WaitTicks(1)
			if Time.GetFrameCount() ~= frame then
				frame = Time.GetFrameCount()
				table.insert(starts, Time.GetTick())
			end
		end
		local steps = starts[2] - starts[1]
		Test.ExpectEqual(starts[3] - starts[2], steps, "every frame runs as many fixed steps")
		Test.Expect(steps >= 2, string.format("a frame runs several fixed steps (%d)", steps))
		-- The ball placed on the goal's trigger enters it in that tick's physics step, so the game finishes in the next
		-- tick, F. Its delayed load falls due when FinishedTicks reaches LoadDelayTicks: at F + LoadDelayTicks, or a
		-- tick later after a Restart pressed before then, since the press's tick returns before FinishedTicks counts.
		-- The case presses Restart at F + LoadDelayTicks, so the load falls due in the next step,
		-- F + LoadDelayTicks + 1, placed as the last step of its frame.
		local delay = current.LoadDelayTicks
		Test.WaitUntil(function()
			return (Time.GetTick() + delay + 3 - starts[1]) % steps == 0
		end, steps)
		local placed = Time.GetTick()
		body:Teleport(find("/Level/Goal").Transform.WorldPosition + vector.create(0, 0.6, 0), Quat.Identity())
		body:SetLinearVelocity(vector.zero)
		body:SetAngularVelocity(vector.zero)
		Test.WaitUntil(function()
			return current.Finished
		end, 30)
		local finished = Time.GetTick()
		-- A press injected in this tick is the next tick's input.
		Test.WaitUntil(function()
			return current.FinishedTicks == delay - 1
		end, delay + 10)
		Test.InjectAction(Actions.Restart, "Tap")
		Test.WaitTicks(1)
		local pressed = Time.GetTick()
		local pressFrame = Time.GetFrameCount()
		Test.Expect(current.Leaving and not current.LoadRequested, "the press requested the restart")
		Test.WaitTicks(1)
		-- Without the Leaving flag the game would request the delayed load here, a second transition in the frame,
		-- which raises InvalidState in its OnFixedUpdate before this case resumes.
		local timeline = string.format("placed in tick %d, finished in %d, pressed in %d, due in %d; frames of %d steps from tick %d", placed, finished, pressed, Time.GetTick(), steps, starts[1])
		Test.ExpectEqual(Time.GetFrameCount(), pressFrame, "the press and the step the load falls due in share a frame: " .. timeline)
		Test.ExpectEqual(current.FinishedTicks, delay, "the delayed load falls due in this step: " .. timeline)
		Test.Expect(not current.LoadRequested, "no load is requested after the restart")
		Test.ExpectEqual(#Test.GetScriptErrors(), 0, "no script error")
	end)

	Test.Case("Level 1: the restart reloads Level 1, not Level 2", function()
		Test.ExpectEqual(Scene.GetName(), "Level1", "the restart's reload took effect, not the delayed load")
		local current = game()
		Test.Expect(not current.Finished and not current.Leaving, "the reloaded level runs afresh")
		Test.Expect(current.Ticks < 10, string.format("the level's own time starts again (%d ticks)", current.Ticks))
		Test.ExpectEqual(current.StartTicks, 0, "Level 1 starts again with no carried time")
	end)
end)
