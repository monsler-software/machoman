#pragma once

#include "mevk/render.h"
#include <cstdint>
#include <memory>
#include <string>

namespace mevk {

struct PresentationStats { std::uint64_t presented = 0, skipped = 0, recreations = 0; };
struct PresentationRegion {
    std::int32_t sx0 = 0, sy0 = 0, sx1 = 0, sy1 = 0;
    std::int32_t dx0 = 0, dy0 = 0, dx1 = 0, dy1 = 0;
    bool empty = true;
};
// Placement numbers match NSViewLayerContentsPlacement. Both coordinate
// systems have their origin at the top left; output is clipped to the layer.
PresentationRegion presentation_region(unsigned width, unsigned height,
    unsigned out_width, unsigned out_height, double logical_width,
    double logical_height, double contents_scale, unsigned placement);

// Owns only Vulkan objects. AppKit owns the borrowed Wayland display/surface
// and must destroy this presenter before destroying either native object.
class WaylandPresenter final {
public:
    static std::unique_ptr<WaylandPresenter> create(const std::shared_ptr<Texture> &texture,
        void *display, void *surface, std::string &error);
    ~WaylandPresenter();
    WaylandPresenter(const WaylandPresenter &) = delete;
    WaylandPresenter &operator=(const WaylandPresenter &) = delete;
    // Queues a GPU blit and presentation. Busy slots/acquisition skip a frame
    // rather than blocking the UI. GPU resources stay retained through fences.
    bool present(const std::shared_ptr<Texture> &texture, unsigned width, unsigned height,
        double logical_width, double logical_height, double contents_scale,
        unsigned placement, std::string &error);
    PresentationStats stats() const;
private:
    struct Impl;
    explicit WaylandPresenter(std::unique_ptr<Impl> impl);
    std::unique_ptr<Impl> impl_;
};
} // namespace mevk
