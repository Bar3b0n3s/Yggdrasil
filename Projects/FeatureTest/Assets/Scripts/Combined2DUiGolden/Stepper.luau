--!strict
-- Combined2DUi.scene's Hud canvas, the combined 2D/UI test (ADR 0028's phase-2 ledger; Docs/Decisions/0033-m22-contract.md,
-- "Landing"): the scene's sprites and its UI draw under a PixelPerfectCamera, and a click on the Step button, which the
-- UiInput phase hit-tests through the camera's output mapping, bubbles to this behaviour, which moves the Hero sprite one
-- unit right and shows the count in the Counter label. So the Combined2DUi replay's hash depends on its input: without
-- its presses the Hero stays where it starts.
export type Stepper = ScriptInstance & {
	Steps: number,
}

local Stepper = {}

function Stepper.OnCreate(self: Stepper)
	self.Steps = 0
end

function Stepper.OnUiClick(self: Stepper, element: Entity): boolean?
	if element.Name ~= "Step" then
		return nil
	end
	self.Steps += 1
	local hero = Scene.FindByName("Hero")
	if hero then
		hero.Transform.Translation = hero.Transform.Translation + vector.create(1, 0, 0)
	end
	local counter = Scene.FindByName("Counter")
	if counter then
		(counter.UiText :: UiText).Text = `Steps {self.Steps}`
	end
	return true
end

return Script.Define("Stepper", Stepper)
