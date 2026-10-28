# VST3 hosting in the ORY-1 desktop app: plan

Status: **all six stages done**: spike, proof of concept, robustness, parameters, editor windows, saving plugin state, and packaging. See [Progress](#progress) at the end.

Goal: in the desktop app (Windows first), a **VST3** section in the Rack lists the user's installed VST3 instruments and effects. Each one becomes a module in a patch. Notes arrive on its event input, sound comes out of its audio output, effects take audio in, a few dials drive plugin parameters, a button opens the plugin's own editor, and the plugin's state is saved with the song. The website shows these modules as placeholders.

## 1. Native host approach

**Use Steinberg's VST3 SDK directly (v3.8.0, MIT-licensed since October 2025), not JUCE.**

- The SDK is MIT-licensed, which is compatible with GPL-3.0, and needs no agreement with Steinberg. (Before 3.8 it was dual GPLv3/proprietary.) JUCE 8 has moved to AGPLv3/commercial and brings a whole framework when ORY-1 only needs hosting.
- The SDK ships the hosting pieces we need: module loading (`public.sdk/source/vst/hosting/module_win32.cpp`), `PlugProvider`, `HostApplication`, `ProcessData`, `EventList`, `ParameterChanges`, memory streams, and the `moduleinfo.json` reader. Its `EditorHost` and `AudioHost` samples are working references.
- It is pinned as a git submodule at `native/vst3sdk` (tag `v3.8.0_build_*`).

**Plugins run in a separate helper process, `ory1-vsthost.exe`, not inside ORY-1.**

A crashing or hanging plugin is common. If it ran in the page's process, it would take the page down with it, along with any unsaved song. With a helper process, a crash only kills the helper. ORY-1 then marks the module **crashed: click to restart**, and the song, wires and saved state are kept.

The pieces:

| Piece | Where it runs | Job |
|---|---|---|
| `ory1-vsthost.exe` (C++) | its own process, started by `desktop/main.js` | scans and loads plugins; processes audio on a real-time thread (MMCSS "Pro Audio" priority); owns the editor windows |
| `vstbridge.node` (small Node-API addon) | the page's process, loaded by `desktop/preload.js` | moves audio and note blocks between the page's `SharedArrayBuffer` and the helper's shared memory, on its own native thread |
| `vst3` module + `vst-bridge` AudioWorklet | `play/index.html` | the module in the patch; the worklet that feeds the plugin and plays what comes back |
| control channel | main process ↔ helper over the helper's stdin and stdout (JSON lines; plugins' own output is moved to stderr); page ↔ main over IPC (`vst:*` in preload) | scan, load, unload, parameter list, get/set state, open editor |

Two changes to the desktop app come with this:

- **The preload loses Chromium's sandbox (`sandbox: false`) so that it can load the addon.** `contextIsolation` stays on, so the page still sees only the `orreryDesktop` bridge and has no Node access.
- **`SharedArrayBuffer` must be available to the page.** Normally that needs cross-origin isolation headers, which can't be set on `file://`. Moving the page to a custom `app://` origin is **not an option**: the song library lives in the `file://` origin's storage and would appear empty. The plan is the Chromium switch `--enable-features=SharedArrayBuffer`, to be proved in stage 0. If the switch doesn't work, the fallback is to pass blocks through `MessagePort`s, which adds jitter and needs a deeper buffer.

**Rejected alternatives:**

- Hosting plugins inside the Electron main process: audio would flow over IPC through the main process's event loop, which jitters, and a plugin crash would close the app.
- Letting the helper play to the sound card itself: plugins would then sit outside ORY-1's audio graph, so there would be no effects chains, Ducker, Looper or master limiter on them.

## 2. Audio between the helper and Web Audio, and the latency

The page's `vst3` module is an `AudioWorkletNode` (stereo in, stereo out). Each 128-frame render quantum, the worklet:

1. writes its input block and the context frame number into a ring buffer in a `SharedArrayBuffer`;
2. reads the output block the helper finished *D* quanta earlier.

On the other side, the bridge thread copies blocks to and from the helper's shared memory. It signals the helper with a Windows event, and the helper calls `IAudioProcessor::process` at the AudioContext's own sample rate (`setupProcessing` uses `ctx.sampleRate`, and is redone when the engine mode rebuilds the context).

**Why *D* is more than 1:** on Windows, Chromium renders several quanta back to back for each sound-card buffer (about 10 ms in the *balanced* mode). The helper only gets time to run between those bursts, so *D* is about one sound-card buffer plus one quantum.

**What that costs:**

- **Instruments: no audible latency.** ORY-1 schedules notes 100–300 ms ahead with exact audio-clock times (`AUD.*.ahead`). The bridge sends each note *D* quanta early, so it sounds exactly on time.
- **Effects: about 13–32 ms extra, depending on the engine mode (measured in stage 2), plus the plugin's own reported latency (`getLatencySamples`).** Audio going through an effect can't be known in advance. Version 1 doesn't delay the dry paths to match. A later option could offer latency compensation for parallel dry/wet patches.
- **When the helper is late:** that quantum plays silence and is counted as a dropout in **Settings → Audio**, next to the existing playback stats.

The exact *D* for each engine mode is measured in stage 0.

## 3. Notes and parameter automation

**Notes:**

- An ORY-1 event `{t, deg, vel, len}` becomes a VST3 `NoteOnEvent` at the exact sample offset, using MIDI `48 + root + degToSemi(deg)` plus **octave** and **transpose** dials (the same mapping the Synth uses).
- A matching `NoteOffEvent` follows at `t + len`, or after a **length** dial's time when the event has no `len`.
- Notes are written by the page's main thread into an event ring in the same `SharedArrayBuffer`. Because they are scheduled 100+ ms ahead, main-thread jitter doesn't matter.
- Stop, unloading the module and restarting the helper all send note-offs for every sounding note.
- Like the other voices, the module has `hit` and `env` outputs.

**Parameters:**

- When a plugin loads, the helper reports its parameter list: id, title, units, step count, default, and whether it can be automated.
- A `vst3` module has **8 dials**. Each dial is assigned to one plugin parameter: by default the first automatable ones, and changed from a parameter picker on the module (searchable, because synths have hundreds). The assignment is kept in `node.data.map`.
- The dials are ordinary ORY-1 dials, so they can be modulated, MIDI-learned, put in Scenes and saved.
- The scheduler tick (every 25 ms) sends dial values, including modulation, as `IParameterChanges` points with sample offsets. That is smooth for sweeps but not audio-rate.
- Changes made in the plugin's editor come back through `IComponentHandler::performEdit` and move the matching dial.
- Dials currently have fixed labels per module type, so the page change here is to let a dial's label and format come from the node.

## 4. Plugin scanning

**Folders scanned:**

- `C:\Program Files\Common Files\VST3` (the standard system folder)
- `%LOCALAPPDATA%\Programs\Common\VST3` (the standard per-user folder)
- `Documents\ORY-1\VST3` (new; created next to `plugins` and `samples`, with **File → Open VST3 Folder**)

**How scanning works:**

- Each `.vst3` bundle is scanned by a short-lived `ory1-vsthost.exe --scan <path>` with a timeout. A plugin that hangs or crashes while it is scanned is skipped and listed as blocked; it can be retried from Settings.
- When the bundle has `Contents/Resources/moduleinfo.json`, that is read without loading the plugin. Otherwise the factory is loaded and its classes listed.
- Only `kVstAudioEffectClass` classes are kept. Their sub-categories decide where they go: **VST3 Instruments** or **VST3 Effects**.
- Results are cached in `%APPDATA%\Orrery\vst3-cache.json`, keyed by path, size and modified time, so only new or changed bundles are scanned on later starts.
- The Rack has a **Rescan** item, and changes to the user folder are watched the way `plugins` and `samples` already are.

## 5. Plugin editor windows

- A button on the module opens the plugin's own editor (`IEditController::createView("editor")`).
- The helper creates a top-level window titled *Plugin name – ORY-1*. It is owned by ORY-1's main window: `win.getNativeWindowHandle()` is passed to the helper and set as the owner, so the editor stays above ORY-1 and minimises with it.
- Resizable editors are supported through `IPlugFrame::resizeView` and `canResize`/`checkSizeConstraint`. HiDPI is handled through `IPlugViewContentScaleSupport`.
- Closing the window only hides the editor; the plugin keeps playing. The window's position is remembered for each module.
- Editors run on the helper's UI thread and message loop, and audio on its real-time thread, as the SDK requires.

## 6. Saving a plugin's state with the song

`serialize()` is synchronous, but asking the helper for state is not. So the page keeps the **latest state already cached**:

- The helper reports that a plugin's state has changed: after an edit, `restartComponent`, `IComponentHandler2::setDirty`, or a dial change.
- About half a second later, the page fetches `IComponent::getState` plus `IEditController::getState` and stores them in `node.data.vst`. It does this again before the window closes.
- `node.data.vst` is `{ cid, name, vendor, version, comp, ctrl }`, with both states as base64.

**Large states:** samplers like Kontakt can hold megabytes of state, and the song library sits in browser storage, which has only a few megabytes. States over 64 KB are therefore written to `Documents\ORY-1\vst-states\<sha256>.vststate`, and the song keeps only the hash. Identical states share a file.

**Loading a song:** `applyPatch` creates the module, the helper loads the plugin by class id and applies `setState` (component first, then controller), and the dials pick up their assignments. If the plugin isn't installed, the existing **missing** placeholder keeps its data and wires, as it does today for missing JavaScript plugins.

## 7. Build toolchain and packaging

**Toolchain:**

- Visual Studio Build Tools 2026 (MSVC v14.5) and VS 2022 Community are both installed on this machine. The VS install includes CMake; otherwise it comes from `winget install Kitware.CMake`.
- `native/host`: a CMake project for `ory1-vsthost.exe`, linking the SDK's `sdk_hosting` and `sdk_common` targets. It doesn't depend on Node or Electron at all.
- `native/bridge`: the `vstbridge.node` addon, built with **Node-API** (via `cmake-js`). Node-API is ABI-stable, so one build works in Electron 45 and later without rebuilding for each Electron version. electron-builder's `npmRebuild` step still rebuilds it as part of packaging.
- New npm scripts: `native:build` (configures and builds both) and `dist:win`, which runs `native:build` first.

**Packaging:**

- `build.extraResources` copies `ory1-vsthost.exe` and `vstbridge.node` outside the asar archive, because they must be real files to run or load.
- The first target is Windows x64 only.
- Neither binary is code-signed, as with the rest of the app today. An unsigned helper may get extra SmartScreen or antivirus attention.

## 8. The website

- There is no VST3 section in the Rack.
- A song that contains VST3 modules opens with them as dashed placeholders titled **VST3: name (desktop app)**. This uses the existing missing-module mechanism, which already keeps their data and wires.
- Saving the song on the web keeps those modules unchanged, so it still opens correctly in the desktop app.

## 9. Stages

0. **Spike (about 2 days).**
   - Confirm `SharedArrayBuffer` works on the `file://` page with the switch.
   - Confirm the preload can load a Node-API addon with `sandbox: false`.
   - Build the SDK and its `again` and `note expression synth` examples.
   - Measure *D* for each engine mode.
1. **Proof of concept.**
   - Scan, list the plugins in the Rack, load one instrument, play notes from a Ring Seq, and hear it.
   - Test plugins: the SDK's example synth, which is deterministic, plus one installed instrument.
2. **Effects.** Audio in, several instances, helper crash recovery and restart.
3. **Parameters.** The 8 assignable dials, the parameter picker, modulation, MIDI-learn, Scenes, and dials following edits in the plugin's editor.
4. **Editor windows.**
5. **Save and load.** Including state files for large states, the web placeholders, and **Settings → VST3** (folders, blocked list, rescan).
6. **Packaging.** An installer built with electron-builder, then installed and tested on Windows.
   - Before installing: back up `%APPDATA%\Orrery\Local Storage` and `Documents\ORY-1\library.json`, and ask the user first.
   - Close ORY-1 normally and wait for it to exit; never force-kill it.

## 10. Known limitations of the first version

- Windows x64 only. macOS (VST3 and possibly AU) and Linux come later, though the code is kept portable.
- Main stereo buses only: no sidechain inputs, no multi-output instruments, no mono/surround layouts.
- Notes and velocity only: no pitch bend, MPE/note expression, or MIDI CC passthrough. Parameters are reachable through the dials.
- Effect latency is not compensated (see section 2).
- Automation is applied every 25 ms, not at audio rate.
- Each plugin uses one 128-frame process call per quantum, so very heavy plugins cost more than they would in a DAW with larger buffers.
- All plugins share one helper process: one plugin crashing stops every VST3 module until the helper restarts, which is automatic. Per-plugin processes are a possible later option.
- Plugins with copy protection (iLok and similar) should work as they do in any host, but only the ones installed here can be tested.

## Progress

### Stage 0: spike (done)

- **`SharedArrayBuffer` works on ORY-1's `file://` page** with `--enable-features=SharedArrayBuffer`. The page keeps its origin, and with it the song library.
- **It reaches the AudioWorklet and the preload through `postMessage`.** The context bridge refuses to carry it, so the page posts it to the preload with `window.postMessage`.
- **The preload loads a Node-API addon** once it runs with `sandbox: false`. The addon imports Node-API from `node.exe` through a delay-load hook that points at the running executable, so the same `.node` file also works in plain Node for tests.
- **Toolchain:** the "Desktop development with C++" workload was added to Visual Studio Build Tools 2026 (MSVC 14.50, Windows SDK 10.0.26100, CMake and Ninja). `npm run native:build` finds all of these through `vswhere`.

### Stage 1: proof of concept (done)

**What exists:**

- **`native/host/main.cpp`: the helper, `ory1-vsthost.exe`.**
  - `--scan` reports a bundle's plugin classes.
  - `--host` creates the shared memory and takes `load`, `unload` and `params` commands as JSON lines.
  - It processes every loaded plugin on one MMCSS "Pro Audio" thread, and catches a crash inside `process()` and reports which plugin it was.
- **`native/bridge/bridge.cpp`: `vstbridge.node`**, whose copy thread moves blocks between each module's `SharedArrayBuffer` and the helper.
- **`native/shared/layout.h`:** the memory layout the page, the bridge and the helper share.
- **`desktop/main.js`:**
  - scans the three VST3 folders, a few bundles at a time, with a 30-second timeout each, cached in `vst3-cache.json`;
  - starts the helper on demand and passes commands to it;
  - adds **File → Open VST3 Folder**;
  - `ORY1_PROFILE=<folder>` runs the app with separate songs and Documents, for testing next to an installed ORY-1.
- **`play/index.html`:**
  - two module types: **VST3 Instrument** (note in, audio, hit and env out) and **VST3 Effect** (audio in, note in, audio out);
  - dials for level, octave, transpose and note length;
  - the `ory-vst` AudioWorklet;
  - **VST3 Instruments** and **VST3 Effects** sections in the Rack, with **Rescan** and **Open VST3 folder**;
  - placeholders on the website.

**Tested on Windows 11 (Electron 45, 48 kHz, *balanced* engine mode):**

- **Scanning:** found all 15 installed bundles plus the SDK's mda bundle, which has 34 classes.
- **Instrument:** Clock → Ring Seq → VST3 Instrument (mda JX10) → Speakers plays in the desktop app.
  - 2,748 blocks were sent and returned with **no late blocks** at *D* = 8 (21 ms).
- **Effect:** JX10 → VST3 Effect (mda Delay) → Speakers works.
  - Two plugins ran together for about 4,200 blocks with no late blocks.
- **Stop and Play:** Stop ends held notes and drops queued ones; Play again carries on with no burst of old notes.
- **Note timing**, measured with `native/test-host.js`: a note sounds **0.00–0.08 ms** from its scheduled time, so the early-sending compensation works.
- **Website:** a song with VST3 modules opens with dashed **VST3: name (desktop app)** placeholders. Their wires are kept, and the Rack has no VST3 sections.
- **Real plugins:**
  - Replika XT (Native Instruments) processes audio as an effect.
  - Massive X loads, takes notes and returns from `process()` normally, but outputs silence.

**Found while testing:**

- **mda's sample synths drop some note-ons.** The SDK's mda synths (DX10, JX10, Piano) ignore a note-on whose sample offset is a non-zero multiple of 16. Their sample-accurate base class hands such an event to the synth at the very end of a 16-sample slice, where it is skipped. This is a bug in the SDK samples, not in the host; real plugins don't share that code. In ORY-1 it means roughly one in 16 notes is lost on the mda test synths.
- **Massive X seemed silent.** Its editor (stage 4) showed why: the first time it runs in a new host, it spends a minute or two scanning its wavetable content before loading a preset. Afterwards it plays (see stage 4).

### Stage 2: robustness (done)

**A plugin that crashes:**
- The helper catches the crash inside `process()` and marks the module **crashed**, with a message.
- The module shows a **↻** button at its lower left. Tapping it loads the plugin again in a fresh slot.
- The crashed plugin's memory is left in the helper without calling into it again, because shutting it down could crash the helper as well.
- The other plugins keep playing throughout.

**The helper itself stops:** ORY-1 starts a new helper and every VST3 module loads again, with no action needed.

**Reloading the page** (View → Reload):
- The bridge drops its slots before the page's memory goes.
- The desktop app closes the old helper, so plugins aren't left running in it.
- Before this fix, a reload could have let the bridge thread write into freed memory.

**Settings → Audio** shows, for example, *VST3: 2 modules, effects +16 ms, no late blocks*.

**Self-adjusting buffer:**
- If a module keeps getting late blocks (2 or more in 2 seconds), it waits 2 blocks longer, up to 32 (85 ms).
- Notes stay on time when it does; only audio through effects gets later.
- In testing, a deliberately too-small buffer recovered within seconds.

**Tested:**
- Both crash cases were simulated in the real app with test commands that only exist when `ORY_VSTHOST_DEBUG` is set: `crash` makes a plugin's next `process()` call fail exactly as a broken plugin would, and `exit` stops the helper.
- Recovery worked in both cases, and a page reload restarted cleanly.

**Measured buffer, *D*** (Windows 11 laptop, 48 kHz, two mda plugins; each setting ran about 10–17 seconds):

| Engine mode | Smallest *D* with no late blocks | Default now | Extra effect latency |
|---|---|---|---|
| Low latency | 4 (3 failed) | 5 | 13 ms |
| Balanced | 4 or less | 6 | 16 ms |
| Stable | not pushed (16 ran clean) | 12 | 32 ms |

The defaults leave a margin of about two blocks; heavier plugins or a busier computer will use the self-adjustment. This hasn't been tried with heavy plugins yet: the NI instruments make no sound (see Massive X), and Kontakt needs an instrument loaded through its editor.

### Stage 3: parameters (done)

**The dials:**
- Each VST3 module has 8 dials, each driving one plugin parameter. Dials 1–3 are rings; dials 4–8 bloom out with octave, transpose and note length when the module is selected.
- A dial is named after its parameter (*VCF Freq*), and its value is shown in the plugin's own words (*90.00 %*, *440 Hz*). Those words are fetched from the helper and cached.
- Dials are ordinary ORY-1 dials, so they can be modulated, MIDI-learned and stored in Scenes. Their values are the plugin's own 0–1 values.
- A new module puts its dials on the first 8 parameters the plugin lets a host automate (skipping bypass and program switches), starting from the plugin's current settings, so the sound doesn't change.
- A saved song sets the plugin from its dials when it loads.

**The picker** (≡ inside the module):
- It lists the 8 dials and the plugin's parameters, with a search box.
- Tap a dial, then a parameter, to assign it; × clears a dial.
- **Learn** puts the next control moved in the plugin's own editor on the chosen dial.

**The plugin's editor:**
- Values ORY-1 sends are passed to the plugin's controller (20 times a second), so its editor shows them.
- Edits made in the editor come back to ORY-1 and move the dial assigned to that parameter.

**Fixed along the way:** the SDK's PlugProvider doesn't copy the plugin's processor state into its controller, which hosts are expected to do. The mda controllers reported every parameter as 0, and a new module's dials would have reset the sound. The helper now does that copy when it loads a plugin.

**Tested in the app with mda JX10:**
- The dials took the plugin's names and settings (OSC Mix 100 %, VCF Freq 90 %).
- Turning VCF Freq to 0.12 reached the plugin.
- An LFO on that dial swept the plugin's value between 0.03 and 0.98.
- Assigning through the picker worked, and so did search.
- Assignments and values survived a page reload and were sent back to the plugin.

**Not tested yet:** editor edits moving a dial, and Learn. Both need the plugin's editor (stage 4).

### Stage 4: editor windows (done)

**Opening an editor:** ⧉ inside a VST3 module, next to ≡, opens the plugin's own editor.
- The helper creates a window titled *Plugin · ORY-1*, owned by ORY-1's window, so it stays above ORY-1 and minimises with it.
- The window is sized to what the plugin asks for, and resizable when the plugin allows (`checkSizeConstraint`, `onSize`, `IPlugFrame::resizeView`).
- The helper runs per-monitor DPI aware and gives each editor its scale (`IPlugViewContentScaleSupport`), so editors are sharp at 125 %.
- Tapping ⧉ again brings an open editor forward. A plugin without an editor of its own (like the SDK's mda plugins, built here without VSTGUI) shows a message.

**Closing it:** the window's close button closes the editor. Its position is kept with the song, as the restored position even if it closed while minimised, and it reopens there if that spot is still on a screen; otherwise it opens centred over ORY-1. Unloading a plugin closes its editor first, as VST3 requires.

**A fix in the helper:** commands now go to a hidden message window instead of the helper's thread queue. Windows drops thread messages while a window is being dragged or resized, so moving an editor could have lost commands.

**Tested in the app:**
- **Replika XT** opened at its full size (1363 × 591 at 125 %) and showed ORY-1's tempo (120 BPM) from the transport events.
- **Editor to dial:** moving its Mix knob moved the ORY-1 dial on Mix, from 0.255 to 0.470.
- **Learn:** with Learn on, moving Mix put it on the chosen dial, then Learn switched itself off. At first it put Mix on every dial in turn, because a drag sends a stream of edits; that was fixed.
- **Position:** an editor closed at (300, 150) reopened there.
- **Massive X** opened, showed *Updating content… Scanning…* for over a minute, then loaded the *Agonic Drone* preset and played (peak about 0.05).
  - While Massive X scanned (using 1.6 GB), its module got 5 late blocks in about 3 minutes (62,700 blocks).
- These editor tests used mouse messages posted to the editor window, not the real mouse, and captured only the helper's own windows.

**Limits:**
- Presets and settings chosen in an editor are kept from stage 5 on.
- **Editor position on some screens:** it is saved in work-area coordinates. With the taskbar at the top or left of the screen, an editor may reopen offset by the taskbar's size.

### Stage 5: saving plugin state with the song (done)

**What's saved:** each VST3 module's song data (`n.data.vst`) holds the plugin's own state: its preset and its settings, including ones no dial shows. Following the VST3 rules, it is saved as the component's state and the controller's state, both base64.
- **Small states** (64 KB or less) sit in the song itself.
- **Larger ones** go in `Documents\ORY-1\vst-states\<sha256>.vststate`, and the song keeps the hash (`stateFile`). Identical states share a file. The desktop app checks the name and gives the helper the path.

**When it's saved:**
- **When the plugin reports a change.** That covers an edit, a preset or a setting (`endEdit`, `restartComponent`, `IComponentHandler2::setDirty`). Saving waits 0.7 seconds, so a drag is saved once. When many parameters change at once, after loading a preset for example, the dials also update to the plugin's new values.
- **When its editor closes,** and every 15 seconds while an editor is open, for plugins that don't report changes. A state identical to the last one isn't saved again.
- **Before the window closes:** the desktop app holds the close, the page saves every plugin's state and then the song, and the window closes. If saving hangs, it closes after 5 seconds anyway.

**Loading:**
- The helper gives the state back before switching the plugin on: the component's state goes to the component and to the controller, then the controller's own state to the controller. The dials are then sent too.
- If a song's state file is missing, the plugin starts with its own settings and ORY-1 says so.

**Tested in the app:**
- **Replika XT** (a 7 KB state, kept inline):
  - Saturation, which is on no dial, was turned from 0 to 1 in its editor. After a page reload, which starts a new helper, the plugin came back at 1.
  - Mix, taken off every dial, was changed from 0.605 to 0.680 and the window closed straight away. After relaunching ORY-1 it came back at 0.680. That shows the save-before-close works.
- **Massive X** (a 40 KB state): its state was written to a state file (39,929 bytes, named by its hash), the song pointed at it, and after a reload the helper restored from the file. It was then re-saved inline, as it is under 64 KB.
- **A song pointing at a missing state file:** the plugin loaded with its own settings, with the message *the saved state file is missing, so it starts with its own settings*.
- **Kontakt 8** with no instrument loaded has a 5 KB state. A Kontakt instrument, which would use a state file, wasn't tried, because loading one needs its own library browser.

**Limits:**
- **State files are never deleted,** even when no song uses them any more. Each saved change of a large state adds a file. A clean-up that keeps only the files songs use is a possible follow-up.
- **Exported songs** (`.ory1.json`) carry inline states but not state files. A song with a large state opens on another computer with the plugin's own settings.
- **Reloading the page** (View → Reload) doesn't wait for a final save as closing does. It relies on the change reports, the 0.7-second save, and the 15-second save while an editor is open.

### Stage 6: packaging (done)

- **Build:** `npm run dist:win` builds the native parts, then the NSIS installer. electron-builder copies `ory1-vsthost.exe` and `vstbridge.node` to `resources\native` (`build.win.extraResources`), outside the asar archive.
- **The installer:** built as *ORY-1 Setup 0.9.0-beta.1.exe*, 122 MB and unsigned like the earlier ones.
  - For testing it was built into a scratch folder, so the existing beta installer in `dist/` wasn't overwritten.
- **The packaged app,** run from `win-unpacked` with a separate test profile:
  - it found the helper and bridge in `resources\native` and listed the VST3 plugins in the Rack;
  - it played the Replika XT test song, restoring both plugins' saved states, with 1 late block in about 8,500.
- **Installed on Windows 11** over 0.9.0-beta.1, after backing up `%APPDATA%\Orrery\Local Storage` and `Documents\ORY-1\library.json` (12 songs) to `%USERPROFILE%\ORY-1 backups\before-vst3-2026-10-27_1906`.
  - The installer's wizard put it in `%LOCALAPPDATA%\Programs\Orrery\ORY-1`, a subfolder of the old install's folder, and removed the old files.
  - On first start, the installed app scanned all 15 plugin bundles and kept all 12 songs.
  - Playing a VST3 module in the installed app was left for the person to try, so that none of their songs were changed.

### Developing

```bash
npm run native:build          # helper and bridge, into native/bin
npm run native:test-plugins   # the SDK's mda synths and effects, into native/build-sdk/VST3/Release
node native/test-host.js "<path.vst3>" <class id> [seconds]   # play notes through a plugin without ORY-1
```

- **Copy test plugins** into `Documents\ORY-1\VST3` (or the test profile's `Documents\ORY-1\VST3`) so ORY-1 finds them.
- **Debug output:** set `ORY_VSTHOST_DEBUG=1` to have the helper print each event as it's delivered, plus a level summary every second. It also enables the `crash` and `exit` test commands, which the page can send with `orreryDesktop.vst.call({cmd: 'crash', slot})`.

### Next

- Possible follow-ups: cleaning up unused state files; carrying state files in exported songs; sidechain and multi-output buses; pitch bend and MPE; macOS.
