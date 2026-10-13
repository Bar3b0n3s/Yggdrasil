--!strict
-- Errors.scene's runtime faults (§15.5: runtime faults only). An instance counts its fixed steps and, once a test arms
-- it, faults in its next OnFixedUpdate according to Mode: a runtime error, an infinite loop the watchdog stops, or an
-- allocation past the VM's memory limit. The loops are pure Luau with no engine call inside, so how long they ran
-- before the watchdog or the allocator stopped them changes no coverage count (ADR 0022 Risk 6).
export type Fault = ScriptInstance & {
	Mode: string,
	Armed: boolean,
	Steps: number,
}

local Fault = {}

Fault.Fields = {
	Mode = Field.Enum({ "Runtime", "Timeout", "Memory" }),
}

function Fault.OnCreate(self: Fault)
	self.Armed = false
	self.Steps = 0
end

function Fault.OnFixedUpdate(self: Fault, dt: number)
	self.Steps += 1
	if not self.Armed then
		return
	end
	self.Armed = false
	if self.Mode == "Runtime" then
		error("deliberate runtime fault") -- Errors.test.luau checks the location of this line (30)
	elseif self.Mode == "Timeout" then
		local spins = 0
		while true do
			spins += 1
		end
	else
		local hoard: { string } = {}
		for index = 1, 4096 do
			hoard[index] = string.rep("x", 1048576) .. index
		end
	end
end

return Script.Define("Fault", Fault)
