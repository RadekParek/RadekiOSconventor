# compat-runtime-v1 report contract

The native report is returned as JSON through `RuntimeBridge` and may be stored under the stable name `compat-runtime-v1-report.json`. The native library name is `libcompat_runtime_v1.so`; the runtime contract identifier is `compat-runtime-v1`.

## Required top-level fields

| Field | Meaning |
| --- | --- |
| `schemaVersion` | Integer report schema, currently `1`. |
| `runtimeContract` | Exactly `compat-runtime-v1`. |
| `runtimeLibrary` | Exactly `libcompat_runtime_v1.so`. |
| `reportArtifactName` | Exactly `compat-runtime-v1-report.json`. |
| `status` | Runtime result. Session 1 reports `not_runnable` unless a future, evidence-backed contract explicitly adds another runtime outcome. |
| `authorizationConfirmed` | Whether the caller confirmed authority to use this input. False always blocks. |
| `inputEmbeddedInRuntimeArtifact` | Always `false`. |
| `firstMissingImport` | The first unresolved Darwin symbol as a string, or JSON `null` if loading was blocked before symbol analysis or no missing import was observed. |
| `resolvedSymbols` | Records for each import resolved through a registered native adapter. |
| `unresolvedSymbols` | Records for each import not resolved through a registered adapter. |
| `loader`, `cpu`, `execution` | Structured loader/backend/execution diagnostics. These are not compatibility percentages. |
| `reason` or `message` | Human-readable failure detail when the input or execution is blocked. |

Each symbol record carries the exact symbol name, source stream, dependency name, signed dylib ordinal encoded as a decimal string, bind address where available, weak-import flag, and resolved/unresolved status. Resolved records additionally name the shim library, adapter, and reserved guest callout address. No missing symbol is silently stubbed or counted as implemented. Weak unresolved imports also block execution.

## Loader statuses

- `LOADED`: supported main image and its observed imports were processed; this does **not** mean the application is runnable or reached a menu.
- `BLOCKED_UNRESOLVED_IMPORTS`: at least one import has no registered native adapter. `firstMissingImport` identifies the first blocker.
- `BLOCKED_ENCRYPTED`: the Mach-O encryption command reports a non-zero `cryptid`; no decryption is attempted.
- `BLOCKED`: malformed, unsupported, or out-of-range input was rejected.

The top-level runner result remains `not_runnable` for all Session 1 loads. Its execution subreport may explain backend unavailability, a CPU fault, an instruction/time limit, or a return from the entry function, but none of those is a smoke-tested `menu` or `playable` result. The CPU backend enforces both instruction and wall-clock limits; a reached time limit is reported as `TIME_LIMIT`.

## Smoke-test statuses

The compatibility database uses only these per-app status values:

- `not runnable`
- `crashes at <symbol>`
- `menu`
- `playable`

`compat-runtime-v1/tools/smoke_db.py record` requires an evidence note, preserves run history, imports symbol names from a same-contract JSON report when supplied, and only records a game as unblocked by a shim family when a smoke result is `menu` or `playable` and the family is explicitly named as resolved. These results are evidence records, not a percent-implemented metric. The initial database has no real-app entries.

There is no in-game diagnostic overlay. Import and CPU diagnostics are returned to the host-side caller for storage or display outside the running guest application.
