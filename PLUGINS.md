# Making Orrery plugins

A plugin is one `.js` file that adds new module types to Orrery. It can be a normal circle module with dials, a voice with its own sound, a free-form shape you draw yourself, or a module with its own HTML controls.

- **Desktop app:** drop the file into `Documents/Orrery/plugins` (**File → Open Plugins Folder**). Orrery watches the folder, so saving the file reloads it while you play.
- **Website:** **Settings → Plugins → Add plugin file…**. The plugin is kept in that browser.

The new modules appear in the Rack, in the section named by their `category`.

The [plugins folder](plugins) holds four working examples:

- [`ratchet.js`](plugins/ratchet.js): events in and events out, drawn as a circle.
- [`sine-bell.js`](plugins/sine-bell.js): a voice with its own audio and an envelope output.
- [`xy-pad.js`](plugins/xy-pad.js): a square you drag in, with two modulation outputs.
- [`step-pad.js`](plugins/step-pad.js): HTML buttons on the module.

Copying one of them is the quickest way to start.

> Plugins are ordinary JavaScript with full access to Orrery and the page. Only install plugins you trust.

## The smallest plugin

```js
export default {
  id: 'my.octave-up',          // unique; letters, numbers, - _ .
  name: 'Octave Up',           // shown on the module and in the Rack
  category: 'Harmony',         // a Rack section; a new name makes a new section
  inputs:  [{ id: 'in',  kind: 'event' }],
  outputs: [{ id: 'out', kind: 'event' }],
  onEvent(node, port, ev, out) {
    out('out', { deg: ev.deg + Orrery.key.notes.length });   // one octave up, in the current key
  },
};
```

A file can also `export default` an array of modules, or call `Orrery.register(module)` for each.

Songs save each module by its `id`, so keep the `id` stable once people use your plugin. If a song uses a plugin that isn't installed, those modules show as dashed **missing** placeholders. Their settings and wires are kept, and they come back when the plugin does.

## Module definition

| Field | What it does |
|---|---|
| `id` | **Required.** Unique type id. Saved in songs. |
| `name` | Name on the module and in the Rack. |
| `category` | Rack section, for example `Logic`, `Voices`, `Mod`, or your own name. Defaults to `Plugins`. |
| `color` | `#rrggbb`. |
| `size` | Radius of a circle module (default 46). |
| `shape` | `{ width, height }` makes a free-form box instead of a circle. |
| `drawBody` | `false` skips Orrery's circle or box, so `draw` draws everything. |
| `inputs`, `outputs` | Ports; see below. |
| `params` | Dials; see below. |
| `init(node)` | Called when the module is created or loaded. Set up `node.data` here. |
| `onEvent(node, port, ev, out)` | A note or pulse arrived on an event input. |
| `schedule(node, from, to, out)` | Make your own events between two times. Used by generators that aren't clocked from outside. |
| `audio(ctx, node, Orrery)` | Build the module's audio. Return `{ input, output, set(params), stop() }`; every field is optional. |
| `modulate(node, port, t)` | The value of a `mod` output at time `t`, roughly −1 to 1. |
| `tick(node, t, dt)` | Called about 40 times a second. Use it for smoothing, followers and anything continuous. |
| `onParam(node, id)` | A dial was changed. |
| `title(node)` | Return a dynamic name for the module. |
| `draw(g, node, t, info)` | Draw on the canvas; see below. |
| `onPointer(node, e)` | Mouse or touch input on the module; see below. |
| `pointerArea(node, x, y)` | Return `true` where `onPointer` should get the pointer. |
| `buttons`, `onButton(node, i)` | Small round buttons around the module. |
| `panel` | An HTML panel over the module; see below. |

## Ports

```js
inputs:  [{ id: 'in', kind: 'event' }, { id: 'sound', kind: 'audio' }],
outputs: [{ id: 'out', kind: 'event' }, { id: 'lfo', kind: 'mod', label: 'lfo' }],
```

| Kind | Wire | Carries |
|---|---|---|
| `event` | ● → ○ | Notes and pulses: `{ t, deg, vel, len? }`. |
| `audio` | ■ → □ | Sound: Web Audio nodes from `audio()`. |
| `mod` | ◆ → any dial | A changing value from `modulate()`. |

- **Placement:** on circles, inputs spread around the left and outputs around the right. Set `angle` (radians, 0 is right) to place one yourself. On box shapes, inputs go on the left edge and outputs on the right. Use `side: 'top' | 'bottom'` or exact `x`, `y` to place one yourself.
- **Labels:** `label` is the small text next to the port.
- **Mod outputs:** each one gets its own audio-rate signal, so it can drive fast dials such as filter cutoff and level smoothly.

## Events

An event is `{ t, deg, vel, len }`:

- `t`: when it happens, in audio-clock seconds. It's usually a little in the future, because Orrery schedules ahead.
- `deg`: a scale degree in the current key, where 0 is the root.
- `vel`: from 0 to 1.
- `len`: an optional note length in seconds.

`out(port, changes)` sends a copy of the incoming event with your changes merged in. For example, `out('out', { t: ev.t + 0.1 })` sends it a tenth of a second later. Never schedule into the past: always use `ev.t` or later.

## Dials (params)

```js
params: [
  { id: 'rate', label: 'rate', min: 0.1, max: 8, value: 1, format: v => v.toFixed(1) + ' Hz' },
  { id: 'mode', label: 'mode', labels: ['up', 'down', 'random'] },     // a stepped choice
  { id: 'steps', label: 'steps', min: 1, max: 16, value: 8, step: 1, dial: 'satellite' },
  { id: 'pan', label: 'pan', min: -1, max: 1, bipolar: true },
]
```

- **Where a dial goes:** circle modules show the first four as rings. The rest, and every dial with `dial: 'satellite'`, bloom out as satellites when the module is selected. Box modules show all their dials as satellites. `dial: 'hidden'` stores a value without showing a dial.
- **Reading values:** use `node.p.<id>`. Inside `onEvent`, the values already include any modulation wired to them.
- **Shared features:** every dial can be modulated, MIDI-learned, stored in Scenes, and saved with the song.

## The node

| Field | |
|---|---|
| `node.p` | Dial values. |
| `node.data` | Your own saved data: anything JSON can hold. It's saved with the song. Call `Orrery.save()` after changing it outside `onPointer`. |
| `node.st` | Scratch state. It's cleared each time Play starts. |
| `node.au` | What `audio()` returned. |
| `node.id`, `node.x`, `node.y` | Identity and position. |

## Drawing

`draw(g, node, t, info)` gets a 2D canvas context already centred on the module and scaled to its size. Draw around `(0, 0)`.

`info` holds:

- `color`
- `flash`: 0 to 1, how recently the module fired.
- `selected`
- `width`, `height`, `radius`

Orrery draws ports, labels and selection itself. With `drawBody: false` you replace the circle or box entirely, and the module can look like anything. Keep `draw` light, because it runs every frame. If it throws, the error is reported and the rest of Orrery carries on.

## Pointer input

With `onPointer(node, e)` your module receives mouse and touch input:

- `e.type` is `'down'`, `'move'` or `'up'`.
- `e.x`, `e.y` are in the module's own units, measured from its centre.
- `e.shift`, `e.alt`, `e.ctrl` are the modifier keys.

By default:

- **Box shapes:** everything except a 16-unit strip along the top goes to the plugin. The strip stays the handle for moving the module.
- **Circles:** the inner part goes to the plugin and the rings stay dials.

Return `true` from `pointerArea(node, x, y)` to choose the area yourself.

## Buttons

```js
buttons: node => [{ label: 'R', x: -12, y: 64, on: node.data.rec, color: '#ff5a78' }, { label: 'C', x: 12, y: 64 }],
onButton(node, i) { if (i === 0) node.data.rec = !node.data.rec; },
```

## HTML panels

```js
panel: {
  width: 200, height: 40, x: 0, y: 8,          // size and offset from the module's centre, in module units
  mount(el, node, Orrery) { /* build any HTML inside el */ },
  unmount(el, node) { /* optional clean-up */ },
}
```

The panel floats over the canvas, follows the module as you pan and zoom, and receives its own clicks. Anything a web page can do works here, such as sliders, text, a small piano or a `<canvas>`. See `step-pad.js`.

## The Orrery object

Available everywhere as `Orrery`, and passed to `audio()` and panels:

| | |
|---|---|
| `Orrery.audio` | The `AudioContext` (null until Play is first pressed). |
| `Orrery.now()` | The audio clock, in seconds. |
| `Orrery.bpm`, `Orrery.beat()`, `Orrery.sixteenth()` | Tempo, and the length of a beat and of a 16th note in seconds. |
| `Orrery.playing` | Whether the patch is playing. |
| `Orrery.key` | `{ root, scale, notes }`: `notes` are the scale's semitones. |
| `Orrery.degreeToMidi(deg, base = 48)` | A scale degree to a MIDI note in the current key. |
| `Orrery.midiToFreq(midi)` | A MIDI note to a frequency in Hz. |
| `Orrery.snap(deg, 'triad', chordRoot)` | Bend a degree onto chord tones (`triad`, `seventh`, `sus`, `fifths`, `root`, `five-note`). |
| `Orrery.noiseBuffer()` | A one-second white-noise `AudioBuffer`. |
| `Orrery.trail(node, t, midi, dur)` | Show a note in the trail behind the patch. |
| `Orrery.envelope(node, t, level, attack, decay)` | Fire the module's envelope (for voices whose `mod` output is an envelope). |
| `Orrery.message(text)` | Show a short message at the top of the canvas. |
| `Orrery.save()` | Save the song soon. |
| `Orrery.alpha('#rrggbb', a)` | A colour with transparency, for drawing. |
| `Orrery.register(module)` | Register a module type yourself. |

## Keeping it light

Orrery has to run on modest computers without the audio crackling:

- **Create audio nodes per note, and stop them.** Always give `start()` a matching `stop()`, the way `sine-bell.js` does, so finished notes free their nodes.
- **Avoid heavy work in `tick`, `draw` and `onEvent`.** No big loops or allocations: `tick` runs about 40 times a second and `draw` every frame.
- **Watch `ScriptProcessor` and big convolutions.** Use built-in Web Audio nodes where you can. If you need custom DSP, an `AudioWorklet` loaded from a Blob URL works.
- **Errors are contained.** A plugin error is caught and reported without stopping the music, but fix it anyway.

## Debugging

- **Desktop:** **View → Toggle Developer Tools** shows the console. Plugin errors are logged with the plugin's name.
- **Settings → Plugins:** lists each file, the modules it added, and any error loading it.
- **Reloading:** **Reload Plugins** (Ctrl+Shift+R in the desktop app) reloads everything. Saving a file in the plugins folder reloads just that file.
