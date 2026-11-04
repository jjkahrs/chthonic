-- The options each character was created with (CharCreate options: {"<key>": "<value>", ...}, checked by the server's
-- C++ and Lua; docs/world/lua.html#character-creation). Rows from before this migration get NULL = none.
ALTER TABLE characters ADD COLUMN options JSON NULL;
