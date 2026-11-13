#ifndef MACHOMAN_AUDIO_NOTIFICATION_DISPATCH_H
#define MACHOMAN_AUDIO_NOTIFICATION_DISPATCH_H
namespace mm::audio {
void dispatch_audio_notification(void *queue, void *context, void (*invoke)(void *), void (*cancel)(void *));
void quiesce_audio_notifications();
template<class T> void queue_audio_notification(void *queue, T *context, void (*invoke)(void *)) {
    dispatch_audio_notification(queue, context, invoke, [](void *p) { delete static_cast<T *>(p); });
}
}
#endif
