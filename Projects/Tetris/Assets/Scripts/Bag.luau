--!strict
-- The 7-bag randomizer: every run of seven draws holds each piece once. It draws from a generator its caller
-- creates in a callback (Random is unavailable at a module's top level).
local Pieces = require("./Pieces")

export type Bag = {
	Generator: RandomGenerator,
	Pending: { string },
	Draw: (self: Bag) -> string,
	Reset: (self: Bag) -> (),
}

local function draw(self: Bag): string
	if #self.Pending == 0 then
		self.Pending = self.Generator:Shuffle(table.clone(Pieces.Names))
	end
	return table.remove(self.Pending, 1) :: string
end

-- Drops the rest of the current bag, so the next draw starts a fresh one.
local function reset(self: Bag)
	table.clear(self.Pending)
end

local Bag = {}

function Bag.New(generator: RandomGenerator): Bag
	local bag: Bag = { Generator = generator, Pending = {}, Draw = draw, Reset = reset }
	return bag
end

return Bag
