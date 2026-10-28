// The only bridge between the page and the computer: the plugins and samples folders, word when either changes, and
// the file copy of the song library.
const { contextBridge, ipcRenderer, webUtils } = require('electron');
const path = require('path');

// VST3: the bridge addon copies audio between each VST3 module's SharedArrayBuffer and the hosting helper.
// The context bridge can't carry a SharedArrayBuffer, so the page posts it here with window.postMessage.
let vstBridge = null, connected = null;
try {
  const dir = process.resourcesPath && !/node_modules[\\/]electron[\\/]dist/.test(process.resourcesPath)
    ? path.join(process.resourcesPath, 'native') : path.join(__dirname, '..', 'native', 'bin');
  vstBridge = require(path.join(dir, 'vstbridge.node'));
} catch (e) { vstBridge = null; /* no VST3 modules: the Rack doesn't offer them */ }
window.addEventListener('message', e => {
  const m = e.source === window && e.data && e.data.oryVstAttach;
  if (m && vstBridge && m.sab instanceof SharedArrayBuffer) vstBridge.attach(m.slot, new Uint8Array(m.sab));
});

contextBridge.exposeInMainWorld('orreryDesktop', {
  listPlugins: () => ipcRenderer.invoke('plugins:list'),
  openPluginsFolder: () => ipcRenderer.invoke('plugins:open'),
  pluginsPath: () => ipcRenderer.invoke('plugins:path'),
  onPluginsChanged: cb => ipcRenderer.on('plugins:changed', () => cb()),
  listSamples: () => ipcRenderer.invoke('samples:list'),
  // file bytes arrive as a Uint8Array; hand the page a plain ArrayBuffer for decoding
  readSample: rel => ipcRenderer.invoke('samples:read', rel).then(u => u.buffer.slice(u.byteOffset, u.byteOffset + u.byteLength)),
  saveSample: (name, bytes) => ipcRenderer.invoke('samples:save', name, new Uint8Array(bytes)),
  openSamplesFolder: () => ipcRenderer.invoke('samples:open'),
  onSamplesChanged: cb => ipcRenderer.on('samples:changed', () => cb()),
  // playing keeps the computer awake and makes sure every audio process is exempt from background throttling
  setPlaying: on => ipcRenderer.send('audio:playing', !!on),
  // read once while the page starts, so it waits for the file; saves are sent and written in the background
  readLibrary: () => ipcRenderer.sendSync('library:read'),
  writeLibrary: text => ipcRenderer.send('library:write', String(text)),
  // recordings: a save dialog, then the program chosen in Settings (if any) opens the file
  saveRecording: (name, bytes) => ipcRenderer.invoke('rec:save', String(name), new Uint8Array(bytes)),
  recordingApp: () => ipcRenderer.invoke('rec:app'),
  chooseRecordingApp: () => ipcRenderer.invoke('rec:chooseApp'),
  clearRecordingApp: () => ipcRenderer.invoke('rec:clearApp'),
  // VST3 hosting (Windows): null when the helper or the bridge isn't there
  vst: vstBridge ? {
    available: () => ipcRenderer.invoke('vst:available'),
    list: force => ipcRenderer.invoke('vst:list', !!force),
    blocked: () => ipcRenderer.invoke('vst:blocked'),
    openFolder: () => ipcRenderer.invoke('vst:open'),
    // start the helper (if it isn't running) and connect the bridge to it
    start: async () => {
      const r = await ipcRenderer.invoke('vst:start');
      if (!r || !r.shm) return { ok: false, error: r && r.error };
      // connecting again drops every attached slot, so only a new helper gets a new connection
      if (r.shm !== connected) { if (!vstBridge.connect(r.shm)) return { ok: false, error: 'could not connect to the VST3 helper' }; connected = r.shm; }
      return { ok: true, shm: r.shm };
    },
    call: cmd => ipcRenderer.invoke('vst:call', cmd),
    // {slot, title, x, y}: the plugin's own editor window
    editor: o => ipcRenderer.invoke('vst:editor', o),
    // a large plugin state, kept in a file of its own: returns the name the song keeps
    putState: text => ipcRenderer.invoke('vst:putState', String(text)),
    // the Rack's + (a dialog) or plugins dropped on the window (their paths): copied into ORY-1's VST3 folder
    importPlugins: paths => ipcRenderer.invoke('vst:import', Array.isArray(paths) ? paths.map(String) : null),
    pathFor: file => webUtils.getPathForFile(file),
    // the window is closing: cb saves every plugin's state, then the window closes
    onFlush: cb => ipcRenderer.on('vst:flush', async () => { try { await cb(); } finally { ipcRenderer.send('vst:flushed'); } }),
    detach: slot => vstBridge.detach(slot),
    onEvent: cb => ipcRenderer.on('vst:event', (e, msg) => cb(msg)),
  } : null,
});
