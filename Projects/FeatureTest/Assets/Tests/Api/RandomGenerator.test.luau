--!strict
-- The RandomGenerator type (§11.5): independent seeded streams that never touch the session's randomness.
return Test.Suite("Api.RandomGenerator", function()
	Test.Case("generators with one seed agree and leave the session stream alone", function()
		Random.Seed(7)
		local session = Random.Integer(1, 1000000)
		Random.Seed(7)
		local a, b = Random.New(99), Random.New(5)
		b:Seed(99)
		for _ = 1, 4 do
			Test.ExpectEqual(a:Integer(0, 1000), b:Integer(0, 1000))
		end
		Test.ExpectEqual(a:Number(1, 2), b:Number(1, 2))
		Test.ExpectEqual(a:Bool(), b:Bool())
		Test.ExpectEqual(a:Choice({ 10, 20, 30 }), b:Choice({ 10, 20, 30 }))
		local left, right = { "x", "y", "z", "w" }, { "x", "y", "z", "w" }
		Test.Expect(a:Shuffle(left) == left, "a shuffle works in place")
		b:Shuffle(right)
		Test.ExpectEqual(table.concat(left), table.concat(right))
		Test.ExpectEqual(a:UnitVector(), b:UnitVector())
		Test.ExpectEqual(Random.Integer(1, 1000000), session, "local draws never advance the session stream")
		Test.ExpectEqual(a:Bool(1), true)
		Test.ExpectEqual(a:Bool(0), false)
		Test.ExpectEqual(a:Integer(3, 3), 3)
	end)
end)
