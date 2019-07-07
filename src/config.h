#ifndef CONFIG_H
#define CONFIG_H 1

#ifndef DEBUG
# define DEBUG 0
#endif

#if DEBUG
# include <assert.h>
# define cik_assert assert
#else
# define cik_assert(expr)
#endif

#define MAX_NUM_BUCKETS        0x10        //  16
#define MIN_BUCKET_SIZE        0x100       // 256 Bytes
#define MAX_BUCKET_SIZE        0x800000    //   8 Megabytes
#define MAX_BUCKET_ENTRY_COUNT 0x80000     // 512 K
#define MAX_TOTAL_MEMORY       0xFFFFFFFF  //   4 Gb -1b

#define NUM_CACHE_ENTRY_MAPS 6421 // Should be a prime
#define CACHE_ENTRY_MAP_SIZE 797  // Should be a prime

#define SERVER_BACKLOG       0x100
#define NUM_WORKERS          0x10
#define MAX_NUM_CLIENTS      0x100
#define MAX_NUM_EVENTS       0x100
#define WORKER_EPOLL_TIMEOUT 1000 // 1s
#define NUM_LOG_QUEUE_ELEMS  0x100 // Must be power of 2

typedef struct _RuntimeConfig RuntimeConfig; // Defined in types.h

RuntimeConfig *parse_args (int, char **);

#endif /* ! CONFIG_H */
