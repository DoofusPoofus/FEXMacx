#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <stdio.h>

int main(void) {
    static atomic_long applied = 0;
    dispatch_apply(64, dispatch_get_global_queue(QOS_CLASS_USER_INITIATED, 0), ^(size_t index) {
        atomic_fetch_add(&applied, (long)index);
    });
    printf("dispatch_apply sum %ld\n", (long)atomic_load(&applied));

    dispatch_queue_t serial = dispatch_queue_create("fexmacx.serial", DISPATCH_QUEUE_SERIAL);
    __block int order = 0;
    for (int step = 0; step < 10; step++) {
        dispatch_async(serial, ^{ order = order * 2 + 1; });
    }
    dispatch_sync(serial, ^{});
    printf("serial queue order %d\n", order);

    dispatch_semaphore_t after_done = dispatch_semaphore_create(0);
    dispatch_time_t asked_at = dispatch_time(DISPATCH_TIME_NOW, 0);
    __block dispatch_time_t fired_at = 0;
    dispatch_after(dispatch_time(DISPATCH_TIME_NOW, 50 * NSEC_PER_MSEC), dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0), ^{
        fired_at = dispatch_time(DISPATCH_TIME_NOW, 0);
        dispatch_semaphore_signal(after_done);
    });
    long after_timed_out = dispatch_semaphore_wait(after_done, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
    int waited_long_enough = !after_timed_out && fired_at - asked_at >= 45 * NSEC_PER_MSEC;
    printf("dispatch_after %s\n", after_timed_out ? "timed out" : (waited_long_enough ? "fired" : "fired too early"));

    dispatch_semaphore_t ticks_done = dispatch_semaphore_create(0);
    dispatch_source_t ticker = dispatch_source_create(DISPATCH_SOURCE_TYPE_TIMER, 0, 0, serial);
    __block int tick_count = 0;
    dispatch_source_set_timer(ticker, dispatch_time(DISPATCH_TIME_NOW, 10 * NSEC_PER_MSEC), 10 * NSEC_PER_MSEC, NSEC_PER_MSEC);
    dispatch_source_set_event_handler(ticker, ^{
        tick_count++;
        if (tick_count == 5) {
            dispatch_source_cancel(ticker);
            dispatch_semaphore_signal(ticks_done);
        }
    });
    dispatch_resume(ticker);
    long ticks_timed_out = dispatch_semaphore_wait(ticks_done, dispatch_time(DISPATCH_TIME_NOW, 3 * NSEC_PER_SEC));
    printf("timer source ticks %d%s\n", tick_count, ticks_timed_out ? " (timed out)" : "");
    return 0;
}
