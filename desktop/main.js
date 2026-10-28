// ORY-1 desktop: the same play/index.html as the website, in its own window, with a plugins folder it watches.
const { app, BrowserWindow, ipcMain, shell, Menu, session, powerSaveBlocker } = require('electron');
const path = require('path');
const fs = require('fs');
const { spawn } = require('child_process');

// keep the audio scheduler at full speed even when the window is hidden or minimised
app.commandLine.appendSwitch('disable-renderer-backgrounding');
app.commandLine.appendSwitch('disable-background-timer-throttling');
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');
// VST3 modules share memory with their AudioWorklet. The page stays on file:// (its saved songs belong to that origin),
// where cross-origin isolation can't be switched on, so SharedArrayBuffer is enabled directly.
app.commandLine.appendSwitch('enable-features', 'SharedArrayBuffer');

// The app was called Orrery before ORY-1. Its saved songs and settings stay where they are: Chromium creates the
// new userData folder before this script runs, so it can't be moved into place, and nobody sees its name anyway.
// The Documents folder people do see, so that one moves to the new name; if it can't (a file in it is open), the old one is used.
const oldData = path.join(app.getPath('appData'), 'Orrery');
// ORY1_PROFILE=<folder> runs with songs, settings and a Documents folder of its own, so a development build can be
// tried while the real ORY-1 is open without touching its songs
const profile = process.env.ORY1_PROFILE;
if (profile) {
  app.setPath('userData', path.join(profile, 'userData'));
  app.setPath('documents', path.join(profile, 'Documents'));
  fs.mkdirSync(app.getPath('documents'), { recursive: true });
} else if (fs.existsSync(oldData)) app.setPath('userData', oldData);
function moveFolder(from, to) {
  if (fs.existsSync(from) && !fs.existsSync(to)) { try { fs.renameSync(from, to); } catch (e) { return from; } }
  return to;
}
let docsDir = '';

if (!app.requestSingleInstanceLock()) app.quit();

// Windows slows down apps you aren't looking at (EcoQoS: power-saving cores, capped clocks, coarse timers).
// Audio is made a few milliseconds ahead, so that turns into crackles; every ORY-1 process opts out, the way audio software does.
let winApi = null;
if (process.platform === 'win32') {
  try {
    const koffi = require('koffi');
    const k32 = koffi.load('kernel32.dll');
    const PPT = koffi.struct('PROCESS_POWER_THROTTLING_STATE', { Version: 'uint32_t', ControlMask: 'uint32_t', StateMask: 'uint32_t' });
    winApi = {
      size: koffi.sizeof(PPT),
      OpenProcess: k32.func('void * __stdcall OpenProcess(uint32_t access, int inherit, uint32_t pid)'),
      CloseHandle: k32.func('int __stdcall CloseHandle(void *h)'),
      SetProcessInformation: k32.func('int __stdcall SetProcessInformation(void *h, int cls, _In_ PROCESS_POWER_THROTTLING_STATE *info, uint32_t size)'),
      SetPriorityClass: k32.func('int __stdcall SetPriorityClass(void *h, uint32_t cls)'),
    };
  } catch (e) { winApi = null; /* without it ORY-1 still runs; it just may crackle in the background */ }
}
function keepAwake(pid) {
  const h = winApi.OpenProcess(0x0200, 0, pid); // PROCESS_SET_INFORMATION
  if (!h) return;
  try {
    // ProcessPowerThrottling: take control of execution speed (0x1) and timer resolution (0x4), and turn both throttles off
    winApi.SetProcessInformation(h, 4, { Version: 1, ControlMask: 0x1 | 0x4, StateMask: 0 }, winApi.size);
    winApi.SetPriorityClass(h, 0x8000); // above normal, so the audio keeps up when other apps are busy
  } finally { winApi.CloseHandle(h); }
}
// Chromium starts processes as it needs them (renderer, audio service, GPU) and may re-throttle a backgrounded one,
// so this runs on a short timer and whenever the window goes to the background
function optOutOfThrottling() {
  if (!winApi) return;
  for (const m of app.getAppMetrics()) { try { keepAwake(m.pid); } catch (e) { /* a process that just exited */ } }
}

// while music plays, the computer must not go to sleep (the screen still can)
let sleepBlock = null;
function setPlaying(on) {
  if (on && sleepBlock === null) sleepBlock = powerSaveBlocker.start('prevent-app-suspension');
  else if (!on && sleepBlock !== null) { powerSaveBlocker.stop(sleepBlock); sleepBlock = null; }
}

let win = null;
let pluginsDir = '';
let samplesDir = '';
const AUDIO_EXT = /\.(wav|wave|mp3|ogg|oga|flac|m4a|aac|aif|aiff|opus|webm)$/i;
const bundled = path.join(__dirname, '..', 'plugins');

// first run: make the plugins folder and put the example plugins and a short readme in it
function preparePluginsDir() {
  docsDir = moveFolder(path.join(app.getPath('documents'), 'Orrery'), path.join(app.getPath('documents'), 'ORY-1'));
  pluginsDir = path.join(docsDir, 'plugins');
  const fresh = !fs.existsSync(pluginsDir);
  fs.mkdirSync(pluginsDir, { recursive: true });
  if (!fresh) return;
  for (const f of fs.readdirSync(bundled)) {
    try { fs.copyFileSync(path.join(bundled, f), path.join(pluginsDir, f)); } catch (e) { /* an example that won't copy is skipped */ }
  }
}

// the samples folder: any audio inside it (subfolders too) shows up in ORY-1's Rack
function prepareSamplesDir() {
  samplesDir = path.join(docsDir, 'samples');
  fs.mkdirSync(samplesDir, { recursive: true });
}

// a path from the page must stay inside the samples folder
function insideSamples(rel) {
  const full = path.resolve(samplesDir, String(rel));
  return full.startsWith(path.resolve(samplesDir) + path.sep) ? full : null;
}

function listSamples() {
  const out = [];
  const walk = (dir, depth) => {
    let entries = [];
    try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch (e) { return; }
    for (const e of entries) {
      if (e.name.startsWith('.')) continue;
      const full = path.join(dir, e.name);
      if (e.isDirectory() && depth < 4) walk(full, depth + 1);
      else if (e.isFile() && AUDIO_EXT.test(e.name)) out.push(path.relative(samplesDir, full).split(path.sep).join('/'));
    }
  };
  walk(samplesDir, 0);
  return out.sort((a, b) => a.localeCompare(b, undefined, { numeric: true, sensitivity: 'base' }));
}

function readSample(rel) {
  const full = insideSamples(rel);
  if (!full) throw new Error('not in the samples folder');
  return fs.readFileSync(full);
}

// imported audio is kept in samples/Imported, never overwriting a file already there
function saveSample(name, bytes) {
  const dir = path.join(samplesDir, 'Imported');
  fs.mkdirSync(dir, { recursive: true });
  const clean = String(name).replace(/[<>:"/\\|?*\x00-\x1f]/g, '_').slice(0, 120) || 'sample.wav';
  const ext = path.extname(clean), base = clean.slice(0, clean.length - ext.length);
  let file = clean;
  for (let i = 2; fs.existsSync(path.join(dir, file)); i++) file = `${base} ${i}${ext}`;
  fs.writeFileSync(path.join(dir, file), Buffer.from(bytes));
  return 'Imported/' + file;
}

function listPlugins() {
  try {
    return fs.readdirSync(pluginsDir)
      .filter(f => f.toLowerCase().endsWith('.js'))
      .sort()
      .map(file => ({ file, code: fs.readFileSync(path.join(pluginsDir, file), 'utf8') }));
  } catch (e) {
    return [];
  }
}

// the song library, mirrored from the page's browser storage into Documents\ORY-1\library.json: if that storage is ever
// damaged (a crash in the middle of a save can leave it opening empty), the page restores the songs from this file.
// Writes wait a second for edits to settle, go to a temporary file first, and the first write of each session keeps
// the file it replaces as library.previous.json.
const libraryFile = () => path.join(docsDir, 'library.json');
let libraryText = null, libraryTimer = null, libraryRotated = false;
function writeLibrary() {
  clearTimeout(libraryTimer); libraryTimer = null;
  if (libraryText === null) return;
  const file = libraryFile(), tmp = file + '.tmp';
  try {
    if (!libraryRotated && fs.existsSync(file)) fs.copyFileSync(file, path.join(docsDir, 'library.previous.json'));
    libraryRotated = true;
    fs.writeFileSync(tmp, libraryText);
    fs.renameSync(tmp, file);
    libraryText = null;
  } catch (e) { /* the file is busy (a sync client, say): the next save tries again */ }
}
function readLibrary() {
  try { return fs.readFileSync(libraryFile(), 'utf8'); } catch (e) { return null; }
}

// ---------- VST3 ----------
// Plugins run in ory1-vsthost.exe, a helper process: a plugin that crashes takes the helper down, not ORY-1.
// Scanning runs one short-lived helper per plugin file, so one that hangs or crashes while scanned is only skipped.
const nativeDir = app.isPackaged ? path.join(process.resourcesPath, 'native') : path.join(__dirname, '..', 'native', 'bin');
const hostExe = path.join(nativeDir, 'ory1-vsthost.exe');
const vstAvailable = () => process.platform === 'win32' && fs.existsSync(hostExe);
let vstDir = '';
function prepareVstDir() { vstDir = path.join(docsDir, 'VST3'); fs.mkdirSync(vstDir, { recursive: true }); }
// a plugin's saved state, when it's too big to sit in the song itself (samplers can save megabytes), goes in
// Documents\ORY-1\vst-states, named by its contents' hash: songs refer to it by that name, and identical states share a file
const statesDir = () => path.join(docsDir, 'vst-states');
const stateName = /^[0-9a-f]{64}$/;
function putState(text) {
  const hash = require('crypto').createHash('sha256').update(String(text)).digest('hex');
  const file = path.join(statesDir(), hash + '.vststate');
  if (!fs.existsSync(file)) { fs.mkdirSync(statesDir(), { recursive: true }); fs.writeFileSync(file + '.tmp', String(text)); fs.renameSync(file + '.tmp', file); }
  return hash;
}
function vstFolders() {
  const common = process.env.CommonProgramFiles || 'C:\\Program Files\\Common Files';
  const local = process.env.LOCALAPPDATA ? path.join(process.env.LOCALAPPDATA, 'Programs', 'Common', 'VST3') : null;
  return [path.join(common, 'VST3'), local, vstDir].filter(Boolean);
}
// every .vst3 bundle or file in the folders, looking inside ordinary subfolders (vendors often add one) but not inside bundles
function findVst3() {
  const out = [];
  const walk = (dir, depth) => {
    let entries = [];
    try { entries = fs.readdirSync(dir, { withFileTypes: true }); } catch (e) { return; }
    for (const e of entries) {
      const full = path.join(dir, e.name);
      if (e.name.toLowerCase().endsWith('.vst3')) out.push(full);
      else if (e.isDirectory() && depth < 3) walk(full, depth + 1);
    }
  };
  for (const d of vstFolders()) walk(d, 0);
  return [...new Set(out)];
}
// a bundle counts as changed when its binary does
function vstStamp(p) {
  const bin = path.join(p, 'Contents', 'x86_64-win', path.basename(p));
  for (const f of [bin, p]) { try { const s = fs.statSync(f); if (s.isFile()) return s.size + ':' + s.mtimeMs; } catch (e) { /* try the next */ } }
  try { return 'dir:' + fs.statSync(p).mtimeMs; } catch (e) { return ''; }
}
const vstCacheFile = () => path.join(app.getPath('userData'), 'vst3-cache.json');
let vstCache = null;
function scanOne(p) {
  return new Promise(resolve => {
    let out = '', done = false;
    const child = spawn(hostExe, ['--scan', p], { stdio: ['ignore', 'pipe', 'ignore'], windowsHide: true });
    const finish = r => { if (!done) { done = true; clearTimeout(timer); resolve(r); } };
    const timer = setTimeout(() => { try { child.kill(); } catch (e) { /* already gone */ } finish({ ok: false, error: 'took too long to scan' }); }, 30000);
    child.stdout.on('data', d => { out += d; });
    child.on('error', e => finish({ ok: false, error: e.message }));
    child.on('exit', code => {
      try { finish(JSON.parse(out.trim().split('\n').pop())); } catch (e) { finish({ ok: false, error: `crashed while scanning (${code})` }); }
    });
  });
}
async function scanVst(force) {
  if (!vstAvailable()) return [];
  if (!vstCache) { try { vstCache = JSON.parse(fs.readFileSync(vstCacheFile(), 'utf8')); } catch (e) { vstCache = {}; } }
  const paths = findVst3(), todo = [];
  for (const p of paths) { const st = vstStamp(p); if (force || !vstCache[p] || vstCache[p].stamp !== st) todo.push([p, st]); }
  for (const p of Object.keys(vstCache)) if (!paths.includes(p)) delete vstCache[p];
  // a few at a time: loading a big plugin can take seconds
  const queue = todo.slice();
  await Promise.all(Array.from({ length: 3 }, async () => {
    while (queue.length) { const [p, stamp] = queue.shift(); const r = await scanOne(p); vstCache[p] = { stamp, classes: r.ok ? r.classes : [], error: r.ok ? null : r.error }; }
  }));
  if (todo.length) { try { fs.writeFileSync(vstCacheFile(), JSON.stringify(vstCache, null, 1)); } catch (e) { /* scanned again next time */ } }
  const list = [];
  for (const [p, entry] of Object.entries(vstCache)) for (const c of entry.classes) {
    list.push({ path: p, cid: c.cid, name: c.name, vendor: c.vendor, version: c.version, sub: c.sub, instrument: /instrument/i.test(c.sub || '') });
  }
  return list.sort((a, b) => a.name.localeCompare(b.name, undefined, { sensitivity: 'base' }));
}
const vstBlocked = () => Object.entries(vstCache || {}).filter(([, e]) => e.error).map(([p, e]) => ({ path: p, error: e.error }));

// the hosting helper: started when a song first needs it, and again after a crash
let vstHost = null;
function startVstHost() {
  if (vstHost) return vstHost.ready;
  const shm = `Local\\ORY1-vst-${process.pid}-${Date.now()}`;
  const child = spawn(hostExe, ['--host', shm], { stdio: ['pipe', 'pipe', 'pipe'], windowsHide: true });
  const h = { child, shm, waiting: new Map(), nextId: 1, buf: '' };
  h.ready = new Promise((resolve, reject) => { h.resolve = resolve; h.reject = reject; });
  child.stdout.on('data', d => {
    h.buf += d;
    let i;
    while ((i = h.buf.indexOf('\n')) >= 0) {
      let msg = null;
      try { msg = JSON.parse(h.buf.slice(0, i)); } catch (e) { /* not ours */ }
      h.buf = h.buf.slice(i + 1);
      if (!msg) continue;
      if (msg.event === 'ready') h.resolve({ shm });
      else if (msg.event) win && win.webContents.send('vst:event', msg);
      else if (h.waiting.has(msg.id)) { h.waiting.get(msg.id)(msg); h.waiting.delete(msg.id); }
    }
  });
  child.stderr.on('data', d => process.stdout.write('[vsthost] ' + d));
  const gone = why => {
    if (vstHost !== h) return;
    vstHost = null;
    for (const res of h.waiting.values()) res({ error: why });
    h.reject(new Error(why));
    win && win.webContents.send('vst:event', { event: 'exit', why });
  };
  child.on('error', e => gone(e.message));
  child.on('exit', code => gone(`the VST3 helper stopped (${code})`));
  h.ready.catch(() => {});
  vstHost = h;
  return h.ready;
}
function vstCall(cmd) {
  if (!vstHost) return Promise.resolve({ error: 'the VST3 helper is not running' });
  // the page names a state file by its hash; the helper gets the path
  if (cmd.stateFile) cmd = { ...cmd, stateFile: stateName.test(cmd.stateFile) ? path.join(statesDir(), cmd.stateFile + '.vststate') : '' };
  const h = vstHost, id = h.nextId++;
  return new Promise(resolve => { h.waiting.set(id, resolve); h.child.stdin.write(JSON.stringify({ ...cmd, id }) + '\n'); });
}
// closing stdin is the helper's cue to shut its plugins down and exit by itself
function stopVstHost() {
  const h = vstHost;
  if (!h) return;
  vstHost = null; // so its exit isn't reported to the page as a crash
  for (const res of h.waiting.values()) res({ error: 'the VST3 helper was closed' });
  h.reject(new Error('the VST3 helper was closed'));
  try { h.child.stdin.end(); } catch (e) { /* already gone */ }
}
app.on('will-quit', stopVstHost);

// editors often write a file in several steps, so wait for the folder to settle before telling the page
let watchTimer = null;
function watchPlugins() {
  try {
    fs.watch(pluginsDir, () => {
      clearTimeout(watchTimer);
      watchTimer = setTimeout(() => win && win.webContents.send('plugins:changed'), 250);
    });
  } catch (e) { /* no watching on this system: Reload plugins still works */ }
}

let sampleTimer = null;
function watchSamples() {
  const ping = () => { clearTimeout(sampleTimer); sampleTimer = setTimeout(() => win && win.webContents.send('samples:changed'), 400); };
  try { fs.watch(samplesDir, { recursive: true }, ping); }
  catch (e) { try { fs.watch(samplesDir, ping); } catch (e2) { /* no watching: the list refreshes on restart */ } }
}

const stateFile = () => path.join(app.getPath('userData'), 'window.json');
function loadBounds() {
  try { return JSON.parse(fs.readFileSync(stateFile(), 'utf8')); } catch (e) { return { width: 1440, height: 900 }; }
}
function saveBounds() {
  if (!win || win.isMinimized()) return;
  try { fs.writeFileSync(stateFile(), JSON.stringify({ ...win.getNormalBounds(), maximized: win.isMaximized() })); } catch (e) { /* not fatal */ }
}

function createWindow() {
  const b = loadBounds();
  win = new BrowserWindow({
    x: b.x, y: b.y, width: b.width || 1440, height: b.height || 900, minWidth: 720, minHeight: 480,
    backgroundColor: '#0a0614', title: 'ORY-1', show: false,
    icon: path.join(__dirname, 'icon.png'),
    webPreferences: {
      preload: path.join(__dirname, 'preload.js'),
      // the preload loads the VST3 bridge addon, which needs Node; the page itself still has none (context isolation)
      contextIsolation: true, nodeIntegration: false, sandbox: false,
      backgroundThrottling: false,
    },
  });
  if (b.maximized) win.maximize();
  win.once('ready-to-show', () => win.show());
  // before the window closes, the page asks each VST3 plugin for its state and saves the song (a few seconds at most)
  let flushed = false;
  win.on('close', e => {
    saveBounds();
    if (flushed || !vstHost) return;
    e.preventDefault();
    flushed = true;
    let closed = false;
    const done = () => { if (closed) return; closed = true; ipcMain.removeListener('vst:flushed', done); if (win && !win.isDestroyed()) win.close(); };
    ipcMain.on('vst:flushed', done);
    setTimeout(done, 5000);
    win.webContents.send('vst:flush');
  });
  for (const ev of ['blur', 'minimize', 'hide']) win.on(ev, () => setTimeout(optOutOfThrottling, 150));
  // links like "by RedtailWorks" open in the real browser, not in an ORY-1 window
  win.webContents.setWindowOpenHandler(({ url }) => { shell.openExternal(url); return { action: 'deny' }; });
  // a reloaded page loads its VST3 modules again from scratch, in a fresh helper
  win.webContents.on('did-start-navigation', e => { if (e.isMainFrame && !e.isSameDocument) stopVstHost(); });
  win.loadFile(path.join(__dirname, '..', 'play', 'index.html'));
}

function buildMenu() {
  const send = ch => () => win && win.webContents.send(ch);
  const template = [
    ...(process.platform === 'darwin' ? [{ role: 'appMenu' }] : []),
    { label: 'File', submenu: [
      { label: 'Open Samples Folder', click: () => shell.openPath(samplesDir) },
      { label: 'Open Plugins Folder', click: () => shell.openPath(pluginsDir) },
      ...(vstAvailable() ? [{ label: 'Open VST3 Folder', click: () => shell.openPath(vstDir) }] : []),
      { label: 'Reload Plugins', accelerator: 'CmdOrCtrl+Shift+R', click: send('plugins:changed') },
      { type: 'separator' },
      process.platform === 'darwin' ? { role: 'close' } : { role: 'quit' },
    ] },
    { role: 'editMenu' },
    { label: 'View', submenu: [
      { role: 'reload' }, { role: 'toggleDevTools' }, { type: 'separator' },
      { role: 'resetZoom' }, { role: 'zoomIn' }, { role: 'zoomOut' }, { type: 'separator' }, { role: 'togglefullscreen' },
    ] },
    { role: 'help', submenu: [
      { label: 'Making Plugins', click: () => shell.openExternal('https://github.com/RustyPawz/orrery/blob/main/PLUGINS.md') },
      { label: 'ORY-1 on the Web', click: () => shell.openExternal('https://rustypawz.github.io/orrery/') },
    ] },
  ];
  Menu.setApplicationMenu(Menu.buildFromTemplate(template));
}

ipcMain.handle('plugins:list', () => listPlugins());
ipcMain.handle('plugins:open', () => shell.openPath(pluginsDir));
ipcMain.handle('plugins:path', () => pluginsDir);
ipcMain.handle('samples:list', () => listSamples());
ipcMain.handle('samples:read', (e, rel) => readSample(rel));
ipcMain.handle('samples:save', (e, name, bytes) => saveSample(name, bytes));
ipcMain.handle('samples:open', () => shell.openPath(samplesDir));
ipcMain.handle('vst:available', () => vstAvailable());
ipcMain.handle('vst:list', (e, force) => scanVst(!!force));
ipcMain.handle('vst:blocked', () => vstBlocked());
ipcMain.handle('vst:start', () => startVstHost().catch(err => ({ error: err.message })));
ipcMain.handle('vst:call', (e, cmd) => vstCall(cmd));
ipcMain.handle('vst:open', () => shell.openPath(vstDir));
ipcMain.handle('vst:putState', (e, text) => putState(text));
// a plugin's editor opens in a window of the helper's, owned by ORY-1's window so it stays above it
ipcMain.handle('vst:editor', (e, o) => {
  const h = win.getNativeWindowHandle();
  const owner = h.length >= 8 ? Number(h.readBigUInt64LE(0)) : h.readUInt32LE(0);
  return vstCall({ cmd: 'editor', slot: o.slot, title: String(o.title || ''), x: o.x, y: o.y, owner });
});
ipcMain.on('audio:playing', (e, on) => { setPlaying(!!on); optOutOfThrottling(); });
ipcMain.on('library:read', e => { e.returnValue = readLibrary(); });
ipcMain.on('library:write', (e, text) => { libraryText = String(text); clearTimeout(libraryTimer); libraryTimer = setTimeout(writeLibrary, 1000); });
app.on('will-quit', writeLibrary);

app.on('second-instance', () => { if (win) { if (win.isMinimized()) win.restore(); win.focus(); } });
app.whenReady().then(() => {
  // Web MIDI (and its SysEx messages) for controllers; nothing else needs a permission
  const allowed = new Set(['midi', 'midiSysex']);
  session.defaultSession.setPermissionRequestHandler((wc, perm, cb) => cb(allowed.has(perm)));
  session.defaultSession.setPermissionCheckHandler((wc, perm) => allowed.has(perm));
  preparePluginsDir();
  prepareSamplesDir();
  prepareVstDir();
  watchPlugins();
  watchSamples();
  buildMenu();
  createWindow();
  optOutOfThrottling();
  setInterval(optOutOfThrottling, 3000);
  app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow(); });
});
app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit(); });
