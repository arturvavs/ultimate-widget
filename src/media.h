#pragma once
// Reads what is playing from Windows' media session (the same source as the volume flyout: Chrome, Edge,
// Brave, Firefox, Spotify ...) and sends play/pause, previous and next to it.
//
// The WinRT interfaces (Windows.Media.Control) have no MinGW headers, so media.cpp calls them through their
// vtables; the interface ids and slot numbers there were read from the Windows metadata on a live system.
// Everything runs on one worker thread, which posts `msg` to `notify` when the state changes.
#include <windows.h>
#include <string>

namespace media {
struct State {
    bool available = false;  // a media session exists
    bool playing = false;
    std::wstring title, artist;
    bool operator==(const State& o) const {
        return available == o.available && playing == o.playing && title == o.title && artist == o.artist;
    }
};
enum class Cmd { Prev, PlayPause, Next };

bool Start(HWND notify, UINT msg);  // no-op when already running
void Stop();
State Get();                        // last state seen by the worker
void Send(Cmd c);                   // executed on the worker, then the state is re-read
}  // namespace media
