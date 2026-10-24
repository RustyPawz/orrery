// Ratchet: an example Orrery plugin. Sometimes turns a note into a quick roll.
// The simplest kind of plugin: events in, events out, drawn as a normal circle module with dials.
export default {
  id: 'example.ratchet',
  name: 'Ratchet',
  category: 'Logic',
  color: '#7ab8ff',
  inputs: [{ id: 'in', kind: 'event' }],
  outputs: [{ id: 'out', kind: 'event' }],
  params: [
    { id: 'hits', label: 'hits', min: 2, max: 8, value: 3, step: 1, format: v => v + '×' },
    { id: 'chance', label: 'chance', min: 0, max: 1, value: 0.4 },
    { id: 'space', label: 'spacing', labels: ['1/32', '1/16', '1/8'], value: 0 },
    { id: 'fade', label: 'fade', min: 0, max: 1, value: 0.3, dial: 'satellite' },
  ],

  // every note that arrives: pass it on, and sometimes add the roll after it
  onEvent(node, port, ev, out) {
    const hits = Math.random() < node.p.chance ? node.p.hits : 1;
    const gap = Orrery.sixteenth() * [0.5, 1, 2][node.p.space];
    for (let i = 0; i < hits; i++) out('out', { t: ev.t + i * gap, vel: ev.vel * (1 - node.p.fade * i / hits) });
  },

  // three bars in the middle of the circle; they light up when the module fires
  draw(g, node, t, { color, flash }) {
    g.fillStyle = color;
    for (let i = 0; i < 3; i++) {
      g.globalAlpha = 0.35 + flash * 0.65 * (1 - i * 0.25);
      g.fillRect(-7 + i * 5, -6, 3, 12);
    }
    g.globalAlpha = 1;
  },
};
