// The only bridge between the page and the computer: the plugins and samples folders, and word when either changes.
const { contextBridge, ipcRenderer } = require('electron');

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
});
