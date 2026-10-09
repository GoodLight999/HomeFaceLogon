# Authentication lifecycle regression fixes

This change builds on the unmerged camera recovery and transactional setup PR.
It is a **candidate for physical validation**, not a verified Windows unlock release.

## Findings and changes

- **Typing a PIN before a face match prevented unlock.** `GetSerialization` checked
  any nonempty PIN before the face result, treating an unfinished PIN as a failed
  attempt. A successful face match now takes priority and consumes the proof once.
  PIN rate limiting remains enforced for PIN attempts.
- **Failed serialization retained an auto-logon lock.** Authentication state is
  now per credential, cleared on every failure/cancellation, and marked as being
  submitted only after credentials have actually been packaged. Duplicate calls
  and tile reselection cannot launch another camera during submission.
- **Worker cancellation could outlive the credential.** Pipe workers post to the
  credential's message window. UI/COM updates run on its owning thread. Session
  generations reject queued results after cancellation. Cancellation drains
  OVERLAPPED I/O and joins the worker before closing its shared handles. Provider
  teardown disconnects any retained credential from the provider.
- **Helper exit/connection failures left the UI waiting.** Pipe waits observe
  cancellation, child exit and an awake-time deadline. Terminal results stop the
  host. A final message already delivered through the pipe is processed when
  child exit and I/O completion coincide.
- **Enrollment/password failure could leave partial active data.** Setup now
  prepares all four files using `FaceLogonSetup --data-dir` in an isolated candidate
  directory. Failure during preparation leaves active data and Enabled unchanged.
  Commit temporarily disables the provider, replaces each file atomically, verifies
  the bytes, and restores its prior Enabled state. Errors attempt restoration of
  every file; an incomplete rollback retains the on-disk backup and its path.
  Re-registration preserves unrelated configuration fields.
  A helper capability check refuses to run an old setup executable that would
  ignore `--data-dir`; development builds resolve their matching native helper.
- **A status read could disable the provider.** Status queries no longer mutate
  HKLM. Settings writes preserve unknown configuration fields, write atomically,
  and restore the previous file if updating HKLM fails.
  The installer also preserves an existing Enabled value during an upgrade.
- **Non-ASCII passwords were read using the wrong encoding.** Redirected setup
  input is explicitly decoded as UTF-8. Invalid UTF-8/NUL is rejected. The manager
  validates line delimiters and PIN format before starting camera work. Input is
  written on a worker so a helper that never reads stdin still times out.
- **Diagnostics could overwrite the real face template.** The DPAPI round trip
  now operates entirely in memory. It never writes, backs up or removes `face.bin`.
  Enrollment/verification pump window events before testing whether a window closed.

## Automated validation

Run from a Windows x64 Visual Studio 2022 development environment:

```powershell
msbuild HomeFaceLogon.sln /m /p:Configuration=Release /p:Platform=x64
msbuild tests/CredentialRegression.vcxproj /p:Configuration=Release /p:Platform=x64
./tests/x64/Release/CredentialRegression.exe
python tests/setup_process_regression.py
cargo test --manifest-path src/gui/src-tauri/Cargo.toml --lib --locked
cd src/gui
npm ci
npm run build
npm run tauri build -- --no-bundle
```

The native tests use the production credential implementation with synthetic
password/PIN loading and a suppressed camera launch. They exercise partial/invalid/
valid/empty PINs at face match, Microsoft account packaging, duplicate submission,
PIN lockout, decryption failure, Windows rejection, stale session delivery,
pending pipe cancellation, and immediate child exit (10 iterations).
Replacing only `GetSerialization` with the previous PR version makes the first
partial-PIN regression fail; the corrected implementation passes the same case.

Rust tests cover process timeout (including a blocked stdin writer), output pipe
draining, input/error handling, preparation failure for initial/re-setup, incomplete
helper output, prior Enabled preservation, activation failure/retry, and attempting
later rollback files after an earlier failure. They use temporary synthetic files
and an injected Enabled setter; they do not modify HKLM or live enrollment.

The setup executable test verifies UTF-8 password storage/decryption through real
DPAPI, rejection without changing synthetic files, and diagnostics preserving a
sentinel face file. Models are required for its diagnostic success assertion.

## Remaining physical checks

- Install with a recoverable copy of the existing binaries/configuration and a
  working standard Windows sign-in method; approve any required restart separately.
- Lock the workstation. Type one or more PIN characters while the camera is
  preparing/searching; do not submit. A face match should unlock immediately.
- Submit an incorrect PIN before a match, then retry face authentication. Check
  that PIN rate limiting is preserved and the camera remains usable.
- Switch away from the face tile during scanning and return. Cancelled results
  must not submit credentials, and the next scan must work.
- Repeat after sleep/resume and with a busy/disconnected camera.
- Cancel enrollment; simulate a setup/password helper error. Existing enrollment,
  Enabled state and unrelated settings should remain usable; retry must succeed.

The hidden-window delivery depends on the real LogonUI message loop and therefore
requires secure-desktop testing. Browser tests with mocked Tauri calls cannot
establish this. Abrupt termination during the multi-file commit is not automatically
recovered: the provider remains disabled and the disk backup is retained. The
individual replacements are atomic, not the complete four-file transaction.
