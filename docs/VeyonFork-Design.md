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

The Remote File Browser is portable Qt (`QDir`/`QFileInfo`/`QStorageInfo`), so it runs on Linux too. This is by far the quickest way to exercise the logic. Dependency list taken verbatim from Veyon's own Ubuntu 24.04 CI image (`.ci/linux.ubuntu.24.04/Dockerfile`):

```bash
sudo apt install -y git ninja-build cmake g++ file fakeroot \
  qt6-base-dev qt6-5compat-dev qt6-tools-dev qt6-l10n-tools qt6-declarative-dev \
  qt6-httpserver-dev qt6-websockets-dev \
  xorg-dev libfakekey-dev libvncserver-dev libssl-dev libpam0g-dev \
  libproc2-dev libldap2-dev libsasl2-dev \
  libqca-qt6-dev libqca-qt6-plugins \
  libavcodec-dev libavformat-dev libavutil-dev libswscale-dev \
  libpipewire-0.3-dev libspa-0.2-dev

git submodule update --init --recursive
cmake -G Ninja -B /tmp/veyon-build -DCMAKE_BUILD_TYPE=Debug -DWITH_LTO=OFF -DWITH_TRANSLATIONS=OFF .
ninja -C /tmp/veyon-build

# confirm the new plugin built:
ls /tmp/veyon-build/plugins/remotefilebrowser/*.so
```

### Path B — produce the real Windows installer (what upstream releases)

Mirrors the `build-windows` job in `.gitlab-ci.yml`:

```bash
git submodule update --init --recursive
docker run --rm -v "$PWD":/src -w /src \
  registry.gitlab.com/veyon/ci-mingw-w64:main \
  .ci/windows/build.sh x86_64        # or i686 for 32-bit
```

Output: `veyon-*win64*` (NSIS installer) in the repo root.

**Caveat:** that image lives in Veyon's own GitLab container registry and may require authentication or may not be publicly pullable. If the pull is refused, the alternative is assembling a MinGW-w64 + Qt6 cross toolchain yourself (`/usr/x86_64-w64-mingw32` with a `qt-cmake` wrapper, as `.ci/windows/build.sh` expects) — a significant undertaking.

### Exercising the feature

1. **Veyon Configurator** → set up **authentication** (key-file auth is easiest for a local test; create + import keys).
2. Ensure the **Veyon Service** is installed and running (Configurator → Service).
3. Add a computer via the builtin directory pointing at `127.0.0.1` for a single-machine test.
4. Launch **Veyon Master** → select the computer → the new **"File browser"** toolbar button appears → drives → navigate → select a file → **Download**.

For fast iteration, once a full build succeeds, rebuilding just the changed plugin target is quick (`ninja -C <build> remotefilebrowser`).

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

1. **Remote File Browser v1** — browse drives/folders + download. *(in progress)*
2. **Dark-mode polish** — verify, then close icon/colour gaps. Small and independent, so it can land any time (it's already ~90% there upstream).
3. **Internet Control v1** — block-all toggle (firewall) + hosts blocklist.
4. **Remote Task Manager v1** — process list + kill.
5. **Exam/Focus mode + USB block** — bundle for the marquee classroom workflow.
6. **v2 passes** — file write-ops; proxy/PAC allowlist (exam-grade); ban enforcement/prevent-launch; live theme switching.

---
*Generated with Claude Code — session continuity via the VeyonFork project.*
