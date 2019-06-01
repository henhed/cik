#ifndef CONFIG_H
#define CONFIG_H 1

#define DEBUG 1

// This config amounts to 2G memory
// 256K entries of size 256 Bytes
// ..
// 8 entries of size 16 Megabytes
#define MAX_NUM_BUCKETS        0xF       //  16
#define MIN_BUCKET_SIZE        0x200     // 256 Bytes
#define MAX_BUCKET_SIZE        0x1000000 //  16 Megabytes
#define MAX_BUCKET_ENTRY_COUNT 0x40000   // 256 K

//#define MAX_NUM_CACHE_ENTRIES 823117
#define MAX_NUM_CACHE_ENTRIES 23

#endif /* ! CONFIG_H */
