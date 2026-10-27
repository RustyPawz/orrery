// A check of the native parts without ORY-1: starts the helper, loads one plugin, plays notes into it through the bridge
// addon at real-time pace (standing in for the AudioWorklet), and reports how loud the plugin's output was.
//   node native/test-host.js "<path.vst3>" <class id> [seconds]
const { spawn } = require('child_process');
const path = require('path');
const bridge = require('./bin/vstbridge.node');

const [file, cid, secs = '3'] = process.argv.slice(2);
const Q = 128, RING = 64, EVR = 1024, SR = 48000, D = 8;
const OFF = { frames: 64, in: 64 + RING * 8 };
OFF.out = OFF.in + RING * 2 * Q * 4; OFF.evF = OFF.out + RING * 2 * Q * 4; OFF.evI = OFF.evF + EVR * 8; OFF.evV = OFF.evI + EVR * 12; OFF.size = OFF.evV + EVR * 4;

const name = 'Local\\ORY1-vst-test-' + process.pid;
const helper = spawn(path.join(__dirname, 'bin', 'ory1-vsthost.exe'), ['--host', name], { stdio: ['pipe', 'pipe', 'inherit'] });
let buf = '', waiting = new Map(), nextId = 1, onReady;
helper.stdout.on('data', d => {
  buf += d;
  let i;
  while ((i = buf.indexOf('\n')) >= 0) {
    const msg = JSON.parse(buf.slice(0, i)); buf = buf.slice(i + 1);
    if (msg.event === 'ready') onReady();
    else if (msg.event) console.log('event', msg);
    else if (waiting.has(msg.id)) { waiting.get(msg.id)(msg); waiting.delete(msg.id); }
  }
});
const call = cmd => new Promise(res => { const id = nextId++; waiting.set(id, res); helper.stdin.write(JSON.stringify({ id, ...cmd }) + '\n'); });

(async () => {
  await new Promise(r => onReady = r);
  console.log('bridge connect:', bridge.connect(name));
  const t0 = Date.now();
  const r = await call({ cmd: 'load', path: file, cid, sampleRate: SR });
  if (r.error) { console.log('load failed:', r.error); helper.stdin.end(); return; }
  console.log(`loaded ${r.name} in slot ${r.slot} (${Date.now() - t0} ms): ${r.inputs} in, ${r.outputs} out, latency ${r.latency}, ${r.params.length} params`);

  const sab = new SharedArrayBuffer(OFF.size);
  const h = new Int32Array(sab, 0, 16), inp = new Float32Array(sab, OFF.in, RING * 2 * Q), frames = new Float64Array(sab, OFF.frames, RING), out = new Float32Array(sab, OFF.out, RING * 2 * Q);
  const evF = new Float64Array(sab, OFF.evF, EVR), evI = new Int32Array(sab, OFF.evI, EVR * 3), evV = new Float32Array(sab, OFF.evV, EVR);
  console.log('bridge attach:', bridge.attach(r.slot, new Uint8Array(sab)));
  const ev = (frame, type, a, b, v) => { const k = Atomics.load(h, 2), i = k % EVR; evF[i] = frame; evI[i * 3] = type; evI[i * 3 + 1] = a; evI[i * 3 + 2] = b; evV[i] = v; Atomics.store(h, 2, k + 1); };

  // a C minor arpeggio, a note every quarter second, starting half a second in
  [60, 63, 67, 72, 67, 63, 60, 55].forEach((p, i) => { const f = SR * (+(process.env.NOTES_AT || 0.5) + i * 0.25); ev(f, 1, p, i + 1, 0.8); ev(f + SR * 0.2, 2, p, i + 1, 0); });

  // the worklet's part: one block every 128 frames of real time, reading back the block from D blocks ago
  let k = 0, under = 0, peak = 0, sumSq = 0, n = 0, onset = -1;
  const bins = [];
  const start = process.hrtime.bigint(), total = Math.round(+secs * SR / Q);
  while (k < total) {
    const due = Number(process.hrtime.bigint() - start) / 1e9 * SR / Q;
    while (k < due && k < total) {
      frames[k % RING] = k * Q + D * Q;
      // with SINE set, a 440 Hz tone goes in, so effects have something to work on
      if (process.env.SINE) { const o = (k % RING) * 2 * Q; for (let j = 0; j < Q; j++) inp[o + j] = inp[o + Q + j] = 0.3 * Math.sin(2 * Math.PI * 440 * (k * Q + j) / SR); }
      Atomics.store(h, 0, k + 1);
      const want = k - D;
      if (want >= 0) {
        if (Atomics.load(h, 1) > want) { const o = (want % RING) * 2 * Q; for (let j = 0; j < 2 * Q; j++) { const s = out[o + j]; if (onset < 0 && j < Q && Math.abs(s) > 0.001) onset = k * Q + j; const bi = Math.floor(k * Q / (SR / 20)); bins[bi] = Math.max(bins[bi] || 0, Math.abs(s)); peak = Math.max(peak, Math.abs(s)); sumSq += s * s; n++; } }
        else under++;
      }
      k++;
    }
    await new Promise(r => setImmediate(r));
  }
  const firstNote = SR * +(process.env.NOTES_AT || 0.5);
  // frames read back now are heard at k * Q, so the first sound should start at the first note's frame
  if (onset >= 0) console.log(`first sound at frame ${onset}, first note due at ${firstNote}: ${((onset - firstNote) / SR * 1000).toFixed(2)} ms off`);
  if (process.env.TRACE) console.log('peak per 50 ms:', bins.map(v => (v || 0).toFixed(2)).join(' '));
  console.log(`played ${k} blocks: peak ${peak.toFixed(3)}, rms ${Math.sqrt(sumSq / Math.max(1, n)).toFixed(4)}, late blocks ${under}`);
  bridge.detach(r.slot);
  await call({ cmd: 'unload', slot: r.slot });
  helper.stdin.end();
  helper.on('exit', c => { console.log('helper exited', c); process.exit(0); });
})();
