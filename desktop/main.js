// ORY-1 desktop: the same play/index.html as the website, in its own window, with a plugins folder it watches.
const { app, BrowserWindow, ipcMain, shell, Menu, session, powerSaveBlocker } = require('electron');
const path = require('path');
const fs = require('fs');

// keep the audio scheduler at full speed even when the window is hidden or minimised
app.commandLine.appendSwitch('disable-renderer-backgrounding');
app.commandLine.appendSwitch('disable-background-timer-throttling');
app.commandLine.appendSwitch('autoplay-policy', 'no-user-gesture-required');

// The app was called Orrery before ORY-1. Move its saved songs and settings (userData) and its Documents folder
// to the new name on first launch; if a folder can't be moved (a file in it is open), keep using the old one.
function moveFolder(from, to) {
  if (fs.existsSync(from) && !fs.existsSync(to)) { try { fs.renameSync(from, to); } catch (e) { return from; } }
  return to;
}
app.setPath('userData', moveFolder(path.join(app.getPath('appData'), 'Orrery'), app.getPath('userData')));
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
      contextIsolation: true, nodeIntegration: false, sandbox: true,
      backgroundThrottling: false,
    },
  });
  if (b.maximized) win.maximize();
  win.once('ready-to-show', () => win.show());
  win.on('close', saveBounds);
  for (const ev of ['blur', 'minimize', 'hide']) win.on(ev, () => setTimeout(optOutOfThrottling, 150));
  // links like "by RedtailWorks" open in the real browser, not in an ORY-1 window
  win.webContents.setWindowOpenHandler(({ url }) => { shell.openExternal(url); return { action: 'deny' }; });
  win.loadFile(path.join(__dirname, '..', 'play', 'index.html'));
}

function buildMenu() {
  const send = ch => () => win && win.webContents.send(ch);
  const template = [
    ...(process.platform === 'darwin' ? [{ role: 'appMenu' }] : []),
    { label: 'File', submenu: [
      { label: 'Open Samples Folder', click: () => shell.openPath(samplesDir) },
      { label: 'Open Plugins Folder', click: () => shell.openPath(pluginsDir) },
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
ipcMain.on('audio:playing', (e, on) => { setPlaying(!!on); optOutOfThrottling(); });

app.on('second-instance', () => { if (win) { if (win.isMinimized()) win.restore(); win.focus(); } });
app.whenReady().then(() => {
  // Web MIDI (and its SysEx messages) for controllers; nothing else needs a permission
  const allowed = new Set(['midi', 'midiSysex']);
  session.defaultSession.setPermissionRequestHandler((wc, perm, cb) => cb(allowed.has(perm)));
  session.defaultSession.setPermissionCheckHandler((wc, perm) => allowed.has(perm));
  preparePluginsDir();
  prepareSamplesDir();
  watchPlugins();
  watchSamples();
  buildMenu();
  createWindow();
  optOutOfThrottling();
  setInterval(optOutOfThrottling, 3000);
  app.on('activate', () => { if (BrowserWindow.getAllWindows().length === 0) createWindow(); });
});
app.on('window-all-closed', () => { if (process.platform !== 'darwin') app.quit(); });
