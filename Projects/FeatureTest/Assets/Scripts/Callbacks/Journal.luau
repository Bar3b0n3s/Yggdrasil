--!strict
-- The shared callback journal of Callbacks.scene. Every Recorder appends "<Label>:<event>" in the order its callbacks
-- run, so the Callbacks suite compares the observed order with Architecture §5.7 and §11.3. Module state lives in the
-- scene's VM, which the suite shares; Scene.Load or a new session starts an empty journal.
--
-- Entries are numbered from 1, the session's first, and a cursor is the number of entries recorded before it. The
-- journal holds only the latest Capacity entries, in a ring, so a long session (the hardening soak plays this scene for
-- 10,000 ticks, about 30 entries each) does not grow the script heap without end. The Callbacks suite reads at most a
-- few hundred ticks back; reading entries the ring has already dropped is an error, never a silent miss.
local Journal = {}

Journal.Capacity = 32768
-- Whether the per-phase callbacks ("fixed", "update", "late") are journalled; lifecycle and physics events always are.
Journal.TracePhases = true
-- The label of every live Recorder by entity, registered at OnCreate, so a contact partner is named without reading
-- the partner itself (a destroyed partner may no longer be read).
Journal.Labels = {} :: { [Entity]: string }
-- The latest collision contact each label received.
Journal.Contacts = {} :: { [string]: CollisionContact }
-- The simulation tick at which each entry was first recorded, so a suite can tell when a callback arrived.
Journal.Ticks = {} :: { [string]: number }

-- The ring of the latest entries, and how many entries were recorded in all.
local ring: { string } = {}
local total = 0

local function Entry(index: number): string
	return ring[(index - 1) % Journal.Capacity + 1]
end

-- The first entry after `cursor`, failing when the ring has dropped any entry after it.
local function First(cursor: number): number
	local oldest = math.max(1, total - Journal.Capacity + 1)
	if cursor + 1 < oldest then
		error(`Journal: the entries after cursor {cursor} were dropped; only the latest {Journal.Capacity} are kept`)
	end
	return cursor + 1
end

function Journal.Record(label: string, event: string)
	local entry = label .. ":" .. event
	total += 1
	ring[(total - 1) % Journal.Capacity + 1] = entry
	if Journal.Ticks[entry] == nil then
		Journal.Ticks[entry] = Time.GetTick()
	end
end

function Journal.LabelOf(entity: Entity): string
	return Journal.Labels[entity] or "?"
end

-- The number of entries recorded so far: a cursor for Since.
function Journal.Count(): number
	return total
end

-- The entries recorded after `cursor`, keeping only those that pass `keep` when it is given.
function Journal.Since(cursor: number, keep: ((string) -> boolean)?): { string }
	local result: { string } = {}
	for index = First(cursor), total do
		local entry = Entry(index)
		if keep == nil or keep(entry) then
			table.insert(result, entry)
		end
	end
	return result
end

-- Whether `entry` was recorded after `cursor`.
function Journal.Has(cursor: number, entry: string): boolean
	for index = First(cursor), total do
		if Entry(index) == entry then
			return true
		end
	end
	return false
end

-- The number of `entry` after `cursor`, or 0.
function Journal.Find(cursor: number, entry: string): number
	for index = First(cursor), total do
		if Entry(index) == entry then
			return index
		end
	end
	return 0
end

return Journal
