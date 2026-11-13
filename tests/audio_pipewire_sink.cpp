/* Test-only PipeWire sink and small linking policy. It connects solely to the
 * private daemon launched by run_pipewire_test.py; no sound card is needed. */
#include <pipewire/pipewire.h>
#include <pipewire/extensions/metadata.h>
#include <spa/param/audio/format-utils.h>
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <string>
#include <unistd.h>

struct fixture;
struct sink {
    fixture *owner;
    pw_stream *stream = nullptr;
    unsigned index;
    std::uint64_t hal_frames = 0, unit_frames = 0, unexpected = 0;
};
struct node {
    fixture *owner = nullptr;
    std::uint32_t id = 0;
    std::string name, serial, target;
    pw_node *proxy = nullptr;
    spa_hook listener{};
    bool properties_known = false;
};
struct port { std::uint32_t node; bool output; std::string channel; };
struct fixture {
    pw_main_loop *loop;
    pw_context *context;
    pw_core *core;
    pw_registry *registry;
    pw_metadata *metadata = nullptr;
    spa_hook registry_listener{}, core_listener{};
    spa_source *input = nullptr;
    std::array<sink, 3> sinks;
    std::map<std::uint32_t, node> nodes;
    std::map<std::uint32_t, port> ports;
    std::map<std::pair<std::uint32_t, std::uint32_t>, pw_proxy *> links;
    std::string commands;
    unsigned default_sink = 0;
    bool ready = false, failed = false;
    static constexpr const char *names[] = {"machoman-test-sink", "machoman-test-sink-2", "machoman-test-sink-3"};
    const char *prop(const spa_dict *props, const char *key) {
        const char *value = props ? spa_dict_lookup(props, key) : nullptr;
        return value ? value : "";
    }
    static void on_node_info(void *opaque, const pw_node_info *info) {
        auto &node = *static_cast<struct node *>(opaque);
        auto &self = *node.owner;
        if (info->props) {
            // Node events carry property deltas; an absent key preserves the
            // previous value. Wait for the initial properties before linking.
            if (const auto *target = spa_dict_lookup(info->props, PW_KEY_TARGET_OBJECT)) node.target = target;
            node.properties_known = true;
        }
        self.relink();
    }
    std::uint32_t destination(const node &source) {
        for (const auto &[id, node] : nodes) {
            if (source.target.empty()) {
                if (node.name == names[default_sink]) return id;
            } else if (source.target == node.serial || source.target == node.name) return id;
        }
        return PW_ID_ANY;
    }
    void relink() {
        for (auto it = links.begin(); it != links.end();) {
            const auto out = ports.find(it->first.first), in = ports.find(it->first.second);
            const bool valid = out != ports.end() && in != ports.end() && nodes.contains(out->second.node) &&
                destination(nodes[out->second.node]) == in->second.node;
            if (!valid) { pw_proxy_destroy(it->second); it = links.erase(it); } else ++it;
        }
        for (const auto &[out_id, out] : ports) {
            const auto source = nodes.find(out.node);
            if (!out.output || source == nodes.end() || source->second.name != "machoman-output" ||
                !source->second.properties_known) continue;
            const auto target = destination(source->second);
            for (const auto &[in_id, in] : ports) {
                if (in.output || in.node != target || in.channel != out.channel || links.contains({out_id, in_id})) continue;
                auto *props = pw_properties_new(nullptr, nullptr);
                pw_properties_setf(props, PW_KEY_LINK_OUTPUT_NODE, "%u", out.node);
                pw_properties_setf(props, PW_KEY_LINK_OUTPUT_PORT, "%u", out_id);
                pw_properties_setf(props, PW_KEY_LINK_INPUT_NODE, "%u", in.node);
                pw_properties_setf(props, PW_KEY_LINK_INPUT_PORT, "%u", in_id);
                auto *link = static_cast<pw_proxy *>(pw_core_create_object(core, "link-factory", PW_TYPE_INTERFACE_Link,
                    PW_VERSION_LINK, &props->dict, 0));
                pw_properties_free(props);
                if (!link) failed = true; else links[{out_id, in_id}] = link;
                if (std::getenv("MACHOMAN_AUDIO_TRACE")) std::fprintf(stderr, "fixture link %u:%u -> %u:%u (%s)\n",
                    out.node, out_id, in.node, in_id, out.channel.c_str());
            }
        }
    }
    void set_default(unsigned index) {
        default_sink = index;
        if (metadata) {
            const auto value = std::string("{\"name\":\"") + names[index] + "\"}";
            pw_metadata_set_property(metadata, PW_ID_CORE, "default.audio.sink", "Spa:String:JSON", value.c_str());
        }
        relink();
    }
    void maybe_ready() {
        if (!ready && metadata && std::count_if(nodes.begin(), nodes.end(), [](const auto &n) {
                return n.second.name == names[0] || n.second.name == names[1] || n.second.name == names[2];
            }) == 3) {
            set_default(0); ready = true; std::puts("ready"); std::fflush(stdout);
        }
    }
    static void on_global(void *opaque, std::uint32_t id, std::uint32_t, const char *type, std::uint32_t,
                          const spa_dict *props) {
        auto &self = *static_cast<fixture *>(opaque);
        if (!std::strcmp(type, PW_TYPE_INTERFACE_Metadata) && !std::strcmp(self.prop(props, PW_KEY_METADATA_NAME), "default"))
            self.metadata = static_cast<pw_metadata *>(pw_registry_bind(self.registry, id, PW_TYPE_INTERFACE_Metadata, PW_VERSION_METADATA, 0));
        if (!std::strcmp(type, PW_TYPE_INTERFACE_Node)) {
            const std::string name = self.prop(props, PW_KEY_NODE_NAME);
            if (name == names[0] || name == names[1] || name == names[2] || name == "machoman-output") {
                auto &node = self.nodes[id];
                node.owner = &self; node.id = id;
                node.name = name; node.serial = self.prop(props, PW_KEY_OBJECT_SERIAL); node.target = self.prop(props, PW_KEY_TARGET_OBJECT);
                if (std::getenv("MACHOMAN_AUDIO_TRACE")) std::fprintf(stderr, "fixture node %u %s serial=%s target=%s\n", id,
                    name.c_str(), node.serial.c_str(), node.target.c_str());
                node.proxy = static_cast<pw_node *>(pw_registry_bind(self.registry, id, PW_TYPE_INTERFACE_Node, PW_VERSION_NODE, 0));
                if (node.proxy) {
                    static const pw_node_events events = [] { pw_node_events e{}; e.version = PW_VERSION_NODE_EVENTS; e.info = on_node_info; return e; }();
                    pw_node_add_listener(node.proxy, &node.listener, &events, &node);
                } else self.failed = true;
            }
        }
        if (!std::strcmp(type, PW_TYPE_INTERFACE_Port)) {
            const auto owner = static_cast<std::uint32_t>(std::strtoul(self.prop(props, PW_KEY_NODE_ID), nullptr, 10));
            self.ports[id] = {owner, !std::strcmp(self.prop(props, PW_KEY_PORT_DIRECTION), "out"), self.prop(props, PW_KEY_AUDIO_CHANNEL)};
            if (std::getenv("MACHOMAN_AUDIO_TRACE")) std::fprintf(stderr, "fixture port %u node=%u direction=%s channel=%s\n",
                id, owner, self.prop(props, PW_KEY_PORT_DIRECTION), self.prop(props, PW_KEY_AUDIO_CHANNEL));
        }
        self.relink(); self.maybe_ready();
    }
    static void on_remove(void *opaque, std::uint32_t id) {
        auto &self = *static_cast<fixture *>(opaque);
        if (auto found = self.nodes.find(id); found != self.nodes.end()) {
            if (found->second.proxy) pw_proxy_destroy(reinterpret_cast<pw_proxy *>(found->second.proxy));
            self.nodes.erase(found);
        }
        self.ports.erase(id); self.relink();
    }
    static void on_error(void *opaque, std::uint32_t id, int, int result, const char *message) {
        auto &self = *static_cast<fixture *>(opaque);
        std::fprintf(stderr, "PipeWire fixture error %u: %d %s\n", id, result, message);
        self.failed = true;
        if (id == PW_ID_CORE) pw_main_loop_quit(self.loop);
    }
    static void capture(void *opaque) {
        auto &sink = *static_cast<struct sink *>(opaque);
        auto *queued = pw_stream_dequeue_buffer(sink.stream);
        if (!queued) return;
        const auto *buffer = queued->buffer;
        if (buffer && buffer->n_datas == 1 && buffer->datas[0].data && buffer->datas[0].chunk) {
            const auto &data = buffer->datas[0];
            const auto offset = std::min(data.chunk->offset, data.maxsize);
            const auto size = std::min(data.chunk->size, data.maxsize - offset);
            const auto *samples = reinterpret_cast<const float *>(static_cast<const std::byte *>(data.data) + offset);
            auto near = [](float a, float b) { return std::abs(a - b) < 1e-6f; };
            for (std::uint32_t i = 0; i + 1 < size / sizeof(float); i += 2) {
                if (near(samples[i], 0.125f) && near(samples[i + 1], -0.25f)) ++sink.hal_frames;
                else if (near(samples[i], 0.25f) && near(samples[i + 1], -0.125f)) ++sink.unit_frames;
                else if (!near(samples[i], 0) || !near(samples[i + 1], 0)) ++sink.unexpected;
            }
        }
        pw_stream_queue_buffer(sink.stream, queued);
    }
    static void on_input(void *opaque, int fd, std::uint32_t) {
        auto &self = *static_cast<fixture *>(opaque);
        char buffer[128]; const auto size = read(fd, buffer, sizeof buffer);
        if (size <= 0) { pw_main_loop_quit(self.loop); return; }
        self.commands.append(buffer, static_cast<std::size_t>(size));
        for (auto newline = self.commands.find('\n'); newline != std::string::npos; newline = self.commands.find('\n')) {
            const auto command = self.commands.substr(0, newline); self.commands.erase(0, newline + 1);
            if (command == "default") self.set_default(1);
            if (command == "remove") {
                self.set_default(0);
                if (self.sinks[1].stream) { pw_stream_destroy(self.sinks[1].stream); self.sinks[1].stream = nullptr; }
            }
            if (command == "disconnect" && self.sinks[2].stream) {
                pw_stream_destroy(self.sinks[2].stream); self.sinks[2].stream = nullptr;
            }
            if (command == "quit") pw_main_loop_quit(self.loop);
        }
    }
    int run() {
        static const pw_core_events core_events = [] { pw_core_events e{}; e.version = PW_VERSION_CORE_EVENTS; e.error = on_error; return e; }();
        pw_core_add_listener(core, &core_listener, &core_events, this);
        registry = pw_core_get_registry(core, PW_VERSION_REGISTRY, 0);
        static const pw_registry_events registry_events{PW_VERSION_REGISTRY_EVENTS, on_global, on_remove};
        pw_registry_add_listener(registry, &registry_listener, &registry_events, this);
        static const pw_stream_events stream_events = [] { pw_stream_events e{}; e.version = PW_VERSION_STREAM_EVENTS; e.process = capture; return e; }();
        for (unsigned i = 0; i < sinks.size(); ++i) {
            sinks[i] = {this, nullptr, i};
            auto *props = pw_properties_new(PW_KEY_MEDIA_CLASS, "Audio/Sink", PW_KEY_NODE_NAME, names[i],
                PW_KEY_NODE_DESCRIPTION, names[i], PW_KEY_AUDIO_CHANNELS, "2",
                PW_KEY_NODE_ALWAYS_PROCESS, "true", PW_KEY_NODE_WANT_DRIVER, "true", PW_KEY_NODE_VIRTUAL, "true",
                "adapter.auto-port-config", "{ mode = dsp position = preserve }", nullptr);
            sinks[i].stream = pw_stream_new(core, names[i], props);
            if (!sinks[i].stream) return 1;
            // One listener per stream must remain alive until destruction.
            pw_stream_add_listener(sinks[i].stream, &sink_listeners[i], &stream_events, &sinks[i]);
            std::uint8_t storage[512]; auto builder = SPA_POD_BUILDER_INIT(storage, sizeof storage);
            spa_audio_info_raw format{}; format.format = SPA_AUDIO_FORMAT_F32; format.rate = 48000; format.channels = 2;
            format.position[0] = SPA_AUDIO_CHANNEL_FL; format.position[1] = SPA_AUDIO_CHANNEL_FR;
            const spa_pod *params[] = {spa_format_audio_raw_build(&builder, SPA_PARAM_EnumFormat, &format)};
            if (pw_stream_connect(sinks[i].stream, PW_DIRECTION_INPUT, PW_ID_ANY, PW_STREAM_FLAG_MAP_BUFFERS, params, 1) < 0) return 1;
        }
        input = pw_loop_add_io(pw_main_loop_get_loop(loop), STDIN_FILENO, SPA_IO_IN | SPA_IO_HUP, false, on_input, this);
        pw_main_loop_run(loop);
        std::printf("captured: HAL=%llu AU=%llu moved-AU=%llu selected-AU=%llu unexpected=%llu\n",
            (unsigned long long)sinks[0].hal_frames, (unsigned long long)sinks[0].unit_frames,
            (unsigned long long)sinks[1].unit_frames, (unsigned long long)sinks[2].unit_frames,
            (unsigned long long)(sinks[0].unexpected + sinks[1].unexpected + sinks[2].unexpected));
        return failed || sinks[0].hal_frames < 512 || sinks[0].unit_frames < 512 || sinks[1].unit_frames < 512 ||
            sinks[2].unit_frames < 512 || sinks[0].unexpected + sinks[1].unexpected + sinks[2].unexpected != 0 ? 1 : 0;
    }
    std::array<spa_hook, 3> sink_listeners{};
};
int main() {
    pw_init(nullptr, nullptr);
    fixture state{};
    state.loop = pw_main_loop_new(nullptr);
    if (!state.loop) return 1;
    state.context = pw_context_new(pw_main_loop_get_loop(state.loop), nullptr, 0);
    if (!state.context) return 1;
    state.core = pw_context_connect(state.context, nullptr, 0);
    if (!state.core) { std::perror("PipeWire fixture connect"); return 1; }
    const int result = state.run();
    if (state.input) pw_loop_destroy_source(pw_main_loop_get_loop(state.loop), state.input);
    for (const auto &[key, link] : state.links) pw_proxy_destroy(link);
    for (auto &sink : state.sinks) if (sink.stream) pw_stream_destroy(sink.stream);
    for (const auto &[id, node] : state.nodes) if (node.proxy) pw_proxy_destroy(reinterpret_cast<pw_proxy *>(node.proxy));
    if (state.metadata) pw_proxy_destroy(reinterpret_cast<pw_proxy *>(state.metadata));
    pw_proxy_destroy(reinterpret_cast<pw_proxy *>(state.registry));
    pw_core_disconnect(state.core); pw_context_destroy(state.context); pw_main_loop_destroy(state.loop);
    return result;
}
