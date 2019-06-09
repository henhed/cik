#ifndef CONFIG_H
#define CONFIG_H 1

#define DEBUG 1

// This config amounts roughly 2G memory
// 512K entries of size 256 Bytes
// ..
// 16 entries of size 8 Megabytes
#define MAX_NUM_BUCKETS        0x10      // 16
#define MIN_BUCKET_SIZE        0x100     // 256 Bytes
#define MAX_BUCKET_SIZE        0x800000  //   8 Megabytes
#define MAX_BUCKET_ENTRY_COUNT 0x80000   // 512 K

#define MAX_NUM_CACHE_ENTRIES 823117
/* #define MAX_NUM_CACHE_ENTRIES 23 */

#define SERVER_PORT 5555
#define SERVER_BACKLOG 0xFF
#define MAX_NUM_CLIENTS 0xFF
#define MAX_NUM_EVENTS 0xFF

#endif /* ! CONFIG_H */
