--!strict
-- Rolling Ball's frame-timing suite (ADR 0038 decisions 10 and 15; the review's G6). It runs on a ScriptedClock (its
-- Testing.Suites entry's Clock) whose frames each run several fixed steps, as live play's frames may. A scene
-- transition requested in a fixed step applies after that tick: the frame runs no further fixed step and replaces the
-- scene at its end, and the next frames run the steps it left in the new scene. So two requests can meet only within
-- one tick, where Game.luau's Leaving flag and its Restart branch's early return request one transition at most. The
-- case during which the transition is requested returns in that tick, and the next case runs in the scene it loads.
local Actions = require("../Scripts/Actions")
local GameScript = require("../Scripts/Game")

-- The plan, constants of the suite because the reload replaces the suite's VM with the scene, so the next case keeps
-- nothing case 1 measured. Every frame runs Steps fixed steps from tick 0 (the Clock's 0.1 s is more than
-- MaxStepsPerFrame's 5 steps of 1/60 s), which case 1 measures over MeasuredFrames frames, and the game sees Restart in
-- step PressIndex (counted from 0) of its frame, a step before the frame's last.
local Steps = 5
local MeasuredFrames = 3
local PressIndex = 3

-- The tick the game sees Restart in: the first at step PressIndex of its frame that falls LoadDelayTicks ticks after a
-- finish the case can arrange. The case places the ball once it has measured the frames, at the earliest in the tick
-- frame MeasuredFrames starts in, and the game finishes in the tick after the placement.
local function pressTick(delay: number): number
	local earliest = MeasuredFrames * Steps + 1 + delay
	return earliest + (PressIndex - earliest) % Steps
end

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
	Test.Case("Level 1: Restart in the tick its delayed load falls due requests one transition and returns in that tick", function()
		local current = game()
		local body = find("/Ball").RigidBody :: RigidBody
		-- The clock's frames: a tick in another frame than the tick before it starts a frame.
		local starts: { number } = {}
		local frame = Time.GetFrameCount()
		while #starts < MeasuredFrames do
			Test.WaitTicks(1)
			if Time.GetFrameCount() ~= frame then
				frame = Time.GetFrameCount()
				table.insert(starts, Time.GetTick())
			end
		end
		for index, start in starts do
			if start ~= index * Steps then
				Test.Fail(string.format("frame %d (from 0) starts in tick %d, not %d: the plan needs frames of %d fixed steps from tick 0", index, start, index * Steps, Steps))
			end
		end
		-- The ball placed on the goal's trigger enters it in that tick's physics step, so the game finishes in the next
		-- tick, F. Its delayed load falls due in tick F + LoadDelayTicks, when FinishedTicks would reach LoadDelayTicks,
		-- and the case presses Restart so that the game sees the press in that very tick.
		local delay = current.LoadDelayTicks
		local press = pressTick(delay)
		local placement = press - delay - 1
		Test.WaitUntil(function()
			return Time.GetTick() >= placement
		end, Steps)
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
		local place = pressed % Steps
		local timeline = string.format("placed in tick %d, finished in %d, pressed in %d, step %d of %d of the frame from tick %d", placed, finished, pressed, place + 1, Steps, pressed - place)
		Log.Info("Frames: " .. timeline)
		Test.ExpectEqual(pressed, press, "the game sees the press in the planned tick: " .. timeline)
		Test.ExpectEqual(pressed, finished + delay, "the press falls in the tick the delayed load falls due: " .. timeline)
		Test.Expect(place ~= Steps - 1, "the press's tick is not its frame's last step: " .. timeline)
		-- The game saw the press and returned before FinishedTicks counted, so the delayed load falling due in this tick
		-- was not requested: a request applies after its tick, so two can meet only within one tick, and Leaving and the
		-- early return keep it to one, which nothing refuses with InvalidState.
		Test.Expect(current.Leaving and not current.LoadRequested, "the press requested the restart and no load")
		Test.ExpectEqual(current.FinishedTicks, delay - 1, "Restart returns before FinishedTicks counts")
		Test.ExpectEqual(#Test.GetScriptErrors(), 0, "no script error")
		-- The restart applies after this tick and the frame runs no further fixed step in this scene, so the case
		-- returns in this tick, and the next case runs in the reloaded scene.
	end)

	Test.Case("Level 1: the restart reloads Level 1 from the tick after the press, not Level 2", function()
		Test.ExpectEqual(Scene.GetName(), "Level1", "the restart's reload took effect, not the delayed load")
		local current = game()
		Test.Expect(not current.Finished and not current.Leaving, "the reloaded level runs afresh")
		Test.ExpectEqual(current.StartTicks, 0, "Level 1 starts again with no carried time")
		Test.Expect(current.Ticks >= 1 and current.Ticks < 10, string.format("the level's own time starts again (%d ticks)", current.Ticks))
		-- The game counts Ticks from the scene's first tick, and its fixed step runs before this case's in a tick. The
		-- press's tick comes from the plan, since this VM keeps nothing of case 1's.
		local first = Time.GetTick() - current.Ticks + 1
		local press = pressTick(current.LoadDelayTicks)
		Log.Info(string.format("Frames: the reloaded scene's first tick is %d, the press was in %d", first, press))
		-- The reload applied after the press's tick, so the reloaded scene runs the step its frame left. Had the press's
		-- frame run its later steps in the old scene, this would be the next frame's first tick, press - PressIndex +
		-- Steps.
		Test.ExpectEqual(first, press + 1, "the reloaded scene's first tick is the tick after the press")
		Test.ExpectEqual(#Test.GetScriptErrors(), 0, "no script error")
	end)
end)
