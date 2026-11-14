--!strict
-- The game's colours (ADR 0036 decision 4), all linear, as every colour field and Color.New take them. A bevel
-- colour is its target face colour over the bevel texture's face texel (sRGB 204, linear 0.6038), which multiplies
-- it; flat colours (the frame, texts, the background) show as they are.
local Palette = {}

local pieces: { [string]: Color } = {
	I = Color.New(0.0, 0.7559, 1.0),
	O = Color.New(1.0, 0.7559, 0.0),
	T = Color.New(0.5051, 0.0748, 1.0),
	S = Color.New(0.0748, 0.7559, 0.0748),
	Z = Color.New(1.0, 0.0528, 0.0528),
	J = Color.New(0.0351, 0.1329, 1.0),
	L = Color.New(1.0, 0.2582, 0.0116),
}

-- The seven pieces' bevel colours, by piece name.
Palette.Pieces = table.freeze(pieces)
-- Bevel colours: blocks the tests set up, the ghost piece, an empty well cell, the HUD boxes and the button.
Palette.Other = Color.New(0.3111, 0.3111, 0.3697)
Palette.Ghost = Color.New(0.1014, 0.1262, 0.211)
Palette.Empty = Color.New(0.0192, 0.0239, 0.0417)
Palette.Panel = Color.New(0.0351, 0.0452, 0.0849)
Palette.Button = Color.New(0.9565, 0.5051, 0.0351)
-- Flat colours: the clear and letterbox colour, the frame and labels, the accent and the values.
Palette.Background = Color.New(0.0037, 0.0044, 0.008)
Palette.Frame = Color.New(0.305, 0.3325, 0.4342)
Palette.Accent = Color.New(0.8714, 0.4564, 0.0212)
Palette.White = Color.New(1.0, 1.0, 1.0)

-- The colour a well cell or preview cell shows for a display name: "" empty, "Ghost", a piece's name, or any other
-- block (the letters the tests' SetRow patterns leave).
function Palette.ForCell(name: string): Color
	if name == "" then
		return Palette.Empty
	elseif name == "Ghost" then
		return Palette.Ghost
	end
	return Palette.Pieces[name] or Palette.Other
end

return Palette
