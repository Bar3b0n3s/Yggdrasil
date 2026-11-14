--!strict
-- Graphs.scene (Docs/Decisions/0035-m24-contract.md decision 16): four visual-scripting graph behaviours, which
-- Scaffold/Graphs.jsonl builds through the graph.* methods alone and every mode runs as their cooked Luau. Together they
-- use every built-in node kind, a generated kind of each category (Event, Call, Get, Set, Const) and every member kind
-- as a field or a variable, and their script slots override fields of several kinds. A graph requires no module and no
-- script requires it, so a suite reads its instance through Entity:GetScript, typed any. Also driven by
-- Assets/Tests/Replays/Graphs.replay (Scaffold/GraphReplay.jsonl), whose Rover moves and jumps only with its input.
-- The last case reads the live-value table that every graph's transpiled text fills (decision 14), which the editor's
-- graph.liveValues serves, so each mode shows that the values the editor would show are in its cooked scripts.
local Support = require("../Api/Support")

-- The behaviour `behaviour` of the entity named `entity`.
local function Instance(entity: string, behaviour: string): any
	local instance = Support.Find(entity):GetScript(behaviour)
	Test.Expect(instance ~= nil, entity .. " runs the graph behaviour " .. behaviour)
	return instance
end

return Test.Suite("Graphs", function()
	Test.Case("each graph is a Script asset that its slot runs as a behaviour", function()
		local graphs: { { Name: string, Asset: AssetRef? } } = {
			{ Name = "Arithmetic", Asset = Assets.Load("Assets/Scripts/Graphs/Arithmetic.graph") },
			{ Name = "Vectors", Asset = Assets.Load("Assets/Scripts/Graphs/Vectors.graph") },
			{ Name = "Flows", Asset = Assets.Load("Assets/Scripts/Graphs/Flows.graph") },
			{ Name = "Steering", Asset = Assets.Load("Assets/Scripts/Graphs/Steering.graph") },
		}
		for _, graph in graphs do
			local asset = graph.Asset
			Test.Expect(asset ~= nil, graph.Name .. ".graph loads")
			if asset ~= nil then
				Test.ExpectEqual(Assets.GetType(asset), "Script", graph.Name .. ".graph is a Script asset")
			end
		end
		Test.WaitTicks(1)
		Instance("Calculator", "Arithmetic")
		Instance("Vectorizer", "Vectors")
		Instance("Flow", "Flows")
		Instance("Rover", "Steering")
	end)

	Test.Case("the arithmetic graph computes its operators from its fields and the slot's overrides", function()
		Test.WaitTicks(1)
		local arithmetic = Instance("Calculator", "Arithmetic")
		Test.ExpectEqual(arithmetic.A, 9.5, "the slot overrides the Number field")
		Test.ExpectEqual(arithmetic.B, 4, "the slot overrides the Integer field")
		Test.ExpectEqual(arithmetic.Label, "Result ", "the String field keeps its default")
		Test.ExpectEqual(arithmetic.Mode, "Down", "the slot overrides the Enum field")
		-- -(A % ((((A + B) - B) * B) / A)): every Number operator once.
		Test.ExpectEqual(arithmetic.Result, -1.5, "Number.Add, Subtract, Multiply, Divide, Modulo and Negate")
		-- Every comparison and logic operator decides the branch, which only its True flow sets to this text.
		Test.ExpectEqual(arithmetic.Text, "Result -1.5", "the branch on the comparisons took True, and Text.Concat")
	end)

	Test.Case("the vector graph computes the vector operators and moves and turns its entity", function()
		Test.WaitTicks(1)
		local vectors = Instance("Vectorizer", "Vectors")
		-- With D the Direction field's default (1, 2, 2) and M = (1, 2, 3): D + M - D x M = (0, 5, 5), so Mixed is
		-- (Dot(Normalize((0, 10, 10)), D), Length((0, 5, 5)), (D x M).y).
		Test.ExpectNear(vectors.Mixed, vector.create(2 * math.sqrt(2), 5 * math.sqrt(2), -1), 1e-5,
			"Vector.Make, Add, Cross, Subtract, Scale, Normalize, Dot, Length and Split")
		Test.ExpectEqual(vectors.Turn, Math.Pi, "Const.Math.Pi")
		Support.ExpectColorNear(vectors.Shade, Color.New(0.2, 0.4, 0.6, 1), 1e-6, "the slot overrides the Color field")
		local transform = Support.Find("Vectorizer").Transform
		Test.ExpectEqual(transform.Translation, vector.create(1, 2, -1), "Set.Transform.Translation added M to its start")
		Support.ExpectQuatNear(transform.Rotation, Quat.New(0, 0.70710677, 0, 0.70710677), 1e-5,
			"Set.Transform.Rotation took the slot's Quat field")
	end)

	Test.Case("the flow graph loops, checks its Entity field for nil and delays a flow", function()
		Test.WaitTicks(1)
		local flows = Instance("Flow", "Flows")
		Test.ExpectEqual(flows.RangeTotal, 10, "Flow.ForRange from 1 to 4")
		Test.ExpectEqual(flows.EachTotal, 6, "Flow.ForEach over the slot's Array field")
		Test.ExpectEqual(flows.Target, Support.Find("Target"), "the slot sets the Entity field")
		Test.ExpectEqual(flows.TargetName, "Target", "Flow.IfNotNil took True and passed the Entity on")
		Test.ExpectEqual(flows.Picked, Assets.Load("Assets/Textures/Tilemaps/Rgba.png"), "the slot's Asset field into an Asset variable")
		Test.ExpectEqual(flows.Waiting, true, "Flow.Delay continued at Then at once")
		Test.ExpectEqual(flows.Fired, false, "Flow.Delay's Later waits its Seconds")
		Test.WaitTicks(24)
		Test.ExpectEqual(flows.Fired, true, "Flow.Delay's Later ran after 0.25 s of simulation time")
		Test.Expect(flows.FiredTick >= 16, "Call.Time.GetTick read the tick Later ran at")
	end)

	Test.Case("the steering graph reads input: MoveX moves the Rover and each Jump press counts", function()
		Test.WaitTicks(1)
		local rover = Support.Find("Rover")
		local steering = Instance("Rover", "Steering")
		local start = rover.Transform.Translation
		Test.WaitTicks(8)
		Test.ExpectEqual(rover.Transform.Translation, start, "without input the Rover stays")
		Test.ExpectEqual(steering.Jumps, 0)
		Test.InjectAction("MoveX", nil, 1)
		Test.WaitTicks(32)
		Test.InjectAction("MoveX", nil, 0)
		Test.WaitTicks(2)
		local moved = rover.Transform.Translation - start
		Test.ExpectNear(moved.x, 1.5, 0.1, "the slot's Speed of 3 units per second for half a second")
		Test.ExpectEqual(moved.y, 0)
		Test.ExpectEqual(moved.z, 0)
		Test.InjectAction("Jump", "Tap")
		Test.WaitTicks(2)
		Test.ExpectEqual(steering.Jumps, 1, "Flow.Branch on Input.IsActionPressed")
	end)

	Test.Case("every value a graph computes is also its live value, under its node and pin", function()
		Test.WaitTicks(1)
		local arithmetic = Instance("Calculator", "Arithmetic").__GraphLive
		Test.ExpectEqual(arithmetic["9.Result"], -1.5, "Number.Negate's Result, a pure node's output")
		Test.ExpectEqual(arithmetic["23.Result"], true, "Logic.And's Result, which the branch read")
		Test.ExpectEqual(arithmetic["27.Result"], "Result -1.5", "Text.Concat's Result, read on the branch's True flow")
		local flows = Instance("Flow", "Flows").__GraphLive
		Test.ExpectEqual(flows["2.Index"], 4, "Flow.ForRange's Index, a statement's output, holds its last run's value")
		Test.ExpectEqual(flows["11.Result"], Support.Find("Target"), "Flow.IfNotNil's Result, the Entity field's value")
		Test.ExpectEqual(flows["13.Value"], "Target", "Get.Entity.Name's Value, on the True flow")
	end)
end)
