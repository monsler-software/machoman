#include "foundation.h"
#include "machoman/objc_framework.h"
#include <atomic>
#include <chrono>
#include <cmath>
#include <condition_variable>
#include <mutex>
#include <thread>

namespace fnd {
FND_CLASS(NSLock);
FND_CLASS(NSRecursiveLock);
FND_CLASS(NSCondition);
FND_CLASS(NSConditionLock);
FND_CLASS(NSThread);
FND_CLASS(NSDate);
FND_CLASS(NSAssertionHandler);
namespace {
using clock = std::chrono::system_clock;
struct date_object {
    mm_objc_object base;
    double seconds = 0;
};
double now() { return std::chrono::duration<double>(clock::now().time_since_epoch()).count(); }
clock::time_point deadline(id date) {
    double value = date && is_kind(date, cls("NSDate")) ? as<date_object>(date)->seconds : now();
    const double max = std::chrono::duration<double>(clock::time_point::max().time_since_epoch()).count();
    if (value >= max)
        return clock::time_point::max();
    if (value <= 0)
        return clock::time_point{};
    return clock::time_point(std::chrono::duration_cast<clock::duration>(std::chrono::duration<double>(value)));
}
std::uint64_t date_init(id o, SEL, double seconds) {
    as<date_object>(o)->seconds = seconds + 978307200.;
    return ret(o);
}
std::uint64_t date_now(id o, SEL) {
    as<date_object>(o)->seconds = now();
    return ret(o);
}
std::uint64_t date_epoch(id klass, SEL, double seconds) {
    auto *o = make<date_object>(reinterpret_cast<Class>(klass));
    o->seconds = seconds;
    return ret(autorelease(&o->base));
}
std::uint64_t date_reference(id klass, SEL, double seconds) { return date_epoch(klass, nullptr, seconds + 978307200.); }
std::uint64_t date_relative(id klass, SEL, double seconds) { return date_epoch(klass, nullptr, now() + seconds); }
std::uint64_t date_current(id klass, SEL) { return date_epoch(klass, nullptr, now()); }
std::uint64_t date_future(id klass, SEL) { return date_epoch(klass, nullptr, 1e12); }
std::uint64_t date_past(id klass, SEL) { return date_epoch(klass, nullptr, -1e12); }
double date_seconds(id o, SEL) { return as<date_object>(o)->seconds; }
double date_refseconds(id o, SEL) { return as<date_object>(o)->seconds - 978307200.; }
double date_since_now(id o, SEL) { return as<date_object>(o)->seconds - now(); }
std::uint64_t date_add(id o, SEL, double seconds) {
    return date_epoch(reinterpret_cast<id>(cls("NSDate")), nullptr, as<date_object>(o)->seconds + seconds);
}
template <class Mutex> struct lock_object {
    mm_objc_object base;
    Mutex mutex;
    id name = nullptr;
    ~lock_object() { release(name); }
};
using plain_lock = lock_object<std::timed_mutex>;
using recursive_lock = lock_object<std::recursive_timed_mutex>;
template <class T> std::uint64_t lock(id o, SEL) {
    as<T>(o)->mutex.lock();
    return 0;
}
template <class T> std::uint64_t unlock(id o, SEL) {
    as<T>(o)->mutex.unlock();
    return 0;
}
template <class T> std::uint64_t try_lock(id o, SEL) { return as<T>(o)->mutex.try_lock(); }
template <class T> std::uint64_t lock_before(id o, SEL, id date) {
    return as<T>(o)->mutex.try_lock_until(deadline(date));
}
template <class T> std::uint64_t name(id o, SEL) { return ret(as<T>(o)->name); }
template <class T> std::uint64_t set_name(id o, SEL, id value) {
    id next = retain(value);
    release(as<T>(o)->name);
    as<T>(o)->name = next;
    return 0;
}
struct condition_object {
    mm_objc_object base;
    std::mutex mutex;
    std::condition_variable changed;
    id name = nullptr;
    ~condition_object() { release(name); }
};
std::uint64_t condition_wait(id o, SEL) {
    auto *c = as<condition_object>(o);
    std::unique_lock held(c->mutex, std::adopt_lock);
    c->changed.wait(held);
    held.release();
    return 0;
}
std::uint64_t condition_wait_until(id o, SEL, id date) {
    auto *c = as<condition_object>(o);
    std::unique_lock held(c->mutex, std::adopt_lock);
    auto result = c->changed.wait_until(held, deadline(date));
    held.release();
    return result != std::cv_status::timeout;
}
std::uint64_t condition_signal(id o, SEL) {
    as<condition_object>(o)->changed.notify_one();
    return 0;
}
std::uint64_t condition_broadcast(id o, SEL) {
    as<condition_object>(o)->changed.notify_all();
    return 0;
}
struct condition_lock_object {
    mm_objc_object base;
    std::mutex mutex;
    std::condition_variable changed;
    std::int64_t condition = 0;
    bool held = false;
    std::thread::id owner;
    id name = nullptr;
    ~condition_lock_object() { release(name); }
};
std::uint64_t condition_init(id o, SEL, std::int64_t value) {
    as<condition_lock_object>(o)->condition = value;
    return ret(o);
}
bool acquire_condition(id o, bool test, std::int64_t value, bool try_only, id date = nullptr) {
    auto *c = as<condition_lock_object>(o);
    std::unique_lock held(c->mutex);
    auto ready = [&] { return !c->held && (!test || c->condition == value); };
    if (try_only) {
        if (!ready())
            return false;
    } else if (date) {
        if (!c->changed.wait_until(held, deadline(date), ready))
            return false;
    } else
        c->changed.wait(held, ready);
    c->held = true;
    c->owner = std::this_thread::get_id();
    return true;
}
std::uint64_t condition_lock(id o, SEL) {
    acquire_condition(o, false, 0, false);
    return 0;
}
std::uint64_t condition_lock_when(id o, SEL, std::int64_t value) {
    acquire_condition(o, true, value, false);
    return 0;
}
std::uint64_t condition_try(id o, SEL) { return acquire_condition(o, false, 0, true); }
std::uint64_t condition_try_when(id o, SEL, std::int64_t value) { return acquire_condition(o, true, value, true); }
std::uint64_t condition_before(id o, SEL, id date) { return acquire_condition(o, false, 0, false, date); }
std::uint64_t condition_when_before(id o, SEL, std::int64_t value, id date) {
    return acquire_condition(o, true, value, false, date);
}
std::uint64_t condition_value(id o, SEL) {
    auto *c = as<condition_lock_object>(o);
    std::lock_guard held(c->mutex);
    return std::uint64_t(c->condition);
}
std::uint64_t condition_release(id o, bool change, std::int64_t value) {
    auto *c = as<condition_lock_object>(o);
    {
        std::lock_guard held(c->mutex);
        if (!c->held || c->owner != std::this_thread::get_id()) {
            std::fprintf(stderr, "Foundation: NSConditionLock unlocked by a non-owner\n");
            std::abort();
        }
        if (change)
            c->condition = value;
        c->held = false;
        c->owner = {};
    }
    c->changed.notify_all();
    return 0;
}
std::uint64_t condition_unlock(id o, SEL) { return condition_release(o, false, 0); }
std::uint64_t condition_unlock_with(id o, SEL, std::int64_t value) { return condition_release(o, true, value); }
struct thread_object {
    mm_objc_object base;
    id target = nullptr, argument = nullptr, block = nullptr, name = nullptr;
    SEL selector = nullptr;
    std::atomic<bool> started = false, executing = false, finished = false, cancelled = false;
    std::uint64_t identity = 0;
    ~thread_object() {
        release(target);
        release(argument);
        release(block);
        release(name);
    }
};
Class thread_class;
thread_local id current_thread = nullptr;
std::uint64_t thread_current(id klass, SEL) {
    if (!current_thread) {
        auto *o = make<thread_object>(reinterpret_cast<Class>(klass));
        o->identity = api()->thread_self();
        o->executing = true;
        current_thread = &o->base;
    }
    return ret(current_thread);
}
std::uint64_t thread_main(id, SEL) { return api()->thread_self() == 1; }
std::uint64_t thread_instance_main(id o, SEL) { return as<thread_object>(o)->identity == 1; }
std::uint64_t thread_init(id o, SEL, id target, SEL selector, id argument) {
    auto *t = as<thread_object>(o);
    t->target = retain(target);
    t->selector = selector;
    t->argument = retain(argument);
    return ret(o);
}
std::uint64_t thread_init_block(id o, SEL, id block) {
    as<thread_object>(o)->block = retain(block);
    return ret(o);
}
std::uint64_t thread_start(id o, SEL) {
    auto *t = as<thread_object>(o);
    bool expected = false;
    if (!t->started.compare_exchange_strong(expected, true))
        return 0;
    retain(o);
    std::thread([o] {
        current_thread = o;
        auto *t = as<thread_object>(o);
        t->executing = true;
        {
            mmfw::autorelease_pool pool;
            const std::uint64_t argument = ret(t->argument);
            if (t->block)
                mm_objc_block_invoke(t->block, 0, nullptr);
            else if (t->target && t->selector)
                mm_objc_send(t->target, t->selector, 1, &argument);
        }
        t->executing = false;
        t->finished = true;
        current_thread = nullptr;
        release(o);
    }).detach();
    return 0;
}
std::uint64_t thread_detach(id klass, SEL, SEL selector, id target, id argument) {
    id o = obj(send(klass, "alloc"));
    thread_init(o, nullptr, target, selector, argument);
    thread_start(o, nullptr);
    release(o);
    return 0;
}
std::uint64_t thread_detach_block(id klass, SEL, id block) {
    id o = obj(send(klass, "alloc"));
    thread_init_block(o, nullptr, block);
    thread_start(o, nullptr);
    release(o);
    return 0;
}
std::uint64_t thread_cancel(id o, SEL) {
    as<thread_object>(o)->cancelled = true;
    return 0;
}
std::uint64_t thread_cancelled(id o, SEL) { return as<thread_object>(o)->cancelled.load(); }
std::uint64_t thread_executing(id o, SEL) { return as<thread_object>(o)->executing.load(); }
std::uint64_t thread_finished(id o, SEL) { return as<thread_object>(o)->finished.load(); }
std::uint64_t thread_sleep(id, SEL, double seconds) {
    if (std::isfinite(seconds) && seconds > 0)
        std::this_thread::sleep_for(std::chrono::duration<double>(seconds));
    return 0;
}
std::uint64_t delayed(id o, SEL, SEL action, id argument, double delay) {
    enqueue_selector(nullptr, o, action, argument, delay, false);
    return 0;
}
std::uint64_t delayed_modes(id o, SEL, SEL action, id argument, double delay, id) {
    return delayed(o, nullptr, action, argument, delay);
}
std::uint64_t on_main(id o, SEL, SEL action, id argument, std::uint64_t wait) {
    id main = obj(send(reinterpret_cast<id>(cls("NSRunLoop")), "mainRunLoop"));
    enqueue_selector(main, o, action, argument, 0, wait & 255);
    return 0;
}
std::uint64_t on_main_modes(id o, SEL, SEL action, id argument, std::uint64_t wait, id) {
    return on_main(o, nullptr, action, argument, wait);
}
std::uint64_t cancel_performs(id, SEL, id target) {
    cancel_selectors(target, nullptr, nullptr, false);
    return 0;
}
std::uint64_t cancel_perform(id, SEL, id target, SEL selector, id argument) {
    cancel_selectors(target, selector, argument, true);
    return 0;
}
std::uint64_t assertion_current(id klass, SEL) {
    static thread_local id handler = nullptr;
    if (!handler)
        handler = obj(send(klass, "new"));
    return ret(handler);
}
std::uint64_t assertion_method(id, SEL, SEL method, id object, id file, std::int64_t line, id message) {
    std::fprintf(stderr, "Foundation: assertion in -[%s %s] (%s:%lld): %s\n",
                 mm_objc_class_name(mm_objc_object_class(object)), method, to_utf8(file).c_str(), (long long)line,
                 to_utf8(message).c_str());
    std::abort();
}
std::uint64_t assertion_function(id, SEL, id function, id file, std::int64_t line, id message) {
    std::fprintf(stderr, "Foundation: assertion in %s (%s:%lld): %s\n", to_utf8(function).c_str(),
                 to_utf8(file).c_str(), (long long)line, to_utf8(message).c_str());
    std::abort();
}
} // namespace
id date_from_epoch(double seconds) {
    auto *o = make<date_object>(cls("NSDate"));
    o->seconds = seconds;
    return &o->base;
}
double date_to_epoch(id date) { return as<date_object>(date)->seconds; }
void register_threading() {
    Class base = cls("NSObject");
    Class date = mm_objc_define_class("NSDate", base, sizeof(date_object), FND_CLASS_REF(NSDate), FND_META_REF(NSDate));
    register_lifecycle<date_object>(date);
    FND_METHODS(date, {"init", IMP(date_now), "@16@0:8"},
                {"initWithTimeIntervalSinceReferenceDate:", IMP(date_init), "@24@0:8d16"},
                {"timeIntervalSince1970", IMP(date_seconds), "d16@0:8"},
                {"timeIntervalSinceReferenceDate", IMP(date_refseconds), "d16@0:8"},
                {"timeIntervalSinceNow", IMP(date_since_now), "d16@0:8"},
                {"dateByAddingTimeInterval:", IMP(date_add), "@24@0:8d16"});
    FND_CLASS_METHODS(date, {"date", IMP(date_current), "@16@0:8"},
                      {"dateWithTimeIntervalSinceNow:", IMP(date_relative), "@24@0:8d16"},
                      {"dateWithTimeIntervalSince1970:", IMP(date_epoch), "@24@0:8d16"},
                      {"dateWithTimeIntervalSinceReferenceDate:", IMP(date_reference), "@24@0:8d16"},
                      {"distantFuture", IMP(date_future), "@16@0:8"}, {"distantPast", IMP(date_past), "@16@0:8"});
#define LOCK_METHODS(klass, T)                                                                                         \
    FND_METHODS(klass, {"lock", IMP(lock<T>), "v16@0:8"}, {"unlock", IMP(unlock<T>), "v16@0:8"},                       \
                {"name", IMP(name<T>), "@16@0:8"}, {"setName:", IMP(set_name<T>), "v24@0:8@16"})
#define TIMED_METHODS(klass, T)                                                                                        \
    FND_METHODS(klass, {"tryLock", IMP(try_lock<T>), "B16@0:8"}, {"lockBeforeDate:", IMP(lock_before<T>), "B24@0:8@16"})
    Class plain = mm_objc_define_class("NSLock", base, sizeof(plain_lock), FND_CLASS_REF(NSLock), FND_META_REF(NSLock));
    register_lifecycle<plain_lock>(plain);
    LOCK_METHODS(plain, plain_lock);
    TIMED_METHODS(plain, plain_lock);
    Class recursive = mm_objc_define_class("NSRecursiveLock", base, sizeof(recursive_lock),
                                           FND_CLASS_REF(NSRecursiveLock), FND_META_REF(NSRecursiveLock));
    register_lifecycle<recursive_lock>(recursive);
    LOCK_METHODS(recursive, recursive_lock);
    TIMED_METHODS(recursive, recursive_lock);
    Class condition = mm_objc_define_class("NSCondition", base, sizeof(condition_object), FND_CLASS_REF(NSCondition),
                                           FND_META_REF(NSCondition));
    register_lifecycle<condition_object>(condition);
    LOCK_METHODS(condition, condition_object);
    FND_METHODS(condition, {"wait", IMP(condition_wait), "v16@0:8"},
                {"waitUntilDate:", IMP(condition_wait_until), "B24@0:8@16"},
                {"signal", IMP(condition_signal), "v16@0:8"}, {"broadcast", IMP(condition_broadcast), "v16@0:8"});
    Class cl = mm_objc_define_class("NSConditionLock", base, sizeof(condition_lock_object),
                                    FND_CLASS_REF(NSConditionLock), FND_META_REF(NSConditionLock));
    register_lifecycle<condition_lock_object>(cl);
    FND_METHODS(
        cl, {"initWithCondition:", IMP(condition_init), "@24@0:8q16"}, {"condition", IMP(condition_value), "q16@0:8"},
        {"lock", IMP(condition_lock), "v16@0:8"}, {"unlock", IMP(condition_unlock), "v16@0:8"},
        {"lockWhenCondition:", IMP(condition_lock_when), "v24@0:8q16"},
        {"unlockWithCondition:", IMP(condition_unlock_with), "v24@0:8q16"}, {"tryLock", IMP(condition_try), "B16@0:8"},
        {"tryLockWhenCondition:", IMP(condition_try_when), "B24@0:8q16"},
        {"lockBeforeDate:", IMP(condition_before), "B24@0:8@16"},
        {"lockWhenCondition:beforeDate:", IMP(condition_when_before), "B32@0:8q16@24"},
        {"name", IMP(name<condition_lock_object>), "@16@0:8"},
        {"setName:", IMP(set_name<condition_lock_object>), "v24@0:8@16"});
    thread_class =
        mm_objc_define_class("NSThread", base, sizeof(thread_object), FND_CLASS_REF(NSThread), FND_META_REF(NSThread));
    register_lifecycle<thread_object>(thread_class);
    FND_CLASS_METHODS(thread_class, {"currentThread", IMP(thread_current), "@16@0:8"},
                      {"isMainThread", IMP(thread_main), "B16@0:8"},
                      {"sleepForTimeInterval:", IMP(thread_sleep), "v24@0:8d16"},
                      {"detachNewThreadSelector:toTarget:withObject:", IMP(thread_detach), "v40@0:8:16@24@32"},
                      {"detachNewThreadWithBlock:", IMP(thread_detach_block), "v24@0:8@?16"});
    FND_METHODS(thread_class, {"initWithTarget:selector:object:", IMP(thread_init), "@40@0:8@16:24@32"},
                {"initWithBlock:", IMP(thread_init_block), "@24@0:8@?16"}, {"start", IMP(thread_start), "v16@0:8"},
                {"isMainThread", IMP(thread_instance_main), "B16@0:8"}, {"cancel", IMP(thread_cancel), "v16@0:8"},
                {"isCancelled", IMP(thread_cancelled), "B16@0:8"}, {"isExecuting", IMP(thread_executing), "B16@0:8"},
                {"isFinished", IMP(thread_finished), "B16@0:8"}, {"name", IMP(name<thread_object>), "@16@0:8"},
                {"setName:", IMP(set_name<thread_object>), "v24@0:8@16"});
    FND_METHODS(
        base, {"performSelector:withObject:afterDelay:", IMP(delayed), "v40@0:8:16@24d32"},
        {"performSelector:withObject:afterDelay:inModes:", IMP(delayed_modes), "v48@0:8:16@24d32@40"},
        {"performSelectorOnMainThread:withObject:waitUntilDone:", IMP(on_main), "v40@0:8:16@24B32"},
        {"performSelectorOnMainThread:withObject:waitUntilDone:modes:", IMP(on_main_modes), "v48@0:8:16@24B32@40"});
    FND_CLASS_METHODS(
        base, {"cancelPreviousPerformRequestsWithTarget:", IMP(cancel_performs), "v24@0:8@16"},
        {"cancelPreviousPerformRequestsWithTarget:selector:object:", IMP(cancel_perform), "v40@0:8@16:24@32"});
    Class handler = mm_objc_define_class("NSAssertionHandler", base, sizeof(mm_objc_object),
                                         FND_CLASS_REF(NSAssertionHandler), FND_META_REF(NSAssertionHandler));
    FND_CLASS_METHODS(handler, {"currentHandler", IMP(assertion_current), "@16@0:8"});
    FND_METHODS(
        handler,
        {"handleFailureInMethod:object:file:lineNumber:description:", IMP(assertion_method), "v56@0:8:16@24@32q40@48"},
        {"handleFailureInFunction:file:lineNumber:description:", IMP(assertion_function), "v48@0:8@16@24q32@40"});
}
} // namespace fnd

extern "C" {
extern EXPORT const double kCFAbsoluteTimeIntervalSince1970_ __asm__("_kCFAbsoluteTimeIntervalSince1970");
extern EXPORT const double kCFAbsoluteTimeIntervalSince1904_ __asm__("_kCFAbsoluteTimeIntervalSince1904");
const double kCFAbsoluteTimeIntervalSince1970_ = 978307200.;
const double kCFAbsoluteTimeIntervalSince1904_ = 3061152000.;
double CFAbsoluteTimeGetCurrent_() DARWIN(CFAbsoluteTimeGetCurrent);
double CFAbsoluteTimeGetCurrent_() {
    /* CF absolute time counts seconds from 2001-01-01, not the Unix epoch. */
    return std::chrono::duration<double>(std::chrono::system_clock::now().time_since_epoch()).count() - 978307200.;
}
}
