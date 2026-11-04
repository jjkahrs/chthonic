-- The map each character is on (map.json "id"). Rows from before this migration get 0 = the old single zone, which
-- the server reads as START_MAP. A character whose map is not loaded (or whose position is off it) enters at
-- START_MAP's spawn.
ALTER TABLE characters ADD COLUMN map_id SMALLINT UNSIGNED NOT NULL DEFAULT 0;
