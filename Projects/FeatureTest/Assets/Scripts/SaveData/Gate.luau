--!strict
-- The gated realm of M28's FeatureTest coverage (Docs/Decisions/0043-m28-game-services.md decision 8, SaveData.jsonl, IdSeed
-- 2502): the save data a game starts with opens a branch. At its creation it reads the store: a saved Realm of 2 or more
-- opens the gate (the entity's Transform rises, which the state hash covers); every fixed step counts its visits in the
-- store and a Checkpoint signal saves the count, so a replay of it verifies only when every playback starts with the store
-- its recording started with and plays its signal back.
export type Gate = ScriptInstance & {
	Open: boolean,
}

local Gate = {}

function Gate.OnCreate(self: Gate)
	local realm = SaveData.Get("Realm")
	self.Open = type(realm) == "number" and realm >= 2
	if self.Open then
		self.Entity.Transform:Translate(vector.create(0, 3, 0))
	end
end

function Gate.OnFixedUpdate(self: Gate)
	if not self.Open then
		return
	end
	local visits = SaveData.Get("Visits")
	local count = (if type(visits) == "number" then visits else 0) + 1
	SaveData.Set("Visits", count)
	-- A checkpoint the player reached (a recorded signal) is saved with the visits so far.
	if Input.WasSignalled("Checkpoint") then
		SaveData.Set("Checkpoint", count)
	end
end

return Script.Define("Gate", Gate)
