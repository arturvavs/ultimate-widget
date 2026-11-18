// Windows.Media.Control (GlobalSystemMediaTransportControls) through raw vtables. See media.h.
//
// Slots below count from 0 = QueryInterface, 1 = AddRef, 2 = Release, 3..5 = IInspectable, 6.. = methods.
// Ids and method order were read from the live runtime objects (IInspectable::GetIids) and the metadata.
#include "media.h"

#include <roapi.h>
#include <winstring.h>
#include <vector>

namespace media {
namespace {

const IID kIidStatics = { 0x2050c4ee, 0x11a0, 0x57de, { 0xae, 0xd7, 0xc9, 0x7c, 0x70, 0x33, 0x82, 0x45 } };
const IID kIidManager = { 0xcace8eac, 0xe86e, 0x504a, { 0xab, 0x31, 0x5f, 0xf8, 0xff, 0x1b, 0xce, 0x49 } };
const IID kIidAsyncInfo = { 0x00000036, 0x0000, 0x0000, { 0xc0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x46 } };

constexpr int kStaticsRequestAsync = 6;       // IGlobalSystemMediaTransportControlsSessionManagerStatics
constexpr int kManagerGetCurrentSession = 6;  // IGlobalSystemMediaTransportControlsSessionManager
constexpr int kSessionTryGetProps = 7;        // IGlobalSystemMediaTransportControlsSession
constexpr int kSessionGetPlaybackInfo = 9;
constexpr int kSessionSkipNext = 16;
constexpr int kSessionSkipPrevious = 17;
constexpr int kSessionTogglePlayPause = 20;
constexpr int kInfoPlaybackStatus = 7;        // ...SessionPlaybackInfo: get_PlaybackStatus (Playing = 4)
constexpr int kPropsTitle = 6;                // ...SessionMediaProperties
constexpr int kPropsArtist = 9;
constexpr int kAsyncGetStatus = 7;            // IAsyncInfo (Started 0, Completed 1, Canceled 2, Error 3)
constexpr int kAsyncOpGetResults = 8;         // IAsyncOperation<T>: put_Completed 6, get_Completed 7, GetResults 8

void** Vt(void* o) { return *reinterpret_cast<void***>(o); }
void Release(void* o) { if (o) reinterpret_cast<ULONG(STDMETHODCALLTYPE*)(void*)>(Vt(o)[2])(o); }

// obj->method(void** out)
HRESULT Get(void* o, int slot, void** out) {
    *out = nullptr;
    return reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, void**)>(Vt(o)[slot])(o, out);
}

// Waits (up to timeoutMs) for an IAsyncOperation<T> and returns its result pointer. Consumes `op`.
void* Await(void* op, int timeoutMs, bool wantResult) {
    if (!op) return nullptr;
    void* info = nullptr;
    void* result = nullptr;
    if (SUCCEEDED(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, REFIID, void**)>(Vt(op)[0])(op, kIidAsyncInfo, &info)) && info) {
        ULONGLONG end = GetTickCount64() + timeoutMs;
        int status = 0;
        for (;;) {
            if (FAILED(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, int*)>(Vt(info)[kAsyncGetStatus])(info, &status))) break;
            if (status != 0 || (LONGLONG)end - (LONGLONG)GetTickCount64() <= 0) break;
            Sleep(5);
        }
        if (status == 1 && wantResult) Get(op, kAsyncOpGetResults, &result);
        Release(info);
    }
    Release(op);
    return result;
}

std::wstring Str(void* o, int slot) {
    HSTRING h = nullptr;
    if (FAILED(reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, HSTRING*)>(Vt(o)[slot])(o, &h)) || !h) return L"";
    UINT32 n = 0;
    const wchar_t* p = WindowsGetStringRawBuffer(h, &n);
    std::wstring s(p ? p : L"", p ? n : 0);
    WindowsDeleteString(h);
    return s;
}

void* g_manager = nullptr;  // worker thread only

void* GetManager() {
    if (g_manager) return g_manager;
    HSTRING cls = nullptr;
    const wchar_t name[] = L"Windows.Media.Control.GlobalSystemMediaTransportControlsSessionManager";
    if (FAILED(WindowsCreateString(name, (UINT32)(sizeof(name) / sizeof(wchar_t) - 1), &cls))) return nullptr;
    void* statics = nullptr;
    HRESULT hr = RoGetActivationFactory(cls, kIidStatics, &statics);
    WindowsDeleteString(cls);
    if (FAILED(hr) || !statics) return nullptr;
    void* op = nullptr;
    if (SUCCEEDED(Get(statics, kStaticsRequestAsync, &op))) g_manager = Await(op, 3000, true);
    Release(statics);
    return g_manager;
}

void* CurrentSession() {
    void* m = GetManager();
    if (!m) return nullptr;
    void* s = nullptr;
    Get(m, kManagerGetCurrentSession, &s);
    return s;
}

State Read() {
    State st;
    void* s = CurrentSession();
    if (!s) return st;
    st.available = true;
    void* info = nullptr;
    if (SUCCEEDED(Get(s, kSessionGetPlaybackInfo, &info)) && info) {
        int status = 0;
        reinterpret_cast<HRESULT(STDMETHODCALLTYPE*)(void*, int*)>(Vt(info)[kInfoPlaybackStatus])(info, &status);
        st.playing = status == 4;
        Release(info);
    }
    void* op = nullptr;
    if (SUCCEEDED(Get(s, kSessionTryGetProps, &op)) && op) {
        if (void* props = Await(op, 2000, true)) {
            st.title = Str(props, kPropsTitle);
            st.artist = Str(props, kPropsArtist);
            Release(props);
        }
    }
    Release(s);
    return st;
}

void Execute(Cmd c) {
    void* s = CurrentSession();
    if (!s) return;
    int slot = c == Cmd::Prev ? kSessionSkipPrevious : c == Cmd::Next ? kSessionSkipNext : kSessionTogglePlayPause;
    void* op = nullptr;
    if (SUCCEEDED(Get(s, slot, &op))) Await(op, 1500, false);
    Release(s);
}

CRITICAL_SECTION g_lock;
bool g_lockInit = false;
State g_state;
std::vector<Cmd> g_cmds;
HANDLE g_thread = nullptr, g_wake = nullptr, g_stop = nullptr;
HWND g_notify = nullptr;
UINT g_msg = 0;

DWORD WINAPI Worker(LPVOID) {
    HRESULT hr = RoInitialize(RO_INIT_MULTITHREADED);
    bool inited = SUCCEEDED(hr);
    HANDLE ev[2] = { g_stop, g_wake };
    bool first = true;
    for (;;) {
        std::vector<Cmd> cmds;
        EnterCriticalSection(&g_lock);
        cmds.swap(g_cmds);
        LeaveCriticalSection(&g_lock);
        for (Cmd c : cmds) Execute(c);
        if (!cmds.empty()) Sleep(150);  // let the player apply it before re-reading

        State st = Read();
        EnterCriticalSection(&g_lock);
        bool changed = first || !(st == g_state);
        g_state = st;
        LeaveCriticalSection(&g_lock);
        first = false;
        if (changed && g_notify) PostMessageW(g_notify, g_msg, 0, 0);

        DWORD w = WaitForMultipleObjects(2, ev, FALSE, 1000);
        if (w == WAIT_OBJECT_0) break;  // stop
    }
    if (g_manager) { Release(g_manager); g_manager = nullptr; }
    if (inited) RoUninitialize();
    return 0;
}

}  // namespace

bool Start(HWND notify, UINT msg) {
    if (g_thread) return true;
    if (!g_lockInit) { InitializeCriticalSection(&g_lock); g_lockInit = true; }
    g_notify = notify;
    g_msg = msg;
    g_state = State{};
    g_cmds.clear();
    g_stop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
    g_wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!g_stop || !g_wake) return false;
    g_thread = CreateThread(nullptr, 0, Worker, nullptr, 0, nullptr);
    return g_thread != nullptr;
}

void Stop() {
    if (!g_thread) return;
    g_notify = nullptr;
    SetEvent(g_stop);
    WaitForSingleObject(g_thread, 5000);
    CloseHandle(g_thread);
    CloseHandle(g_stop);
    CloseHandle(g_wake);
    g_thread = g_stop = g_wake = nullptr;
}

State Get() {
    State s;
    if (!g_lockInit) return s;
    EnterCriticalSection(&g_lock);
    s = g_state;
    LeaveCriticalSection(&g_lock);
    return s;
}

void Send(Cmd c) {
    if (!g_thread) return;
    EnterCriticalSection(&g_lock);
    g_cmds.push_back(c);
    LeaveCriticalSection(&g_lock);
    SetEvent(g_wake);
}

}  // namespace media
