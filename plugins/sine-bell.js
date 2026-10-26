// Sine Bell: an example ORY-1 plugin. A voice made of four inharmonic sine partials, like a small bell.
// Shows a plugin with its own audio, notes that follow the key, and a modulation output.
const PARTIALS = [[1, 1], [2.76, 0.5], [5.4, 0.25], [8.93, 0.12]]; // ratio, loudness

export default {
  id: 'example.sine-bell',
  name: 'Sine Bell',
  category: 'Voices',
  color: '#ffd27a',
  inputs: [{ id: 'in', kind: 'event' }],
  outputs: [
    { id: 'out', kind: 'audio' },
    { id: 'env', kind: 'mod', label: 'env' },
  ],
  params: [
    { id: 'decay', label: 'decay', min: 0.1, max: 6, value: 1.8, format: v => v.toFixed(1) + 's' },
    { id: 'bright', label: 'brightness', min: 0, max: 1, value: 0.5 },
    { id: 'level', label: 'level', min: 0, max: 1, value: 0.5 },
    { id: 'octave', label: 'octave', min: -2, max: 2, value: 0, step: 1, dial: 'satellite', format: v => (v > 0 ? '+' : '') + v },
  ],

  // one output gain for the whole module; every note connects into it
  audio(ctx) {
    const output = ctx.createGain();
    return { output };
  },

  onEvent(node, port, ev) {
    const ctx = ORY.audio;
    node.st.hit = ev.t;
    if (!ctx || !node.au) return;
    const p = node.p, midi = ORY.degreeToMidi(ev.deg, 60) + 12 * p.octave, f = ORY.midiToFreq(midi);
    PARTIALS.forEach(([ratio, amp], i) => {
      if (f * ratio > 16000) return;
      const o = ctx.createOscillator(), g = ctx.createGain();
      const peak = ev.vel * p.level * 0.25 * amp * (i ? p.bright * 2 : 1), dec = p.decay / (1 + i * (1 - p.bright));
      o.frequency.value = f * ratio;
      g.gain.setValueAtTime(0, ev.t);
      g.gain.linearRampToValueAtTime(peak, ev.t + 0.004);
      g.gain.setTargetAtTime(0, ev.t + 0.004, dec / 4);
      o.connect(g); g.connect(node.au.output);
      o.start(ev.t); o.stop(ev.t + dec * 1.5 + 0.1);
    });
    ORY.trail(node, ev.t, midi, p.decay);
  },

  // the env output: how loud the last strike still is
  modulate(node, port, t) {
    const hit = node.st.hit;
    return hit == null || t < hit ? 0 : Math.exp(-(t - hit) / (node.p.decay / 4));
  },

  draw(g, node, t, { color }) {
    const v = this.modulate(node, 'env', ORY.now());
    g.strokeStyle = color;
    g.lineWidth = 1.6;
    g.beginPath();
    g.moveTo(-7, 4); g.quadraticCurveTo(-7, -8, 0, -8); g.quadraticCurveTo(7, -8, 7, 4); g.closePath();
    g.stroke();
    g.fillStyle = color;
    g.beginPath(); g.arc(Math.sin(t * 14) * 3 * v, 7, 2, 0, Math.PI * 2); g.fill();
  },
};
