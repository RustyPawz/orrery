// The memory ORY-1's page, the bridge addon and the helper agree on. One slot per loaded plugin.
// Audio moves in 128-frame blocks (one Web Audio render quantum); counters only ever grow and wrap.
#pragma once
#include <atomic>
#include <cstdint>

namespace ory {

constexpr int kQuantum = 128;     // frames per block
constexpr int kRing = 64;         // blocks per ring, in each direction
constexpr int kEvRing = 1024;     // queued events per slot
constexpr int kMaxSlots = 32;     // plugins one helper can hold at once
constexpr uint32_t kMagic = 0x4f525931; // 'ORY1'
constexpr uint32_t kVersion = 1;

enum EventType : int32_t {
	kNoteOn = 1,      // a = pitch, b = note id, v = velocity 0..1
	kNoteOff = 2,     // a = pitch, b = note id
	kParam = 3,       // a = parameter id, v = normalised value 0..1
	kAllNotesOff = 4,
	kTransport = 5,   // a = playing, v = tempo in bpm
};

struct Event {
	double frame;     // when, in AudioContext frames
	int32_t type, a, b;
	float v;
};

// The helper's shared memory. The bridge writes inWrite and evWrite; the helper writes outDone and evRead.
struct Slot {
	std::atomic<uint32_t> inWrite, outDone, evWrite, evRead;
	uint32_t pad[12];
	double frames[kRing];                 // the frame each block will be heard at
	float in[kRing][2][kQuantum];
	float out[kRing][2][kQuantum];
	Event ev[kEvRing];
};

struct Shm {
	uint32_t magic, version;
	uint32_t pad[14];
	Slot slot[kMaxSlots];
};

// The page's SharedArrayBuffer for one slot (byte offsets). Same idea, written by JavaScript:
// header Int32[16]: 0 inWrite (worklet), 1 outDone (bridge), 2 evWrite (page), 3 evRead (bridge), 4 underruns, 5 latency in blocks
namespace sab {
constexpr int kHeader = 0;
constexpr int kFrames = 64;                                    // Float64[kRing]
constexpr int kIn = kFrames + kRing * 8;                       // Float32[kRing * 2 * kQuantum]
constexpr int kOut = kIn + kRing * 2 * kQuantum * 4;           // Float32[kRing * 2 * kQuantum]
constexpr int kEvFrame = kOut + kRing * 2 * kQuantum * 4;      // Float64[kEvRing]
constexpr int kEvInt = kEvFrame + kEvRing * 8;                 // Int32[kEvRing * 3]: type, a, b
constexpr int kEvVal = kEvInt + kEvRing * 12;                  // Float32[kEvRing]
constexpr int kSize = kEvVal + kEvRing * 4;
} // namespace sab

} // namespace ory
