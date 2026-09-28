# Edison duel engine

Headless Yu-Gi-Oh! duel engine for the **March 2010 ("Edison") format**, built on
the [EDOPro](https://github.com/edo9300/ygopro-core) ocgcore and driven over
stdin/stdout by a small C++ harness.

It powers the duels on **Edison Online** (<https://playedison.com>). The
repository is published separately because the engine links and loads
AGPL-3.0-or-later code: **this is the Corresponding Source for the engine the
hosted service runs**, offered to every user of that service under AGPL §13.

The application around it — API server, deck analysis, matchmaking, web
frontend — is a separate work. It communicates with this engine only by spawning
it as a child process and exchanging JSON over pipes, and it is not part of this
repository.

## Contents

| path | what it is | license |
| --- | --- | --- |
| `edo-core/` | vendored EDOPro ocgcore fork — the duel rules engine | AGPL-3.0-or-later, with MIT portions |
| `script/` | Project Ignis card scripts — the per-card Lua | AGPL-3.0-or-later |
| `lua54/`, `lua54.tar.gz` | Lua 5.4.8, compiled as C++ with exceptions | MIT |
| `duel_runner_edo.cpp` | the harness: seating, prompts, policy, search | AGPL-3.0-or-later |
| `policy.h`, `search.h`, `interactive.h`, `nn_cpp.h` | harness headers | AGPL-3.0-or-later |
| `build.sh`, `build_lua.sh` | the build | — |

`duel_runner_edo.cpp` and its headers are statically linked against `edo-core`,
so they form a single work with it and carry the same license.

## Build

Requires `g++` (C++17), `ar`, `make` and `libsqlite3-dev` — the harness reads
card data through SQLite.

```sh
./build.sh
```

That is the whole build. `build.sh` first builds the vendored Lua from
`lua54.tar.gz` (`build_lua.sh`, skipped when `lua54/liblua_cpp.a` already
exists), then compiles `edo-core/` into `libocgcore_edo.a` and links
`duel_runner_edo.cpp` against it. The result is `./duel_runner_edo`.

## Runtime inputs that are deliberately not here

- **`cards.cdb`** — the card database the engine opens relative to its working
  directory. It is Konami-derived card data and is **not** redistributed in this
  repository; supply your own (an EDOPro/YGOPro `cards.cdb` build works).
- `script/` **is** included, because the engine loads the card scripts at
  runtime; it must sit next to the binary.

## Provenance

| component | upstream | pinned at |
| --- | --- | --- |
| `edo-core/` | `edo9300/ygopro-core` | `c677e4521b12c5e362bf983be02864c3394859eb` — see `edo-core/VENDORED.md` |
| `script/` | `ProjectIgnis/CardScripts` | `150ec5170097128b675b8d5b257b793a6ad6c91e` — see `.cardscripts_tree.json` |
| `lua54/` | lua.org | 5.4.8 |

Local modifications are kept in this repository, as the AGPL requires:

- **`edo-core/libduel.cpp`** — the only file that differs from the pinned
  upstream commit. It adds the four era activity checks the modern core dropped
  (`Duel.CheckSummonActivity`, `CheckFlipSummonActivity`,
  `CheckSpecialSummonActivity`, `CheckAttackActivity`). Without them eight Edison
  cards were silently unactivatable. `edo-core/VENDORED.md` explains the
  mechanism and the guards.
- **`script/utility.lua`** — Edison-era support helpers.

`edo-core` is vendored as plain files rather than a submodule because the pinned
upstream commit no longer exists on GitHub (upstream rewrote its history), so
`git submodule update --init` cannot fetch it.

`lua54/` and `lua54.tar.gz` are the same Lua release, one extracted and one
packed; `build_lua.sh` uses the tarball so a fresh clone builds without network
access.

## License

**AGPL-3.0-or-later** for this repository as a whole — see `LICENSE`.
`edo-core/` additionally carries its upstream `LICENSE` and `COPYING`, which
cover EDOPro (AGPL-3.0-or-later) and the MIT-licensed Fluorohydride portions.
`lua54/` is MIT and keeps its own notices. `script/` is Project Ignis
CardScripts, AGPL-3.0-or-later.

If you run a modified version of this engine as a network service, AGPL §13
requires you to offer its users the Corresponding Source.

## Not affiliated with Konami

This is an unofficial, non-commercial fan project. It is not affiliated with,
endorsed by, or sponsored by Konami Digital Entertainment or Shueisha.
"Yu-Gi-Oh!" and all card names, card text and card images are the property of
their respective rights holders.
