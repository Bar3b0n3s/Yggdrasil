--!strict
return Test.Suite("{{Name}}", function()
	Test.Case("checks an invariant", function()
		Test.ExpectEqual(2 + 2, 4)
	end)
end)
