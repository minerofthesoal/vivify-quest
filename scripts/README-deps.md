# Building without qpackages.com

`qpm restore` resolves every dependency through **qpackages.com**. When that
registry is down, slow, or blocked, the project cannot be built at all. These
scripts replace it with a manifest that resolves against **github.com only**.

| File | Purpose |
| --- | --- |
| `dependencies.json` | Every dependency: source repo, git ref, release asset URLs, compile options. The single source of truth. |
| `restore-deps.py` | Populates `extern/` and regenerates `extern.cmake`. Contacts only github.com. |
| `discover-deps.py` | One-time helper to complete `dependencies.json` (see below). |

## Normal use

```sh
python3 scripts/restore-deps.py      # instead of `qpm restore`
qpm s build                          # or: pwsh ./scripts/build.ps1
```

`--clean` wipes `extern/` first; `--check` validates the manifest without
downloading anything.

`extern.cmake` is generated from the manifest and is byte-for-byte equivalent in
effect to the one qpm produces — the include directories and compile flags are
verified to match exactly. Don't hand-edit it.

## Choosing the source in CI

The build workflow takes a `dependency_source` input when run manually
(Actions -> Build Quest Mod -> Run workflow):

| Value | Behaviour |
| --- | --- |
| `auto` (default) | `restore-deps.py` — GitHub for every dependency the manifest can source that way, qpackages.com for the remainder. What push and pull-request runs get. |
| `github` | `restore-deps.py --no-qpackages` — GitHub only. Fails the build rather than touching the registry, so it only succeeds once the manifest is complete. |
| `qpackages` | `qpm restore` only. Nothing is fetched from GitHub. |

The chosen source and the reason for it go to the log and the run summary, so
it is always visible which one a build used.

`--no-qpackages` works on the command line too, including with `--check`:

```sh
python3 scripts/restore-deps.py --check --no-qpackages   # is GitHub-only viable yet?
```

## Finishing the manifest (one time)

Most dependencies publish their native library as a GitHub release asset, so
their repository is recoverable straight from `qpm.shared.json`. Six
headers-only packages do not, and their repository is recorded **only** on
qpackages.com:

`conditional-dependencies`, `cpp-semver`, `fmt`, `libil2cpp`,
`rapidjson-macros`, `sombrero`

Every package does, however, record its own repository in the `qpm.json` at the
root of its extracted headers. If you have an `extern/` tree from any previous
`qpm restore`, that tree holds the answers:

```sh
python3 scripts/discover-deps.py                    # reads ./extern
python3 scripts/discover-deps.py --extern /path/to/extern
```

It writes the discovered repositories back into `dependencies.json`. **Commit
that file** — from then on nothing ever contacts qpackages.com again, on your
machine or in CI.

Where a package has no `branchName` recorded, the ref is guessed as
`version/vMAJOR_MINOR_PATCH`, which is the convention qpm uses for most
packages but not all (`cpp-semver` uses `version-v0.1.2`, for instance). A
wrong guess shows up immediately as a 404 from `restore-deps.py`; correct the
`ref` in the manifest by hand and re-run.

## Going further: vendoring

For a build that needs no network at all, commit the restored `extern/`
directory (drop the `extern/` line from `.gitignore`). It is a few hundred MB
of headers, so it is a trade-off — but it is the only way to be completely
independent of anyone else's hosting.
