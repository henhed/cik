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

// This config amounts roughly 2G memory
// 512K entries of size 256 Bytes
// ..
// 16 entries of size 8 Megabytes
#define MAX_NUM_BUCKETS        0x10      //  16
#define MIN_BUCKET_SIZE        0x100     // 256 Bytes
#define MAX_BUCKET_SIZE        0x800000  //   8 Megabytes
#define MAX_BUCKET_ENTRY_COUNT 0x80000   // 512 K
#define MAX_TOTAL_MEMORY       0x100000000 // 4Gb

#define NUM_CACHE_ENTRY_MAPS 6421
#define CACHE_ENTRY_MAP_SIZE 797

#define SERVER_PORT          5555
#define SERVER_BACKLOG       0x100
#define NUM_WORKERS          0x10
#define MAX_NUM_CLIENTS      0x100
#define MAX_NUM_EVENTS       0x100
#define WORKER_EPOLL_TIMEOUT 1000 // 1s

#endif /* ! CONFIG_H */
