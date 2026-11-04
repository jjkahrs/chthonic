-- Lua components with persist = true, per character: {"<Name>": {...}, ...} (docs/world/lua.html).
-- Entries with no current persisted definition are written back unchanged, so a removed definition loses no data.
ALTER TABLE characters ADD COLUMN components JSON NULL;
