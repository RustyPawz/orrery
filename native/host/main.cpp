// ory1-vsthost: hosts VST3 plugins for ORY-1 in a process of its own, so a plugin that crashes takes only this helper down.
//
//   ory1-vsthost --scan <path.vst3>   prints one JSON line describing the bundle's plugin classes, then exits
//   ory1-vsthost --host <name>        creates shared memory <name> and takes JSON-line commands on stdin
//
// Threads: the main thread runs a Win32 message loop and does everything the VST3 rules say belongs on the UI thread
// (loading, unloading, controllers, editors); one real-time thread processes audio for every loaded plugin.
#define NOMINMAX
#include <windows.h>
#include <avrt.h>
#include <xmmintrin.h>
#include <fcntl.h>
#include <io.h>

#include <algorithm>
#include <cmath>
#include <atomic>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

#include "public.sdk/source/common/memorystream.h"
#include "public.sdk/source/vst/hosting/eventlist.h"
#include "public.sdk/source/vst/hosting/hostclasses.h"
#include "public.sdk/source/vst/hosting/module.h"
#include "public.sdk/source/vst/hosting/parameterchanges.h"
#include "public.sdk/source/vst/hosting/plugprovider.h"
#include "public.sdk/source/vst/hosting/processdata.h"
#include "public.sdk/source/vst/utility/stringconvert.h"
#include "pluginterfaces/gui/iplugview.h"
#include "pluginterfaces/gui/iplugviewcontentscalesupport.h"
#include "pluginterfaces/vst/ivstaudioprocessor.h"
#include "pluginterfaces/vst/ivsteditcontroller.h"
#include "pluginterfaces/vst/ivstevents.h"
#include "pluginterfaces/vst/ivstprocesscontext.h"

#include "../shared/layout.h"
#include "json.h"

using namespace Steinberg;
using namespace Steinberg::Vst;
using ory::Json;

static HANDLE gOut = INVALID_HANDLE_VALUE;
static std::mutex gOutLock;

// one line of JSON to ORY-1; plugins never get to write here (their stdout goes to stderr)
static void send (const Json& j)
{
	std::string s = j.dump () + "\n";
	std::lock_guard<std::mutex> lk (gOutLock);
	DWORD n = 0;
	WriteFile (gOut, s.data (), (DWORD)s.size (), &n, nullptr);
}

static void takeStdout ()
{
	// a copy of the handle, because pointing the C runtime's stdout at stderr closes the original
	DuplicateHandle (GetCurrentProcess (), GetStdHandle (STD_OUTPUT_HANDLE), GetCurrentProcess (), &gOut, 0, FALSE, DUPLICATE_SAME_ACCESS);
	HANDLE err = GetStdHandle (STD_ERROR_HANDLE);
	SetStdHandle (STD_OUTPUT_HANDLE, err);
	_dup2 (_fileno (stderr), _fileno (stdout));
}

static std::string utf8 (const std::wstring& w)
{
	if (w.empty ()) return {};
	int n = WideCharToMultiByte (CP_UTF8, 0, w.data (), (int)w.size (), nullptr, 0, nullptr, nullptr);
	std::string s (n, '\0');
	WideCharToMultiByte (CP_UTF8, 0, w.data (), (int)w.size (), s.data (), n, nullptr, nullptr);
	return s;
}

//------------------------------------------------------------------------
// --scan
//------------------------------------------------------------------------
static int scan (const std::string& path)
{
	Json out = Json::object ();
	out["path"] = path;
	std::string err;
	auto module = VST3::Hosting::Module::create (path, err);
	if (!module)
	{
		out["ok"] = false;
		out["error"] = err.empty () ? std::string ("could not load the module") : err;
		send (out);
		return 1;
	}
	auto factory = module->getFactory ();
	auto info = factory.info ();
	Json classes = Json::array ();
	for (auto& ci : factory.classInfos ())
	{
		if (ci.category () != kVstAudioEffectClass) continue;
		Json c = Json::object ();
		c["cid"] = ci.ID ().toString ();
		c["name"] = ci.name ();
		c["vendor"] = ci.vendor ().empty () ? info.vendor () : ci.vendor ();
		c["version"] = ci.version ();
		c["sub"] = ci.subCategoriesString ();
		classes.push (c);
	}
	out["ok"] = true;
	out["classes"] = classes;
	send (out);
	// leave without unloading: some plugins crash in their own unload, and the scan is already reported
	TerminateProcess (GetCurrentProcess (), 0);
	return 0;
}

//------------------------------------------------------------------------
// --host
//------------------------------------------------------------------------
static ory::Shm* gShm = nullptr;
static HANDLE gWake = nullptr, gDone = nullptr;
static std::atomic<bool> gRunning {true};
static constexpr UINT WM_COMMAND_LINE = WM_APP + 1, WM_STDIN_CLOSED = WM_APP + 2;
// commands are posted to a hidden window rather than the thread: Windows drops thread messages while a window is being moved or sized
static HWND gMsgWnd = nullptr;
static const bool gDebug = getenv ("ORY_VSTHOST_DEBUG") != nullptr;

struct Slot;
static void pushEdit (Slot& s, ParamID id, ParamValue v);

// what a plugin's controller calls when its own editor changes something: a parameter (performEdit), many at once
// (restartComponent, after loading a preset, say) or settings that aren't parameters at all (setDirty)
static void reportChanged (Slot* s, bool params);
class ComponentHandler : public IComponentHandler, public IComponentHandler2
{
public:
	Slot* slot = nullptr;
	tresult PLUGIN_API beginEdit (ParamID) override { return kResultOk; }
	tresult PLUGIN_API performEdit (ParamID id, ParamValue v) override;
	tresult PLUGIN_API endEdit (ParamID) override { reportChanged (slot, false); return kResultOk; }
	tresult PLUGIN_API restartComponent (int32 flags) override
	{
		reportChanged (slot, (flags & (kParamValuesChanged | kReloadComponent)) != 0);
		return kResultOk;
	}
	tresult PLUGIN_API setDirty (TBool dirty) override
	{
		if (dirty) reportChanged (slot, false);
		return kResultOk;
	}
	tresult PLUGIN_API requestOpenEditor (FIDString) override { return kNotImplemented; }
	tresult PLUGIN_API startGroupEdit () override { return kResultOk; }
	tresult PLUGIN_API finishGroupEdit () override { return kResultOk; }
	tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override
	{
		if (FUnknownPrivate::iidEqual (iid, IComponentHandler::iid) || FUnknownPrivate::iidEqual (iid, FUnknown::iid))
		{
			*obj = static_cast<IComponentHandler*> (this);
			return kResultOk;
		}
		if (FUnknownPrivate::iidEqual (iid, IComponentHandler2::iid))
		{
			*obj = static_cast<IComponentHandler2*> (this);
			return kResultOk;
		}
		*obj = nullptr;
		return kNoInterface;
	}
	// lives as long as its slot, which outlives the plugin
	uint32 PLUGIN_API addRef () override { return 1000; }
	uint32 PLUGIN_API release () override { return 1000; }
};

// what a plugin's editor calls to ask for a different window size
class PlugFrame : public IPlugFrame
{
public:
	Slot* slot = nullptr;
	tresult PLUGIN_API resizeView (IPlugView* view, ViewRect* r) override;
	tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override
	{
		if (FUnknownPrivate::iidEqual (iid, IPlugFrame::iid) || FUnknownPrivate::iidEqual (iid, FUnknown::iid))
		{
			*obj = this;
			return kResultOk;
		}
		*obj = nullptr;
		return kNoInterface;
	}
	uint32 PLUGIN_API addRef () override { return 1000; }
	uint32 PLUGIN_API release () override { return 1000; }
};

struct Slot
{
	int index = 0;
	std::mutex lock;                     // held by the main thread while loading or unloading; the audio thread only try-locks
	std::atomic<bool> ready {false};
	IPtr<PlugProvider> provider;
	IPtr<IComponent> component;
	IPtr<IAudioProcessor> processor;
	IPtr<IEditController> controller;
	ComponentHandler handler;
	PlugFrame frame;
	IPtr<IPlugView> view;                // the plugin's editor, while its window is open
	HWND editor = nullptr;
	bool sizing = false;                 // the window is being sized to the view (so its WM_SIZE isn't passed back)
	HostProcessData data;
	EventList events {512};
	ParameterChanges paramIn {256};
	ProcessContext context {};
	std::vector<ory::Event> pending;     // events waiting for their block, in time order
	std::set<std::pair<int, int>> held;  // sounding notes: pitch, id
	std::mutex editLock;
	std::vector<std::pair<ParamID, ParamValue>> edits;   // from the plugin's editor, on their way to its processor
	std::mutex ctlLock;
	std::map<ParamID, ParamValue> toController;           // from ORY-1's dials, on their way to its editor (latest only)
	double sampleRate = 48000, tempo = 120;
	bool playing = false, flush = false, crashed = false;
	std::atomic<bool> crashTest {false};
	int inChannels = 0, outChannels = 0;
	std::string name;
};
static Slot gSlots[ory::kMaxSlots];

static void reportChanged (Slot* s, bool params)
{
	if (!s) return;
	Json j = Json::object ();
	j["event"] = "changed";
	j["slot"] = s->index;
	j["params"] = params;
	send (j);
}

//------------------------------------------------------------------------
// plugin state: what the plugin saves of itself (its preset, settings that aren't parameters), as base64 for JSON
//------------------------------------------------------------------------
static const char kB64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static std::string toBase64 (const char* data, size_t n)
{
	std::string out;
	out.reserve ((n + 2) / 3 * 4);
	for (size_t i = 0; i < n; i += 3)
	{
		uint32_t v = (uint8_t)data[i] << 16 | (i + 1 < n ? (uint8_t)data[i + 1] << 8 : 0) | (i + 2 < n ? (uint8_t)data[i + 2] : 0);
		out += kB64[v >> 18 & 63];
		out += kB64[v >> 12 & 63];
		out += i + 1 < n ? kB64[v >> 6 & 63] : '=';
		out += i + 2 < n ? kB64[v & 63] : '=';
	}
	return out;
}

static std::string fromBase64 (const std::string& in)
{
	int map[256];
	for (int& m : map) m = -1;
	for (int i = 0; i < 64; i++) map[(uint8_t)kB64[i]] = i;
	std::string out;
	out.reserve (in.size () * 3 / 4);
	uint32_t v = 0;
	int bits = 0;
	for (unsigned char c : in)
	{
		if (map[c] < 0) continue;
		v = v << 6 | map[c];
		bits += 6;
		if (bits >= 8)
		{
			bits -= 8;
			out += (char)(v >> bits & 0xff);
		}
	}
	return out;
}

// the component's or the controller's state, as base64 ("" when it has none)
static std::string saveState (FUnknown* part, bool controller)
{
	IPtr<MemoryStream> stream = owned (new MemoryStream ());
	tresult r = controller ? static_cast<IEditController*> (part)->getState (stream) : static_cast<IComponent*> (part)->getState (stream);
	if (r != kResultOk) return {};
	return toBase64 (stream->getData (), (size_t)stream->getSize ());
}

// a state saved earlier, given back to the plugin: the component's to the component and (so its editor and parameters
// agree) to the controller, then the controller's own
static void restoreState (Slot& s, const std::string& comp, const std::string& ctrl)
{
	if (!comp.empty ())
	{
		std::string bytes = fromBase64 (comp);
		IPtr<MemoryStream> a = owned (new MemoryStream (bytes.data (), (TSize)bytes.size ()));
		s.component->setState (a);
		if (s.controller)
		{
			IPtr<MemoryStream> b = owned (new MemoryStream (bytes.data (), (TSize)bytes.size ()));
			s.controller->setComponentState (b);
		}
	}
	if (!ctrl.empty () && s.controller)
	{
		std::string bytes = fromBase64 (ctrl);
		IPtr<MemoryStream> c = owned (new MemoryStream (bytes.data (), (TSize)bytes.size ()));
		s.controller->setState (c);
	}
}

// a large state is kept in a file of its own (ORY-1 passes its path): {"comp": "...", "ctrl": "..."}
static bool readStateFile (const std::string& path, std::string& comp, std::string& ctrl)
{
	int n = MultiByteToWideChar (CP_UTF8, 0, path.data (), (int)path.size (), nullptr, 0);
	std::wstring w (n, L'\0');
	MultiByteToWideChar (CP_UTF8, 0, path.data (), (int)path.size (), w.data (), n);
	HANDLE f = CreateFileW (w.c_str (), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
	if (f == INVALID_HANDLE_VALUE) return false;
	LARGE_INTEGER size {};
	GetFileSizeEx (f, &size);
	std::string text ((size_t)size.QuadPart, '\0');
	DWORD got = 0;
	bool ok = ReadFile (f, text.data (), (DWORD)text.size (), &got, nullptr) && got == text.size ();
	CloseHandle (f);
	if (!ok) return false;
	Json j = Json::parse (text);
	comp = j.get ("comp").str ();
	ctrl = j.get ("ctrl").str ();
	return true;
}

//------------------------------------------------------------------------
// editor windows: the plugin's own interface, in a window ORY-1's window owns (so it stays above ORY-1 and minimises with it)
//------------------------------------------------------------------------
static constexpr wchar_t kEditorClass[] = L"ORY1VstEditor";
static constexpr DWORD kEditorExStyle = 0;

// size a window so its inside is w x h
static void sizeWindowTo (Slot& s, int w, int h)
{
	RECT rc {0, 0, w, h};
	DWORD style = (DWORD)GetWindowLongPtrW (s.editor, GWL_STYLE);
	AdjustWindowRectExForDpi (&rc, style, FALSE, kEditorExStyle, GetDpiForWindow (s.editor));
	s.sizing = true;
	SetWindowPos (s.editor, nullptr, 0, 0, rc.right - rc.left, rc.bottom - rc.top, SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
	s.sizing = false;
}

tresult PLUGIN_API PlugFrame::resizeView (IPlugView* view, ViewRect* r)
{
	if (!slot || !slot->editor || !view || !r) return kInvalidArgument;
	if (gDebug) fprintf (stderr, "[vsthost] editor asks for %dx%d\n", r->getWidth (), r->getHeight ());
	sizeWindowTo (*slot, r->getWidth (), r->getHeight ());
	view->onSize (r);
	return kResultTrue;
}

static void closeEditor (Slot& s, bool report)
{
	if (!s.editor) return;
	// where the window sits when it isn't minimised (a minimised one is parked far off screen)
	WINDOWPLACEMENT place {sizeof (WINDOWPLACEMENT)};
	GetWindowPlacement (s.editor, &place);
	RECT rc = place.rcNormalPosition;
	if (s.crashed) s.view.take (); // a crashed plugin isn't called again
	else if (s.view)
	{
		s.view->removed ();
		s.view->setFrame (nullptr);
	}
	s.view = nullptr;
	HWND w = s.editor;
	s.editor = nullptr;
	DestroyWindow (w);
	if (!report) return;
	Json j = Json::object ();
	j["event"] = "editor";
	j["slot"] = s.index;
	j["open"] = false;
	j["x"] = (int)rc.left;
	j["y"] = (int)rc.top;
	send (j);
}

static LRESULT CALLBACK editorProc (HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	auto* s = (Slot*)GetWindowLongPtrW (w, GWLP_USERDATA);
	switch (msg)
	{
		case WM_CLOSE:
			if (s) closeEditor (*s, true);
			return 0;
		case WM_SIZE:
			// the person resized a resizable editor: the plugin may adjust the size, then lays itself out
			if (s && s->view && !s->sizing && wp != SIZE_MINIMIZED)
			{
				ViewRect r {0, 0, LOWORD (lp), HIWORD (lp)};
				ViewRect want = r;
				if (s->view->checkSizeConstraint (&want) == kResultTrue && (want.getWidth () != r.getWidth () || want.getHeight () != r.getHeight ()))
					sizeWindowTo (*s, want.getWidth (), want.getHeight ());
				s->view->onSize (&want);
			}
			return 0;
		case WM_DPICHANGED:
			if (s && s->view)
			{
				if (auto scale = U::cast<IPlugViewContentScaleSupport> (s->view)) scale->setContentScaleFactor (HIWORD (wp) / 96.f);
				auto* r = (RECT*)lp;
				SetWindowPos (w, nullptr, r->left, r->top, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
				ViewRect vr {};
				if (s->view->getSize (&vr) == kResultOk) sizeWindowTo (*s, vr.getWidth (), vr.getHeight ());
			}
			return 0;
	}
	return DefWindowProcW (w, msg, wp, lp);
}

static std::wstring wide (const std::string& s)
{
	if (s.empty ()) return {};
	int n = MultiByteToWideChar (CP_UTF8, 0, s.data (), (int)s.size (), nullptr, 0);
	std::wstring w (n, L'\0');
	MultiByteToWideChar (CP_UTF8, 0, s.data (), (int)s.size (), w.data (), n);
	return w;
}

// {slot, owner (ORY-1's window), title, x?, y?}: opens the plugin's editor, or brings its window forward if it's open
static Json openEditor (Slot& s, const Json& cmd)
{
	Json r = Json::object ();
	if (s.editor)
	{
		ShowWindow (s.editor, SW_SHOWNORMAL);
		SetForegroundWindow (s.editor);
		r["ok"] = true;
		return r;
	}
	if (!s.ready || !s.controller) { r["error"] = "the plugin isn't running"; return r; }
	IPtr<IPlugView> view = owned (s.controller->createView (ViewType::kEditor));
	if (!view) { r["error"] = "this plugin has no editor of its own"; return r; }
	if (view->isPlatformTypeSupported (kPlatformTypeHWND) != kResultTrue) { r["error"] = "this plugin's editor doesn't support Windows"; return r; }
	bool resizable = view->canResize () == kResultTrue;
	DWORD style = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | (resizable ? WS_THICKFRAME | WS_MAXIMIZEBOX : 0);
	HWND owner = (HWND)(intptr_t)(int64)cmd.get ("owner").num (0);
	if (owner && !IsWindow (owner)) owner = nullptr;
	HWND w = CreateWindowExW (kEditorExStyle, kEditorClass, wide (cmd.get ("title").str ("Plugin")).c_str (), style, CW_USEDEFAULT, CW_USEDEFAULT, 200, 100,
	                          owner, nullptr, GetModuleHandleW (nullptr), nullptr);
	if (!w) { r["error"] = "could not make a window"; return r; }
	SetWindowLongPtrW (w, GWLP_USERDATA, (LONG_PTR)&s);
	s.editor = w;
	s.view = view;
	s.frame.slot = &s;
	view->setFrame (&s.frame);
	if (auto scale = U::cast<IPlugViewContentScaleSupport> (view)) scale->setContentScaleFactor (GetDpiForWindow (w) / 96.f);
	ViewRect vr {0, 0, 400, 300};
	view->getSize (&vr);
	if (gDebug) fprintf (stderr, "[vsthost] editor size %dx%d at dpi %u, resizable %d\n", vr.getWidth (), vr.getHeight (), GetDpiForWindow (w), (int)resizable);
	sizeWindowTo (s, vr.getWidth (), vr.getHeight ());

	// where it was last time, if that's still on a screen; otherwise centred over ORY-1
	RECT wr {};
	GetWindowRect (w, &wr);
	int ww = wr.right - wr.left, wh = wr.bottom - wr.top, x, y;
	POINT at {(LONG)cmd.get ("x").num (-100000), (LONG)cmd.get ("y").num (-100000)};
	if (MonitorFromPoint ({at.x + 40, at.y + 10}, MONITOR_DEFAULTTONULL)) { x = at.x; y = at.y; }
	else
	{
		RECT orc {0, 0, GetSystemMetrics (SM_CXSCREEN), GetSystemMetrics (SM_CYSCREEN)};
		if (owner) GetWindowRect (owner, &orc);
		x = (orc.left + orc.right - ww) / 2;
		y = (orc.top + orc.bottom - wh) / 2;
	}
	SetWindowPos (w, nullptr, x, y, 0, 0, SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);

	if (view->attached ((void*)w, kPlatformTypeHWND) != kResultOk)
	{
		view->setFrame (nullptr);
		s.view = nullptr;
		s.editor = nullptr;
		DestroyWindow (w);
		r["error"] = "the plugin's editor didn't open";
		return r;
	}
	if (gDebug)
	{
		ViewRect after {};
		view->getSize (&after);
		RECT cr {};
		GetClientRect (w, &cr);
		fprintf (stderr, "[vsthost] attached: view %dx%d, window inside %dx%d, scale support %d\n", after.getWidth (), after.getHeight (), (int)cr.right, (int)cr.bottom,
		         (int)(bool)U::cast<IPlugViewContentScaleSupport> (view));
	}
	ShowWindow (w, SW_SHOW);
	SetForegroundWindow (w);
	r["ok"] = true;
	return r;
}

// an edit in the plugin's own editor goes to its processor, and to ORY-1 so a dial assigned to it follows
tresult PLUGIN_API ComponentHandler::performEdit (ParamID id, ParamValue v)
{
	if (!slot) return kResultOk;
	pushEdit (*slot, id, v);
	Json j = Json::object ();
	j["event"] = "param";
	j["slot"] = slot->index;
	j["id"] = (double)id;
	j["value"] = v;
	send (j);
	return kResultOk;
}

// ORY-1's dial values reach the processor on the audio thread; the controller (and the editor showing it) hears about
// them here, on the main thread, a few times a second
static void CALLBACK syncControllers (HWND, UINT, UINT_PTR, DWORD)
{
	for (auto& s : gSlots)
	{
		if (!s.ready || !s.controller) continue;
		std::map<ParamID, ParamValue> latest;
		{
			std::lock_guard<std::mutex> lk (s.ctlLock);
			latest.swap (s.toController);
		}
		for (auto& [id, v] : latest) s.controller->setParamNormalized (id, v);
	}
}

static void pushEdit (Slot& s, ParamID id, ParamValue v)
{
	std::lock_guard<std::mutex> lk (s.editLock);
	s.edits.emplace_back (id, v);
}

// modules stay loaded for the helper's whole life: unloading a plugin's DLL is where many of them crash
static std::map<std::string, VST3::Hosting::Module::Ptr> gModules;
static HostApplication* gHostApp = nullptr;

static VST3::Hosting::Module::Ptr moduleFor (const std::string& path, std::string& err)
{
	auto it = gModules.find (path);
	if (it != gModules.end ()) return it->second;
	auto m = VST3::Hosting::Module::create (path, err);
	if (m) gModules[path] = m;
	return m;
}

static int busChannels (IComponent* c, BusDirection dir)
{
	BusInfo info {};
	if (c->getBusCount (kAudio, dir) < 1 || c->getBusInfo (kAudio, dir, 0, info) != kResultOk) return 0;
	return info.channelCount;
}

// ask for stereo main buses where the plugin allows it; any other buses keep the plugin's own choice
static void setupBuses (IComponent* comp, IAudioProcessor* proc)
{
	int nIn = comp->getBusCount (kAudio, kInput), nOut = comp->getBusCount (kAudio, kOutput);
	std::vector<SpeakerArrangement> ins (nIn), outs (nOut);
	for (int i = 0; i < nIn; i++) proc->getBusArrangement (kInput, i, ins[i]);
	for (int i = 0; i < nOut; i++) proc->getBusArrangement (kOutput, i, outs[i]);
	if (nIn) ins[0] = SpeakerArr::kStereo;
	if (nOut) outs[0] = SpeakerArr::kStereo;
	if (proc->setBusArrangements (ins.data (), nIn, outs.data (), nOut) != kResultTrue)
	{
		// some plugins refuse a change to buses they don't own; ask again with only the main ones changed
		for (int i = 0; i < nIn; i++) proc->getBusArrangement (kInput, i, ins[i]);
		for (int i = 0; i < nOut; i++) proc->getBusArrangement (kOutput, i, outs[i]);
	}
	for (int i = 0; i < nIn; i++) comp->activateBus (kAudio, kInput, i, i == 0);
	for (int i = 0; i < nOut; i++) comp->activateBus (kAudio, kOutput, i, i == 0);
	if (comp->getBusCount (kEvent, kInput) > 0) comp->activateBus (kEvent, kInput, 0, true);
}

static Json paramList (IEditController* ctl)
{
	Json list = Json::array ();
	if (!ctl) return list;
	int n = ctl->getParameterCount ();
	for (int i = 0; i < n; i++)
	{
		ParameterInfo pi {};
		if (ctl->getParameterInfo (i, pi) != kResultOk) continue;
		if (pi.flags & ParameterInfo::kIsHidden) continue;
		Json p = Json::object ();
		p["id"] = (double)pi.id;
		p["title"] = Steinberg::Vst::StringConvert::convert (pi.title);
		p["units"] = Steinberg::Vst::StringConvert::convert (pi.units);
		p["steps"] = (int)pi.stepCount;
		p["def"] = pi.defaultNormalizedValue;
		p["value"] = ctl->getParamNormalized (pi.id);
		p["auto"] = (pi.flags & ParameterInfo::kCanAutomate) != 0;
		if (pi.flags & ParameterInfo::kIsBypass) p["bypass"] = true;
		if (pi.flags & ParameterInfo::kIsProgramChange) p["program"] = true;
		if (pi.flags & ParameterInfo::kIsReadOnly) p["readonly"] = true;
		list.push (p);
	}
	return list;
}

static void unloadSlot (Slot& s)
{
	closeEditor (s, false);
	s.ready = false;
	std::lock_guard<std::mutex> lk (s.lock);
	if (s.crashed)
	{
		// a plugin that crashed is in no state to be shut down: calling into it again could take the helper with it,
		// so its memory is left behind instead, until the helper exits
		s.crashed = false;
		s.processor.take ();
		s.controller.take ();
		s.component.take ();
		s.provider.take ();
		s.data.unprepare ();
		s.pending.clear ();
		s.held.clear ();
		s.edits.clear ();
		s.toController.clear ();
		s.flush = false;
		return;
	}
	if (s.processor) s.processor->setProcessing (false);
	if (s.component) s.component->setActive (false);
	s.data.unprepare ();
	if (s.controller) s.controller->setComponentHandler (nullptr);
	s.processor = nullptr;
	s.controller = nullptr;
	s.component = nullptr;
	s.provider = nullptr; // terminates and releases both halves of the plugin
	s.pending.clear ();
	s.held.clear ();
	s.edits.clear ();
	s.toController.clear ();
	s.flush = false;
}

static Json load (const Json& cmd)
{
	Json r = Json::object ();
	int free = -1;
	for (int i = 0; i < ory::kMaxSlots; i++)
		if (!gSlots[i].provider) { free = i; break; }
	if (free < 0) { r["error"] = "too many plugins are loaded"; return r; }

	std::string path = cmd.get ("path").str (), err;
	auto module = moduleFor (path, err);
	if (!module) { r["error"] = err.empty () ? std::string ("could not load ") + path : err; return r; }
	auto uid = VST3::UID::fromString (cmd.get ("cid").str ());
	if (!uid) { r["error"] = "bad class id"; return r; }
	auto& factory = module->getFactory ();
	const VST3::Hosting::ClassInfo* found = nullptr;
	auto infos = factory.classInfos ();
	for (auto& ci : infos)
		if (ci.ID () == *uid) { found = &ci; break; }
	if (!found) { r["error"] = "this plugin is no longer in its file"; return r; }

	Slot& s = gSlots[free];
	std::lock_guard<std::mutex> lk (s.lock);
	s.provider = owned (new PlugProvider (factory, *found, true));
	if (!s.provider->initialize ()) { s.provider = nullptr; r["error"] = "the plugin would not start"; return r; }
	s.component = s.provider->getComponentPtr ();
	s.controller = s.provider->getControllerPtr ();
	s.processor = U::cast<IAudioProcessor> (s.component);
	if (!s.processor) { s.provider = nullptr; s.component = nullptr; s.controller = nullptr; r["error"] = "the plugin doesn't process audio"; return r; }
	s.handler.slot = &s;
	if (s.controller) s.controller->setComponentHandler (&s.handler);
	// the controller starts out knowing nothing of the processor's settings: hand it the component's state, as hosts do,
	// so the values it reports (and its editor shows) are the ones the plugin plays with
	if (s.controller)
	{
		IPtr<MemoryStream> state = owned (new MemoryStream ());
		if (s.component->getState (state) == kResultOk)
		{
			state->seek (0, IBStream::kIBSeekSet, nullptr);
			s.controller->setComponentState (state);
		}
	}

	s.sampleRate = cmd.get ("sampleRate").num (48000);
	bool restored = false;
	{
		std::string comp = cmd.get ("comp").str (), ctrl = cmd.get ("ctrl").str ();
		if (comp.empty () && ctrl.empty () && !cmd.get ("stateFile").str ().empty () && !readStateFile (cmd.get ("stateFile").str (), comp, ctrl))
			r["stateError"] = "the saved state file is missing";
		if (!comp.empty () || !ctrl.empty ())
		{
			restoreState (s, comp, ctrl);
			restored = true;
		}
	}
	setupBuses (s.component, s.processor);
	ProcessSetup setup {kRealtime, kSample32, ory::kQuantum, s.sampleRate};
	if (s.processor->setupProcessing (setup) != kResultOk)
	{
		s.provider = nullptr; s.component = nullptr; s.controller = nullptr; s.processor = nullptr;
		r["error"] = "the plugin doesn't support this sample rate";
		return r;
	}
	s.data.prepare (*s.component, ory::kQuantum, kSample32);
	s.data.inputEvents = &s.events;
	s.data.inputParameterChanges = &s.paramIn;
	s.data.processContext = &s.context;
	s.context = {};
	s.context.sampleRate = s.sampleRate;
	s.context.timeSigNumerator = 4;
	s.context.timeSigDenominator = 4;
	s.component->setActive (true);
	s.processor->setProcessing (true);
	s.inChannels = busChannels (s.component, kInput);
	s.outChannels = busChannels (s.component, kOutput);
	s.name = found->name ();

	ory::Slot& z = gShm->slot[free];
	z.inWrite = 0; z.outDone = 0; z.evWrite = 0; z.evRead = 0;
	s.ready = true;

	r["slot"] = free;
	r["name"] = s.name;
	r["inputs"] = s.inChannels;
	r["outputs"] = s.outChannels;
	r["latency"] = (int)s.processor->getLatencySamples ();
	r["restored"] = restored;
	r["params"] = paramList (s.controller);
	return r;
}

static void handle (const std::string& line)
{
	Json cmd = Json::parse (line);
	Json r = Json::object ();
	r["id"] = cmd.get ("id");
	std::string what = cmd.get ("cmd").str ();
	if (what == "load") { Json l = load (cmd); for (auto& [k, v] : l.obj) r[k] = v; }
	else if (what == "unload")
	{
		int i = (int)cmd.get ("slot").num (-1);
		if (i >= 0 && i < ory::kMaxSlots) unloadSlot (gSlots[i]);
		r["ok"] = true;
	}
	else if (what == "params")
	{
		int i = (int)cmd.get ("slot").num (-1);
		if (i >= 0 && i < ory::kMaxSlots && gSlots[i].ready) r["params"] = paramList (gSlots[i].controller);
		else r["error"] = "no plugin in that slot";
	}
	// how the plugin writes a parameter's value, e.g. "440 Hz": {slot, param, value} -> {text}
	// ("param", not "id": every command's "id" is the request's own, for matching its reply)
	else if (what == "paramText")
	{
		int i = (int)cmd.get ("slot").num (-1);
		if (i >= 0 && i < ory::kMaxSlots && gSlots[i].ready && gSlots[i].controller)
		{
			auto id = (ParamID)(uint32)(int64)cmd.get ("param").num (0);
			String128 text {};
			if (gSlots[i].controller->getParamStringByValue (id, cmd.get ("value").num (0), text) == kResultOk)
			{
				ParameterInfo pi {};
				std::string units;
				for (int k = 0, n = gSlots[i].controller->getParameterCount (); k < n; k++)
					if (gSlots[i].controller->getParameterInfo (k, pi) == kResultOk && pi.id == id) { units = Steinberg::Vst::StringConvert::convert (pi.units); break; }
				std::string t = Steinberg::Vst::StringConvert::convert (text);
				r["text"] = units.empty () || t.find (units) != std::string::npos ? t : t + " " + units;
			}
			else r["error"] = "no text";
		}
		else r["error"] = "no plugin in that slot";
	}
	else if (what == "editor")
	{
		int i = (int)cmd.get ("slot").num (-1);
		if (i >= 0 && i < ory::kMaxSlots) { Json e = openEditor (gSlots[i], cmd); for (auto& [k, v] : e.obj) r[k] = v; }
		else r["error"] = "no plugin in that slot";
	}
	else if (what == "closeEditor")
	{
		int i = (int)cmd.get ("slot").num (-1);
		if (i >= 0 && i < ory::kMaxSlots) closeEditor (gSlots[i], true);
		r["ok"] = true;
	}
	else if (what == "getState")
	{
		int i = (int)cmd.get ("slot").num (-1);
		if (i >= 0 && i < ory::kMaxSlots && gSlots[i].ready)
		{
			r["comp"] = saveState (gSlots[i].component, false);
			r["ctrl"] = gSlots[i].controller ? saveState (gSlots[i].controller, true) : std::string ();
		}
		else r["error"] = "no plugin in that slot";
	}
	else if (what == "hello") { r["ok"] = true; r["version"] = (int)ory::kVersion; }
	// for testing crash recovery, only with ORY_VSTHOST_DEBUG set: "crash" fails a plugin's next process call, "exit" stops the helper
	else if (gDebug && what == "crash")
	{
		int i = (int)cmd.get ("slot").num (-1);
		if (i >= 0 && i < ory::kMaxSlots) gSlots[i].crashTest = true;
		r["ok"] = true;
	}
	else if (gDebug && what == "exit") TerminateProcess (GetCurrentProcess (), 3);
	else r["error"] = "unknown command " + what;
	send (r);
}

//------------------------------------------------------------------------
// audio
//------------------------------------------------------------------------
// a plugin that crashes inside process() is caught here, so the helper can say which one it was
static bool safeProcess (IAudioProcessor* p, ProcessData& d, bool fault)
{
	__try
	{
		if (fault) *(volatile int*)nullptr = 0; // the "crash" test command: fails here exactly as a broken plugin would
		tresult r = p->process (d);
		if (gDebug && r != kResultOk)
		{
			static int n = 0;
			if (n++ < 3) fprintf (stderr, "[vsthost] process returned %d\n", (int)r);
		}
		return true;
	}
	__except (EXCEPTION_EXECUTE_HANDLER) { return false; }
}

static void addEvent (Slot& s, const ory::Event& e, int offset)
{
	Event ev {};
	ev.busIndex = 0;
	ev.sampleOffset = offset;
	switch (e.type)
	{
		case ory::kNoteOn:
			ev.type = Event::kNoteOnEvent;
			ev.noteOn.pitch = (int16)e.a;
			ev.noteOn.velocity = e.v;
			ev.noteOn.noteId = e.b;
			s.events.addEvent (ev);
			s.held.insert ({e.a, e.b});
			break;
		case ory::kNoteOff:
			ev.type = Event::kNoteOffEvent;
			ev.noteOff.pitch = (int16)e.a;
			ev.noteOff.noteId = e.b;
			s.events.addEvent (ev);
			s.held.erase ({e.a, e.b});
			break;
		case ory::kAllNotesOff:
			for (auto& [pitch, id] : s.held)
			{
				Event off {};
				off.type = Event::kNoteOffEvent;
				off.sampleOffset = offset;
				off.noteOff.pitch = (int16)pitch;
				off.noteOff.noteId = id;
				s.events.addEvent (off);
			}
			s.held.clear ();
			// anything still queued belongs to the notes just stopped
			break;
		case ory::kParam:
		{
			int32 qi = 0, pi = 0;
			if (auto* q = s.paramIn.addParameterData ((ParamID)(uint32)e.a, qi)) q->addPoint (offset, e.v, pi);
			std::unique_lock<std::mutex> lk (s.ctlLock, std::try_to_lock);
			if (lk) s.toController[(ParamID)(uint32)e.a] = e.v;
			break;
		}
		case ory::kTransport:
			s.tempo = e.v > 0 ? e.v : s.tempo;
			s.playing = e.a != 0;
			break;
	}
}

static void processBlock (Slot& s, ory::Slot& z, int b)
{
	const double start = z.frames[b], end = start + ory::kQuantum;
	s.events.clear ();
	s.paramIn.clearQueue ();
	{
		std::unique_lock<std::mutex> lk (s.editLock, std::try_to_lock);
		if (lk)
		{
			for (auto& [id, v] : s.edits) { int32 qi = 0, pi = 0; if (auto* q = s.paramIn.addParameterData (id, qi)) q->addPoint (0, v, pi); }
			s.edits.clear ();
		}
	}
	if (s.flush)
	{
		s.flush = false;
		addEvent (s, ory::Event {start, ory::kAllNotesOff, 0, 0, 0}, 0);
	}
	size_t used = 0;
	while (used < s.pending.size () && s.pending[used].frame < end)
	{
		auto& e = s.pending[used++];
		if (gDebug) fprintf (stderr, "[vsthost] block %.0f: event %d (%d) due %.0f\n", start, e.type, e.a, e.frame);
		addEvent (s, e, std::clamp ((int)(e.frame - start), 0, ory::kQuantum - 1));
	}
	s.pending.erase (s.pending.begin (), s.pending.begin () + used);

	auto& d = s.data;
	if (d.numInputs > 0 && d.inputs[0].numChannels > 0)
	{
		auto& bus = d.inputs[0];
		for (int c = 0; c < bus.numChannels; c++)
		{
			float* dst = bus.channelBuffers32[c];
			if (bus.numChannels == 1)
				for (int i = 0; i < ory::kQuantum; i++) dst[i] = 0.5f * (z.in[b][0][i] + z.in[b][1][i]);
			else if (c < 2)
				memcpy (dst, z.in[b][c], sizeof (float) * ory::kQuantum);
			else
				memset (dst, 0, sizeof (float) * ory::kQuantum);
		}
	}
	auto& ctx = s.context;
	ctx.projectTimeSamples = (int64)start;
	ctx.continousTimeSamples = (int64)start;
	ctx.tempo = s.tempo;
	ctx.projectTimeMusic = start / s.sampleRate * s.tempo / 60.0;
	ctx.barPositionMusic = floor (ctx.projectTimeMusic / 4.0) * 4.0;
	ctx.state = ProcessContext::kTempoValid | ProcessContext::kTimeSigValid | ProcessContext::kProjectTimeMusicValid |
	            ProcessContext::kBarPositionValid | ProcessContext::kContTimeValid | (s.playing ? ProcessContext::kPlaying : 0);
	d.numSamples = ory::kQuantum;

	if (!safeProcess (s.processor, d, s.crashTest.exchange (false)))
	{
		s.ready = false;
		s.crashed = true;
		Json j = Json::object ();
		j["event"] = "crashed";
		j["slot"] = s.index;
		send (j);
		memset (z.out[b], 0, sizeof z.out[b]);
		return;
	}

	if (d.numOutputs > 0 && d.outputs[0].numChannels > 0)
	{
		auto& bus = d.outputs[0];
		memcpy (z.out[b][0], bus.channelBuffers32[0], sizeof (float) * ory::kQuantum);
		memcpy (z.out[b][1], bus.channelBuffers32[bus.numChannels > 1 ? 1 : 0], sizeof (float) * ory::kQuantum);
	}
	else
		memset (z.out[b], 0, sizeof z.out[b]);

	if (gDebug)
	{
		static float peak = 0;
		static int blocks = 0, events = 0;
		for (int i = 0; i < ory::kQuantum; i++) peak = std::max (peak, fabsf (z.out[b][0][i]));
		events += s.events.getEventCount ();
		if (++blocks % 375 == 0)
		{
			fprintf (stderr, "[vsthost] slot %d frame %.0f: %d events, peak %.4f, %d outs\n", s.index, start, events, peak, d.numOutputs);
			peak = 0;
			events = 0;
		}
	}
}

static bool serviceSlot (Slot& s)
{
	ory::Slot& z = gShm->slot[s.index];
	uint32_t ew = z.evWrite.load (std::memory_order_acquire), er = z.evRead.load (std::memory_order_relaxed);
	if (ew != er)
	{
		for (; er != ew; er++)
		{
			const ory::Event& e = z.ev[er % ory::kEvRing];
			// Stop and tempo act on arrival: ORY-1 suspends its audio on Stop, so no block may come to carry them.
			// Stop also drops the notes still queued, or they would all sound at once on the next Play.
			if (e.type == ory::kAllNotesOff)
			{
				s.pending.clear ();
				s.flush = true;
				continue;
			}
			if (e.type == ory::kTransport)
			{
				addEvent (s, e, 0);
				continue;
			}
			// keep pending in time order; events nearly always arrive in order, so this is a short walk from the back
			auto at = s.pending.end ();
			while (at != s.pending.begin () && (at - 1)->frame > e.frame) --at;
			s.pending.insert (at, e);
		}
		z.evRead.store (er, std::memory_order_release);
	}
	uint32_t m = z.inWrite.load (std::memory_order_acquire), o = z.outDone.load (std::memory_order_relaxed);
	if (m == o) return false;
	if (m - o > (uint32_t)ory::kRing) o = m - ory::kRing; // far behind: skip what's already been overwritten
	for (; o != m && s.ready; o++) processBlock (s, z, o % ory::kRing);
	z.outDone.store (m, std::memory_order_release);
	return true;
}

static void audioThread ()
{
	DWORD task = 0;
	HANDLE mm = AvSetMmThreadCharacteristicsW (L"Pro Audio", &task);
	if (mm) AvSetMmThreadPriority (mm, AVRT_PRIORITY_HIGH);
	// flush denormals: decaying reverbs and filters get very slow without this
	_mm_setcsr (_mm_getcsr () | 0x8040);
	while (gRunning)
	{
		WaitForSingleObject (gWake, 2);
		bool any = false;
		for (auto& s : gSlots)
		{
			if (!s.ready) continue;
			std::unique_lock<std::mutex> lk (s.lock, std::try_to_lock);
			if (lk && s.ready) any |= serviceSlot (s);
		}
		if (any) SetEvent (gDone);
	}
	if (mm) AvRevertMmThreadCharacteristics (mm);
}

// commands arrive one per line on stdin; each is handed to the main thread
static void readerThread ()
{
	HANDLE in = GetStdHandle (STD_INPUT_HANDLE);
	std::string buf;
	char chunk[65536];
	DWORD n = 0;
	while (ReadFile (in, chunk, sizeof chunk, &n, nullptr) && n > 0)
	{
		buf.append (chunk, n);
		size_t nl;
		while ((nl = buf.find ('\n')) != std::string::npos)
		{
			auto* line = new std::string (buf.substr (0, nl));
			buf.erase (0, nl + 1);
			PostMessageW (gMsgWnd, WM_COMMAND_LINE, 0, (LPARAM)line);
		}
	}
	// ORY-1 closed the pipe: it has quit or restarted the helper
	PostMessageW (gMsgWnd, WM_STDIN_CLOSED, 0, 0);
}

static LRESULT CALLBACK messageProc (HWND w, UINT msg, WPARAM wp, LPARAM lp)
{
	if (msg == WM_COMMAND_LINE)
	{
		std::unique_ptr<std::string> line ((std::string*)lp);
		handle (*line);
		return 0;
	}
	if (msg == WM_STDIN_CLOSED)
	{
		PostQuitMessage (0);
		return 0;
	}
	return DefWindowProcW (w, msg, wp, lp);
}

static int host (const std::string& name)
{
	// editors draw sharp on every monitor; each is told its scale (IPlugViewContentScaleSupport)
	SetProcessDpiAwarenessContext (DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
	std::wstring wname (name.begin (), name.end ());
	HANDLE map = CreateFileMappingW (INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof (ory::Shm), wname.c_str ());
	if (!map) return 2;
	gShm = (ory::Shm*)MapViewOfFile (map, FILE_MAP_ALL_ACCESS, 0, 0, sizeof (ory::Shm));
	if (!gShm) return 2;
	memset (gShm, 0, sizeof (ory::Shm));
	gShm->magic = ory::kMagic;
	gShm->version = ory::kVersion;
	gWake = CreateEventW (nullptr, FALSE, FALSE, (wname + L"-wake").c_str ());
	gDone = CreateEventW (nullptr, FALSE, FALSE, (wname + L"-done").c_str ());
	for (int i = 0; i < ory::kMaxSlots; i++) gSlots[i].index = i;

	gHostApp = new HostApplication ();
	PluginContextFactory::instance ().setPluginContext (gHostApp);

	HINSTANCE inst = GetModuleHandleW (nullptr);
	WNDCLASSW mc {};
	mc.lpfnWndProc = messageProc;
	mc.hInstance = inst;
	mc.lpszClassName = L"ORY1VstMessages";
	RegisterClassW (&mc);
	gMsgWnd = CreateWindowW (mc.lpszClassName, L"", 0, 0, 0, 0, 0, HWND_MESSAGE, nullptr, inst, nullptr);
	WNDCLASSW ec {};
	ec.lpfnWndProc = editorProc;
	ec.hInstance = inst;
	ec.hCursor = LoadCursor (nullptr, IDC_ARROW);
	ec.hbrBackground = (HBRUSH)GetStockObject (BLACK_BRUSH);
	ec.lpszClassName = kEditorClass;
	RegisterClassW (&ec);
	SetTimer (gMsgWnd, 1, 50, syncControllers);
	MSG msg;
	std::thread audio (audioThread);
	std::thread reader (readerThread);
	reader.detach ();

	Json hello = Json::object ();
	hello["event"] = "ready";
	hello["version"] = (int)ory::kVersion;
	hello["pid"] = (int)GetCurrentProcessId ();
	send (hello);

	while (GetMessageW (&msg, nullptr, 0, 0) > 0)
	{
		TranslateMessage (&msg);
		DispatchMessageW (&msg);
	}

	// closing: give the plugins a moment to shut down properly, but never hang ORY-1's quit on one that won't
	std::thread ([] { Sleep (3000); TerminateProcess (GetCurrentProcess (), 0); }).detach ();
	gRunning = false;
	SetEvent (gWake);
	audio.join ();
	for (auto& s : gSlots)
		if (s.provider) unloadSlot (s);
	TerminateProcess (GetCurrentProcess (), 0);
	return 0;
}

int wmain (int argc, wchar_t** argv)
{
	takeStdout ();
	std::vector<std::string> args;
	for (int i = 1; i < argc; i++) args.push_back (utf8 (argv[i]));
	CoInitializeEx (nullptr, COINIT_APARTMENTTHREADED); // editors and some plugins expect COM on the UI thread
	if (args.size () >= 2 && args[0] == "--scan") return scan (args[1]);
	if (args.size () >= 2 && args[0] == "--host") return host (args[1]);
	fprintf (stderr, "usage: ory1-vsthost --scan <path.vst3> | --host <name>\n");
	return 1;
}
