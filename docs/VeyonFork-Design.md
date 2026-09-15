# VeyonFork — Design & Roadmap

**Base:** Veyon 4.11.2 (`origin` = veyon/veyon, working branch `veyonfork`)
**Status:** Living design doc. Priority feature = **Remote File Browser**. Internet Control to cover **all three modes**.
**Audience:** Matt (fork owner) + future Claude sessions. This doc is the source of truth; it lives in the `VeyonFork` project and in the repo at `docs/VeyonFork-Design.md`.

---

## 1. TL;DR — decisions so far

| Decision | Choice |
|---|---|
| First feature to build | **Remote File Browser** (interactive pull of student files) |
| Internet Control scope | **Block-all toggle**, **website blocklist**, **allowlist/exam mode** |
| Delivery model | I write/iterate code → your Windows folder + project; **you compile/test on Windows** |
| Branch | `veyonfork` (keep `origin` on upstream so you can pull Veyon updates) |
| Enable "from the start" | New feature plugins are **enabled by default** (only things listed in the `DisabledFeatures` config are off), so a shipped plugin is live out of the box |
| Dark mode | **Already shipped in 4.11.2** (System/Light/Dark, selectable in the Configurator) — our work is *polish + gap-fixing*, not building it |

**Why file-browser first:** Veyon already ships a `filetransfer` plugin with *distribute* (push) and *collect* (pull-by-pattern) directions, including chunked data transfer over the feature channel and a background worker. An interactive browser reuses ~70% of that plumbing — lowest risk, high value.

---

## 2. Architecture primer (how Veyon is put together)

**Processes / components**

- **Master** (`master/`) — the teacher GUI: the monitor grid + a toolbar auto-populated from available *features*.
- **Service** (`service/`) — a system-level (SYSTEM) background service on each student PC. Manages per-session **server** instances (`WindowsServiceCore`).
- **Server** (`server/`) — runs inside each interactive user session; hosts the VNC server and dispatches feature messages.
- **Worker** (`worker/`) — a helper process the server spawns *in the user's context* for heavier or user-scoped jobs (demo client, file collection). Managed by `FeatureWorkerManager`.

**Plugin + Feature framework** (all in `core/src/`)

- Everything the teacher can *do* is a **Feature** provided by a plugin implementing `FeatureProviderInterface` (+ `PluginInterface`, optionally `ConfigurationPagePluginInterface`).
- A plugin is a `QObject` with `Q_PLUGIN_METADATA(IID "io.veyon.Veyon.Plugins.<Name>")`, discovered and loaded automatically. Built with the `build_veyon_plugin()` CMake macro (`cmake/modules/BuildVeyonPlugin.cmake`).
- A `Feature` carries a **UID** (QUuid), display name/icon/shortcut, and **flags**: `Mode`/`Action`/`Session`/`Meta`/`Option`/`Checked` (behaviour) and `Master`/`Service`/`Worker`/`Builtin` (which components it lives in).
- Features talk across components via **`FeatureMessage`**: `{ featureUid, command (enum), arguments (int-keyed QVariantMap) }`. Sent with `ComputerControlInterface::sendFeatureMessage(msg)`.

**Message handling seams** — one plugin object gets called on every tier; you implement only the tiers you need:

```
Master  →  controlFeature() / startFeature()            // decide + send messages
        →  handleFeatureMessage(ComputerControlInterface, msg)   // handle replies FROM a client
Server  →  handleFeatureMessage(VeyonServerInterface, ctx, msg)  // runs on the student PC (in-session)
        →  handleFeatureMessageFromWorker(server, msg)
        →  sendAsyncFeatureMessages(server, ctx)         // push notifications/state
Worker  →  handleFeatureMessage(VeyonWorkerInterface, msg)       // user-context heavy lifting
```

**Platform abstraction** (`core/src/Platform*Functions.h`, Windows impls in `plugins/platform/windows/`)

- `PlatformCoreFunctions` — `reboot()`, `powerDown()`, `runProgramAsUser()/AsAdmin()`, `getApplicationName(pid)`, `isRunningAsAdmin()`…
- `PlatformNetworkFunctions` — **`configureFirewallException(appPath, desc, enabled)`**, `ping()`… (our Internet-Control hook)
- `PlatformServiceFunctions`, `PlatformFilesystemFunctions`, `PlatformUserFunctions`, `PlatformSessionFunctions`, `PlatformInputDeviceFunctions`.
- New OS-level primitives (enumerate/kill processes) get added here with a Windows implementation.

**Config** — `VeyonConfigurationProperties.h`. `DisabledFeatures` is a `QStringList` of feature UIDs; anything not listed is enabled. New config objects (e.g. a process ban-list) plug into the same system and are readable by master + service.

---

## 3. Feature designs

### 3.1 Remote File Browser  *(priority — build first)*

**Goal:** teacher selects a student, opens a two-pane explorer, navigates the student's drives/folders **live**, and downloads (retrieves) files. v2 adds upload / delete / rename / new-folder.

**Model:** new plugin `plugins/remotefilebrowser/` (keep `filetransfer` untouched; reuse its transfer *patterns*). Could alternatively be a 3rd feature *inside* filetransfer — but a separate plugin keeps your fork's changes isolated and easy to rebase onto upstream.

**Where the client side runs:** filesystem access must be in the **user's session context** so it sees the student's files/permissions. Follow the `filetransfer` `FileCollectWorker` pattern (worker process); confirm during implementation whether the in-session server is sufficient for listing (it likely is) vs. needing the worker for large reads.

**Message protocol** (new `Command` + `Argument` enums on the plugin):

| Direction | Command | Arguments |
|---|---|---|
| M→C | `GetDrives` | — |
| C→M | `DriveList` | `Drives[]` (letter, label, type, free/total) |
| M→C | `ListDirectory` | `Path`, `RequestId` |
| C→M | `DirectoryListing` | `RequestId`, `Path`, `Entries[]` (name, type=file/dir/link, size, mtimeMs, attrs), `Error?` |
| M→C | `DownloadFile` | `TransferId`, `Path` |
| C→M | `FileInfo` | `TransferId`, `FileSize`, `Error?` |
| C→M | `FileDataChunk` | `TransferId`, `Data` (QByteArray, ~64–256 KB/chunk) |
| C→M | `FileFinished` | `TransferId` |
| M→C | `CancelTransfer` | `TransferId` |
| *(v2)* M→C | `UploadFile` / `Delete` / `Rename` / `MakeDir` | path(s), data |
| *(v2)* C→M | `OperationResult` | `RequestId`, ok/Error |

Download reuses the `filetransfer` *collect* flow in reverse-of-distribute: client worker reads the file in chunks and streams `FileDataChunk` messages; master appends to the chosen local destination and tracks progress. (Chunk sizing + backpressure: mirror `FileReadThread`/`ProgressItemDelegate`.)

**Master UI:** `startFeature()` opens a `RemoteFileBrowserDialog` (a `.ui`): left = drive/tree, right = file list (name/size/type/modified), a path bar, Download/Refresh buttons, a transfer progress list. Multi-select of students in the grid → browse the first, or queue the same download path across all selected (nice-to-have).

**Files (v1):**
```
plugins/remotefilebrowser/
  CMakeLists.txt
  RemoteFileBrowserPlugin.{h,cpp}      // FeatureProviderInterface + PluginInterface
  RemoteFileBrowserFeature.*           // Feature UID/flags/metadata
  RemoteFileBrowserDialog.{h,cpp,ui}   // master UI
  RemoteFileListModel.{h,cpp}          // Qt model for the file pane
  RemoteFileBrowserWorker.{h,cpp}      // client-side FS access + chunked read
  FileEntry.h                          // serialisable dir-entry struct
  remotefilebrowser.qrc + icons
```

**Windows specifics:** `QDir`/`QFileInfo`/`QStorageInfo` cover listing, sizes, drives, timestamps cross-platform — minimal native code needed for v1. Long-path (>260 char) and permission-denied cases handled per-entry (return an `Error` on the entry, keep going).

**Security (see §5):** ride Veyon's existing master↔client auth; **audit-log** every download (operator, path, bytes, time); consider a service-config **allow/deny root list** so admins can constrain what's browsable (e.g. block `C:\Windows`, credential stores). Destructive v2 ops get a confirm + log.

**Effort:** v1 (browse + download) **M** (reuses transfer plumbing). v2 (write ops) **S–M**.

---

### 3.2 Internet Control  *(three modes)*

**Goal:** control student internet, class-wide or per-student. New plugin `plugins/internetcontrol/`, **enforced in the service** (SYSTEM/admin — required to touch firewall/hosts/proxy). Feature exposes a mode toggle + a config page for lists. Enabled by default.

| Mode | What it does | Recommended Windows mechanism | Caveats |
|---|---|---|---|
| **Block all (toggle)** | Cut internet for selected students on demand; restore on release | **Windows Firewall**: block all outbound except Veyon + essentials (reuse `configureFirewallException`; drive via WFP or `netsh advfirewall`) | Clean + reliable; local LAN to teacher stays up |
| **Blocklist** | Named sites/domains always blocked | **hosts file** (→ `0.0.0.0`) for v1; DNS/proxy for stronger | Domain-level only; bypassable via hardcoded IPs, DoH, VPN |
| **Allowlist / exam** | Only approved sites reachable; everything else blocked | **System proxy / PAC** that permits only listed domains, **plus** block-all-except-proxy so it can't be bypassed | Strongest; the exam-grade path. Requires proxy/PAC push |

**Best long-term engine:** a **Windows Filtering Platform (WFP)** provider gives kernel-level, per-app/IP/port control for block-all and app rules; combine with a **filtering proxy/PAC** for domain allowlists (firewall alone can't match URLs). v1 can ship firewall + hosts to get value fast; v2 upgrades exam mode to proxy/PAC (+ optional WFP).

**Protocol sketch:** M→C `SetMode(Off|BlockAll|Blocklist|Allowlist)`, `SetBlocklist(domains[])`, `SetAllowlist(domains[])`; C→M `StateChanged(mode, active)`. Lists persist in a new config object so they survive restarts and can be set as a global policy.

**Caveats to surface to users:** DoH/VPN/mobile-tethering can defeat hosts/DNS approaches; real lockdown needs proxy/WFP. Always allow the teacher/LAN so control isn't lost.

**Effort:** v1 (block-all + hosts blocklist) **M**. v2 (proxy/PAC allowlist, WFP) **L**.

---

### 3.3 Remote Task Manager  *(view / kill / ban)*

**Goal:** teacher opens a live process list for a student, can **kill** a process, and maintain a **ban list** that the service enforces — kill-on-sight and (stronger) prevent-launch — per client or pushed to all clients.

**New platform primitive** (`PlatformCoreFunctions` or a new `PlatformProcessFunctions`):
```cpp
struct ProcessInfo { qint64 pid, parentPid; QString name, path, user; quint64 memoryBytes; };
QList<ProcessInfo> runningProcesses();
bool killProcess(qint64 pid);
```
**Windows impl:** `CreateToolhelp32Snapshot`/`Process32Next` (or `EnumProcesses` + `QueryFullProcessImageName`) for the list; `GetProcessMemoryInfo` for RAM; `WTSEnumerateProcesses` to attribute a session/user; `TerminateProcess` to kill. Runs in the **service** (SYSTEM) so it can see and kill any session's processes.

**Ban enforcement (tiers):**
1. **v1 — kill-on-sight:** service keeps a persisted banned-image-name list; on a timer (or a WMI `Win32_ProcessStartTrace` event hook) it terminates any match. Simple, effective, slight lag.
2. **v2 — prevent-launch:** Image File Execution Options ("Debugger" redirect) or AppLocker/SRP policy to stop the binary starting at all. Stronger, needs care/admin.

**"Ban for any client":** master pushes the ban list either to selected clients or to **all** (global policy stored in Veyon config, applied by every service on start). Persisted so it holds across reboots.

**Protocol:** M→C `RequestProcessList`→ C→M `ProcessList(entries[])`; M→C `KillProcess(pid)`; M→C `SetBanList(names[], scope)`; C→M `BanListAck`.

**Master UI:** a `RemoteTaskManagerDialog` — sortable table (name/PID/user/RAM), End-Task button, and a Ban-list editor.

**Security:** highest-privilege feature here — killing/banning affects a live machine. Confirm destructive kills, audit every kill/ban (operator/target/process/time), and never expose an unauthenticated path. Guard against banning critical system processes (maintain a protected-process denylist so a teacher can't brick `winlogon`/`csrss`).

**Effort:** v1 (list + kill) **M** (native code is the main cost). Ban enforcement **M**; prevent-launch **M–L**.

---

### 3.4 Dark Mode (Master + Configurator)  *— already present; scope is polish*

**Status: shipped upstream in 4.11.2.** Before writing any code, here's what already exists:

- `VeyonCore::UiColorScheme { System, Light, Dark }` and `UiStyle { Fusion, Native }` (Fusion is the default) — `core/src/VeyonCore.h`.
- Config properties `ColorScheme` and `Style` under the `UI` section — `core/src/VeyonConfigurationProperties.h` (default `System`).
- **The Configurator already exposes both as dropdowns** — `configurator/src/GeneralConfigurationPage.ui` contains `uiStyle` and `uiColorScheme` combo boxes. A user can already choose Light / Dark / System today.
- `VeyonCore::useDarkMode()` resolves the choice; `System` follows the OS via `QGuiApplication::styleHints()->colorScheme()` (Qt ≥ 6.5), and startup applies `setColorScheme(Dark|Light)`.
- Dark icon variants (`*-dark.png`) exist across `core/resources` and `master/resources`, selected at runtime in `MainWindow`, `ComputerMonitoringWidget`, `SlideshowPanel`, `SpotlightPanel`, `ScreenshotManagementPanel` and `Toast`.

**So the fork's job is closing gaps, not building the feature.** Candidate work, in order:

1. **Verify** — build and eyeball Master + Configurator in explicit Dark and in System-follows-Windows. Note anything unreadable or off.
2. **Icon-coverage audit** — among *plugins*, only `filetransfer/FileCollectDialog` calls `useDarkMode()`. Other plugin icons/dialogs likely ship a single light-oriented variant that reads badly on dark. Script an audit for icons with no `-dark` sibling and fill the gaps.
3. **Hardcoded colours** — check `VncViewWidget`, the monitoring-grid background, `Toast`, and any plugin dialog that sets a fixed colour instead of using palette roles.
4. **Live switching** — the scheme is applied at startup, so changing it likely needs a restart today. Applying instantly (re-broadcast palette + reload icon set) is a genuine QOL win.
5. **Qt version gate** — `System` detection is behind `#if QT_VERSION >= 6.5`. Build against Qt ≥ 6.5 or `System` silently won't follow Windows.
6. **Fork rule: new plugins are dark-aware from day one** — the file browser, task manager and internet-control dialogs use `VeyonCore::useDarkMode()` for icon suffixes and palette roles, never hardcoded colours.

**Effort:** verification **S**; icon/colour gap-fixing **S–M**; live switching **M**.

---

### 3.5 Lockable Audio Control

**Goal:** teacher sets volume / mutes student machines, and can **lock** it so students can't change it back. Confirmed greenfield — Veyon has no audio feature of any kind (nothing in `veyon-cli feature list`, no audio API usage anywhere in the tree).

**Feature modelling** — mirror `screenlock`, the existing "mode that enforces something until released":

```cpp
// screenlock's pattern:
Feature::Flag::Mode | Feature::Flag::AllComponents          // parent
Feature::Flag::Mode | Feature::Flag::AllComponents | Meta   // sub-feature
```

- `AudioControl` — parent, `Mode` (stays active until stopped)
- `SetVolume` — `Action`, carries level 0–100
- `MuteAudio` — `Mode | Checked` toggle
- `LockAudio` — `Mode`, enables client-side enforcement

**Protocol:** M→C `SetVolume(level)`, `SetMute(bool)`, `SetLock(bool)`; C→M `AudioState(level, muted, locked)` so the master reflects real state.

**⚠️ Placement trap:** audio endpoints are **per-session**. The Veyon *service* runs as SYSTEM in session 0 and has **no audio device**. This must run in the **server/worker (user session)**, like the file browser's worker. Getting this wrong produces silent no-ops that are painful to debug.

**Windows implementation** — Core Audio:

```
IMMDeviceEnumerator → GetDefaultAudioEndpoint(eRender, eConsole) → IAudioEndpointVolume
  SetMasterVolumeLevelScalar(0.0–1.0, &contextGuid)
  SetMute(BOOL, &contextGuid)
```

*Enforcement (the "lock"):* there is no OS-level lock API. Register an `IAudioEndpointVolumeCallback`; on `OnNotify`, if the change did **not** originate from our own context GUID and the lock is active, immediately re-apply the enforced level. A revert-watchdog is how this is done in practice.

*Plus:* extend the existing `KeyboardShortcutTrapper` (core abstraction, `WindowsKeyboardShortcutTrapper` impl, surfaced via `PlatformInputDeviceFunctions`) to swallow `VK_VOLUME_UP` / `VK_VOLUME_DOWN` / `VK_VOLUME_MUTE`. Without it the watchdog still wins, but the slider visibly fights the student; swallowing the keys is much cleaner.

**Linux implementation:** PipeWire (already a Veyon dependency — `libpipewire-0.3-dev`, used by the pipewire VNC plugin) or PulseAudio via `pactl set-sink-volume` / `set-sink-mute`; enforce by subscribing to sink events and reverting.

**New platform primitive** (`PlatformCoreFunctions`, or a new `PlatformAudioFunctions`):

```cpp
bool setMasterVolume(int percent);
int  masterVolume() const;
bool setMuted(bool muted);
bool isMuted() const;
```

**Safety requirements — not optional:**

1. **Auto-release on master disconnect.** If the teacher's app crashes or the network drops, a student must never be left permanently muted or locked. Mirror `screenlock`'s `Operation::Stop`/disconnect handling, and restore the *previous* volume on release rather than a hardcoded default.
2. **Accessibility.** A forced mute silences screen readers and assistive audio for students who depend on them. Prefer "lock at level X" over "mute everything" as the default teaching action, and consider exempting assistive apps via per-app session volume (`IAudioSessionManager2`) instead of muting the endpoint.
3. **Audit** lock/unlock events like other high-privilege actions.

**Effort:** v1 (set volume + mute + revert-watchdog lock) **M**. Key-swallowing and per-app exemptions **S–M** on top.

---

### 3.6 Screen share / demo at the Windows login screen

**Goal:** broadcast the teacher's screen (and messages) onto student machines that are sitting at the login prompt, before anyone has logged in.

**Most of the machinery already exists:**

- The service starts a server per WTS session using **`winlogon.exe`'s token** as the base process — `WindowsServerProcess::start()` calls `WtsSessionManager::findProcessId("winlogon.exe", sessionId)` and passes it to `runProgramInSession()`. `winlogon.exe` exists *before* anyone logs in, so a Veyon server is already running at the login screen.
- `runProgramInSession(program, params, env, baseProcessId, **desktop**, stdin)` already takes a target **desktop**.
- `activeDesktopName()` reports the active desktop (Windows: `GetUserObjectInformation(..., UOI_NAME, ...)`), and `DesktopInputController` already does `OpenInputDesktop` + `SetThreadDesktop`.

**The single blocker** — `core/src/FeatureWorkerManager.cpp`:

```cpp
const auto currentUser = VeyonCore::platform().userFunctions()
        .queryCurrentUserProperty(PlatformUserFunctions::UserProperty::LoginName);
if( currentUser.isEmpty() )
{
    vDebug() << "could not determine current user - probably a console session with logon screen";
    return false;          // <-- worker never starts, so the demo client never appears
}
```

Features that paint UI on the client (Demo, TextMessage) run as a **worker**, and workers are launched via `runProgramAsUser()`. With nobody logged in there is no user to run as, so the worker is refused. The upstream comment names this exact scenario.

**Fix:** when `currentUser` is empty, launch the worker as **SYSTEM in the console session on the `Winlogon` desktop** instead — reusing the winlogon-token path `VeyonServerProcess` already uses, with `Winlogon` as the desktop argument `runProgramInSession` already accepts.

1. Add `PlatformCoreFunctions::runProgramAsSystemOnDesktop(program, args, desktop)` (Windows: winlogon base process + `STARTUPINFO.lpDesktop`).
2. In `FeatureWorkerManager::startWorker()`, branch on empty `currentUser` to that path rather than returning `false`.
3. Follow desktop switches — watch `activeDesktopName()` and restart/reattach the worker when the input desktop changes (Ctrl+Alt+Del, UAC, or a user logging in).

**Security — deliberate constraints, not optional:**

1. **Output only on the secure desktop.** A full-screen overlay on the login screen is structurally identical to a credential-harvesting attack. The pre-login worker must **never capture keyboard input**, and remote *control* of the `Winlogon` desktop belongs behind a separate, default-off admin setting.
2. **Unmistakably not a login prompt** — no password-shaped fields, visible "broadcast" branding, and the real login always reachable.
3. **Audit** when a pre-login broadcast starts and stops.

**The other direction** (teacher *views* login screens in the grid) most likely already works, since the server runs pre-login and the builtin VNC server captures the console session — worth testing before building anything.

**Effort:** **M**. The plumbing exists; it's one new platform primitive, removing one guard, and desktop-switch handling.

---

## 4. QOL / enhancement backlog (prioritised)

1. **Exam / Focus mode** — one click = internet allowlist + USB-storage block + ban distracting apps + freeze new launches + full-screen notice. Bundles the three features into the highest-value classroom workflow.
2. **USB mass-storage block** — service toggles the `USBSTOR` policy; big for exam integrity. Small, high value.
3. **Two-way chat + "raise hand"/help request** — Veyon has one-way text messages today; add student→teacher. Improves everyday teaching.
4. **Activity logging / attendance** — who was online when, foreground app/window over time, exportable. (Privacy-sensitive — see §5.)
5. **Remote clipboard get/set** — quick assist / paste to students.
6. **Session screen recording** — record a student (or the demo) to disk for review. (Privacy-sensitive.)
7. **Groups & saved policies** — apply a lockdown profile to a room/class in one action.
8. **Foreground app/site indicator in the grid** — at-a-glance "who's off-task."
9. **Print control** — block printing during exams.
10. **Remote command / PowerShell runner** (admin-restricted) — powerful; gate hard.
11. **Audio** — broadcast teacher audio / listen to a student. (Consent-sensitive.)

*(Wake-on-LAN / power on already exists in `powercontrol` — verify before duplicating.)*

---

## 5. Security & responsible use

These features are legitimate for **managed/institutional devices**, but they are genuinely powerful (remote file read, process kill, internet lockdown, activity capture). Build them so they can't be misused or turned against the operator:

- **Transparency & policy.** Monitoring/control should be disclosed to students and deployed under an acceptable-use policy. Some jurisdictions restrict webcam/mic/keystroke capture and off-task monitoring — worth checking before shipping the more invasive QOL items (recording, audio, keylogging is intentionally *not* on the list).
- **Ride existing auth.** Veyon already authenticates master↔client (key-file or logon auth) over its protocol. Every new high-privilege feature must use that same channel — **no new unauthenticated endpoints.**
- **Audit everything.** File retrievals, kills, bans, and lockdowns each get a log line: operator, target, action, time. This is both accountability and debugging.
- **Least privilege & guardrails.** Optional browsable-root allow/deny list; a protected-process denylist for the task manager; confirmations on destructive/irreversible ops; always keep the teacher/LAN reachable so control can't be self-severed.
- **Attack surface.** Remote file browse/download + remote kill raise the stakes if a master key leaks. Keep the transport encrypted/authenticated, and treat the new capabilities as security-relevant in reviews.

---

## 6. Build & test on Windows

> **Correction:** an earlier draft of this doc said to use Visual Studio / MSVC. **That was wrong.** Veyon does not build with MSVC. Per `.gitlab-ci.yml` and `.ci/windows/build.sh`, Windows binaries are **cross-compiled from Linux using MinGW-w64**. Don't install Visual Studio for this.

### Path A — build for Linux (fastest iteration on feature code)

The Remote File Browser is portable Qt (`QDir`/`QFileInfo`/`QStorageInfo` — all present in both Qt5 and Qt6), so it runs on Linux too. This is by far the quickest way to exercise the logic.

**Debian is a first-class CI target** (`debian.11`, `debian.12`, `debian.13` are all in the build matrix). Which release you pick decides the Qt major version:

| Release | Qt | Notes |
|---|---|---|
| Debian 13 "trixie" | **Qt6** | Recommended — the modern path |
| Debian 12 "bookworm" | **Qt5** | Also fully tested by CI |
| Debian 11 "bullseye" | Qt5 | Oldest supported |

Check what you have with `cat /etc/debian_version`, then use the matching list (both taken verbatim from Veyon's own CI Dockerfiles).

**Debian 13 / trixie (Qt6)** — from `.ci/linux.debian.13/Dockerfile`:

```bash
sudo apt install -y --no-install-recommends \
  dpkg-dev ca-certificates git binutils gcc g++ ninja-build cmake file fakeroot bzip2 \
  qt6-base-dev qt6-5compat-dev qt6-l10n-tools qt6-tools-dev qt6-declarative-dev \
  qt6-httpserver-dev qt6-websockets-dev \
  libpipewire-0.3-dev libspa-0.2-dev xorg-dev libfakekey-dev libvncserver-dev \
  libssl-dev libpam0g-dev libproc2-dev libldap2-dev libsasl2-dev \
  libqca-qt6-dev libqca-qt6-plugins \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev
```

**Debian 12 / bookworm (Qt5)** — from `.ci/linux.debian.12/Dockerfile`:

```bash
sudo apt install -y --no-install-recommends \
  dpkg-dev ca-certificates git binutils gcc g++ ninja-build cmake file fakeroot bzip2 \
  qtbase5-dev qtbase5-private-dev qtbase5-dev-tools qttools5-dev qttools5-dev-tools \
  qtdeclarative5-dev qtquickcontrols2-5-dev \
  libpipewire-0.3-dev libspa-0.2-dev xorg-dev libfakekey-dev \
  libpng-dev libjpeg-dev zlib1g-dev liblzo2-dev libvncserver-dev \
  libssl-dev libpam0g-dev libproc2-dev libldap2-dev libsasl2-dev \
  libqca-qt5-2-dev libqca-qt5-2-plugins \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev
```

Then, in either case:

```bash
git submodule update --init --recursive
cmake -G Ninja -B /tmp/veyon-build -DCMAKE_BUILD_TYPE=Debug -DWITH_LTO=OFF -DWITH_TRANSLATIONS=OFF .
ninja -C /tmp/veyon-build

# confirm the new plugin built:
ls /tmp/veyon-build/plugins/remotefilebrowser/*.so
```

**On LMDE:** LMDE is Debian stable underneath, so whichever Debian base it tracks, use that release's list above. Note LMDE is *not* an official WSL distro — it needs a manual `wsl --import` of a rootfs, and its value (the Cinnamon desktop) is irrelevant to a headless build environment. For WSL, plain `wsl --install -d Debian` is the simpler and CI-matching choice.

### Path B — produce the real Windows installer (what upstream releases)

Mirrors the `build-windows` job in `.gitlab-ci.yml`:

```bash
git submodule update --init --recursive
docker run --rm -v "$PWD":/src -w /src \
  registry.gitlab.com/veyon/ci-mingw-w64:main \
  .ci/windows/build.sh x86_64        # or i686 for 32-bit
```

Output: `veyon-*win64*` (NSIS installer) in the repo root.

**⛔ CONFIRMED UNAVAILABLE (2026-09-08):** `docker pull registry.gitlab.com/veyon/ci-mingw-w64:main` returns *"error from registry: access forbidden"* (HTTP 403). The image is private to Veyon's CI. **Use Path C instead.**

### Path C — MSYS2 native Windows build *(recommended for Windows)*

Veyon's Windows code paths are gated on CMake's `WIN32`:

```cmake
if(WIN32)
    set(VEYON_BUILD_WINDOWS 1)
```

`WIN32` is true for a **native** MinGW build, and `MinGWCrossCompile.cmake` only applies if you explicitly pass `Win64Toolchain.cmake`. So a native MSYS2 build activates the correct Windows paths without any cross-compile machinery.

Use the **MINGW64** shell (matches CI's `x86_64-w64-mingw32` triplet — not UCRT64, not the plain MSYS shell).

Dependencies, derived from the `find_package` calls in `CMakeLists.txt` — Qt6 `Core/Core5Compat/Concurrent/Gui/Widgets/Network`, plus `Qca-qt6` and `OpenSSL` (both `REQUIRED`), plus zlib/png/jpeg/lzo for the bundled libvncserver:

```bash
pacman -S --needed git \
  mingw-w64-x86_64-gcc mingw-w64-x86_64-cmake mingw-w64-x86_64-ninja \
  mingw-w64-x86_64-pkgconf \
  mingw-w64-x86_64-qt6-base mingw-w64-x86_64-qt6-5compat mingw-w64-x86_64-qt6-tools \
  mingw-w64-x86_64-qca-qt6 mingw-w64-x86_64-openssl \
  mingw-w64-x86_64-zlib mingw-w64-x86_64-libpng \
  mingw-w64-x86_64-libjpeg-turbo mingw-w64-x86_64-lzo2
```

`mingw-w64-x86_64-qca-qt6` is confirmed to exist in MSYS2 — that was the dependency most at risk.

```bash
git submodule update --init --recursive
cmake -G Ninja -B build -DCMAKE_BUILD_TYPE=RelWithDebInfo -DWITH_TRANSLATIONS=OFF .
ninja -C build remotefilebrowser     # fast loop: just the plugin
ninja -C build                       # everything
```

Notes: do **not** pass `-DCMAKE_TOOLCHAIN_FILE=...Win64Toolchain.cmake` (that's the cross path). `WITH_LTO` is forced off on Windows automatically. The `windows-binaries`/NSIS installer target is cross-oriented and may need work natively — irrelevant for development, where building the plugin target is enough.

**Caveat:** upstream only tests cross-compilation, so expect some CMake friction on this path. The hard parts (GCC-family compiler + Qt6 + QCA) are all solved by MSYS2.

### Exercising the feature

1. **Veyon Configurator** → set up **authentication** (key-file auth is easiest for a local test; create + import keys).
2. Ensure the **Veyon Service** is installed and running (Configurator → Service).
3. Add a computer via the builtin directory pointing at `127.0.0.1` for a single-machine test.
4. Launch **Veyon Master** → select the computer → the new **"File browser"** toolbar button appears → drives → navigate → select a file → **Download**.

For fast iteration, once a full build succeeds, rebuilding just the changed plugin target is quick (`ninja -C <build> remotefilebrowser`).

### Verified build status (2026-09-08)

Built and installed successfully on **Debian 13 (trixie) / Qt6** under WSL2:

- `ninja remotefilebrowser` → `[27/27]`, clean; full tree → `[187/187]`, clean.
- `sudo ninja install` places `remotefilebrowser.so` in `/usr/local/lib/veyon/` alongside stock plugins.

**Headless verification — no GUI required, and a stronger check than looking for the toolbar button.** These prove the `.so` loads, plugin metadata/IID is valid, interface casts resolve, and the feature registers with `FeatureManager`:

```bash
veyon-cli plugin show     # → RemoteFileBrowser | Browse and retrieve files from remote computers | 1.0
veyon-cli feature list    # → RemoteFileBrowser
```

Both pass. Useful adjacent CLI modules: `veyon-cli config get|set|list`, `veyon-cli service`, `veyon-cli authkeys`.

**GUI under WSL2 — known issue.** WSLg renders Veyon's Qt windows blank (GPU path), under both `wayland` and `xcb`, and software-rendering env vars don't fix it. Working workaround, using the Qt VNC platform plugin that's already installed:

```bash
QT_QPA_PLATFORM=vnc veyon-master     # serves the UI on localhost:5900
```

Connect any VNC viewer from Windows to `localhost:5900` (WSL2 forwards listening ports to the host). The `does not support createPlatformOpenGLContext` warning is harmless. Alternatively run a real X server on Windows (VcXsrv/X410). Native Windows builds avoid this entirely.

**Dependency gotcha found while verifying:** `veyon-core` pulls in **QCA (Qt Cryptographic Architecture)** — `core/src/CryptoCore.h` does `#include <QtCrypto>`. On Linux that's `libqca-qt6-dev`; on Windows you'll need QCA built/available for your MSVC + Qt6 toolchain. It is easy to miss because nothing else hints at it until the first compile fails.

### Verification status of the Remote File Browser plugin

| Check | Result |
|---|---|
| `g++ -fsyntax-only` against real Qt 6 + Veyon headers | **passes** (both .cpp files) |
| `moc` on both headers (`Q_OBJECT`, `Q_INTERFACES`, `Q_ENUM`, `Q_PLUGIN_METADATA`) | **passes** |
| Compile moc output + sources to object files | **passes** (all 4 objects) |
| Windows / MSVC compile | **not yet done** — your step |
| Link + runtime behaviour | **not yet done** — needs a full build |

So the code is known to be *syntactically and semantically valid C++/Qt against Veyon's actual headers* on GCC. Expect MSVC to be stricter in places, and treat runtime behaviour as untested until it runs.

---

## 7. How we work across sessions

**Persists (safe to rely on):** your Windows folder `…\Development\VeyonFork` (code + branch `veyonfork`) and the **VeyonFork project** (this doc + decisions/notes).
**Ephemeral:** my cloud scratch copy and this chat transcript.

**Loop:** I write/modify plugin code → deliver into your folder → **you build & test on Windows** → tell me what happened → I fix/extend. To resume later, just point me at the VeyonFork project; I'll re-read this doc and the repo and continue. No need to wait for a "better" session — the only thing that ever needs your machine is the compile/run step.

---

## 8. Suggested roadmap

1. **Remote File Browser v1** — browse drives/folders + download. *(**built & verified**; end-to-end run pending)*
2. **Dark-mode polish** — verify, then close icon/colour gaps. Small and independent, so it can land any time (it's already ~90% there upstream).
3. **Internet Control v1** — block-all toggle (firewall) + hosts blocklist.
4. **Remote Task Manager v1** — process list + kill.
5. **Lockable Audio Control v1** — set volume / mute / lock. Self-contained and immediately useful day-to-day; pairs naturally with Exam/Focus mode.
6. **Exam/Focus mode + USB block** — bundle for the marquee classroom workflow (internet allowlist + audio lock + app bans + USB off, in one click).
7. **v2 passes** — file write-ops; proxy/PAC allowlist (exam-grade); ban enforcement/prevent-launch; audio key-swallowing + per-app exemptions; live theme switching.

---
*Generated with Claude Code — session continuity via the VeyonFork project.*
