# Security boundary

Only authorized, unprotected inputs may be processed. Authorization confirmation in the UI/CLI is
not a license-verification mechanism. Encrypted Mach-O slices are rejected, including encrypted
secondary slices/images. This project does not decrypt FairPlay, patch DRM checks, or treat Apple
code signatures as a source of trust.

Input limits include a 512 MiB archive, 1 GiB expanded data, 256 MiB per host archive member (64 MiB
per on-device executable), 20,000 members, 250:1 maximum member expansion ratio, 8 MiB plist, and
bounded paths/depth. PNG dimensions and CgBI inflation are bounded. Native counts/ranges, ULEB
overflow, export-trie recursion and FAT overlaps are checked. Android ZIP64 archives are unsupported.

Extraction occurs under newly created private workspaces; links/special files, traversal, absolute
paths, case collisions and encrypted archives are rejected. Partial extraction is removed on failure.
On Android, an authorized input is retained in app-private storage until the library entry is deleted;
the authorization dialog discloses this. Import does not automatically build an APK. A separate user
action builds only a minimal, signed placeholder from the bundled source-free template; the original
IPA and executable are never copied into it, and no gameplay code is translated or run by the
converter.

The host pipeline's closed-integer leaf assessment is not a complete game conversion. The former
launcher wrapper has been disabled, and `build_apk` refuses to package that subset. No no-op iOS API
providers or fake framework symbols are generated. The experimental portable runtime is not an Apple
Objective-C ABI implementation.

A host APK can be attached only after source identity, architecture, generated API-replacement
metadata, resource/lifecycle claims, package identity, signer presence, and APK structure are
checked. Those checks are static and some completeness fields are host-converter attestations; they
do **not** prove correct behavior, runtime safety or gameplay. The UI records device execution and
gameplay as untested. The current host tool emits no `complete-game-v1` APK. Android's package
installer performs its own signature/package verification before installation.

The Android app has no INTERNET permission and does not upload IPAs. `ResultProvider` is read-only
and URI-granted. Complete-game host APKs are served only under the existing IPA-basename and
`complete-game-v1` checks; placeholders use a different filename and are accepted only when their
explicit non-game metadata, source hash and output digest match. The placeholder signer is generated
per converter installation and stored in app-private no-backup storage; this development signer is
not a security endorsement of source code. Recovered icon/metadata resources may be malformed or
malicious; bounded decoding and copying do not establish trust. Bundle data remains opaque unless a
bounded converter explicitly handles it. Native analysis is memory-safe-by-validation C++, not a
formal proof of parser correctness; fuzz/sanitizer testing is advisable for production deployment.

Host analysis runs with the invoking user's privileges, not a kernel/container sandbox. Do not run as
root or use a workspace writable by another untrusted process. Private-directory ownership is
assumed during filesystem operations; concurrent same-UID adversaries and local process tampering are
outside this boundary. Production multi-user deployments should add an unprivileged
container/seccomp/resource-limited worker around the CLI.
