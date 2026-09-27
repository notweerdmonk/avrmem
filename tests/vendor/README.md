# greatest.h — dual source: submodule + offline fallback

- Upstream: https://github.com/silentbicycle/greatest/ (Scott Vokes, MIT).
- Pinned: v1.5.0 (`11a6af1`), verified byte-identical to the fallback at pin time.

## Layout

```text
tests/vendor/
  greatest/              # git submodule mount — PRIMARY source (when initialized)
    greatest.h
    ...
  greatest.h             # bundled offline fallback — used when the submodule
                         #   is not initialized (fresh clone w/o --recurse-submodules,
                         #   tarball, offline machine)
  resolve_greatest.sh    # picks the include dir (inspection only, no network)
  test_resolve.sh        # hermetic self-test for the resolver
  README.md              # this file
```

## Resolution (`resolve_greatest.sh`)

Prints exactly one line (the include dir) to stdout; diagnostics to
stderr; exit 0 unless both sources are dead. Submodule header wins when
present and sane; a version other than the vetted 1.5.0 prints a stderr
warning but is still used. Normal builds never touch the network.

```text
submodule ok ──→ tests/vendor/greatest
       │ missing / corrupt / drifted (warns)
       ▼
fallback ok ───→ tests/vendor (+ stderr note)
       │ dead
       ▼
     error, exit 1
```

## Upgrade ritual (maintainer)

1. `git -C tests/vendor/greatest fetch && git -C tests/vendor/greatest checkout <new-tag>`
2. `make vendor-sync` (re-inits if needed, refreshes the fallback from
   the pinned file — the ONLY network-touching target)
3. `make clean && make test && make test-asan` fully green
4. Commit together: gitlink + refreshed fallback + this README's pin line

## Rules

- Never hand-edit `greatest.h` (either copy) — change it only via the ritual above.
- Fresh clones wanting the submodule: `git clone --recurse-submodules`;
  without it the fallback covers the build with a one-line stderr note.
