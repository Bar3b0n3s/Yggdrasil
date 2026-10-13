--!strict
-- Scene.Load with parameters and the VM reset (§11.10, §15.5). The suite starts in SceneLoadA with the suite's
-- Parameters; a case requesting Scene.Load returns in that frame, and the next case runs in the new scene's VM, which
-- re-collects this suite with the same cases.
local ArrivalScript = require("../../Scripts/SceneLoad/Arrival")
local Carry = require("../../Scripts/SceneLoad/Carry")

local function Greeter(name: string): ArrivalScript.Arrival
	local entity = Scene.FindByName(name)
	assert(entity ~= nil, name .. " exists")
	return entity:GetScript() :: ArrivalScript.Arrival
end

return Test.Suite("SceneLoad", function()
	Test.Case("the suite's parameters reach the first scene", function()
		Test.ExpectEqual(Scene.GetName(), "SceneLoadA")
		local parameters = Scene.GetLoadParameters()
		Test.ExpectEqual(parameters.Attempt, 1)
		Test.ExpectEqual(parameters.Origin, "Suite")
		Test.ExpectEqual(#parameters.Weights, 2)
		Test.ExpectEqual(parameters.Weights[1], 0.5)
		Test.ExpectEqual(parameters.Weights[2], 2)
		local gate = Greeter("Gate")
		Test.ExpectEqual(gate.Greeting, "From A")
		Test.ExpectEqual(gate.SeenScene, "SceneLoadA")
		Test.ExpectEqual(gate.SeenAttempt, 1)
		Test.ExpectEqual(gate.SeenOrigin, "Suite")
		Test.Expect(Scene.FindByName("OnlyInA") ~= nil, "SceneLoadA's own entity exists")
		Test.ExpectEqual(Scene.FindByName("OnlyInB"), nil)
		Test.ExpectEqual(Carry.Marker, nil)
	end)

	Test.Case("Scene.Load requests the next scene with a copy of its parameters", function()
		Carry.Marker = 42
		Carry.Loads += 1
		local outgoing = { Attempt = 2, Origin = "SceneLoadA", Nested = { Ready = true }, Items = { "a", "b" } }
		Scene.Load(Assets.Load("Assets/Scenes/SceneLoadB.scene") :: AssetRef, outgoing)
		outgoing.Attempt = 99
		outgoing.Items[2] = "changed"
		-- The replacement happens at the end of the frame; this case returns in the requesting frame.
		Test.ExpectEqual(Scene.GetName(), "SceneLoadA")
		Test.Expect(Scene.FindByName("OnlyInA") ~= nil, "the old scene lives until the frame ends")
		Test.ExpectEqual(Scene.GetLoadParameters().Attempt, 1)
	end)

	Test.Case("the next case runs in the new scene with a fresh VM and the copied parameters", function()
		Test.ExpectEqual(Scene.GetName(), "SceneLoadB")
		local parameters = Scene.GetLoadParameters()
		Test.ExpectEqual(parameters.Attempt, 2, "parameters were copied when Scene.Load was called")
		Test.ExpectEqual(parameters.Origin, "SceneLoadA")
		Test.ExpectEqual(parameters.Nested.Ready, true)
		Test.ExpectEqual(parameters.Items[2], "b")
		Test.ExpectEqual(Carry.Marker, nil, "module state does not survive the VM reset")
		Test.ExpectEqual(Carry.Loads, 0)
		Test.ExpectEqual(Scene.FindByName("OnlyInA"), nil)
		Test.Expect(Scene.FindByName("OnlyInB") ~= nil, "SceneLoadB's own entity exists")
		local arrival = Greeter("Arrival")
		Test.ExpectEqual(arrival.Greeting, "Welcome to B")
		Test.ExpectEqual(arrival.SeenScene, "SceneLoadB")
		Test.ExpectEqual(arrival.SeenAttempt, 2)
		Test.ExpectEqual(arrival.SeenOrigin, "SceneLoadA")
		Test.ExpectEqual(arrival.Starts, 1)
	end)

	Test.Case("loading the first scene again starts it over with the new parameters", function()
		Carry.Marker = 7
		Scene.Load(Assets.Load("Assets/Scenes/SceneLoadA.scene") :: AssetRef, { Attempt = 3 })
	end)

	Test.Case("the reloaded first scene sees only the latest parameters", function()
		Test.ExpectEqual(Scene.GetName(), "SceneLoadA")
		local parameters = Scene.GetLoadParameters()
		Test.ExpectEqual(parameters.Attempt, 3)
		Test.ExpectEqual(parameters.Origin, nil)
		local gate = Greeter("Gate")
		Test.ExpectEqual(gate.SeenAttempt, 3)
		Test.ExpectEqual(gate.SeenOrigin, "")
		Test.ExpectEqual(gate.Starts, 1)
		Test.ExpectEqual(Carry.Marker, nil)
		Test.ExpectEqual(Scene.FindByName("OnlyInB"), nil)
	end)
end)
