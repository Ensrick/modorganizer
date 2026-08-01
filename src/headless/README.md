# MO2Headless

`MO2Headless.exe` is a console-only controller for a portable Mod Organizer 2
instance. It performs profile, mod and plugin operations without constructing a
Qt window. Every result is one JSON object on stdout or stderr, and failures use
stable non-zero exit codes.

## Safety model

- One `QLockFile` serializes mutating operations.
- `--dry-run` validates without writing.
- List changes use `QSaveFile` atomic replacement.
- Each mutation creates `headless-journal/<transaction>/transaction.json` plus
  before-images of changed files.
- `rollback <transaction>` restores list files and reverses directory moves.
- Replacement and removal move content into recoverable trash; nothing is
  recursively deleted.
- Archive entries containing absolute paths or `..` traversal are rejected.
- Links in source or extracted trees are rejected.
- FOMOD archives require a deterministic `--install-plan`; installing all
  conditional branches is never used as a fallback.

The GUI and headless controller must not mutate the same instance concurrently.
The controller lock protects autonomous processes; MO2 itself does not yet
participate in that lock.

## Core commands

```text
MO2Headless init GAME_PATH [--game-name NAME] [--game-edition EDITION]
MO2Headless status
MO2Headless profile-list
MO2Headless profile-create NAME [--clone PROFILE] [--select]
MO2Headless profile-select NAME
MO2Headless profile-trash NAME --yes
MO2Headless mod-list [-p PROFILE]
MO2Headless mod-stage DIRECTORY NAME [--enable] [--priority N] [--replace]
MO2Headless mod-install ARCHIVE NAME [--install-plan PLAN] [--enable] [--replace]
MO2Headless mod-enable NAME
MO2Headless mod-disable NAME
MO2Headless mod-priority NAME N
MO2Headless mod-trash NAME --yes
MO2Headless plugin-list
MO2Headless plugin-enable PLUGIN
MO2Headless plugin-disable PLUGIN
MO2Headless plugin-priority PLUGIN N
MO2Headless snapshot
MO2Headless apply STATE.json
MO2Headless audit
MO2Headless rollback TRANSACTION_ID
MO2Headless run PROGRAM [--arguments TEXT] [--cwd PATH] [--overwrite MOD]
```

`mod-priority` follows MO2 semantics: priority zero is the lowest-conflict
winner and larger numbers win. `plugin-priority` follows game load order:
priority zero loads first.

`run` starts the sibling `ModOrganizer.exe headless-run` command. MO2 initializes
the selected game plugin and USVFS but returns before constructing its main
window. The child program's real exit code is propagated.

`init` auto-detects Steam, GOG, and Epic editions from the game directory. Use
`--game-edition` to override the detected value or provide one for an
unrecognized layout.

## Deterministic FOMOD resolution

An install plan maps paths from the fully extracted archive into the resolved
mod tree. Mappings are applied in order; later files replace earlier files.
This is enough to encode selections made after inspecting `ModuleConfig.xml`
without invoking an installer UI. See `install-plan.schema.json`.

## Exit codes

- `0`: success
- `64`: bad invocation
- `65`: invalid or unsafe input
- `66`: required input missing
- `73`: destination cannot be created
- `74`: I/O or child extraction failure
- `75`: lock contention or timeout
- `78`: invalid/missing instance configuration
