--!strict
-- The Log module (§11.5): every level and print write entries without raising a script error or failing the case.
return Test.Suite("Api.Log", function()
	Test.Case("every level logs without raising a script error", function()
		local errors = #Test.GetScriptErrors()
		Log.Trace("FeatureTest trace", 1, true)
		Log.Info("FeatureTest info", vector.create(1, 2, 3))
		Log.Warn("FeatureTest warning", nil)
		Log.Error("FeatureTest error entry (expected)", Color.New(1, 0, 0))
		print("FeatureTest print", 42)
		local custom = setmetatable({}, {
			__tostring = function(): string
				return "custom value"
			end,
		})
		Log.Info(custom)
		Test.ExpectEqual(#Test.GetScriptErrors(), errors, "logging never raises a script error")
	end)
end)
