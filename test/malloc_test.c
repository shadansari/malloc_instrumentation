#include <stdlib.h>
#include <stdio.h>
#include <sys/types.h>
#include <unistd.h>
#include <time.h>
#include <math.h>

#define TEST_NUM 110
#define NUM_SIZE_BUCKETS 12

int main(int agrc, char **argv) {
    srand(time(NULL));
    void *p[NUM_SIZE_BUCKETS][TEST_NUM];

    for (int i = 0; i < NUM_SIZE_BUCKETS; i++) {
        for (int j = 0; j < TEST_NUM; j++) {
            p[i][j] = malloc(pow(2, i+1)+1);
        }
        sleep(2);
    }

    for (int i= 0; i < NUM_SIZE_BUCKETS; i++) {
        for (int j = 0; j < TEST_NUM; j++) {
            free(p[i][j]);
        }
        sleep(2);
    }

    return (0);
}

