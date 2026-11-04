# maps/

The default maps folder: `docker-compose.yml` mounts `${MAPS_DIR:-./maps}` as the world's (and the bot's) `/app/maps`,
and `.env.example` sets `MAP_ROOT=./maps` for a world run on the host.

It is empty by default apart from `start/map.json`: a flat 2048 x 2048 m map (id 1, the default `START_MAP`, spawn at
(1024, 0, 1024)) with no areas, NPC spawns, heightmaps or navmesh. The world server refuses to start without at least
one map, so this is the smallest folder the stack runs on.

To run on real maps, point `MAPS_DIR` (in `.env` or the shell) at a maps folder, `<folder>/map.json` each
(docs/world/maps.html), then `docker compose up -d`. The integration scenarios that need the sample maps
(`start` with its portals and spawns, `cave`, `hills`, `crypt`) report SKIP when they are missing; the self-check's
sample-map checks need `CHTHONIC_SAMPLE_MAPS` (CMake cache variable, default this folder) to hold all seven sample maps
(`start cave hills plains marsh crypt tilegrid`).
