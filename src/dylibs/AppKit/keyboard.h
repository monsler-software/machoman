#pragma once

#include <cstdint>

namespace ak {
/* Host Ctrl is the native shortcut modifier on Linux and Windows.
 * Swap Ctrl/Command, preserving access to Darwin Control through Super.
 * Backends can opt out without altering layout or physical key identity. */
inline std::uint64_t native_shortcut_modifiers(std::uint64_t flags, bool enabled = true) {
    if (!enabled) return flags;
    constexpr auto control = 1ull << 18, command = 1ull << 20;
    return (flags & ~(control | command)) | ((flags & control) ? command : 0) | ((flags & command) ? control : 0);
}
inline std::uint32_t native_shortcut_keycode(std::uint32_t key, bool enabled = true) {
    if (!enabled) return key;
    switch (key) {
    case 59: return 55; case 62: return 54;
    case 55: return 59; case 54: return 62;
    default: return key;
    }
}
/* NSEvent uses Apple's physical virtual-key codes, independent of layout.
 * Wayland supplies evdev codes; the additional XKB offset belongs only to
 * the host keymap lookup, never to the guest event. */
inline std::uint32_t mac_keycode_from_evdev(std::uint32_t key) {
    switch (key) {
    case 1: return 53; /* escape */
    case 2: return 18; case 3: return 19; case 4: return 20; case 5: return 21;
    case 6: return 23; case 7: return 22; case 8: return 26; case 9: return 28;
    case 10: return 25; case 11: return 29; case 12: return 27; case 13: return 24;
    case 14: return 51; case 15: return 48; /* backspace, tab */
    case 16: return 12; case 17: return 13; case 18: return 14; case 19: return 15;
    case 20: return 17; case 21: return 16; case 22: return 32; case 23: return 34;
    case 24: return 31; case 25: return 35; case 26: return 33; case 27: return 30;
    case 28: return 36; case 29: return 59; /* return, left control */
    case 30: return 0; case 31: return 1; case 32: return 2; case 33: return 3;
    case 34: return 5; case 35: return 4; case 36: return 38; case 37: return 40;
    case 38: return 37; case 39: return 41; case 40: return 39; case 41: return 50;
    case 42: return 56; case 43: return 42; /* left shift, backslash */
    case 44: return 6; case 45: return 7; case 46: return 8; case 47: return 9;
    case 48: return 11; case 49: return 45; case 50: return 46; case 51: return 43;
    case 52: return 47; case 53: return 44; case 54: return 60; case 55: return 67;
    case 56: return 58; case 57: return 49; case 58: return 57;
    case 59: return 122; case 60: return 120; case 61: return 99; case 62: return 118;
    case 63: return 96; case 64: return 97; case 65: return 98; case 66: return 100;
    case 67: return 101; case 68: return 109; case 69: return 71;
    case 71: return 89; case 72: return 91; case 73: return 92; case 74: return 78;
    case 75: return 86; case 76: return 87; case 77: return 88; case 78: return 69;
    case 79: return 83; case 80: return 84; case 81: return 85; case 82: return 82;
    case 83: return 65; case 86: return 10; case 87: return 103; case 88: return 111;
    case 96: return 76; case 97: return 62; case 98: return 75; case 100: return 61;
    case 102: return 115; case 103: return 126; case 104: return 116;
    case 105: return 123; case 106: return 124; case 107: return 119;
    case 108: return 125; case 109: return 121; case 110: return 114; case 111: return 117;
    case 125: return 55; case 126: return 54;
    default: return 0xffff;
    }
}
}
