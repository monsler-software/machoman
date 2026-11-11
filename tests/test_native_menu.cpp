#include "../src/dylibs/AppKit/native_menu.h"
#include <cstdio>
#include <cstdlib>
using namespace ak;
void require(bool value, const char *message) { if (!value) { std::fprintf(stderr, "FAIL: %s\n", message); std::exit(1); } }
int main() {
    native_menu_item file; file.token = 1; file.title = "File";
    native_menu_item disabled; disabled.token = 2; disabled.title = "Disabled"; disabled.enabled = false;
    native_menu_item separator; separator.separator = true;
    native_menu_item open; open.token = 3; open.title = "Open"; open.shortcut = "Ctrl+O";
    native_menu_item nested; nested.token = 4; nested.title = "Nested";
    native_menu_item leaf; leaf.token = 5; leaf.title = "Leaf"; nested.children = {leaf};
    file.children = {disabled, separator, open, nested};
    native_menu menu; menu.resize(320, 240); menu.replace({file});
    platform_event key; key.kind = event_kind::key_down; key.keycode = 109;
    require(menu.handle(key, {}).opening == 1 && menu.opened(), "F10 opens the application menu");
    key.keycode = 125; menu.handle(key, {}); key.keycode = 36;
    require(menu.handle(key, {}).action == 3 && !menu.opened(), "keyboard skips disabled and separator rows");
    platform_event click; click.kind = event_kind::mouse_down;
    require(menu.handle(click, {8, 12}).opening == 1, "mouse opens the bar without sending content clicks");
    platform_event release; release.kind = event_kind::mouse_up;
    require(menu.handle(release, {8, 12}).consumed, "matching release is consumed");
    require(!menu.handle(click, {15, 40}).action && menu.opened(), "disabled row cannot run an action");
    platform_event motion; motion.kind = event_kind::mouse_moved;
    require(menu.handle(motion, {15, 100}).opening == 4, "hover opens nested submenu");
    key.keycode = 125; menu.handle(key, {}); key.keycode = 36;
    require(menu.handle(key, {}).action == 5 && !menu.opened(), "nested keyboard action retains its item identity");
    menu.handle(click, {8, 12}); menu.handle(release, {8, 12});
    require(menu.handle(click, {300, 200}).consumed && !menu.opened(), "click outside dismisses popup");
    require(menu.handle(release, {300, 200}).consumed, "dismissal does not leak an unmatched mouse release");
    require(!menu.handle(click, {300, 200}).consumed, "normal content events pass through");
    menu.handle(click, {8, 12}); menu.replace({});
    require(menu.empty() && !menu.opened(), "removing the menu invalidates open panel pointers");
    menu.replace({file}); menu.handle(click, {8, 12});
    platform_event focus; focus.kind = event_kind::focus_out;
    require(!menu.handle(focus, {}).consumed && !menu.opened(), "focus loss closes popup and reaches the application");
    std::puts("native menu input, submenus, disabled rows and dismissal passed");
}
