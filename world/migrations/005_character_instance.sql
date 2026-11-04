-- The instance copy each character was saved in (docs/world/instances.html#relog): the process-unique
-- instanceId of a live copy of an instance map, 0 = none (a world map). Copies live in memory only, so on entering, an
-- id that names no live copy of the saved map (destroyed, or from before a restart) sends the character to that map's
-- "exit". Rows from before this migration get 0.
ALTER TABLE characters ADD COLUMN instance_id INT UNSIGNED NOT NULL DEFAULT 0;
