// Step Pad: an example Orrery plugin. Eight on/off steps as real HTML buttons on a free-form module.
// Shows an HTML panel: any UI you can build in a web page, riding along with the module as you pan and zoom.
const STEPS = 8;

export default {
  id: 'example.step-pad',
  name: 'Step Pad',
  category: 'Sequence',
  color: '#5ef0c8',
  shape: { width: 230, height: 70 },
  inputs: [{ id: 'clk', kind: 'event', label: 'clk' }, { id: 'rst', kind: 'event', label: 'rst' }],
  outputs: [{ id: 'out', kind: 'event' }],
  params: [{ id: 'note', label: 'note', min: 0, max: 14, value: 0, step: 1, format: v => 'deg ' + v }],

  init(node) {
    if (!Array.isArray(node.data.steps)) node.data.steps = [1, 0, 0, 1, 0, 1, 0, 0];
  },

  onEvent(node, port, ev, out) {
    if (port === 'rst') { node.st.i = -1; return; }
    const i = node.st.i = ((node.st.i ?? -1) + 1) % STEPS;
    (node.st.seen ||= []).push({ t: ev.t, i });
    if (node.st.seen.length > 8) node.st.seen.shift();
    if (node.data.steps[i]) out('out', { deg: ev.deg + node.p.note });
  },

  // the panel sits over the module, below its top strip (which stays the handle for moving it)
  panel: {
    width: 210, height: 40, y: 8,
    mount(el, node, api) {
      el.style.cssText += 'display:grid;grid-template-columns:repeat(8,1fr);gap:4px';
      const buttons = Array.from({ length: STEPS }, (_, i) => {
        const b = document.createElement('button');
        b.type = 'button';
        b.style.cssText = 'border-radius:6px;padding:0;border:1px solid #2a1f46;cursor:pointer';
        b.onclick = () => { node.data.steps[i] = node.data.steps[i] ? 0 : 1; api.save(); };
        el.append(b);
        return b;
      });
      // repaint every frame until the module is removed: lit steps, and the one playing now
      const paint = () => {
        if (!el.isConnected) return;
        let cur = -1;
        for (const s of node.st.seen || []) if (s.t <= api.now()) cur = s.i;
        buttons.forEach((b, i) => {
          const on = node.data.steps[i];
          b.style.background = i === cur ? '#ffffff' : on ? '#5ef0c8' : '#120a22';
          b.style.opacity = on || i === cur ? '1' : '0.7';
        });
        requestAnimationFrame(paint);
      };
      paint();
    },
  },
};
