--!strict
-- The project's input action names (ADR 0038 decision 1), defined once: the game reads them and the suites inject
-- them only through this table, so a renamed action is one edit and a misspelt one a type error, never a silently
-- unknown action.
local Actions = table.freeze({
	MoveX = "MoveX",
	MoveZ = "MoveZ",
	Restart = "Restart",
})

return Actions
