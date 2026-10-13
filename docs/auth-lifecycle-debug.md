# Authentication lifecycle regression fixes

This change builds on the unmerged camera recovery and transactional setup PR.
The core unlock paths have been validated on the test PC. Sleep/resume, camera
contention and interactive enrollment failure paths still require physical checks.

## Findings and changes

- **Typing a PIN before a face match prevented unlock.** `GetSerialization` checked
  any nonempty PIN before the face result, treating an unfinished PIN as a failed
  attempt. A successful face match now takes priority and consumes the proof once.
  PIN rate limiting remains enforced for PIN attempts.
- **Failed serialization retained an auto-logon lock.** Authentication state is
  now per credential, cleared on every failure/cancellation, and marked as being
  submitted only after credentials have actually been packaged. Duplicate calls
  and tile reselection cannot launch another camera during submission.
- **The first physical test exposed an auto-logon refresh loop.** After a match,
  LogonUI returned `autoLogon=0` without calling serialization and restarted the
  camera. The candidate was rolled back with enrollment data preserved.
  `UnAdvise` releases a replaced event callback; it now preserves a pending face
  proof or submission across that refresh. Selection reports a pending face proof
  as auto-logon. Actual deselection, failure and provider detachment still clear it.
  An unused face proof expires after ten seconds, including time spent suspended.
  A regression reproduces the callback replacement, selection and duplicate-call
  sequence; reverting only `UnAdvise` makes it fail. Physical retesting succeeded.
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
- **Installer registration silently failed.** The DLL exports its COM class
  factory but no `DllRegisterServer`/`DllUnregisterServer`. The installer now writes
  the scoped 64-bit COM and credential-provider registrations directly, removes
  them through its uninstall registry entries, and skips manager launch in silent
  updates. This was found in the actual upgrade log (regsvr32 exit code 4).
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
They also attach a real provider and replace credential callbacks between a
queued match and selection/serialization, including replacement during submission
and provider destruction while the credential is retained.
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

## Physical validation on 2026-10-09

The upgraded binaries were compared with the tested build, COM activation and
installed diagnostics succeeded, and all four existing enrollment/configuration
files remained byte-identical with Enabled preserved. No restart was required.

The user confirmed unlock with a partially typed, unsubmitted PIN, unlock with no
PIN input, and unlock after switching to the standard Windows provider and back.
The real LogonUI trace now shows callback replacement preserving the face proof,
`autoLogon=1`, finished serialization, and `ReportResult(STATUS_SUCCESS)`.

The sample book/Controls bitmap has also been replaced with a licensed Lucide face
scan icon, a 72x72 transparent DIB and a descriptive sign-in option tooltip.
The native resource check confirmed its alpha pixels survive loading from the DLL.
After the icon update, the user confirmed both its lock-screen display and unlock
on the installed build.

## Lock-time network observation

A bounded HTTPS probe ran while the user locked the PC and covered the camera for
about thirteen seconds before unlocking. All 36 requests across 12 completed
locked samples received HTTP responses: Cloudflare returned 200, and ChatGPT and
the Codex cloud environment API returned 403 without authentication. These responses
confirm DNS/TLS/HTTPS connectivity during the observed interval; they do not verify
the authenticated Codex streaming connection. No request failed in the full probe.

The reported Codex error says the peer closed a TLS connection without sending
`close_notify`. This observation does not establish which peer or intermediary
closed the stream, or why. A network-wide disconnect was not observed during this
test. However, at 23:14:36-43 (shortly after unlock), the actual desktop log records
multiple Codex cloud WebSocket connections closing with code 1006. Its local work
executor also reports Windows socket error 10054 and exits twice before reconnecting
at 23:14:48. The HTTPS probes continued receiving responses during this interval.
This establishes a real Codex connection failure despite successful short requests;
the probes cannot rule out selective or transient network-path failures. The reported
model response stream error may be a separate connection, and the local logs do not
identify its cause. No network, VPN, power or Codex settings were changed.

## Remaining physical checks

- Install with a recoverable copy of the existing binaries/configuration and a
  working standard Windows sign-in method; approve any required restart separately.
- Submit an incorrect PIN before a match, then retry face authentication. Check
  that PIN rate limiting is preserved and the camera remains usable.
- Repeat after sleep/resume and with a busy/disconnected camera.
- Cancel enrollment; simulate a setup/password helper error. Existing enrollment,
  Enabled state and unrelated settings should remain usable; retry must succeed.

Hidden-window delivery has been exercised in the real LogonUI message loop for
the confirmed cases above. Browser tests with mocked Tauri calls cannot establish
this. Abrupt termination during the multi-file commit is not automatically
recovered: the provider remains disabled and the disk backup is retained. The
individual replacements are atomic, not the complete four-file transaction.
