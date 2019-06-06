#ifndef CONFIG_H
#define CONFIG_H 1

#define DEBUG 1

// This config amounts roughly 2G memory
// 256K entries of size 256 Bytes
// ..
// 16 entries of size 8 Megabytes
#define MAX_NUM_BUCKETS        0xF       // 16
#define MIN_BUCKET_SIZE        0x200     // 256 Bytes
#define MAX_BUCKET_SIZE        0x800000 //  8 Megabytes
#define MAX_BUCKET_ENTRY_COUNT 0x40000   // 256 K

/* #define MAX_NUM_CACHE_ENTRIES 823117 */
#define MAX_NUM_CACHE_ENTRIES 23
#define MAX_NUM_TAGS_PER_ENTRY 3 // This is hard coded in the spec

#define SERVER_PORT 5555
#define SERVER_BACKLOG 5
#define MAX_NUM_CLIENTS 3
#define MAX_NUM_EVENTS 3

#endif /* ! CONFIG_H */
