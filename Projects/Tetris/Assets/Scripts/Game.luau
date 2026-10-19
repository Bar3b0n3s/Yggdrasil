--!strict
-- The Tetris behaviour: feeds the player's input to a Match every tick and mirrors the match into the pooled
-- cells of the well, the hold and next previews, the HUD texts and one-shot sounds. Every cell is created once
-- in OnStart and only shown, hidden or recoloured afterwards, and every write happens only on a change.
local Match = require("./Match")
local Pieces = require("./Pieces")

export type Game = ScriptInstance & {
	CellPrefab: AssetRef?,
	Materials: { AssetRef? },
	MoveSound: AssetRef?,
	RotateSound: AssetRef?,
	LockSound: AssetRef?,
	DropSound: AssetRef?,
	ClearSound: AssetRef?,
	TetrisSound: AssetRef?,
	HoldSound: AssetRef?,
	LevelSound: AssetRef?,
	GameOverSound: AssetRef?,
	Match: Match.Match,
	Controls: Match.Controls,
	Cells: { Entity },
	Display: { string },
	ShownName: { string },
	ShownMaterial: { string },
	Previews: { Entity },
	PreviewName: { string },
	Texts: { [string]: Entity },
	TextShown: { [string]: string },
	HudScore: number,
	HudLevel: number,
	HudLines: number,
	ClearTimer: number,
}

local Game = {}

Game.Fields = {
	CellPrefab = Field.Asset("Prefab"),
	-- The materials of the I, O, T, S, Z, J and L blocks, then of the ghost piece and of any other block.
	Materials = Field.Array(Field.Asset("Material")),
	MoveSound = Field.Asset("AudioClip"),
	RotateSound = Field.Asset("AudioClip"),
	LockSound = Field.Asset("AudioClip"),
	DropSound = Field.Asset("AudioClip"),
	ClearSound = Field.Asset("AudioClip"),
	TetrisSound = Field.Asset("AudioClip"),
	HoldSound = Field.Asset("AudioClip"),
	LevelSound = Field.Asset("AudioClip"),
	GameOverSound = Field.Asset("AudioClip"),
}

local Columns, Rows = 10, 20
local CellScale = 0.94
local PreviewScale = 0.75
-- World centres (x, y) of the preview slots: the hold, then the three next pieces.
local PreviewSlots: { { number } } = { { -7.5, 7.5 }, { 7.5, 7.5 }, { 7.5, 4.5 }, { 7.5, 1.5 } }
local MaterialIndex: { [string]: number } = { I = 1, O = 2, T = 3, S = 4, Z = 5, J = 6, L = 7, Ghost = 8 }
local OtherMaterial = 9
local ClearNames = { "SINGLE", "DOUBLE", "TRIPLE", "TETRIS!" }
local ClearShowTicks = 120
local HudTexts = { "ScoreText", "LevelText", "LinesText", "ClearText", "StatusText" }
local GameOverMessage = "GAME OVER\nPress Enter to restart"

local function cellPosition(column: number, row: number): vector
	return vector.create(column - 5.5, row - 10.5, 0)
end

local function materialFor(self: Game, name: string): AssetRef?
	return self.Materials[MaterialIndex[name] or OtherMaterial]
end

local function play(clip: AssetRef?)
	if clip then
		Audio.PlayOneShot(clip)
	end
end

local function setText(self: Game, key: string, value: string)
	if self.TextShown[key] == value then
		return
	end
	local entity = self.Texts[key]
	local text = if entity then entity.Text else nil
	if text then
		text.Text = value
		self.TextShown[key] = value
	end
end

local function onEvent(self: Game, event: string)
	local match = self.Match
	if event == "Move" then
		play(self.MoveSound)
	elseif event == "Rotate" then
		play(self.RotateSound)
	elseif event == "HardDrop" then
		play(self.DropSound)
	elseif event == "Lock" then
		if table.find(match.Events, "HardDrop") == nil then
			play(self.LockSound)
		end
	elseif event == "Hold" then
		play(self.HoldSound)
	elseif event == "LevelUp" then
		play(self.LevelSound)
	elseif event == "GameOver" then
		play(self.GameOverSound)
	elseif event == "Restart" then
		self.ClearTimer = 0
		setText(self, "ClearText", "")
	elseif string.sub(event, 1, 5) == "Clear" then
		local lines = math.clamp(tonumber(string.sub(event, 6)) or 1, 1, 4)
		play(if lines == 4 then self.TetrisSound else self.ClearSound)
		setText(self, "ClearText", `{ClearNames[lines]}\n+{match.LastClearPoints}`)
		self.ClearTimer = ClearShowTicks
	end
end

-- Shows the locked blocks, the ghost and the falling piece in the visible rows, touching only changed cells.
local function refreshWell(self: Game)
	local match = self.Match
	local display = self.Display
	local board = match.Board.Cells
	for i = 1, Columns * Rows do
		display[i] = board[i]
	end
	if not match.GameOver and match.Piece ~= "" then
		local blocks = Pieces.Get(match.Piece).States[match.State + 1]
		local ghostTop = match.Top - match:DropDistance()
		for _, block in blocks do
			local column, row = match.Left + block.X, ghostTop - block.Y
			if row >= 1 and row <= Rows and display[(row - 1) * Columns + column] == "" then
				display[(row - 1) * Columns + column] = "Ghost"
			end
		end
		for _, block in blocks do
			local column, row = match.Left + block.X, match.Top - block.Y
			if row >= 1 and row <= Rows then
				display[(row - 1) * Columns + column] = match.Piece
			end
		end
	end
	for i = 1, Columns * Rows do
		local name = display[i]
		if name ~= self.ShownName[i] then
			local cell = self.Cells[i]
			if name == "" then
				cell:SetActive(false)
			else
				if name ~= self.ShownMaterial[i] then
					local renderer = cell.MeshRenderer
					if renderer then
						renderer:SetMaterial(1, materialFor(self, name))
					end
					self.ShownMaterial[i] = name
				end
				cell:SetActive(true)
			end
			self.ShownName[i] = name
		end
	end
end

-- Shows a piece (or nothing) centred in a preview slot.
local function showPreview(self: Game, slot: number, name: string)
	if self.PreviewName[slot] == name then
		return
	end
	self.PreviewName[slot] = name
	local base = (slot - 1) * 4
	if name == "" then
		for i = 1, 4 do
			self.Previews[base + i]:SetActive(false)
		end
		return
	end
	local blocks = Pieces.Get(name).States[1]
	local minX: number = 99
	local maxX: number = -99
	local minY: number = 99
	local maxY: number = -99
	for _, block in blocks do
		minX = math.min(minX, block.X)
		maxX = math.max(maxX, block.X)
		minY = math.min(minY, block.Y)
		maxY = math.max(maxY, block.Y)
	end
	local centreX: number = (minX + maxX) / 2
	local centreY: number = (minY + maxY) / 2
	local slotX: number = PreviewSlots[slot][1]
	local slotY: number = PreviewSlots[slot][2]
	local material = materialFor(self, name)
	for i, block in blocks do
		local cell = self.Previews[base + i]
		local x = slotX + (block.X - centreX) * PreviewScale
		local y = slotY - (block.Y - centreY) * PreviewScale
		cell.Transform:Teleport(vector.create(x, y, 0))
		local renderer = cell.MeshRenderer
		if renderer then
			renderer:SetMaterial(1, material)
		end
		cell:SetActive(true)
	end
end

local function refreshHud(self: Game)
	local match = self.Match
	if match.Score ~= self.HudScore then
		self.HudScore = match.Score
		setText(self, "ScoreText", `SCORE\n{match.Score}`)
	end
	if match.Level ~= self.HudLevel then
		self.HudLevel = match.Level
		setText(self, "LevelText", `LEVEL\n{match.Level}`)
	end
	if match.Lines ~= self.HudLines then
		self.HudLines = match.Lines
		setText(self, "LinesText", `LINES\n{match.Lines}`)
	end
	setText(self, "StatusText", if match.GameOver then GameOverMessage else "")
end

local function refresh(self: Game)
	refreshWell(self)
	local match = self.Match
	showPreview(self, 1, match.Hold)
	for i = 1, #PreviewSlots - 1 do
		showPreview(self, i + 1, match.Queue[i] or "")
	end
	refreshHud(self)
end

function Game.OnStart(self: Game)
	self.Match = Match.New(Random.New(Random.Integer(0, 2147483647)))
	self.Controls = Match.NewControls()
	self.Cells = {}
	self.Display = table.create(Columns * Rows, "")
	self.ShownName = table.create(Columns * Rows, "")
	self.ShownMaterial = table.create(Columns * Rows, "")
	self.Previews = {}
	self.PreviewName = {}
	self.Texts = {}
	self.TextShown = {}
	self.HudScore = -1
	self.HudLevel = -1
	self.HudLines = -1
	self.ClearTimer = 0

	for _, key in HudTexts do
		local found = Scene.FindByName(key)
		if found then
			self.Texts[key] = found
			local text = found.Text
			self.TextShown[key] = if text then text.Text else ""
		end
	end

	local prefab = self.CellPrefab
	if prefab == nil then
		Log.Error("Game: CellPrefab is not set")
		return
	end
	local cellsParent = self.Entity:FindChild("Cells") or self.Entity
	for row = 1, Rows do
		for column = 1, Columns do
			local cell = Scene.Instantiate(prefab, cellPosition(column, row), nil, cellsParent)
			cell.Name = string.format("Cell%02d_%02d", row, column)
			cell:SetActive(false)
			table.insert(self.Cells, cell)
		end
	end
	local previewParent = self.Entity:FindChild("Previews") or self.Entity
	local previewSize = CellScale * PreviewScale
	for slot, centre in PreviewSlots do
		for i = 1, 4 do
			local cell = Scene.Instantiate(prefab, vector.create(centre[1], centre[2], 0), nil, previewParent)
			cell.Name = string.format("Preview%d_%d", slot, i)
			cell.Transform.Scale = vector.create(previewSize, previewSize, previewSize)
			cell:SetActive(false)
			table.insert(self.Previews, cell)
		end
		self.PreviewName[slot] = ""
	end
	refresh(self)
end

function Game.OnFixedUpdate(self: Game, _dt: number)
	if #self.Cells == 0 then
		return
	end
	local match = self.Match
	if Input.IsActionPressed("Restart") then
		match:Reset()
	else
		local controls = self.Controls
		controls.Left = Input.IsActionDown("MoveLeft")
		controls.Right = Input.IsActionDown("MoveRight")
		controls.LeftPressed = Input.IsActionPressed("MoveLeft")
		controls.RightPressed = Input.IsActionPressed("MoveRight")
		controls.SoftDrop = Input.IsActionDown("SoftDrop")
		controls.HardDrop = Input.IsActionPressed("HardDrop")
		controls.RotateCW = Input.IsActionPressed("RotateCW")
		controls.RotateCCW = Input.IsActionPressed("RotateCCW")
		controls.Hold = Input.IsActionPressed("Hold")
		match:Step(controls)
	end
	for _, event in match.Events do
		onEvent(self, event)
	end
	if self.ClearTimer > 0 then
		self.ClearTimer -= 1
		if self.ClearTimer == 0 then
			setText(self, "ClearText", "")
		end
	end
	refresh(self)
end

return Script.Define("Game", Game)
