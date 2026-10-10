--!strict
return Test.Suite("FixtureTest", function()
	Test.Case("checks an invariant", function()
		Test.ExpectEqual(2 + 2, 4)
	end)
end)
