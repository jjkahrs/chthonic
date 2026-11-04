# Map Bake

`chthonic_mapbake` lints a zone map folder (height seams, spawn, object and interactable files) and bakes one Recast
navtile per declared tile, with its obstacles and interactables cut out, into
`<mapDir>/.server/nav/` for the world server's navmesh. Exit 0: all baked; 1: lint or bake errors (nothing written);
2: usage.

Full documentation: [docs/index.html](docs/index.html).

## Build

Built with the world server, from `world` in a VS 2022 x64 developer prompt with `VCPKG_ROOT` set (see
[World Server setup](../../../docs/world/setup.html)):

```bat
cmake --preset debug
cmake --build --preset debug
```

Output: `world/build/debug/chthonic_mapbake.exe`. The Docker image (`docker compose build`) contains it too.

## Run

```sh
chthonic_mapbake <mapDir> [--out <dir>]
build/debug/chthonic_mapbake.exe maps/hills      # from world, with the sample maps there
```
