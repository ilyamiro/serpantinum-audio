#include <fcntl.h>
#include <pipewire/extensions/metadata.h>
#include <pipewire/pipewire.h>
#include <poll.h>
#include <spa/param/audio/format-utils.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>

#include "control.h"
#include "dsp/chain.h"
#include "ring.h"

#ifndef SA_VERSION
#define SA_VERSION "dev"
#endif

namespace {

constexpr const char* kSinkName = "serpantinum_sink";
constexpr const char* kOutputName = "serpantinum_output";
constexpr const char* kConfiguredKey = "default.configured.audio.sink";
constexpr const char* kResolvedKey = "default.audio.sink";
constexpr const char* kTargetObjectKey = "target.object";
constexpr const char* kTargetNodeKey = "target.node";
constexpr uint32_t kRate = 48000;
constexpr uint32_t kChannels = 2;
constexpr uint32_t kStride = sizeof(float) * kChannels;

std::string envOr(const char* name, const std::string& fallback) {
    const char* v = std::getenv(name);
    return v && *v ? v : fallback;
}

std::string home() { return envOr("HOME", "/tmp"); }

std::string socketPath() {
    const std::string runtime = envOr("XDG_RUNTIME_DIR", "");
    if (!runtime.empty()) return runtime + "/serpantinum-audio.sock";
    return "/tmp/serpantinum-audio-" + std::to_string(getuid()) + ".sock";
}

std::string runtimeDir() { return envOr("XDG_RUNTIME_DIR", "/tmp") + "/serpantinum"; }

std::string stateDir() { return envOr("XDG_STATE_HOME", home() + "/.local/state") + "/serpantinum"; }

void makeDirs(const std::string& path) {
    for (size_t i = 1; i <= path.size(); ++i) {
        if (i == path.size() || path[i] == '/') mkdir(path.substr(0, i).c_str(), 0755);
    }
}

std::string jsonName(const char* value) {
    if (!value) return {};
    const char* key = std::strstr(value, "\"name\"");
    if (!key) return {};
    const char* open = std::strchr(key + 6, '"');
    if (!open) return {};
    const char* close = std::strchr(open + 1, '"');
    if (!close) return {};
    return std::string(open + 1, close);
}

int connectSocket(const std::string& path) {
    const int fd = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0);
    if (fd < 0) return -1;
    sockaddr_un addr{};
    addr.sun_family = AF_UNIX;
    std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
    if (connect(fd, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0) {
        close(fd);
        return -1;
    }
    timeval tv{1, 0};
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
    return fd;
}

bool request(const std::string& line, std::string& reply) {
    const int fd = connectSocket(socketPath());
    if (fd < 0) return false;
    const std::string out = line + "\n";
    if (write(fd, out.data(), out.size()) != static_cast<ssize_t>(out.size())) {
        close(fd);
        return false;
    }
    reply.clear();
    char buf[512];
    ssize_t n;
    while ((n = read(fd, buf, sizeof buf)) > 0) {
        reply.append(buf, n);
        if (reply.find('\n') != std::string::npos) break;
    }
    close(fd);
    while (!reply.empty() && (reply.back() == '\n' || reply.back() == '\r')) reply.pop_back();
    return true;
}

class Daemon {
public:
    Daemon() : ring_(1 << 15) {}

    int run() {
        std::string reply;
        if (request("ping", reply)) {
            std::fprintf(stderr, "serpantinum-audio: already running\n");
            return 1;
        }

        pw_init(nullptr, nullptr);
        loop_ = pw_main_loop_new(nullptr);
        if (!loop_) return 1;
        pw_loop* loop = pw_main_loop_get_loop(loop_);
        pw_loop_add_signal(loop, SIGINT, onSignal, this);
        pw_loop_add_signal(loop, SIGTERM, onSignal, this);
        pw_loop_add_signal(loop, SIGHUP, onSignal, this);
        signal(SIGPIPE, SIG_IGN);

        context_ = pw_context_new(loop, nullptr, 0);
        core_ = context_ ? pw_context_connect(context_, nullptr, 0) : nullptr;
        if (!core_) {
            std::fprintf(stderr, "serpantinum-audio: cannot connect to PipeWire\n");
            return 1;
        }
        pw_core_add_listener(core_, &coreListener_, &coreEventsTable(), this);

        chain_.setSampleRate(kRate);
        loadState();
        pushParams();

        if (!openSocket(loop)) return 1;

        registry_ = pw_core_get_registry(core_, PW_VERSION_REGISTRY, 0);
        pw_registry_add_listener(registry_, &registryListener_, &registryEventsTable(), this);
        syncSeq_ = pw_core_sync(core_, PW_ID_CORE, 0);

        createSink();

        pw_main_loop_run(loop_);

        shutdown();
        return 0;
    }

private:
    static void onSignal(void* data, int) { pw_main_loop_quit(static_cast<Daemon*>(data)->loop_); }

    static void onCoreDone(void* data, uint32_t id, int seq) {
        Daemon* self = static_cast<Daemon*>(data);
        if (id != PW_ID_CORE) return;
        if (seq == self->shutdownSeq_) self->shutdownDone_ = true;
        if (self->synced_ || seq != self->syncSeq_) return;
        if (++self->syncRounds_ < 2) {
            self->syncSeq_ = pw_core_sync(self->core_, PW_ID_CORE, 0);
            return;
        }
        self->synced_ = true;
        self->onSynced();
    }

    void onSynced() {
        if (metadata_ && configuredName_ == kSinkName)
            pw_metadata_set_property(metadata_, PW_ID_CORE, kConfiguredKey, nullptr, nullptr);
        for (auto& entry : streams_) route(entry.first);
    }

    static void onCoreError(void* data, uint32_t id, int, int res, const char* message) {
        if (id == PW_ID_CORE && res == -EPIPE) {
            std::fprintf(stderr, "serpantinum-audio: PipeWire connection lost: %s\n", message);
            pw_main_loop_quit(static_cast<Daemon*>(data)->loop_);
        }
    }

    void loadState() {
        std::ifstream f(stateDir() + "/audio.state");
        std::string line;
        if (f && std::getline(f, line)) sa::applyCommand(params_, line);
    }

    void saveState() {
        const std::string dir = stateDir();
        makeDirs(dir);
        const std::string tmp = dir + "/audio.state.tmp";
        {
            std::ofstream f(tmp, std::ios::trunc);
            f << sa::serialize(params_) << '\n';
        }
        rename(tmp.c_str(), (dir + "/audio.state").c_str());
    }

    void pushParams() {
        std::lock_guard<std::mutex> lock(paramLock_);
        pending_ = params_;
        hasPending_.store(true, std::memory_order_release);
    }

    bool openSocket(pw_loop* loop) {
        const std::string path = socketPath();
        unlink(path.c_str());
        listenFd_ = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
        sockaddr_un addr{};
        addr.sun_family = AF_UNIX;
        std::snprintf(addr.sun_path, sizeof addr.sun_path, "%s", path.c_str());
        if (listenFd_ < 0 || bind(listenFd_, reinterpret_cast<sockaddr*>(&addr), sizeof addr) != 0 ||
            listen(listenFd_, 8) != 0) {
            std::fprintf(stderr, "serpantinum-audio: cannot bind %s\n", path.c_str());
            return false;
        }
        chmod(path.c_str(), 0600);
        socketSource_ = pw_loop_add_io(loop, listenFd_, SPA_IO_IN, false, onSocket, this);
        return true;
    }

    static void onSocket(void* data, int fd, uint32_t) { static_cast<Daemon*>(data)->serve(fd); }

    void serve(int listenFd) {
        const int fd = accept4(listenFd, nullptr, nullptr, SOCK_CLOEXEC);
        if (fd < 0) return;
        pollfd pfd{fd, POLLIN, 0};
        std::string line;
        if (poll(&pfd, 1, 200) > 0) {
            char buf[512];
            const ssize_t n = read(fd, buf, sizeof buf - 1);
            if (n > 0) line.assign(buf, n);
        }
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r' || line.back() == ' ')) line.pop_back();
        const std::string reply = handle(line) + "\n";
        if (write(fd, reply.data(), reply.size()) < 0) {}
        close(fd);
    }

    std::string handle(const std::string& line) {
        if (line == "ping") return "ok";
        if (line == "quit") {
            pw_main_loop_quit(loop_);
            return "ok";
        }
        if (line == "status") {
            unsigned routed = 0;
            for (const auto& entry : streams_)
                if (entry.second.routed) ++routed;
            return "ok sink=" + std::string(kSinkName) + " target=" + (target_.empty() ? "-" : target_) +
                   " streams=" + std::to_string(routed) + " underruns=" + std::to_string(underruns_.load()) + " " +
                   sa::serialize(params_);
        }
        if (line == "get") return sa::serialize(params_);
        if (!sa::applyCommand(params_, line)) return "error";
        pushParams();
        saveState();
        return "ok";
    }

    static void onSinkProcess(void* data) { static_cast<Daemon*>(data)->captureCycle(); }
    static void onOutProcess(void* data) { static_cast<Daemon*>(data)->playbackCycle(); }

    static void onStreamState(void* data, pw_stream_state, pw_stream_state state, const char* error) {
        (void)data;
        if (state == PW_STREAM_STATE_ERROR) std::fprintf(stderr, "serpantinum-audio: stream error: %s\n", error);
    }

    void captureCycle() {
        pw_buffer* b = pw_stream_dequeue_buffer(sink_);
        if (!b) return;
        spa_data& d = b->buffer->datas[0];
        if (d.data && d.chunk) {
            const uint32_t frames = d.chunk->size / kStride;
            const bool empty = d.chunk->flags & SPA_CHUNK_FLAG_EMPTY;
            const float* src = reinterpret_cast<const float*>(static_cast<const uint8_t*>(d.data) + d.chunk->offset);
            if (ring_.push(empty ? nullptr : src, frames) < frames) overruns_.fetch_add(1, std::memory_order_relaxed);
        }
        pw_stream_queue_buffer(sink_, b);
    }

    void playbackCycle() {
        pw_buffer* b = pw_stream_dequeue_buffer(out_);
        if (!b) return;
        spa_data& d = b->buffer->datas[0];
        if (!d.data) {
            pw_stream_queue_buffer(out_, b);
            return;
        }
        uint32_t frames = d.maxsize / kStride;
        if (b->requested && b->requested < frames) frames = static_cast<uint32_t>(b->requested);
        float* dst = static_cast<float*>(d.data);

        if (!primed_ && ring_.available() >= frames) primed_ = true;
        size_t got = primed_ ? ring_.pop(dst, frames) : 0;
        if (primed_ && got < frames) underruns_.fetch_add(1, std::memory_order_relaxed);
        if (got < frames) std::memset(dst + got * 2, 0, (frames - got) * kStride);

        if (hasPending_.load(std::memory_order_acquire) && paramLock_.try_lock()) {
            chain_.setParams(pending_);
            hasPending_.store(false, std::memory_order_release);
            paramLock_.unlock();
        }
        chain_.process(dst, frames);

        d.chunk->offset = 0;
        d.chunk->stride = kStride;
        d.chunk->size = frames * kStride;
        pw_stream_queue_buffer(out_, b);
    }

    pw_properties* streamProps(const char* name, const char* description) {
        pw_properties* p = pw_properties_new(PW_KEY_NODE_NAME, name, PW_KEY_NODE_DESCRIPTION, description,
                                             PW_KEY_NODE_GROUP, "serpantinum-audio", PW_KEY_NODE_LINK_GROUP,
                                             "serpantinum-audio", PW_KEY_AUDIO_CHANNELS, "2", PW_KEY_AUDIO_RATE, "48000",
                                             "audio.position", "[ FL FR ]", PW_KEY_APP_NAME, "Serpantinum audio",
                                             nullptr);
        return p;
    }

    bool connectStream(pw_stream* stream, pw_direction dir) {
        uint8_t buffer[1024];
        spa_pod_builder b = SPA_POD_BUILDER_INIT(buffer, sizeof buffer);
        spa_audio_info_raw info{};
        info.format = SPA_AUDIO_FORMAT_F32;
        info.rate = kRate;
        info.channels = kChannels;
        info.position[0] = SPA_AUDIO_CHANNEL_FL;
        info.position[1] = SPA_AUDIO_CHANNEL_FR;
        const spa_pod* params[1] = {spa_format_audio_raw_build(&b, SPA_PARAM_EnumFormat, &info)};
        return pw_stream_connect(stream, dir, PW_ID_ANY,
                                 static_cast<pw_stream_flags>(PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS |
                                                              PW_STREAM_FLAG_RT_PROCESS),
                                 params, 1) == 0;
    }

    void createSink() {
        pw_properties* props = streamProps(kSinkName, "Serpantinum");
        pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Audio/Sink");
        pw_properties_set(props, PW_KEY_NODE_VIRTUAL, "true");
        pw_properties_set(props, PW_KEY_MEDIA_NAME, "Serpantinum sink");
        sinkEvents_ = {};
        sinkEvents_.version = PW_VERSION_STREAM_EVENTS;
        sinkEvents_.process = onSinkProcess;
        sinkEvents_.state_changed = onStreamState;
        sink_ = pw_stream_new(core_, kSinkName, props);
        pw_stream_add_listener(sink_, &sinkListener_, &sinkEvents_, this);
        if (!connectStream(sink_, PW_DIRECTION_INPUT)) std::fprintf(stderr, "serpantinum-audio: sink connect failed\n");
    }

    void createOutput(const std::string& target) {
        destroyOutput();
        ring_.clear();
        primed_ = false;
        pw_properties* props = streamProps(kOutputName, "Serpantinum output");
        pw_properties_set(props, PW_KEY_MEDIA_CLASS, "Stream/Output/Audio");
        pw_properties_set(props, PW_KEY_MEDIA_TYPE, "Audio");
        pw_properties_set(props, PW_KEY_MEDIA_CATEGORY, "Playback");
        pw_properties_set(props, PW_KEY_MEDIA_ROLE, "Music");
        pw_properties_set(props, PW_KEY_MEDIA_NAME, "Serpantinum output");
        pw_properties_set(props, "target.object", target.c_str());
        pw_properties_set(props, "node.dont-reconnect", "true");
        pw_properties_set(props, "stream.dont-remix", "true");
        outEvents_ = {};
        outEvents_.version = PW_VERSION_STREAM_EVENTS;
        outEvents_.process = onOutProcess;
        outEvents_.state_changed = onStreamState;
        out_ = pw_stream_new(core_, kOutputName, props);
        pw_stream_add_listener(out_, &outListener_, &outEvents_, this);
        if (!connectStream(out_, PW_DIRECTION_OUTPUT)) std::fprintf(stderr, "serpantinum-audio: output connect failed\n");
        target_ = target;
        publishTarget();
    }

    void destroyOutput() {
        if (!out_) return;
        spa_hook_remove(&outListener_);
        pw_stream_destroy(out_);
        out_ = nullptr;
        target_.clear();
        publishTarget();
    }

    void publishTarget() {
        const std::string dir = runtimeDir();
        makeDirs(dir);
        const std::string tmp = dir + "/audio-target.tmp";
        uint32_t id = 0;
        for (const auto& entry : sinkNodes_)
            if (entry.second == target_) id = entry.first;
        {
            std::ofstream f(tmp, std::ios::trunc);
            f << target_ << '\n';
            if (id) f << id << '\n';
        }
        rename(tmp.c_str(), (dir + "/audio-target").c_str());
    }

    static void onGlobal(void* data, uint32_t id, uint32_t, const char* type, uint32_t, const spa_dict* props) {
        Daemon* self = static_cast<Daemon*>(data);
        if (!props) return;
        if (std::strcmp(type, PW_TYPE_INTERFACE_Metadata) == 0) {
            const char* name = spa_dict_lookup(props, PW_KEY_METADATA_NAME);
            if (name && std::strcmp(name, "default") == 0 && !self->metadata_) {
                self->metadata_ = static_cast<pw_metadata*>(
                    pw_registry_bind(self->registry_, id, type, PW_VERSION_METADATA, 0));
                pw_metadata_add_listener(self->metadata_, &self->metadataListener_, &metadataEventsTable(), self);
                if (self->synced_) {
                    self->synced_ = false;
                    self->syncRounds_ = 1;
                    self->syncSeq_ = pw_core_sync(self->core_, PW_ID_CORE, 0);
                }
            }
        } else if (std::strcmp(type, PW_TYPE_INTERFACE_Node) == 0) {
            const char* cls = spa_dict_lookup(props, PW_KEY_MEDIA_CLASS);
            const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
            const char* serial = spa_dict_lookup(props, PW_KEY_OBJECT_SERIAL);
            if (!cls || !name) return;
            if (std::strcmp(cls, "Audio/Sink") == 0) {
                if (std::strcmp(name, kSinkName) == 0) {
                    self->ownId_ = id;
                    self->ownSerial_ = serial ? serial : "";
                    return;
                }
                self->sinkNodes_[id] = name;
                if (serial) self->sinkSerials_[id] = serial;
                if (self->target_.empty() || self->defaultName_ == name) self->adoptTarget(name);
            } else if (std::strcmp(cls, "Stream/Output/Audio") == 0 && std::strcmp(name, kOutputName) != 0) {
                self->probe(id, type);
            }
        }
    }

    struct Probe {
        Daemon* self = nullptr;
        uint32_t id = 0;
        pw_proxy* proxy = nullptr;
        spa_hook listener{};
        bool decided = false;
    };

    void probe(uint32_t id, const char* type) {
        auto p = std::make_unique<Probe>();
        p->self = this;
        p->id = id;
        p->proxy = static_cast<pw_proxy*>(pw_registry_bind(registry_, id, type, PW_VERSION_NODE, 0));
        if (!p->proxy) return;
        pw_node_add_listener(reinterpret_cast<pw_node*>(p->proxy), &p->listener, &nodeEventsTable(), p.get());
        probes_[id] = std::move(p);
    }

    void dropProbe(uint32_t id) {
        auto it = probes_.find(id);
        if (it == probes_.end()) return;
        spa_hook_remove(&it->second->listener);
        pw_proxy_destroy(it->second->proxy);
        probes_.erase(it);
    }

    static void onNodeInfo(void* data, const pw_node_info* info) {
        Probe* p = static_cast<Probe*>(data);
        if (p->decided || !info || !info->props) return;
        p->decided = true;
        Daemon* self = p->self;
        if (!routable(info->props)) return;
        self->streams_[p->id] = {};
        if (self->synced_) self->route(p->id);
    }

    static bool isTrue(const char* value) { return value && (std::strcmp(value, "true") == 0 || std::strcmp(value, "1") == 0); }

    static bool routable(const spa_dict* props) {
        const char* name = spa_dict_lookup(props, PW_KEY_NODE_NAME);
        if (name && std::strcmp(name, kOutputName) == 0) return false;
        if (spa_dict_lookup(props, PW_KEY_NODE_LINK_GROUP)) return false;
        if (spa_dict_lookup(props, kTargetObjectKey) || spa_dict_lookup(props, kTargetNodeKey) ||
            spa_dict_lookup(props, "node.target"))
            return false;
        const char* autoconnect = spa_dict_lookup(props, PW_KEY_NODE_AUTOCONNECT);
        if (autoconnect && !isTrue(autoconnect)) return false;
        return !isTrue(spa_dict_lookup(props, "node.dont-move"));
    }

    bool isOwnTarget(const std::string& value) const {
        if (value.empty()) return false;
        return value == kSinkName || (!ownSerial_.empty() && value == ownSerial_) ||
               (ownId_ != SPA_ID_INVALID && value == std::to_string(ownId_));
    }

    bool isOtherSink(const std::string& value) const {
        if (value.empty()) return false;
        for (const auto& entry : sinkNodes_)
            if (entry.second == value || std::to_string(entry.first) == value) return true;
        for (const auto& entry : sinkSerials_)
            if (entry.second == value) return true;
        return false;
    }

    void route(uint32_t id) {
        auto it = streams_.find(id);
        if (it == streams_.end() || !metadata_) return;
        StreamState& s = it->second;
        if (s.routed || s.pinned) return;
        const auto pinned = streamTargets_.find(id);
        if (pinned != streamTargets_.end() && isOtherSink(pinned->second)) {
            s.pinned = true;
            return;
        }
        s.routed = true;
        pw_metadata_set_property(metadata_, id, kTargetObjectKey, "Spa:String", kSinkName);
    }

    void unrouteAll() {
        if (!metadata_) return;
        for (auto& entry : streams_) {
            if (!entry.second.routed) continue;
            pw_metadata_set_property(metadata_, entry.first, kTargetObjectKey, nullptr, nullptr);
            entry.second.routed = false;
        }
    }

    void onStreamTarget(uint32_t subject, const char* value) {
        const std::string v = value ? value : "";
        if (v.empty()) {
            streamTargets_.erase(subject);
        } else {
            streamTargets_[subject] = v;
        }
        auto it = streams_.find(subject);
        if (it == streams_.end()) return;
        StreamState& s = it->second;
        if (isOwnTarget(v)) {
            s.routed = true;
            s.pinned = false;
        } else if (isOtherSink(v)) {
            s.routed = false;
            s.pinned = true;
        } else if (v.empty() && (s.routed || s.pinned)) {
            s.routed = false;
            s.pinned = false;
            if (synced_) route(subject);
        }
    }

    static void onGlobalRemove(void* data, uint32_t id) {
        Daemon* self = static_cast<Daemon*>(data);
        self->dropProbe(id);
        self->streams_.erase(id);
        self->streamTargets_.erase(id);
        self->sinkSerials_.erase(id);
        if (id == self->ownId_) {
            self->ownId_ = SPA_ID_INVALID;
            self->ownSerial_.clear();
        }
        auto it = self->sinkNodes_.find(id);
        if (it == self->sinkNodes_.end()) return;
        const std::string gone = it->second;
        self->sinkNodes_.erase(it);
        if (gone != self->target_) return;
        self->destroyOutput();
        if (self->sinkNodes_.empty()) return;
        self->adoptTarget(self->hasSink(self->defaultName_) ? self->defaultName_ : self->sinkNodes_.begin()->second);
    }

    static int onMetadataProperty(void* data, uint32_t subject, const char* key, const char*, const char* value) {
        Daemon* self = static_cast<Daemon*>(data);
        if (!key) {
            if (subject != PW_ID_CORE) self->onStreamTarget(subject, nullptr);
            return 0;
        }
        if (subject != PW_ID_CORE) {
            if (std::strcmp(key, kTargetObjectKey) == 0 || std::strcmp(key, kTargetNodeKey) == 0)
                self->onStreamTarget(subject, value);
            return 0;
        }
        if (std::strcmp(key, kConfiguredKey) == 0) {
            self->configuredName_ = jsonName(value);
        } else if (std::strcmp(key, kResolvedKey) == 0) {
            const std::string name = jsonName(value);
            if (name == kSinkName) return 0;
            self->defaultName_ = name;
            if (!name.empty() && self->hasSink(name)) self->adoptTarget(name);
        }
        return 0;
    }

    bool hasSink(const std::string& name) const {
        for (const auto& entry : sinkNodes_)
            if (entry.second == name) return true;
        return false;
    }

    void adoptTarget(const std::string& name) {
        if (name != target_ || !out_) createOutput(name);
    }

    void shutdown() {
        if (metadata_) {
            unrouteAll();
            shutdownDone_ = false;
            shutdownSeq_ = pw_core_sync(core_, PW_ID_CORE, 0);
            for (int i = 0; i < 20 && !shutdownDone_; ++i) pw_loop_iterate(pw_main_loop_get_loop(loop_), 50);
        }
        while (!probes_.empty()) dropProbe(probes_.begin()->first);
        destroyOutput();
        if (sink_) {
            spa_hook_remove(&sinkListener_);
            pw_stream_destroy(sink_);
        }
        if (metadata_) pw_proxy_destroy(reinterpret_cast<pw_proxy*>(metadata_));
        if (registry_) pw_proxy_destroy(reinterpret_cast<pw_proxy*>(registry_));
        if (socketSource_) pw_loop_destroy_source(pw_main_loop_get_loop(loop_), socketSource_);
        if (listenFd_ >= 0) close(listenFd_);
        unlink(socketPath().c_str());
        unlink((runtimeDir() + "/audio-target").c_str());
        if (core_) pw_core_disconnect(core_);
        if (context_) pw_context_destroy(context_);
        pw_main_loop_destroy(loop_);
        pw_deinit();
    }

    static const pw_core_events& coreEventsTable() {
        static const pw_core_events t = [] {
            pw_core_events e{};
            e.version = PW_VERSION_CORE_EVENTS;
            e.done = onCoreDone;
            e.error = onCoreError;
            return e;
        }();
        return t;
    }

    static const pw_registry_events& registryEventsTable() {
        static const pw_registry_events t = [] {
            pw_registry_events e{};
            e.version = PW_VERSION_REGISTRY_EVENTS;
            e.global = onGlobal;
            e.global_remove = onGlobalRemove;
            return e;
        }();
        return t;
    }

    static const pw_node_events& nodeEventsTable() {
        static const pw_node_events t = [] {
            pw_node_events e{};
            e.version = PW_VERSION_NODE_EVENTS;
            e.info = onNodeInfo;
            return e;
        }();
        return t;
    }

    static const pw_metadata_events& metadataEventsTable() {
        static const pw_metadata_events t = [] {
            pw_metadata_events e{};
            e.version = PW_VERSION_METADATA_EVENTS;
            e.property = onMetadataProperty;
            return e;
        }();
        return t;
    }

    pw_main_loop* loop_ = nullptr;
    pw_context* context_ = nullptr;
    pw_core* core_ = nullptr;
    pw_registry* registry_ = nullptr;
    pw_metadata* metadata_ = nullptr;
    pw_stream* sink_ = nullptr;
    pw_stream* out_ = nullptr;
    spa_hook coreListener_{}, registryListener_{}, metadataListener_{}, sinkListener_{}, outListener_{};
    pw_stream_events sinkEvents_{}, outEvents_{};
    spa_source* socketSource_ = nullptr;
    int listenFd_ = -1;

    struct StreamState {
        bool routed = false;
        bool pinned = false;
    };

    std::map<uint32_t, std::string> sinkNodes_;
    std::map<uint32_t, std::string> sinkSerials_;
    std::map<uint32_t, StreamState> streams_;
    std::map<uint32_t, std::unique_ptr<Probe>> probes_;
    std::map<uint32_t, std::string> streamTargets_;
    std::string target_;
    std::string defaultName_;
    std::string configuredName_;
    uint32_t ownId_ = SPA_ID_INVALID;
    std::string ownSerial_;
    int syncSeq_ = -1;
    int syncRounds_ = 0;
    int shutdownSeq_ = -1;
    bool synced_ = false;
    bool shutdownDone_ = false;

    sa::FrameRing ring_;
    sa::Chain chain_;
    sa::Params params_;
    sa::Params pending_;
    std::mutex paramLock_;
    std::atomic<bool> hasPending_{false};
    std::atomic<unsigned> underruns_{0}, overruns_{0};
    bool primed_ = false;
};

int startDetached(const char* self) {
    std::string reply;
    if (request("ping", reply)) return 0;

    const pid_t pid = fork();
    if (pid < 0) return 1;
    if (pid == 0) {
        setsid();
        const std::string dir = stateDir();
        makeDirs(dir);
        const int log = open((dir + "/audio.log").c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
        const int null = open("/dev/null", O_RDONLY | O_CLOEXEC);
        if (null >= 0) dup2(null, STDIN_FILENO);
        if (log >= 0) {
            dup2(log, STDOUT_FILENO);
            dup2(log, STDERR_FILENO);
        }
        execl(self, self, "daemon", static_cast<char*>(nullptr));
        _exit(127);
    }

    for (int i = 0; i < 60; ++i) {
        usleep(50000);
        if (request("ping", reply)) return 0;
    }
    return 1;
}

}

int main(int argc, char** argv) {
    const std::string cmd = argc > 1 ? argv[1] : "";

    if (cmd == "version" || cmd == "--version") {
        std::printf("serpantinum-audio %s\n", SA_VERSION);
        return 0;
    }

    if (cmd == "daemon") return Daemon().run();

    if (cmd == "start") {
        char self[4096];
        const ssize_t n = readlink("/proc/self/exe", self, sizeof self - 1);
        if (n <= 0) return 1;
        self[n] = '\0';
        return startDetached(self);
    }

    if (cmd == "ctl" && argc > 2) {
        std::string line;
        for (int i = 2; i < argc; ++i) line += (i > 2 ? " " : "") + std::string(argv[i]);
        std::string reply;
        if (!request(line, reply)) {
            std::fprintf(stderr, "serpantinum-audio: daemon is not running\n");
            return 2;
        }
        std::printf("%s\n", reply.c_str());
        return reply.rfind("ok", 0) == 0 || reply.rfind("set", 0) == 0 ? 0 : 1;
    }

    std::fprintf(stderr, "usage: serpantinum-audio daemon | start | ctl <command...> | version\n");
    return 64;
}
