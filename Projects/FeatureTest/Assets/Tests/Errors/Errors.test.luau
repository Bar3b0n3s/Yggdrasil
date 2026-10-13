--!strict
-- Runtime faults of Errors.scene (§11.1, §11.7, §15.5): a runtime error, a timeout under the suite's 50 ms callback
-- budget and a breach of its 16 MB memory limit. Each is claimed with Test.ExpectScriptError, reported as a structured
-- script error and disables only the faulting instance. The suite runs with Isolation "Case", so every case starts
-- from a fresh session of the scene.
local FaultScript = require("../../Scripts/Errors/Fault")

local FAULT_SCRIPT = "Assets/Scripts/Errors/Fault.luau"

local function Instance(name: string): (Entity, FaultScript.Fault)
	local entity = Scene.FindByName(name)
	assert(entity ~= nil, name .. " exists")
	local instance = entity:GetScript() :: FaultScript.Fault
	assert(instance ~= nil, name .. " runs the Fault behaviour")
	return entity, instance
end

-- Arms `name`, claims the fault it raises in its next fixed step and returns the structured error.
local function Provoke(name: string, pattern: string): (Entity, FaultScript.Fault, ScriptError)
	local entity, instance = Instance(name)
	Test.ExpectEqual(#Test.GetScriptErrors(), 0, "a fresh session has no script errors")
	local armed = Time.GetTick()
	instance.Armed = true
	Test.ExpectScriptError(pattern, 3)
	local errors = Test.GetScriptErrors()
	Test.ExpectEqual(#errors, 1, "one error was reported")
	local report = errors[1]
	Test.ExpectEqual(report.script, FAULT_SCRIPT)
	Test.ExpectEqual(report.callback, "OnFixedUpdate")
	Test.ExpectEqual(report.entity, entity.ID)
	Test.ExpectEqual(report.entityName, name)
	Test.ExpectEqual(report.tick, armed + 1, "the fault happened in the next fixed step")
	Test.ExpectEqual(report.count, 1)
	Test.Expect(report.id > 0, "the error has an id")
	Test.Expect(string.find(report.message, pattern) ~= nil, "the message matches: " .. report.message)
	return entity, instance, report
end

-- The faulting instance is disabled while the bystander keeps running (§11.7).
local function ExpectIsolated(entity: Entity, instance: FaultScript.Fault)
	local _, bystander = Instance("Bystander")
	local steps, others = instance.Steps, bystander.Steps
	Test.WaitTicks(3)
	Test.ExpectEqual(instance.Steps, steps, "the faulting instance gets no further callbacks")
	Test.ExpectEqual(bystander.Steps, others + 3, "the other instances keep running")
	Test.Expect(entity:IsValid() and entity:IsActive(), "the faulting instance's entity stays in the scene")
end

return Test.Suite("Errors", function()
	Test.Case("a runtime error is reported with its location and disables only its instance", function()
		local entity, instance, report = Provoke("Thrower", "deliberate runtime fault")
		Test.ExpectEqual(report.kind, "runtime")
		Test.ExpectEqual(report.line, 30)
		Test.Expect(#report.traceback >= 1, "the error carries a traceback")
		Test.ExpectEqual(report.traceback[1].script, FAULT_SCRIPT)
		Test.ExpectEqual(report.traceback[1].line, 30)
		ExpectIsolated(entity, instance)
	end)

	Test.Case("an infinite loop is stopped at the suite's 50 ms callback budget", function()
		local entity, instance, report = Provoke("Spinner", "exceeded 50 ms")
		Test.ExpectEqual(report.kind, "timeout")
		Test.Expect(string.find(report.message, "Fault.OnFixedUpdate (possible infinite loop)", 1, true) ~= nil, report.message)
		ExpectIsolated(entity, instance)
	end)

	Test.Case("a breach of the suite's 16 MB memory limit faults its instance and the VM stays usable", function()
		local entity, instance, report = Provoke("Hog", "exceeded memory limit 16 MB")
		Test.ExpectEqual(report.kind, "memory")
		Test.Expect(string.find(report.message, "in Fault.OnFixedUpdate", 1, true) ~= nil, report.message)
		local spare: { number } = {}
		for index = 1, 1000 do
			spare[index] = index * 2
		end
		Test.ExpectEqual(spare[1000], 2000, "the VM still allocates after the breach")
		ExpectIsolated(entity, instance)
	end)

	Test.Case("every case starts from a fresh session of the scene", function()
		Test.ExpectEqual(#Test.GetScriptErrors(), 0)
		local tick = Time.GetTick()
		for _, name in { "Thrower", "Spinner", "Hog", "Bystander" } do
			local _, instance = Instance(name)
			Test.ExpectEqual(instance.Armed, false)
			Test.ExpectEqual(instance.Steps, tick + 1, name .. " ran every fixed step of this session")
		end
		Test.WaitTicks(2)
		local _, thrower = Instance("Thrower")
		Test.ExpectEqual(thrower.Steps, tick + 3, "the earlier case's disabled instance runs again")
	end)
end)
