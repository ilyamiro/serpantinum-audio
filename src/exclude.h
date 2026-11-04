#pragma once

#include <cctype>
#include <string>

namespace sa {

inline std::string normalizeName(const char* value) {
    std::string out;
    if (!value) return out;
    for (const char* c = value; *c; ++c)
        if (std::isalnum(static_cast<unsigned char>(*c))) out += static_cast<char>(std::tolower(static_cast<unsigned char>(*c)));
    return out;
}

inline bool isVoiceApp(const char* value) {
    static const char* const kApps[] = {
        "discord",  "discordptb", "discordcanary", "vesktop",           "equibop",         "legcord", "armcord",
        "webcord",  "zoom",       "teamsforlinux", "webrtcvoiceengine", "zoomvoiceengine",
    };
    const std::string name = normalizeName(value);
    if (name.empty()) return false;
    for (const char* app : kApps)
        if (name == app) return true;
    return false;
}

inline bool isCallRole(const char* role) {
    const std::string r = normalizeName(role);
    return r == "communication" || r == "phone";
}

inline bool isExcludedStream(const char* appName, const char* binary, const char* role) {
    return isVoiceApp(appName) || isVoiceApp(binary) || isCallRole(role);
}

}
