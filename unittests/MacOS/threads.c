#include <pthread.h>
#include <stdio.h>

struct worker {
    int index;
    long long sum;
};

static void* add_numbers(void* raw_worker) {
    struct worker* worker = raw_worker;
    long long sum = 0;
    for (int number = 1; number <= 100000; number++) {
        sum += number * (worker->index + 1);
    }
    worker->sum = sum;
    return worker;
}

int main(void) {
    pthread_t threads[4];
    struct worker workers[4];
    for (int index = 0; index < 4; index++) {
        workers[index].index = index;
        workers[index].sum = 0;
        if (pthread_create(&threads[index], NULL, add_numbers, &workers[index]) != 0) {
            printf("pthread_create failed\n");
            return 1;
        }
    }

    long long total = 0;
    for (int index = 0; index < 4; index++) {
        void* returned = NULL;
        pthread_join(threads[index], &returned);
        total += ((struct worker*)returned)->sum;
    }
    printf("4 threads done, total %lld\n", total);
    return 0;
}
