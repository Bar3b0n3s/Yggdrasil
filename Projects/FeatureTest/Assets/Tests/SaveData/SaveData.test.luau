--!strict
-- M28's save data and the session's audio mix (Docs/Decisions/0043-m28-game-services.md decisions 8 and 9) in SaveData.scene
-- (SaveData.jsonl, IdSeed 2502), in every run mode: a test run keeps its store in memory, empty until Test.SetSaveData
-- seeds it; the store and the group volumes last across Scene.Reload, and a seeded store opens the gated branch. The cases
-- share one play session (Isolation Suite), so the last two see one scene replacement: a case that requests it returns
-- in the requesting tick, and the next case checks the reloaded scene.
local GateScript = require("../../Scripts/SaveData/Gate")

local function gate(): Entity
	local entity = Scene.FindByName("Gate")
	assert(entity, "the scene holds Gate")
	return entity
end

return Test.Suite("SaveData", function()
	Test.Case("a test run starts with an empty store, reads and writes it, and commits in memory", function()
		Test.ExpectEqual(#SaveData.GetKeys(), 0)
		Test.ExpectEqual(SaveData.GetLoadError(), nil)
		SaveData.Set("Options", { Volume = 0.5, Keys = { "W", "A", "S", "D" } })
		SaveData.Set("Name", "Ash")
		Test.ExpectEqual(SaveData.Get("Name"), "Ash")
		Test.ExpectEqual((SaveData.Get("Options") :: any).Keys[4], "D")
		Test.ExpectEqual(SaveData.GetKeys()[1], "Name")
		Test.ExpectEqual(SaveData.Delete("Name"), true)
		Test.ExpectEqual(SaveData.Delete("Name"), false)
		local committed, reason = SaveData.Commit()
		Test.ExpectEqual(committed, true)
		Test.ExpectEqual(reason, nil)
	end)

	Test.Case("keys, values and sizes the store refuses raise", function()
		Test.SetSaveData({})
		Test.ExpectEqual((pcall(function()
			SaveData.Set("", 1)
		end)), false)
		Test.ExpectEqual((pcall(function()
			SaveData.Set(string.rep("k", 257), 1)
		end)), false)
		-- The cap counts the whole store: a value that fits alone does not fit beside another.
		SaveData.Set("A", string.rep("x", 600000))
		Test.ExpectEqual((pcall(function()
			SaveData.Set("B", string.rep("x", 600000))
		end)), false)
		Test.ExpectEqual(#SaveData.GetKeys(), 1)
	end)

	Test.Case("the store joins the state hash", function()
		Test.SetSaveData({})
		local hash = Test.GetStateHash()
		Test.ExpectEqual(Test.GetStateHash(), hash)
		SaveData.Set("Realm", 1)
		Test.Expect(Test.GetStateHash() ~= hash, "the save-data block follows the store")
	end)

	Test.Case("a seeded store and the group volumes set before Scene.Reload", function()
		local closed = gate():GetScript("Gate") :: GateScript.Gate
		Test.ExpectEqual(closed.Open, false)
		Test.SetSaveData({ Realm = 2 })
		Test.ExpectEqual(SaveData.Get("Realm"), 2)
		Audio.SetGroupVolume("Music", 0.25)
		Audio.SetGroupVolume("Ui", 0.5)
		-- The replacement comes at the end of this tick: the case returns now, and the next one sees the reloaded scene.
		Scene.Reload()
	end)

	Test.Case("after Scene.Reload the seeded store opens the gated branch and a group volume set before it holds", function()
		Test.WaitTicks(2)
		local opened = gate():GetScript("Gate") :: GateScript.Gate
		Test.ExpectEqual(opened.Open, true)
		Test.ExpectEqual(SaveData.Get("Realm"), 2)
		Test.Expect((SaveData.Get("Visits") :: number) >= 1, "the open gate counts its visits in the store")
		Test.ExpectEqual(Audio.GetGroupVolume("Music"), 0.25)
		Test.ExpectEqual(Audio.GetGroupVolume("Ui"), 0.5)
		Test.ExpectEqual(Audio.GetGroupVolume("Sfx"), 1)
	end)
end)
