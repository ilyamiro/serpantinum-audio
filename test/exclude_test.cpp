#include <cassert>
#include <cstdio>

#include "../src/exclude.h"

int main() {
    assert(sa::isExcludedStream("Discord", "Discord", nullptr));
    assert(sa::isExcludedStream(nullptr, "DiscordPTB", nullptr));
    assert(sa::isExcludedStream("Discord Canary", nullptr, nullptr));
    assert(sa::isExcludedStream("WEBRTC VoiceEngine", nullptr, nullptr));
    assert(sa::isExcludedStream("ZOOM VoiceEngine", "zoom", nullptr));
    assert(sa::isExcludedStream("Vesktop", "electron", nullptr));
    assert(sa::isExcludedStream("vesktop", "vesktop", "Music"));
    assert(sa::isExcludedStream("Equibop", nullptr, nullptr));
    assert(sa::isExcludedStream("legcord", nullptr, nullptr));
    assert(sa::isExcludedStream("WebCord", nullptr, nullptr));
    assert(sa::isExcludedStream("Teams for Linux", "teams-for-linux", nullptr));
    assert(sa::isExcludedStream("GNOME Calls", "gnome-calls", "Communication"));
    assert(sa::isExcludedStream("softphone", nullptr, "phone"));

    assert(!sa::isExcludedStream("Firefox", "firefox", nullptr));
    assert(!sa::isExcludedStream("Chromium", "chrome", nullptr));
    assert(!sa::isExcludedStream("Telegram Desktop", "telegram-desktop", "Game"));
    assert(!sa::isExcludedStream("Spotify", "spotify", "Music"));
    assert(!sa::isExcludedStream("OBS", "obs", nullptr));
    assert(!sa::isExcludedStream("discord-rpc-bridge", nullptr, nullptr));
    assert(!sa::isExcludedStream("electron", "electron", nullptr));
    assert(!sa::isExcludedStream(nullptr, nullptr, nullptr));
    assert(!sa::isExcludedStream("", "", ""));

    std::printf("exclude ok\n");
    return 0;
}
