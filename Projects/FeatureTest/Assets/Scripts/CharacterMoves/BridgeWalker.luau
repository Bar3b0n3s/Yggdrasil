--!strict
-- CharacterMoves.scene's Walker (Docs/Decisions/0066-m48-characters-off-the-ground-and-on-light-bodies.md; Skerries' brief
-- S19): walks along X at Speed with Move, L forwards and J back, across the bridge of thirteen hinged 25 kg planks.
export type BridgeWalker = ScriptInstance & {
	Speed: number,
}

local BridgeWalker = {}

BridgeWalker.Fields = {
	Speed = Field.Number(4.2, { Min = 0 }),
}

function BridgeWalker.OnFixedUpdate(self: BridgeWalker, dt: number)
	local character = self.Entity.CharacterController
	if character == nil then
		return
	end
	local along = (if Input.IsKeyDown("L") then 1 else 0) - (if Input.IsKeyDown("J") then 1 else 0)
	character:Move(vector.create(along * self.Speed, 0, 0))
end

return Script.Define("BridgeWalker", BridgeWalker)
