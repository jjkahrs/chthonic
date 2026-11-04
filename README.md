# Chthonic

The backend of an MMORPG: four services that run together as one Docker Compose stack.

| Service | Folder | What it does | Docs |
|---|---|---|---|
| Auth server | `auth/` | Node 24 + TypeScript + Express + MySQL: accounts, sessions, the realm list, JWT access tokens and their JWKS | [docs/auth](docs/auth/index.html) |
| World server | `world/` | C++20 (CMake + vcpkg): TLS game connections, maps, server Lua (`chthonic.*` API), NPCs, persistence; includes the headless test bot and the `chthonic_mapbake` navmesh baker | [docs/world](docs/world/index.html) |
| Chat service | `chat/` | C++20: whispers, channels, zone chat, GM mutes; compiles `world/`'s protocol and shared server code | [docs/chat](docs/chat/index.html) |
| Admin app | `admin/` | Node/TypeScript API + React SPA: accounts, characters, realms, who is online, kicks, the audit log | [docs/admin](docs/admin/index.html) |

```
auth/  admin/  chat/  world/      the services (each with its own .env.example)
docs/<service>/index.html         the documentation (HTML; start at docs/index.html)
world/maps/                       the default maps folder (MAPS_DIR)
```

## Prerequisites

- Docker (Docker Desktop on Windows) for the stacks.
- Node 24 for building and testing `auth/` and `admin/` outside Docker.
- To build `world/` and `chat/` on Windows: Visual Studio 2022 with the C++ workload (MSVC v143), CMake 3.25+ and
  Ninja on `PATH`, and a vcpkg checkout with `VCPKG_ROOT` set (e.g. `C:/vcpkg`). Dependencies install from each
  service's `vcpkg.json` on first configure. The presets set `VCPKG_VISUAL_STUDIO_PATH` to
  `C:/Program Files/Microsoft Visual Studio/2022/Community`; edit `CMakePresets.json` if VS 2022 lives elsewhere.

## Start the services

Each `docker compose` stack is self-contained; their host ports differ, so both can run at once.

**Auth server alone** (Node 24 + MySQL 8.4 + Mailpit; details in [Auth Server setup](docs/auth/setup.html)):

```sh
cd auth
cp .env.example .env        # dev defaults; change the passwords
docker compose up -d --build
curl -s localhost:3000/health   # {"status":"ok","db":"ok"}
```

**The full stack: world server, chat service, auth and the admin app** (world + chat + auth built from `../auth` +
admin built from `../admin` + MySQL + Mailpit; details in [World Server setup](docs/world/setup.html) and
[Chat service setup](docs/chat/setup.html)):

```sh
cd world
cp .env.example .env              # dev defaults; change the passwords and CHAT_LINK_SECRET (16+ characters)
docker compose up -d --build      # world (7777) + chat (7778) + auth (3100) + admin (3200) + MySQL 8.4 + Mailpit
docker compose ps                 # all healthy
docker compose logs world         # "migration applied", "listening", "chat link: up"
docker compose logs chat          # "listening", "world link up"
```

The compose project is `chthonic-world`, its images `chthonic-world:dev`, `chthonic-chat:dev` and
`chthonic-admin:dev`, and its databases `chthonic_auth`, `chthonic_world`, `chthonic_chat` and `chthonic_admin`.

**Maps.** Compose mounts `MAPS_DIR` (set it in `world/.env` or the shell; default `./maps`, i.e. `world/maps`) as the
world's `/app/maps`. `world/maps` holds only a minimal flat start map (id 1), because the world server refuses to start
without a map; see [world/maps/README.md](world/maps/README.md) and [Maps](docs/world/maps.html). To run on a real
map set, point `MAPS_DIR` at it (relative to `world/`, or absolute) and `docker compose up -d`.

**Create accounts** with the user CLI. It runs in the `auth` container, so it works from `auth/` or `world/` while
that stack is up. Accounts are created pre-verified, and the role is `player`, `gm` or `admin`. Details in
[Auth Server setup](docs/auth/setup.html#commands).

```sh
docker compose exec auth node dist/user-cli.js create you@example.com admin   # prompts for the password (hidden)
docker compose exec auth node dist/user-cli.js set-role you@example.com gm    # change an existing account's role
```

Without `--password`, `create` prompts for the password, which keeps it out of shell history. Passwords are 10-128
characters. `create` fails if the email is already taken; use `set-role` to change that account instead. From a host
checkout with `npm ci` done, `npm run user -- ...` in `auth/` does the same against `DB_*` in `.env`.

**Admin app** (in the full stack; details in [Admin App setup](docs/admin/setup.html)): set `ADMIN_DB_PASSWORD`,
`ADMIN_WORLD_DB_PASSWORD`, `WORLD_ADMIN_SECRET`, `SEED_ADMIN_EMAIL` and `SEED_ADMIN_PASSWORD` in `world/.env` (see
`.env.example`), then `docker compose up -d --build` also builds and starts `admin`. Open http://localhost:3200 and
sign in with the seed admin. To build it outside Docker:

```sh
cd admin
npm ci
npm run build       # API -> dist/, SPA -> web/dist/
npm start           # serves both on :3200 (configure .env from .env.example first)
npm run dev         # development: Vite on :5173, proxying /api to `npm run dev:api` on :3200
```

`docker compose down` stops a stack and keeps its data; `down -v` also deletes it.

## Building the world server and chat service on Windows

From a VS 2022 x64 developer prompt with `VCPKG_ROOT` set (details in
[World Server setup](docs/world/setup.html#building-on-windows-msvc--vcpkg)):

```bat
cd world
cmake --preset debug          :: or release; static triplet x64-windows-static
cmake --build --preset debug  :: build/debug/chthonic_world.exe, chthonic_bot.exe, chthonic_mapbake.exe, world_self_check.exe
ctest --preset debug          :: world_self_check, log_pressure, log_flush, mapbake_cli

cd ..\chat
cmake --preset debug
cmake --build --preset debug
ctest --preset debug
```

The world self-check's sample-map checks are compiled only when the CMake cache variable `CHTHONIC_SAMPLE_MAPS`
(default `world/maps`) holds all seven sample maps; otherwise they are skipped
([Tests](docs/world/tests.html#sample-maps)).

## Running the tests

- **auth**: `cd auth && npm ci && npm run build && npm run typecheck && npm run lint && npm test`. `npm test` needs
  `auth/.env` and the auth stack's `mysql` (and `mailpit`) running: `docker compose up -d mysql mailpit` in `auth/`
  ([Auth Server setup](docs/auth/setup.html)).
- **admin**: `cd admin && npm ci && npm run build && npm run typecheck && npm run lint && npm test`; the tests use the
  same MySQL and read `auth/.env` ([Admin App setup](docs/admin/setup.html)).
- **world / chat**: `ctest --preset debug` in each, after the build above.
- **docs**: `bash world/tests/docs_check.sh` checks that every opcode, code and environment variable is documented.
- **integration** (against the running full stack, from `world/`): `bash tests/integration.sh`, and the
  `--instances`, `--layers` and `--admin` modes. Scenarios that need the sample maps report `SKIP` when `MAPS_DIR`
  lacks them; the summary prints the pass / fail / skip counts ([Tests](docs/world/tests.html)).
