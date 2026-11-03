--!strict
-- The SpriteAnimator proxy (Docs/Decisions/0032-m21-contract.md decisions 5 and 13): Play and Stop drive the clip, which
-- shows each step for its ticks and holds the last frame of a clip without Loop, and every field reads and writes.
local Support = require("./Support")

-- Waits one tick at a time and lists the frames shown.
local function watch(sprite: SpriteRenderer, ticks: number): string
	local shown: { number } = {}
	for _ = 1, ticks do
		Test.WaitTicks(1)
		table.insert(shown, sprite.Frame)
	end
	return table.concat(shown, ",")
end

return Test.Suite("Api.SpriteAnimator", function()
	Test.Case("Play shows each step of the clip for its ticks and holds the last frame", function()
		local waver = Support.Find("Waver")
		local sprite = waver.SpriteRenderer :: SpriteRenderer
		local animator = waver.SpriteAnimator :: SpriteAnimator
		-- The scene's animator is stopped: its frame stays.
		Test.ExpectEqual(watch(sprite, 2), "3,3")
		-- Wave: frame 0 for 3 ticks, frame 1 for 3, then frame 4 for 6, without Loop.
		animator:Play("Wave")
		Test.ExpectEqual(animator.Clip, "Wave")
		Test.ExpectEqual(animator.Step, 0)
		Test.ExpectEqual(animator.StepTicks, 0)
		Test.ExpectEqual(animator.Playing, true)
		Test.ExpectEqual(watch(sprite, 14), "0,0,0,1,1,1,4,4,4,4,4,4,4,4")
		Test.ExpectEqual(animator.Playing, false, "a clip without Loop stops on its last step")
		Test.ExpectEqual(animator.Step, 2)
	end)

	Test.Case("Stop keeps the frame shown", function()
		local waver = Support.Find("Waver")
		local sprite = waver.SpriteRenderer :: SpriteRenderer
		local animator = waver.SpriteAnimator :: SpriteAnimator
		animator:Play("Wave")
		Test.ExpectEqual(watch(sprite, 4), "0,0,0,1")
		animator:Stop()
		Test.ExpectEqual(animator.Playing, false)
		Test.ExpectEqual(watch(sprite, 5), "1,1,1,1,1")
		Test.ExpectEqual(animator.Clip, "Wave", "Stop keeps the clip")
	end)

	Test.Case("every field reads and writes through the proxy", function()
		local animator = Support.Find("Waver").SpriteAnimator :: SpriteAnimator
		animator:Stop()
		Test.ExpectEqual(type(Support.RoundTrip(animator, "Clip", "Idle")), "string")
		Test.ExpectEqual(type(Support.RoundTrip(animator, "Playing", true)), "boolean")
		Test.ExpectEqual(type(Support.RoundTrip(animator, "Step", 1)), "number")
		Test.ExpectEqual(type(Support.RoundTrip(animator, "StepTicks", 5)), "number")
		Test.Expect(not pcall(function()
			animator.Step = -1
		end), "a negative step is refused")
	end)
end)
