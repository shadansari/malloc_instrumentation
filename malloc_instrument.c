#define _GNU_SOURCE
#include <dlfcn.h>
#include <stdio.h>
#include <time.h>
#include <pthread.h>
#include <math.h>
#include "uthash.h"

#define REPORT_INTERVAL_SECS 5
#define PROGRESS_BAR_UNIT 10 /* 8123 ? */

static void* (*real_malloc)(size_t size);
static void* (*real_calloc)(size_t nmemb, size_t size);
static void* (*real_realloc)(void *ptr, size_t size);
static void  (*real_free)(void *ptr);

__thread unsigned int entered = 0;

int start_call() {
    unsigned int t = entered;
    entered++;
    return t;
}

void end_call() {
    entered--;
}

static int alloc_init_pending = 0;

char tmpbuf[1024];
unsigned long tmppos = 0;
unsigned long tmpallocs = 0;

pthread_mutex_t lock;

unsigned long num_curr_allocs = 0; /* Number of current allocations */
static inline void incr_num_curr_allocs(int n) {
  num_curr_allocs += n;
}
static inline void decr_num_curr_allocs(int n) {
  num_curr_allocs -= n;
}

unsigned long num_overall_allocs = 0; /* Number of overall allocations since start */
static inline void incr_num_overall_allocs(int n) {
  num_overall_allocs += n;
}

unsigned long curr_alloc_size = 0; /* Current total allocated size (in bytes) */
static inline void incr_curr_alloc_size(int size) {
  curr_alloc_size += size;
}
static inline void decr_curr_alloc_size(int size) {
  curr_alloc_size -= size;
}

struct timespec timer_1 = {0, 0};
struct timespec timer_10 = {0, 0};
struct timespec timer_100 = {0, 0};
struct timespec timer_1000 = {0, 0};
struct timespec last = {0, 0};
struct timespec report_timestamp = {0, 0};

void* dummy_malloc(size_t size) {
    if (tmppos + size >= sizeof(tmpbuf)) exit(1);
    void *retptr = tmpbuf + tmppos;
    tmppos += size;
    ++tmpallocs;
    return retptr;
}

void* dummy_calloc(size_t nmemb, size_t size) {
    void *ptr = dummy_malloc(nmemb * size);
    unsigned int i = 0;
    for (; i < nmemb * size; ++i)
        *((char*)(ptr + i)) = '\0';
    return ptr;
}

void dummy_free(void *ptr) {
}

struct h_struct {
    void* ptr;            /* hash key */
    size_t size;
    struct timespec timestamp;
    UT_hash_handle hh; /* makes this structure hashable */
};

struct h_struct *h_map = NULL;

static inline void h_add(void* ptr, size_t size, struct timespec *now) {
    struct h_struct *s = (struct h_struct *)malloc(sizeof(struct h_struct));
    if (!s) {
        fprintf(stderr, "ERROR h_add() failed to malloc(%zu): %s\n", size, dlerror());
        return;
    }
    s->ptr = ptr;
    s->size = size;
    s->timestamp.tv_sec = now->tv_sec;
    s->timestamp.tv_nsec = now->tv_nsec;
    HASH_ADD_PTR( h_map, ptr, s );
}

static inline size_t h_delete(void* ptr, struct timespec *timestamp) {
    struct h_struct *s = NULL;
    size_t size = 0;
    HASH_FIND_PTR(h_map, &ptr, s);
    if (s != NULL) {
        size = s->size;
        timestamp->tv_sec = s->timestamp.tv_sec;
        timestamp->tv_nsec = s->timestamp.tv_nsec;
        HASH_DEL(h_map, s);
        free(s);
    }
    return size;
}

static inline struct h_struct* h_get(void* ptr) {
    struct h_struct *s;

    HASH_FIND_PTR(h_map, &ptr, s );
    return s;
}

#define NUM_SIZE_BUCKETS 12
long size_bucket[NUM_SIZE_BUCKETS];
const char* size_bucket_label[NUM_SIZE_BUCKETS] = {
    "0 - 3 bytes:",
    "4 - 7 bytes:",
    "8 - 15 bytes:",
    "16 - 31 bytes:",
    "32 - 63 bytes:",
    "64 - 127 bytes:",
    "128 -255 bytes:",
    "256 - 511 bytes:",
    "512 - 1023 bytes:",
    "1024 - 2047 bytes:",
    "2048 - 4095 bytes:",
    "4096 + bytes:"
};

#define NUM_AGE_BUCKETS 5
signed long age_bucket[NUM_AGE_BUCKETS];
const char* age_bucket_label[NUM_AGE_BUCKETS] = {
    "< 1 sec:",
    "< 10 sec:",
    "< 100 sec:",
    "< 1000 sec:",
    "> 1000 sec:"
};

static inline int size_bucket_index(size_t numOfBytes)
{
    if (numOfBytes > 4096) {
        return NUM_SIZE_BUCKETS-1;
    }
    if (numOfBytes <= 1) {
        return 0;
    }
    for (int i = 0; i < 64; i++)
    {
        if (numOfBytes < (1 << i))
            return i - 2;
    }
    return NUM_SIZE_BUCKETS-1;
}

static inline int timestamp_to_index(struct timespec *timestamp) {
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    time_t diff = now.tv_sec - timestamp->tv_sec;

    /* TODO - Improve on this */

    if (diff < 1) {
        return 0;
    } else if (diff < 10) {
        return 1;
    } else if (diff < 100) {
        return 2;
    } else if (diff < 1000) {
        return 3;
    } else {
        return 4;
    }
}

// https://gist.github.com/diabloneo/9619917
# define timespec_diff(a, b, result)                  \
  do {                                                \
    (result)->tv_sec = (a)->tv_sec - (b)->tv_sec;     \
    (result)->tv_nsec = (a)->tv_nsec - (b)->tv_nsec;  \
    if ((result)->tv_nsec < 0) {                      \
      --(result)->tv_sec;                             \
      (result)->tv_nsec += 1000000000;                \
    }                                                 \
  } while (0)

static inline void update_age_bucket(struct timespec *now) {

    /*  TODO - Fix this!!! */

	struct timespec diff;
	timespec_diff(now, &last, &diff);
	last = *now;

	if (timer_1000.tv_sec + diff.tv_sec > 1000) {
        age_bucket[4] += age_bucket[3] + age_bucket[2] + age_bucket[1] + age_bucket[0];
        age_bucket[0] = age_bucket[1] = age_bucket[2] = age_bucket[3] = 0;
		timer_1000.tv_sec = timer_100.tv_sec = timer_10.tv_sec = timer_1.tv_sec = 0;
	} else if (timer_100.tv_sec + diff.tv_sec > 100) {
        age_bucket[3] += age_bucket[2] + age_bucket[1] + age_bucket[0];
        age_bucket[0] = age_bucket[1] = age_bucket[2] = 0;
		timer_100.tv_sec = timer_10.tv_sec = timer_1.tv_sec = 0;
		timer_1000.tv_sec += diff.tv_sec;
	} else if (timer_10.tv_sec + diff.tv_sec > 10) {
		age_bucket[2] += age_bucket[1] + age_bucket[0];
        age_bucket[0] = age_bucket[1] = 0;
		timer_10.tv_sec = timer_1.tv_sec = 0;
		timer_100.tv_sec += diff.tv_sec;
		timer_1000.tv_sec += diff.tv_sec;
	} else if (timer_1.tv_sec + diff.tv_sec > 1) {
        age_bucket[1] += age_bucket[0];
        age_bucket[0] = 0;
		timer_1.tv_sec = 0;
		timer_10.tv_sec += diff.tv_sec;
	} else {
		// TODO
	}
    return;
}

static inline char* bytes_to_string(unsigned long bytes, char *buf) {
    int i = 0;
    const char* units[] = {"B", "kB", "MB", "GB", "TB", "PB", "EB", "ZB", "YB"};
    while (bytes > 1024) {
        bytes /= 1024;
        i++;
    }
    sprintf(buf, "%lu%s", bytes, units[i]);
    return buf;
}

static inline char* progress_bar(unsigned long n, char *buf) {
    buf[0] = 0;
    int i;
    for (i = 0; n >= PROGRESS_BAR_UNIT; i++) {
        buf[i] = '#';
        n -= PROGRESS_BAR_UNIT;
    }
    buf[i] = 0;
    return buf;
}

static inline void print_stats() {
	char buf[4096];
    char *ts = asctime(gmtime(&report_timestamp.tv_sec));
    ts[strlen(ts) - 1] = 0;
    fprintf(stderr, ">>>>>>>>>>>>> %s <<<<<<<<<<<\n", ts);
    fprintf(stderr, "Overall stats:\n");
    fprintf(stderr, "%lu Current allocations\n", num_curr_allocs);
    fprintf(stderr, "%lu Overall allocations since start\n", num_overall_allocs);
    fprintf(stderr, "%s (%lu bytes) Current total allocated size\n", bytes_to_string(curr_alloc_size, buf), curr_alloc_size);
    fprintf(stderr, "\n");
    fprintf(stderr, "Current allocations by size:  (# = %d current allocations)\n", PROGRESS_BAR_UNIT);
    for (int i = 0; i < NUM_SIZE_BUCKETS; i++) {
        fprintf(stderr, "%s %s\n", size_bucket_label[i], progress_bar(size_bucket[i], buf));
        //fprintf(stderr, "%s %lu\n", size_bucket_label[i], size_bucket[i]);
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "Current allocations by age:  (# = %d current allocations)\n", PROGRESS_BAR_UNIT);
    for (int i = 0; i < NUM_AGE_BUCKETS; i++) {
        //fprintf(stderr, "%s %s\n", age_bucket_label[i], progress_bar(age_bucket[i], buf));
        fprintf(stderr, "%s %ld\n", age_bucket_label[i], age_bucket[i]);
    }

    fprintf(stderr, "\n\n");
}

static inline void stats_alloc(void* ptr, size_t size, struct timespec *now) {
    incr_curr_alloc_size(size);
    incr_num_curr_allocs(1);
    incr_num_overall_allocs(1);
    ++size_bucket[size_bucket_index(size)];
    ++age_bucket[0];
    h_add(ptr, size, now);
}

static inline void stats_free(void* ptr) {
    struct timespec timestamp;
    size_t size;
    if ((size = h_delete(ptr, &timestamp)) != 0) {
        decr_curr_alloc_size(size);
        decr_num_curr_allocs(1);
        --size_bucket[size_bucket_index(size)];
        --age_bucket[timestamp_to_index(&timestamp)];
    }
}

static inline void do_stats(void* ptr, size_t size, void* realloc_orig_ptr) {
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);

    update_age_bucket(&now);

    if (size) { /* malloc, calloc, realloc */
        if (realloc_orig_ptr) { /* realloc */
            stats_free(realloc_orig_ptr);
        }
        if (size) {
            stats_alloc(ptr, size, &now);
        }
    } else { /* free */
        stats_free(ptr);
    }

    if ((now.tv_sec - report_timestamp.tv_sec) > REPORT_INTERVAL_SECS) {
        report_timestamp.tv_sec = now.tv_sec;
        report_timestamp.tv_nsec = now.tv_nsec;
        print_stats();
    }
}

static void init() {
    alloc_init_pending = 1;

    if (pthread_mutex_init(&lock, NULL) != 0) {
        fprintf(stderr, "ERROR: failed to load __FILE__\n");
        exit(1);
        return;
    }

    clock_gettime(CLOCK_MONOTONIC, &report_timestamp);

    real_malloc         = dlsym(RTLD_NEXT, "malloc");
    real_calloc         = dlsym(RTLD_NEXT, "calloc");
    real_realloc        = dlsym(RTLD_NEXT, "realloc");
    real_free           = dlsym(RTLD_NEXT, "free");

    if (!real_malloc || !real_calloc || !real_realloc || !real_free)
    {
        fprintf(stderr, "Error in `dlsym`: %s\n", dlerror());
        exit(1);
    }

    alloc_init_pending = 0;
}

void __attribute__ ((destructor)) finish() {
    start_call();
    print_stats();
    end_call();
}

void* malloc(size_t size) {
    void *ptr = NULL;
    int internal = start_call();
    if (alloc_init_pending) {
        //fputs("alloc.so: malloc internal\n", stderr);
        ptr = dummy_malloc(size);
    } else {
        if (!real_malloc) {
            init();
        }
        ptr = real_malloc(size);
    }
    if (!internal && ptr && size) {
        pthread_mutex_lock(&lock);
        do_stats(ptr, size, NULL);
        pthread_mutex_unlock(&lock);
    }
    end_call();
    return ptr;
}

void* calloc(size_t nmemb, size_t size) {
    void *ptr = NULL;
    int internal = start_call();
    if (alloc_init_pending) {
        //fputs("alloc.so: malloc internal\n", stderr);
        ptr = dummy_calloc(nmemb, size);
    } else {
        if (!real_malloc) {
            init();
        }
        ptr = real_calloc(nmemb, size);
    }
    if (!internal && ptr && size) {
        pthread_mutex_lock(&lock);
        do_stats(ptr, size, NULL);
        pthread_mutex_unlock(&lock);
    }
    end_call();
    return ptr;
}

void* realloc(void *in_ptr, size_t size) {
    void *ptr = NULL;
    int internal = start_call();
    if (alloc_init_pending) {
        ptr = dummy_malloc(size);
    } else {
        if (!real_malloc) {
            init();
        }
        ptr = real_realloc(in_ptr, size);
    }
    if (!internal && ptr && size)
    {
        pthread_mutex_lock(&lock);
        do_stats(ptr, size, in_ptr);
        pthread_mutex_unlock(&lock);
    }
    end_call();
    return ptr;
}

void free(void *ptr) {
    int internal = start_call();
    if (alloc_init_pending) {
        dummy_free(ptr);
    } else {
        if (!real_malloc) {
            init();
        }
        real_free(ptr);
    }
    if (!internal && ptr) {
        pthread_mutex_lock(&lock);
        do_stats(ptr, 0, NULL);
        pthread_mutex_unlock(&lock);
    }
    end_call();
}
