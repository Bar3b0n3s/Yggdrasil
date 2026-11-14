--!strict
-- The Tetris behaviour on /Game: feeds the player's input to a Match every tick and mirrors the match into the scene:
-- the 200 pooled SpriteRenderer cells of the well (each showing the empty, ghost or a piece's colour), the 16 UiImage
-- cells of the hold and next previews, the HUD's UiText values and clear banner, the game-over panel, and one-shot
-- sounds. The well cells are instantiated once in OnStart and only recoloured afterwards, and every write happens
-- only on a change. A click on the panel's RESTART button restarts like the Restart action, once per tick whichever
-- way: the click arrives in OnUiClick, before OnFixedUpdate in the same tick.
local Match = require("./Match")
local Palette = require("./Palette")
local Pieces = require("./Pieces")

export type Game = ScriptInstance & {
	CellPrefab: AssetRef?,
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
	Previews: { Entity },
	PreviewName: { string },
	Texts: { [string]: Entity },
	TextShown: { [string]: string },
	Panel: Entity?,
	PanelShown: boolean,
	HudScore: number,
	HudLevel: number,
	HudLines: number,
	ClearTimer: number,
	RestartClicked: boolean,
	Restarts: number,
}

local Game = {}

Game.Fields = {
	CellPrefab = Field.Asset("Prefab"),
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
-- A preview cell's size and the size of the hold and next boxes, in canvas units (reference pixels).
local CellPixels = 16
local BoxWidth, BoxHeight = 80, 48
-- Preview slot 1 is the hold box, slots 2 to 4 the three next boxes; each has four cells, Preview<slot>_<i>.
local PreviewSlots = 4
local ClearNames = { "SINGLE", "DOUBLE", "TRIPLE", "TETRIS!" }
local ClearShowTicks = 120
local HudTexts = { "ScoreText", "LevelText", "LinesText", "ClearText" }

local function cellPosition(column: number, row: number): vector
	return vector.create(column - 5.5, row - 10.5, 0)
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
	local text = if entity then entity.UiText else nil
	if text then
		text.Text = value
		self.TextShown[key] = value
	end
end

local function setActive(entity: Entity, active: boolean)
	if entity:IsActiveSelf() ~= active then
		entity:SetActive(active)
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

-- Shows the locked blocks, the ghost and the falling piece in the visible rows, recolouring only changed cells.
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
			local renderer = self.Cells[i].SpriteRenderer
			if renderer then
				renderer.Color = Palette.ForCell(name)
			end
			self.ShownName[i] = name
		end
	end
end

-- Shows a piece (or nothing) centred in its 80 x 48 preview box: whole-pixel offsets for every piece.
local function showPreview(self: Game, slot: number, name: string)
	if self.PreviewName[slot] == name then
		return
	end
	self.PreviewName[slot] = name
	local base = (slot - 1) * 4
	if name == "" then
		for i = 1, 4 do
			setActive(self.Previews[base + i], false)
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
	local left = (BoxWidth - (maxX - minX + 1) * CellPixels) // 2
	local top = (BoxHeight - (maxY - minY + 1) * CellPixels) // 2
	local color = Palette.ForCell(name)
	for i, block in blocks do
		local cell = self.Previews[base + i]
		local rect = cell.UiRect
		if rect then
			rect.Position = vector.create(left + (block.X - minX) * CellPixels, top + (block.Y - minY) * CellPixels, 0)
		end
		local image = cell.UiImage
		if image then
			image.Color = color
		end
		setActive(cell, true)
	end
end

local function refreshHud(self: Game)
	local match = self.Match
	if match.Score ~= self.HudScore then
		self.HudScore = match.Score
		setText(self, "ScoreText", tostring(match.Score))
	end
	if match.Level ~= self.HudLevel then
		self.HudLevel = match.Level
		setText(self, "LevelText", tostring(match.Level))
	end
	if match.Lines ~= self.HudLines then
		self.HudLines = match.Lines
		setText(self, "LinesText", tostring(match.Lines))
	end
	if match.GameOver ~= self.PanelShown then
		self.PanelShown = match.GameOver
		local panel = self.Panel
		if panel then
			setActive(panel, match.GameOver)
		end
	end
end

local function refresh(self: Game)
	refreshWell(self)
	local match = self.Match
	showPreview(self, 1, match.Hold)
	for i = 1, PreviewSlots - 1 do
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
	self.Previews = {}
	self.PreviewName = table.create(PreviewSlots, "")
	self.Texts = {}
	self.TextShown = {}
	self.HudScore = -1
	self.HudLevel = -1
	self.HudLines = -1
	self.ClearTimer = 0
	self.RestartClicked = false
	self.Restarts = 0

	for _, key in HudTexts do
		local found = Scene.FindByName(key)
		if found then
			self.Texts[key] = found
			local text = found.UiText
			self.TextShown[key] = if text then text.Text else ""
		end
	end
	local panel = Scene.FindByName("GameOverPanel")
	self.Panel = panel
	self.PanelShown = if panel then panel:IsActiveSelf() else false

	for slot = 1, PreviewSlots do
		for i = 1, 4 do
			local cell = Scene.FindByName(`Preview{slot}_{i}`)
			if cell == nil then
				Log.Error(`Game: Main.scene has no Preview{slot}_{i}`)
				return
			end
			table.insert(self.Previews, cell)
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
			table.insert(self.Cells, cell)
		end
	end
	refresh(self)
end

-- The RESTART button on the game-over panel; the click bubbles here from the button through the HUD.
function Game.OnUiClick(self: Game, element: Entity): boolean?
	if element.Name ~= "RestartButton" then
		return nil
	end
	self.RestartClicked = true
	return true
end

function Game.OnFixedUpdate(self: Game, _dt: number)
	if #self.Cells == 0 then
		return
	end
	local match = self.Match
	local restart = Input.IsActionPressed("Restart") or self.RestartClicked
	self.RestartClicked = false
	if restart then
		match:Reset()
		self.Restarts += 1
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
