# Vendored EDOPro core

These sources are the **EDOPro ocgcore fork** that the duel runner links against
(`sim/build.sh` compiles `edo-core/*.cpp` into `libocgcore_edo.a`). They are
vendored into this repository rather than tracked as a git submodule, because
the commit this project was pinned to no longer exists upstream:

- upstream: <https://github.com/edo9300/ygopro-core>
- vendored commit: `c677e4521b12c5e362bf983be02864c3394859eb` ("fixes")
- vendored on: 2026-09-15

`git submodule update --init` had become impossible: upstream rewrote its
history (its `master` is now `122e0d0`, pushed 2026-09-06) and GitHub answers
"No commit found for SHA" for the pinned commit. A submodule pin that cannot be
fetched is worse than a copy, so the working sources — the exact ones this
project has been building and testing against — live here as plain files.

## License

AGPL-3.0-or-later (see `LICENSE` and `COPYING`, kept from upstream). Vendoring
third-party AGPL source is fine as long as the license and attribution travel
with it, which is why both files are committed here.

## Local patches

Three files differ from upstream. Any re-vendor has to re-apply all three.

**`libduel.cpp`** adds the four era activity checks the modern core dropped —
`Duel.CheckSummonActivity`,
`Duel.CheckFlipSummonActivity`, `Duel.CheckSpecialSummonActivity` and
`Duel.CheckAttackActivity` — right next to upstream's
`CheckNormalSummonActivity`. The 2011 scripts gate "you cannot Summon other
monsters / declare an attack the turn you activate this" with
`not Duel.CheckXActivity(tp)`, so without these bindings eight Edison cards
(Scapegoat, Fires of Doomsday, Gateway to Dark World, Dark Magic Curtain, Proof
of Powerlessness, Blaze Accelerator, Tri-Blaze Accelerator, Destiny HERO - Doom
Lord) were silently unactivatable. Each one reads the per-turn state counter
`Duel.GetActivityCount` reports (`summon_state_count`, `flipsummon_state_count`,
`spsummon_state_count`, `attack_state_count`) and returns "already spent".

**`processor.cpp`** passes the triggering effect's own description to
`Processors::SelectEffectYesNo` in `SelectChain` (both the SEGOC path and the
single-optional-trigger path) instead of the generic `0`/`221`, so the client can
name *which* effect is being offered rather than showing a bare Yes/No.

**`operations.cpp`** consumes the Deck-shuffle request inside
`Processors::SendTo`, right after the move, instead of leaving it to the
cost/target/operation processors' end-blocks. `add_card` raises
`shuffle_deck_check` for `SEQ_DECKSHUFFLE` ("into the Deck and shuffle it") and
`remove_card` raises it for a card leaving the Deck, but `Duel.SendtoDeck`
suspends the Lua coroutine while a `Processors::SendTo` performs the move — so by
the time the flag is raised the operation's end-block has already run, the next
processor's start cleared the flag without shuffling, and the Deck kept the exact
order it had. The card was put back **on top**: `Duel.SendtoDeck(c,nil,2,…)` is how
Ehren, Lightsworn Monk, the Gladiator Beasts, Trap Dustshoot and 30-odd other era
scripts return a card to the Deck, so a monster could simply be re-drawn. 34 of
the 37 scripts that pass `2` rely on it alone (Moray of Greed and Pot of Avarice
also call `Duel.ShuffleDeck`, which is why they were never affected); every one of
them means "return it to the Deck", which shuffles by rule, and none means "put it
on top" — that is sequence 0. `.smoke/deck_shuffle_test.py` pins it.

The `rsync` update below copies straight over the sources, so re-apply the patches
and check the guards:

```bash
diff <(git --git-dir=.edo-core-upstream show <sha>:libduel.cpp) sim/edo-core/libduel.cpp
../venv/bin/python3 ../.smoke/activity_check_test.py    # the behaviour
../venv/bin/python3 ../.smoke/script_api_test.py        # no script calls a name the core lacks
../venv/bin/python3 ../.smoke/deck_shuffle_test.py      # a card returned to the Deck shuffles it
```

## Updating

There is no automated path, on purpose: this is a *rules engine* pinned to the
behaviour the Edison tests expect, and a newer core changes rulings. If you do
want to move it forward:

```bash
git clone https://github.com/edo9300/ygopro-core.git /tmp/edo-core
git -C /tmp/edo-core checkout <new-sha>
# copy the sources (not .git, .github, scripts/ or build output) over sim/edo-core/
rsync -a --delete --exclude .git --exclude .github --exclude scripts \
      --exclude build --exclude build_wrap /tmp/edo-core/ sim/edo-core/
cd sim && ./build.sh                       # rebuild the runner
../venv/bin/python3 ../.smoke/union_test.py   # smoke the engine
cd .. && for f in .smoke/*_test.py; do venv/bin/python3 "$f" || echo "FAIL $f"; done
```

Then update the commit hash above.

The old clone's history is parked in `.edo-core-upstream/` (gitignored). Keep it
until you decide on an upgrade: because upstream rewrote its history, that copy
is now the *only* place the pinned commit's history exists, so it is also the
only way to `git log`/`git diff` what this project is actually running.
