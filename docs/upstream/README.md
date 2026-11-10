# Upstream patch queue

Changes from this fork prepared for the upstream repository, one patch per
future pull request. **Nothing here has been sent upstream.** The queue is
preparation only; filing anything is David's decision.

Each patch is made on upstream's own tree, not cut from the fork's history, so
it carries a short message of its own and none of the fork's tests, tools or
platform code.

## Layout

- `BASE`: the upstream commit every patch is made on.
- `patches/NNNN-topic.patch`: one `git format-patch` file per future pull
  request. Each applies to `BASE` alone.

## The queue

| Patch | Fixes | Fork change it comes from |
| --- | --- | --- |
| 0001 | `rcon` is accepted with an empty password when the server has not set `rcon_password` | security fixes merge `52fdd24` |
| 0002 | The flashbang white-out and the concussion blur never show in multiplayer | commit `57dec53` |
| 0003 | `where` and the invalid-map vote reply format with missing arguments | commit `f662443` |
| 0004 | Color dvars registered with red above zero start at red 1/255 | commit `fcb454d` |
| 0005 | A server listing more than 32 fastfiles makes the client write past two 32-entry arrays | commit `79cf6ff` |
| 0006 | The "Invalid command time" message uses text holding the player's name as its format | security fixes merge `52fdd24` |
| 0007 | A retry after a refused connect is dropped by the reconnect throttle | commit `8e5317e` |
| 0008 | The cached tag name's string reference is lost on every map change | commit `5195157` |
| 0009 | `deactivateReverb` and `deactivateChannelVolumes` send a format the client does not read | commit `f662443` |
| 0010 | The script debugger reports a known field as unknown when its id has a zero low byte | commit `b39e6b9` |

Patches 0001 to 0004 are the proposed first batch; the rest follow in a
second one.

## Rules

`scripts/tools/upstream_queue.py lint` checks these; the workflow
[`upstream-queue.yaml`](../../.github/workflows/upstream-queue.yaml) runs it.

- The subject uses upstream's style: `fix:`, `security:`, `refactor:` and so
  on, then a plain phrase.
- The body is at most 12 lines: what was wrong, the fix, how it was checked
  when that is not obvious.
- No issue or pull request number written with a hash sign, no fork pull
  request numbers, no links into the fork, no notes about the fork's test
  machines, no session trailers. Write an upstream issue as "upstream issue 52".
- The smallest change that fixes the fault. No fork-only paths (`src/linux`,
  `scripts/tests`, `scripts/tools`, `docs`).

## Checks

```sh
# Rules only; needs nothing but this checkout.
python3 scripts/tools/upstream_queue.py lint

# Rules, and that every patch applies to the pinned base alone and all apply
# in order. Fetching upstream is a read of a public repository.
git init -q /tmp/upstream
git -C /tmp/upstream fetch -q --depth 1 https://github.com/SwagSoftware/KisakCOD.git "$(cat docs/upstream/BASE)"
git -C /tmp/upstream checkout -q --detach FETCH_HEAD
python3 scripts/tools/upstream_queue.py check --upstream /tmp/upstream
```

The hosted workflow does the same and then builds the patched upstream tree
with MSVC (Debug and Release; sp, mp and dedi), using the steps of upstream's
own workflow. That build is of all patches together.

## Adding a patch

1. Check out upstream at `BASE` in a scratch directory.
2. Make the change there and commit it with a message that keeps the rules.
3. `git format-patch -1 --zero-commit -N --no-signature --stdout > docs/upstream/patches/NNNN-topic.patch`
4. Add a row to the table above and run the checks.

When upstream moves, update `BASE` and refresh any patch that no longer
applies.
