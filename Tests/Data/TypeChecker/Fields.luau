--!strict
local Fields = {}
Fields.Fields = {
	Number = Field.Number(),
	Integer = Field.Integer(),
	Bool = Field.Bool(),
	String = Field.String(),
	Vector = Field.Vector(),
	Color = Field.Color(),
	Quat = Field.Quat(),
	Target = Field.Entity(),
	Asset = Field.Asset("AudioClip"),
	Enum = Field.Enum({ "First", "Second" }),
	Array = Field.Array(Field.Array(Field.Number(0, { Min = 0, Max = 1, Step = 0.25 }))),
}
return Script.Define("Fields", Fields)
