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

static void* (*temp_malloc)(size_t size);
static void* (*temp_calloc)(size_t nmemb, size_t size);
static void* (*temp_realloc)(void *ptr, size_t size);
static void  (*temp_free)(void *ptr);

__thread unsigned int entered = 0;

int start_call() {
  return __sync_fetch_and_add(&entered, 1);
}

void end_call() {
  __sync_fetch_and_sub(&entered, 1);
}

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

unsigned long last_timestamp = 0;
struct timespec last_ts = { 0, 0 };
unsigned long report_timestamp = 0;
static inline void incr_report_timestamp(int n) {
  report_timestamp += n;
}

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
    unsigned long timestamp;
    UT_hash_handle hh; /* makes this structure hashable */
};

struct h_struct *h_map = NULL;

static inline void h_add(void* ptr, size_t size, unsigned long now) {
    struct h_struct *s = (struct h_struct *)malloc(sizeof(struct h_struct));
    if (!s) {
        fprintf(stderr, "ERROR h_add() failed to malloc(%zu): %s\n", size, dlerror());
        return;
    }
    s->ptr = ptr;
    s->size = size;
    s->timestamp = now;
    HASH_ADD_PTR( h_map, ptr, s );
}

static inline size_t h_delete(void* ptr, unsigned long* timestamp) {
    struct h_struct *s = NULL;
    size_t size = 0;
    HASH_FIND_PTR(h_map, &ptr, s);
    if (s != NULL) {
        size = s->size;
        *timestamp = s->timestamp;
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

static inline int timestamp_to_index(unsigned long timestamp) {
    unsigned long diff = (unsigned long)time(NULL) - timestamp;

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

static inline void update_age_bucket(unsigned long now) {

    /*  TODO - Fix this!!! */

    /*
    unsigned long diff = now - last_timestamp;
    if (diff < 1)
        return;

    if (diff > 1) {
        age_bucket[1] += age_bucket[0];
        age_bucket[0] = 0;
    }

    if (diff > 100) {
        age_bucket[2] += age_bucket[1];
        age_bucket[0] = age_bucket[1] = 0;
    } else if (diff < 1000) {
        age_bucket[3] += age_bucket[2] + age_bucket[1] + age_bucket[0];
        age_bucket[0] = age_bucket[1] = age_bucket[2] = 0;
    } else {
        age_bucket[4] += age_bucket[3] + age_bucket[2] + age_bucket[1] + age_bucket[0];
        age_bucket[0] = age_bucket[1] = age_bucket[2] = age_bucket[3] = 0;
    }
    last_timestamp = now;
    */
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
    char *ts = asctime(gmtime((time_t*)&report_timestamp));
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

static inline void stats_alloc(void* ptr, size_t size, unsigned long now) {
    incr_curr_alloc_size(size);
    incr_num_curr_allocs(1);
    incr_num_overall_allocs(1);
    ++size_bucket[size_bucket_index(size)];
    ++age_bucket[0];
    h_add(ptr, size, now);
}

static inline void stats_free(void* ptr) {
    unsigned long timestamp;
    size_t size;
    if ((size = h_delete(ptr, &timestamp)) != 0) {
        decr_curr_alloc_size(size);
        decr_num_curr_allocs(1);
        --size_bucket[size_bucket_index(size)];
        --age_bucket[timestamp_to_index(timestamp)];
    }
}

static inline void do_stats(void* ptr, size_t size, void* realloc_orig_ptr) {
    unsigned long now = (unsigned long)time(NULL);

    update_age_bucket(now);

    if (size) { /* malloc, calloc, realloc */
        if (realloc_orig_ptr) { /* realloc */
            stats_free(realloc_orig_ptr);
        }
        if (size) {
            stats_alloc(ptr, size, now);
        }
    } else { /* free */
        stats_free(ptr);
    }

    if ((now - report_timestamp) > REPORT_INTERVAL_SECS) {
        report_timestamp = (unsigned long)time(NULL);
        print_stats();
    }
}

void __attribute__((constructor)) init() {
    start_call();

    if (pthread_mutex_init(&lock, NULL) != 0) {
        fprintf(stderr, "ERROR: failed to load __FILE__\n");
        exit(1);
        return;
    }

    last_timestamp = (unsigned long)time(NULL);
    report_timestamp = (unsigned long)time(NULL);

    real_malloc         = dummy_malloc;
    real_calloc         = dummy_calloc;
    real_realloc        = NULL;
    real_free           = dummy_free;

    temp_malloc         = dlsym(RTLD_NEXT, "malloc");
    temp_calloc         = dlsym(RTLD_NEXT, "calloc");
    temp_realloc        = dlsym(RTLD_NEXT, "realloc");
    temp_free           = dlsym(RTLD_NEXT, "free");

    if (!temp_malloc || !temp_calloc || !temp_realloc || !temp_free)
    {
        fprintf(stderr, "Error in `dlsym`: %s\n", dlerror());
        exit(1);
    }

    real_malloc         = temp_malloc;
    real_calloc         = temp_calloc;
    real_realloc        = temp_realloc;
    real_free           = temp_free;

    end_call();
}

void __attribute__ ((destructor)) finish() {
    start_call();
    print_stats();
    end_call();
}

void* malloc(size_t size) {
    int internal = start_call();
    void* ptr = real_malloc(size);
    if (!internal && ptr && size) {
        pthread_mutex_lock(&lock);
        do_stats(ptr, size, NULL);
        pthread_mutex_unlock(&lock);
    }
    end_call();
    return ptr;
}

void* calloc(size_t nmemb, size_t size) {
    int internal = start_call();
    void* ptr = real_calloc(nmemb, size);
    if (!internal && ptr && size) {
        pthread_mutex_lock(&lock);
        do_stats(ptr, size, NULL);
        pthread_mutex_unlock(&lock);
    }
    end_call();
    return ptr;
}

void* realloc(void *in_ptr, size_t size) {
    int internal = start_call();
    void* out_ptr = real_realloc(in_ptr, size);
    if (!internal && out_ptr && size)
    {
        pthread_mutex_lock(&lock);
        do_stats(out_ptr, size, in_ptr);
        pthread_mutex_unlock(&lock);
    }
    end_call();
    return out_ptr;
}

void free(void *ptr) {
    int internal = start_call();
    real_free(ptr);
    if (!internal && ptr) {
        pthread_mutex_lock(&lock);
        do_stats(ptr, 0, NULL);
        pthread_mutex_unlock(&lock);
    }
    end_call();
}
