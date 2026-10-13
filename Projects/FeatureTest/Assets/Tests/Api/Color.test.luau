--!strict
-- The Color type (§11.5): constructors, interpolation, channels and exact equality.
return Test.Suite("Api.Color", function()
	Test.Case("colors are built from channels and hex, interpolate and compare exactly", function()
		local c = Color.New(1, 2, 3)
		Test.Expect(c.r == 1 and c.g == 2 and c.b == 3 and c.a == 1, "alpha defaults to one; HDR channels are kept")
		c.r = 0.5
		c.g = 0.25
		c.b = 0
		c.a = 0.5
		Test.Expect(c.r == 0.5 and c.g == 0.25 and c.b == 0 and c.a == 0.5, "channels are writable")
		local half = Color.Lerp(Color.New(0, 0, 0, 0), Color.New(1, 2, 4, 1), 0.5)
		Test.Expect(half == Color.New(0.5, 1, 2, 0.5), "lerp interpolates every channel")
		Test.Expect(Color.Lerp(c, half, 2) == half, "t is clamped to one")
		local hex = Color.FromHex("#ff8000")
		Test.ExpectEqual(hex.r, 1)
		Test.ExpectNear(hex.g, 128 / 255, 1e-7)
		Test.ExpectEqual(hex.b, 0)
		Test.ExpectEqual(hex.a, 1)
		Test.ExpectNear(Color.FromHex("#00000040").a, 64 / 255, 1e-7)
		Test.Expect(Color.New(1, 0, 0) == Color.FromHex("#FF0000"), "equal channels compare equal")
		Test.Expect(Color.New(1, 0, 0) ~= Color.New(1, 0, 0, 0.5), "alpha takes part in equality")
		Test.Expect(not pcall(Color.FromHex, "#12345"), "malformed hex is refused")
	end)
end)
