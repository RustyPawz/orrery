# Orrery

![Orrery running the Crypt demo patch](preview.png)

**Play it: <https://rustypawz.github.io/orrery/>**

Orrery is a generative modular music studio that runs in your browser. You drop modules onto a canvas, wire them together, and the patch plays itself: clocks fire pulses, sequencers and chord modules pick notes, synths, drums and samplers sound them, and modulation keeps reshaping everything over time. Every event travels visibly down its wire, so you can watch why each note happened.

It's a companion to the [RedtailWorks FM-1 firmwares](https://rustypawz.github.io/fm1-HYP1/). A [RedtailWorks](https://www.redtail.works) project.

## What's in the rack

- **Time:** Clock with ×4 ×3 ×2 ×1 ÷2 ÷4 ÷8 outputs and swing, free-running Pulsar, clock Divider.
- **Sequence:** Ring Seq (1–32 steps, forward, reverse, pendulum or random, rotate, step chance) and a Euclidean rhythm generator.
- **Harmony:** Key (scale and root for the whole patch, with key walks), Chord (9 chord types, inversions, strum, 7 progressions, arpeggio modes), Shift.
- **Logic:** Chance, Walker, Repeater.
- **Voices:** a fully shapeable synth (wave, detune, FM, filter and its envelope, attack, hold, release, pitch sweep) with Pluck, Bell, Bass, Pad, Lead and Zap presets.
- **Drums:** three synthesized packs: Bloom 808, Chitin (alien percussion) and Dust (lofi).
- **Samples:** drop any audio file to make a sampler that plays it as a one-shot, a pitched instrument across your scale, a tempo-locked loop, or a slicer.
- **FX:** Delay, Reverb, Chorus, Flanger, Phaser, Tremolo, Ring Mod, Auto-Pan, Drive, Crusher, Filter, EQ, Compressor.
- **Mod:** LFO, Envelope, Random, Macro and an audio Follower. Drag a ◆ output onto any dial of any module. Every instrument also has its own envelope output, so a kick can open a bass filter or duck a pad.
- **Mixer:** as many Speakers channel strips as you like, each with level, pan, mute and solo.
- **MIDI:** map any dial, mute, solo or Play to a knob, fader or key on your controller with MIDI learn, and play the patch from a keyboard through a MIDI In module.

## Songs

Patches are saved as songs in your browser, as many as you like, and you swap between them from the song menu in the header. The open song saves itself as you work. Use **Export song file** to move a song to another computer; any audio you imported travels inside the file.

## Patching

- Tap a module to bloom its extra dials, drag rings up and down to change them, scroll over a dial for fine steps.
- Wire ● outputs to ○ inputs for events, ■ to □ for audio, ◆ onto any dial for modulation. Drag a mod wire's knob to set its depth; below zero inverts it.
- Audio you don't wire anywhere goes straight to the speakers.
- Drag a selected module's rim to resize it, double-tap a module to name it.
- **◯ Section** draws a labelled circle around part of the patch; drag its label to move everything inside.

MIDI needs Chrome or Edge (Web MIDI). Everything else works in any current browser.

## License

GPL-3.0, like the FM-1 firmwares.
