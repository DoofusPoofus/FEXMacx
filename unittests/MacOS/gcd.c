#include <dispatch/dispatch.h>
#include <stdatomic.h>
#include <stdio.h>

int main(void) {
    static atomic_long total = 0;
    dispatch_group_t group = dispatch_group_create();
    dispatch_queue_t queue = dispatch_get_global_queue(QOS_CLASS_DEFAULT, 0);

    for (int multiplier = 1; multiplier <= 8; multiplier++) {
        dispatch_group_async(group, queue, ^{
            long sum = 0;
            for (int number = 1; number <= 1000; number++) {
                sum += number * multiplier;
            }
            atomic_fetch_add(&total, sum);
        });
    }

    dispatch_group_wait(group, DISPATCH_TIME_FOREVER);
    printf("gcd done, total %ld\n", (long)atomic_load(&total));
    return 0;
}
