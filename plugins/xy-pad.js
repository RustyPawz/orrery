// XY Pad: an example Orrery plugin. A square you drag in; its X and Y are two modulation outputs.
// Shows a free-form shape (not a circle), drawing it yourself, pointer input, and data saved with the song.
const SIZE = 140, PAD = 12;

export default {
  id: 'example.xy-pad',
  name: 'XY Pad',
  category: 'Mod',
  color: '#ff9ad5',
  shape: { width: SIZE, height: SIZE },
  outputs: [
    { id: 'x', kind: 'mod', label: 'x' },
    { id: 'y', kind: 'mod', label: 'y' },
  ],
  params: [{ id: 'glide', label: 'glide', min: 0, max: 2, value: 0.15, format: v => v.toFixed(2) + 's' }],

  // node.data is saved with the song: the puck stays where you left it
  init(node) {
    if (node.data.x == null) { node.data.x = 0; node.data.y = 0; }
  },

  // x and y arrive in the module's own units, measured from its centre
  onPointer(node, e) {
    const half = SIZE / 2 - PAD;
    node.data.x = Math.max(-1, Math.min(1, e.x / half));
    node.data.y = Math.max(-1, Math.min(1, -e.y / half));
  },

  // glide: the outputs ease toward the puck instead of jumping
  tick(node, t, dt) {
    const k = node.p.glide > 0 ? Math.min(1, dt / node.p.glide) : 1;
    node.st.x = (node.st.x ?? node.data.x) + (node.data.x - (node.st.x ?? node.data.x)) * k;
    node.st.y = (node.st.y ?? node.data.y) + (node.data.y - (node.st.y ?? node.data.y)) * k;
  },

  modulate(node, port) {
    return port === 'x' ? node.st.x ?? node.data.x : node.st.y ?? node.data.y;
  },

  draw(g, node, t, { color }) {
    const half = SIZE / 2 - PAD;
    g.strokeStyle = Orrery.alpha(color, 0.18);
    g.lineWidth = 1;
    for (let i = -2; i <= 2; i++) {
      g.beginPath(); g.moveTo(i * half / 2, -half); g.lineTo(i * half / 2, half); g.stroke();
      g.beginPath(); g.moveTo(-half, i * half / 2); g.lineTo(half, i * half / 2); g.stroke();
    }
    const px = (node.st.x ?? node.data.x) * half, py = -(node.st.y ?? node.data.y) * half;
    g.strokeStyle = Orrery.alpha(color, 0.5);
    g.beginPath(); g.moveTo(px, -half); g.lineTo(px, half); g.moveTo(-half, py); g.lineTo(half, py); g.stroke();
    g.fillStyle = color;
    g.beginPath(); g.arc(px, py, 7, 0, Math.PI * 2); g.fill();
    g.fillStyle = Orrery.alpha(color, 0.6);
    g.font = '9px "IBM Plex Mono", monospace';
    g.fillText('drag', -half, -half - 2);
  },
};
