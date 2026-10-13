--!strict
-- Lives in Crate.prefab: tags its entity in OnCreate and keeps a reference to the prefab's Lid, which every instance
-- remaps to its own member (§5.5).
export type Tagger = ScriptInstance & {
	Tag: string,
	Lid: Entity?,
	CreatedTick: number,
}

local Tagger = {}

Tagger.Fields = {
	Tag = Field.String("Tagged"),
	Lid = Field.Entity(),
}

function Tagger.OnCreate(self: Tagger)
	self.Entity:AddTag(self.Tag)
	self.CreatedTick = Time.GetTick()
end

return Script.Define("Tagger", Tagger)
