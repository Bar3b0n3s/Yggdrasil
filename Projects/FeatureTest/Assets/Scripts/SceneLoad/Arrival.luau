--!strict
-- Records the load parameters and scene name its instance saw when the scene started (SceneLoadA and SceneLoadB).
export type Arrival = ScriptInstance & {
	Greeting: string,
	SeenScene: string,
	SeenAttempt: number,
	SeenOrigin: string,
	Starts: number,
}

local Arrival = {}

Arrival.Fields = {
	Greeting = Field.String("Hello", { Tooltip = "Text the instance keeps, overridden per scene" }),
}

function Arrival.OnCreate(self: Arrival)
	self.Starts = 0
end

function Arrival.OnStart(self: Arrival)
	local parameters = Scene.GetLoadParameters()
	self.SeenScene = Scene.GetName()
	self.SeenAttempt = parameters.Attempt or 0
	self.SeenOrigin = parameters.Origin or ""
	self.Starts += 1
end

return Script.Define("Arrival", Arrival)
