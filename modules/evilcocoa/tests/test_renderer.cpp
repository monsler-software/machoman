#include <array>
#include <cstdio>
#include <cstdlib>
#include <epoxy/egl.h>
#include <epoxy/gl.h>
#include <evilcocoa/renderer.h>
#include <vector>
void require(bool value, const char *message) {
    if (!value) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        std::exit(1);
    }
}
int main() {
    EGLDisplay display = eglGetDisplay(EGL_DEFAULT_DISPLAY);
    require(eglInitialize(display, nullptr, nullptr), "surfaceless EGL display");
    eglBindAPI(EGL_OPENGL_API);
    EGLint attributes[] = {EGL_SURFACE_TYPE,
                           EGL_PBUFFER_BIT,
                           EGL_RENDERABLE_TYPE,
                           EGL_OPENGL_BIT,
                           EGL_RED_SIZE,
                           8,
                           EGL_GREEN_SIZE,
                           8,
                           EGL_BLUE_SIZE,
                           8,
                           EGL_NONE};
    EGLConfig config;
    EGLint count = 0;
    require(eglChooseConfig(display, attributes, &config, 1, &count) && count, "EGL config");
    EGLint size[] = {EGL_WIDTH, 240, EGL_HEIGHT, 140, EGL_NONE};
    EGLSurface surface = eglCreatePbufferSurface(display, config, size);
    EGLint context_attrs[] = {
        EGL_CONTEXT_MAJOR_VERSION,           3,       EGL_CONTEXT_MINOR_VERSION, 3, EGL_CONTEXT_OPENGL_PROFILE_MASK,
        EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT, EGL_NONE};
    EGLContext context = eglCreateContext(display, config, EGL_NO_CONTEXT, context_attrs);
    require(eglMakeCurrent(display, surface, surface, context), "OpenGL 3.3 current context");
    {
        evilcocoa::renderer renderer;
        evilcocoa::item label;
        label.frame = {10, 8, 220, 28};
        label.clip = {0, 0, 240, 140};
        label.text = "Привет · Cocoa";
        evilcocoa::item button;
        button.type = evilcocoa::kind::button;
        button.frame = {10, 50, 140, 40};
        button.clip = {0, 0, 240, 140};
        button.text = "Open folder";
        auto read = [&] {
            std::vector<unsigned char> p(240 * 140 * 4);
            glReadPixels(0, 0, 240, 140, GL_RGBA, GL_UNSIGNED_BYTE, p.data());
            require(glGetError() == GL_NO_ERROR, "valid GL draw");
            return p;
        };
        auto pixel = [](const auto &p, int x, int y) { return p[(std::size_t(139 - y) * 240 + x) * 4]; };
        std::array items{label, button};
        renderer.draw(items, 240, 140);
        auto normal = read();
        require(pixel(normal, 15, 70) > pixel(normal, 200, 120), "button fill visible");
        int text_pixels = 0;
        for (int y = 8; y < 36; y++)
            for (int x = 10; x < 230; x++)
                text_pixels += pixel(normal, x, y) > 180;
        require(text_pixels > 40, "Unicode glyphs composited");
        items[1].pressed = true;
        renderer.draw(items, 240, 140);
        auto pressed = read();
        require(pixel(pressed, 15, 70) < pixel(normal, 15, 70), "pressed state changes fill");
        items[1].pressed = false;
        items[1].enabled = false;
        renderer.draw(items, 240, 140);
        auto disabled = read();
        require(disabled != normal, "disabled text state");
        items[1].clip = {0, 0, 5, 5};
        renderer.draw(items, 240, 140);
        auto clipped = read();
        require(pixel(clipped, 15, 70) == pixel(clipped, 200, 120), "clip excludes control");
        renderer.draw(items, 240, 140, 2);
        read();
        evilcocoa::item centered;
        centered.frame = {10, 10, 200, 25};
        centered.clip = {0, 0, 240, 140};
        centered.text = "Center";
        centered.align = evilcocoa::alignment::center;
        renderer.draw(std::span(&centered, 1), 240, 140);
        auto center = read();
        int left = 0, middle = 0;
        for (int y = 10; y < 35; ++y)
            for (int x = 10; x < 210; ++x) {
                if (pixel(center, x, y) > 180) {
                    if (x < 60)
                        ++left;
                    if (x >= 80 && x <= 150)
                        ++middle;
                }
            }
        require(left == 0 && middle > 20, "centered glyph texture retains alignment padding");
        auto bitmap = std::make_shared<evilcocoa::bitmap>();
        bitmap->width = 2;
        bitmap->height = 2;
        bitmap->argb = {0xffff0000, 0xff00ff00, 0xff0000ff, 0x80800000};
        evilcocoa::item picture;
        picture.type = evilcocoa::kind::image;
        picture.frame = {10, 10, 40, 40};
        picture.clip = {0, 0, 240, 140};
        picture.pixels = bitmap;
        picture.scaling = evilcocoa::image_scaling::stretch;
        renderer.draw(std::span(&picture, 1), 240, 140);
        auto colored = read();
        auto channel = [&](int x, int y, int c) { return colored[(std::size_t(139 - y) * 240 + x) * 4 + c]; };
        require(channel(12, 12, 0) > 230 && channel(12, 12, 1) < 10, "full-color image keeps red");
        require(channel(48, 12, 1) > 230 && channel(48, 12, 0) < 10, "image orientation and green channel");
        require(channel(12, 48, 2) > 230, "image bottom row remains below top row");
        require(channel(48, 48, 0) > 130 && channel(48, 48, 0) < 170,
                "premultiplied alpha blends without multiplying twice");
        for (auto kind :
             {evilcocoa::kind::checkbox, evilcocoa::kind::radio, evilcocoa::kind::popup, evilcocoa::kind::combo_box,
              evilcocoa::kind::slider, evilcocoa::kind::progress, evilcocoa::kind::scroller,
              evilcocoa::kind::scroll_view, evilcocoa::kind::table_row, evilcocoa::kind::segments}) {
            evilcocoa::item control;
            control.type = kind;
            control.frame = {10, 50, 180, 30};
            control.clip = {0, 0, 240, 140};
            control.text = "Control";
            control.value = .5;
            control.checked = true;
            control.segments = {"One", "Two"};
            control.selected = 1;
            renderer.draw(std::span(&control, 1), 240, 140);
            auto pixels = read();
            int changed = 0;
            for (int y = 50; y < 80; ++y)
                for (int x = 10; x < 190; ++x)
                    changed += pixel(pixels, x, y) != pixel(pixels, 220, 120);
            require(changed > 20, "additional control paints visible pixels");
        }
    }
    eglMakeCurrent(display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    eglDestroyContext(display, context);
    eglDestroySurface(display, surface);
    eglTerminate(display);
    std::puts("evilcocoa OpenGL controls, Unicode, clipping and interaction states passed");
}
