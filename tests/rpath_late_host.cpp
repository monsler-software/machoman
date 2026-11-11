// A later host candidate must not shadow an earlier guest library.
extern "C" __attribute__((visibility("default"))) int value() __asm__("_local_value");
extern "C" int value(){return 99;}
