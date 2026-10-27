// vstbridge.node: runs in ORY-1's page process (loaded by the preload) and moves blocks between each VST3 module's
// SharedArrayBuffer and the helper's shared memory, on a thread of its own so the page's main thread is never involved.
//
//   connect(name)          map the helper's shared memory and events; true when it worked
//   attach(slot, bytes)    start serving a slot; bytes is a Uint8Array over the module's SharedArrayBuffer
//   detach(slot)           stop serving it
#define NOMINMAX
#include <windows.h>
#include <timeapi.h>
#include <node_api.h>

#include <atomic>
#include <cstring>
#include <map>
#include <mutex>
#include <string>
#include <thread>

#include "../shared/layout.h"

namespace {

struct Attached
{
	uint8_t* base = nullptr;
	napi_ref ref = nullptr; // keeps the SharedArrayBuffer alive while the thread uses its memory
};

ory::Shm* shm = nullptr;
HANDLE mapping = nullptr, wake = nullptr, done = nullptr;
std::mutex lock;
std::map<int, Attached> slots;
std::thread worker;
std::atomic<bool> running {false};

// one pass over a slot: page events and input blocks go to the helper, finished output blocks come back
bool serve (int i, uint8_t* p)
{
	using namespace ory;
	Slot& z = shm->slot[i];
	auto* h = reinterpret_cast<int32_t*> (p + sab::kHeader);
	bool fed = false;

	uint32_t pw = (uint32_t)std::atomic_ref<int32_t> (h[2]).load (std::memory_order_acquire);
	uint32_t pr = (uint32_t)h[3];
	if (pw != pr)
	{
		uint32_t zw = z.evWrite.load (std::memory_order_relaxed), zr = z.evRead.load (std::memory_order_acquire);
		auto* ef = reinterpret_cast<double*> (p + sab::kEvFrame);
		auto* ei = reinterpret_cast<int32_t*> (p + sab::kEvInt);
		auto* evv = reinterpret_cast<float*> (p + sab::kEvVal);
		for (; pr != pw && zw - zr < (uint32_t)kEvRing; pr++, zw++)
		{
			int k = pr % kEvRing;
			z.ev[zw % kEvRing] = Event {ef[k], ei[k * 3], ei[k * 3 + 1], ei[k * 3 + 2], evv[k]};
		}
		z.evWrite.store (zw, std::memory_order_release);
		std::atomic_ref<int32_t> (h[3]).store ((int32_t)pr, std::memory_order_release);
		fed = true;
	}

	uint32_t k = (uint32_t)std::atomic_ref<int32_t> (h[0]).load (std::memory_order_acquire);
	uint32_t m = z.inWrite.load (std::memory_order_relaxed);
	if (k != m)
	{
		if (k - m > (uint32_t)kRing) m = k - kRing;
		auto* frames = reinterpret_cast<double*> (p + sab::kFrames);
		auto* in = reinterpret_cast<float*> (p + sab::kIn);
		for (; m != k; m++)
		{
			int b = m % kRing;
			z.frames[b] = frames[b];
			memcpy (z.in[b], in + b * 2 * kQuantum, sizeof z.in[b]);
		}
		z.inWrite.store (k, std::memory_order_release);
		fed = true;
	}

	uint32_t o = z.outDone.load (std::memory_order_acquire);
	uint32_t q = (uint32_t)h[1];
	if (o != q)
	{
		if (o - q > (uint32_t)kRing) q = o - kRing;
		auto* out = reinterpret_cast<float*> (p + sab::kOut);
		for (; q != o; q++)
		{
			int b = q % kRing;
			memcpy (out + b * 2 * kQuantum, z.out[b], sizeof z.out[b]);
		}
		std::atomic_ref<int32_t> (h[1]).store ((int32_t)o, std::memory_order_release);
	}
	return fed;
}

void loop ()
{
	SetThreadPriority (GetCurrentThread (), THREAD_PRIORITY_TIME_CRITICAL);
	timeBeginPeriod (1);
	while (running)
	{
		// the helper signals when it has finished blocks; otherwise look again within a millisecond
		WaitForSingleObject (done, 1);
		bool fed = false;
		{
			std::lock_guard<std::mutex> lk (lock);
			for (auto& [i, a] : slots) fed |= serve (i, a.base);
		}
		if (fed) SetEvent (wake);
	}
	timeEndPeriod (1);
}

napi_value boolean (napi_env env, bool v)
{
	napi_value r;
	napi_get_boolean (env, v, &r);
	return r;
}

napi_value Connect (napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value argv[1];
	napi_get_cb_info (env, info, &argc, argv, nullptr, nullptr);
	char name[256] = {};
	size_t len = 0;
	if (argc < 1 || napi_get_value_string_utf8 (env, argv[0], name, sizeof name, &len) != napi_ok) return boolean (env, false);

	if (running)
	{
		running = false;
		worker.join ();
	}
	std::lock_guard<std::mutex> lk (lock);
	slots.clear (); // a new helper means every module attaches again
	if (shm) UnmapViewOfFile (shm);
	for (HANDLE* h : {&mapping, &wake, &done})
		if (*h) { CloseHandle (*h); *h = nullptr; }
	shm = nullptr;

	std::string n (name, len);
	mapping = OpenFileMappingA (FILE_MAP_ALL_ACCESS, FALSE, n.c_str ());
	wake = OpenEventA (EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, (n + "-wake").c_str ());
	done = OpenEventA (EVENT_MODIFY_STATE | SYNCHRONIZE, FALSE, (n + "-done").c_str ());
	if (!mapping || !wake || !done) return boolean (env, false);
	shm = (ory::Shm*)MapViewOfFile (mapping, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (ory::Shm));
	if (!shm || shm->magic != ory::kMagic || shm->version != ory::kVersion) return boolean (env, false);
	running = true;
	worker = std::thread (loop);
	return boolean (env, true);
}

napi_value Attach (napi_env env, napi_callback_info info)
{
	size_t argc = 2;
	napi_value argv[2];
	napi_get_cb_info (env, info, &argc, argv, nullptr, nullptr);
	int32_t slot = -1;
	if (argc < 2 || napi_get_value_int32 (env, argv[0], &slot) != napi_ok || slot < 0 || slot >= ory::kMaxSlots || !shm)
		return boolean (env, false);
	napi_typedarray_type type;
	size_t length = 0, offset = 0;
	void* data = nullptr;
	napi_value buffer;
	if (napi_get_typedarray_info (env, argv[1], &type, &length, &data, &buffer, &offset) != napi_ok || type != napi_uint8_array ||
	    length < (size_t)ory::sab::kSize || !data)
		return boolean (env, false);
	Attached a;
	a.base = static_cast<uint8_t*> (data);
	napi_create_reference (env, argv[1], 1, &a.ref);
	napi_ref old = nullptr;
	{
		std::lock_guard<std::mutex> lk (lock);
		auto it = slots.find (slot);
		if (it != slots.end ()) old = it->second.ref;
		slots[slot] = a;
	}
	if (old) napi_delete_reference (env, old);
	return boolean (env, true);
}

napi_value Detach (napi_env env, napi_callback_info info)
{
	size_t argc = 1;
	napi_value argv[1];
	napi_get_cb_info (env, info, &argc, argv, nullptr, nullptr);
	int32_t slot = -1;
	if (argc < 1 || napi_get_value_int32 (env, argv[0], &slot) != napi_ok) return boolean (env, false);
	napi_ref ref = nullptr;
	{
		std::lock_guard<std::mutex> lk (lock);
		auto it = slots.find (slot);
		if (it == slots.end ()) return boolean (env, false);
		ref = it->second.ref;
		slots.erase (it);
	}
	napi_delete_reference (env, ref);
	return boolean (env, true);
}

napi_value Init (napi_env env, napi_value exports)
{
	napi_property_descriptor props[] = {
	    {"connect", nullptr, Connect, nullptr, nullptr, nullptr, napi_default, nullptr},
	    {"attach", nullptr, Attach, nullptr, nullptr, nullptr, napi_default, nullptr},
	    {"detach", nullptr, Detach, nullptr, nullptr, nullptr, napi_default, nullptr},
	};
	napi_define_properties (env, exports, 3, props);
	return exports;
}

} // namespace

NAPI_MODULE (NODE_GYP_MODULE_NAME, Init)
