# Runtime import and authorization policy

`RuntimeBridge.runAuthorizedIpa` accepts a URI chosen by the user (or a caller-owned input stream), an explicit path for the app's main executable inside the IPA, and a caller-provided authorization confirmation. The executable path must be a direct file under `Payload/<App>.app/`; the caller is responsible for reading `CFBundleExecutable` from the selected app's bundle metadata. Automatic binary-plist parsing and app selection are outside this session's skeleton.

The bridge reads the selected IPA only at runtime. It streams ZIP entries, retains only the selected main executable in bounded memory, never extracts the full app tree to the Android package, and never adds the IPA or executable to `compat-runtime-v1`'s build inputs. The archive scan is capped at 1 GiB expanded input, and the main executable is capped at 256 MiB. Invalid paths, missing entries, oversized inputs, archive errors, absent authorization, and JNI initialization errors produce a `not_runnable` report.

A supported Mach-O encryption-info command with non-zero `cryptid` produces `BLOCKED_ENCRYPTED`. No FairPlay decryption, key retrieval, protection removal, or encrypted-code execution is attempted. Authorization is a user confirmation for the user's own permitted file; it does not override encryption or parser/backend safety checks.

The returned report must retain `inputEmbeddedInRuntimeArtifact: false`. If an application persists a report, it should use the contract name `compat-runtime-v1-report.json` in app-private storage and keep it separate from the guest's own files. No import or report diagnostics are drawn over a running game.
