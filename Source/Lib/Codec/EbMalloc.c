/*
* Copyright(c) 2019 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/
#include <stdio.h>
#include <inttypes.h>


#include "EbMalloc.h"
#include "EbThreads.h"
#include "EbUtility.h"

#ifdef DEBUG_MEMORY_USAGE

static EB_HANDLE gMallocMutex;

#ifdef _WIN32

#include <windows.h>

static INIT_ONCE gMallocOnce = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK CreateMallocMutex (
    PINIT_ONCE InitOnce,
    PVOID Parameter,
    PVOID *lpContext)
{
    (void)InitOnce;
    (void)Parameter;
    (void)lpContext;
    gMallocMutex = EbCreateMutex();
    return TRUE;
}

static EB_HANDLE GetMallocMutex()
{
    InitOnceExecuteOnce(&gMallocOnce, CreateMallocMutex, NULL, NULL);
    return gMallocMutex;
}
#else
#include <pthread.h>
static void CreateMallocMutex()
{
    gMallocMutex = EbCreateMutex();
}

static pthread_once_t gMallocOnce = PTHREAD_ONCE_INIT;

static EB_HANDLE GetMallocMutex()
{
    pthread_once(&gMallocOnce, CreateMallocMutex);
    return gMallocMutex;
}
#endif // _WIN32

//hash function to speedup etnry search
uint32_t hash(void* p)
{
#define MASK32 ((((uint64_t)1)<<32)-1)

    uint64_t v = (uint64_t)p;
    uint64_t low32 = v & MASK32;
    return (uint32_t)((v >> 32) + low32);
}

typedef struct MemoryEntry{
    void* ptr;
    EB_PTRType type;
    size_t count;
    const char* file;
    uint32_t line;
} MemoryEntry;

//+1 to get a better hash result
#define MEM_ENTRY_SIZE (4 * 1024 * 1024 + 1)

MemoryEntry gMemEntry[MEM_ENTRY_SIZE];

#define TO_INDEX(v) ((v) % MEM_ENTRY_SIZE)
static EB_BOOL gAddMemEntryWarning = EB_TRUE;
static EB_BOOL gRemoveMemEntryWarning = EB_TRUE;


/*********************************************************************************
*
* @brief
*  compare and update current memory entry.
*
* @param[in] e
*  current memory entry.
*
* @param[in] param
*  param you set to ForEachMemEntry
*
*
* @returns  return EB_TRUE if you want get early exit in ForEachMemEntry
*
s*
********************************************************************************/

typedef EB_BOOL (*Predicate)(MemoryEntry* e, void* param);

/*********************************************************************************
*
* @brief
*  Loop through mem entries.
*
* @param[in] bucket
*  the hash bucket
*
* @param[in] start
*  loop start position
*
* @param[in] pred
*  return EB_TRUE if you want early exit
*
* @param[out] param
*  param send to pred.
*
* @returns  return EB_TRUE if we got early exit.
*
*
********************************************************************************/
static EB_BOOL ForEachHashEntry(MemoryEntry* bucket, uint32_t start, Predicate pred, void* param)
{

    uint32_t s = TO_INDEX(start);
    uint32_t i = s;

    do {
        MemoryEntry* e = bucket + i;
        if (pred(e, param)) {
            return EB_TRUE;
        }
        i++;
        i = TO_INDEX(i);
    } while (i != s);
     return EB_FALSE;
}

static EB_BOOL ForEachMemEntry(uint32_t start, Predicate pred, void* param)
{
    EB_BOOL ret;
    EB_HANDLE m = GetMallocMutex();
    EbBlockOnMutex(m);
    ret = ForEachHashEntry(gMemEntry, start, pred, param);
    EbReleaseMutex(m);
    return ret;
}

static const char* ResourceTypeName(EB_PTRType type)
{
    static const char *name[EB_PTR_TYPE_TOTAL] = {"malloced memory", "calloced memory", "aligned memory", "mutex", "semaphore", "thread"};
    return name[type];
}

static EB_BOOL AddMemEntry(MemoryEntry* e, void* param)
{
    MemoryEntry* newItem = (MemoryEntry*)param;
    if (!e->ptr) {
        EB_MEMCPY(e, newItem, sizeof(MemoryEntry));
        return EB_TRUE;
    }
    return EB_FALSE;
}


void EbAddMemEntry(void* ptr,  EB_PTRType type, size_t count, const char* file, uint32_t line)
{
    MemoryEntry item;
    item.ptr = ptr;
    item.type = type;
    item.count = count;
    item.file = file;
    item.line = line;
    if (ForEachMemEntry(hash(ptr), AddMemEntry, &item))
        return;
    if (gAddMemEntryWarning) {
        fprintf(stderr, "SVT: can't add memory entry.\r\n");
        fprintf(stderr, "SVT: You have memory leak or you need increase MEM_ENTRY_SIZE\r\n");
        gAddMemEntryWarning = EB_FALSE;
    }
}

static EB_BOOL RemoveMemEntry(MemoryEntry* e, void* param)
{
    MemoryEntry* item = (MemoryEntry*)param;
    if (e->ptr == item->ptr) {
        if (e->type == item->type) {
            e->ptr = NULL;
            return EB_TRUE;
        } else if (e->type == EB_C_PTR && item->type == EB_N_PTR) {
            //speical case, we use EB_FREE to free calloced memory
            e->ptr = NULL;
            return EB_TRUE;
        }
    }
    return EB_FALSE;
}

void EbRemoveMemEntry(void* ptr, EB_PTRType type)
{
    if (!ptr)
        return;
    MemoryEntry item;
    item.ptr = ptr;
    item.type = type;
    if (ForEachMemEntry(hash(ptr), RemoveMemEntry, &item))
        return;
    if (gRemoveMemEntryWarning) {
        fprintf(stderr, "SVT: something wrong. you freed a unallocated resource %p, type = %s\r\n", ptr, ResourceTypeName(type));
        gRemoveMemEntryWarning = EB_FALSE;
    }
}

typedef struct MemSummary {
    uint64_t amount[EB_PTR_TYPE_TOTAL];
    uint32_t occupied;
} MemSummary;

static EB_BOOL CountMemEntry(MemoryEntry* e, void* param)
{
    MemSummary* sum = (MemSummary*)param;
    if (e->ptr) {
        sum->amount[e->type] += e->count;
        sum->occupied++;
    }
    return EB_FALSE;
}

static void GetMemoryUsageAndScale(uint64_t amount, double* usage, char* scale)
{
    char scales[] = { ' ', 'K', 'M', 'G' };
    size_t i;
    uint64_t v;
    for (i = 1; i < sizeof(scales); i++) {
        v = (uint64_t)1 << (i * 10);
        if (amount < v)
            break;
    }
    i--;
    v = (uint64_t)1 << (i * 10);
    *usage = (double)amount / v;
    *scale = scales[i];
}

//this need more memory and cpu
#define PROFILE_MEMORY_USAGE
#ifdef PROFILE_MEMORY_USAGE

//if we use a static array here, this size + sizeof(gMemEntry) will exceed max size allowed on windows.
static MemoryEntry* gProfileEntry;

uint32_t GetHashLocation(FILE* f, int line) {
#define MASK32 ((((uint64_t)1)<<32)-1)

    uint64_t v = (uint64_t)f;
    uint64_t low32 = v & MASK32;
    return (uint32_t)((v >> 32) + low32 + line);
}

static EB_BOOL AddLocation(MemoryEntry* e, void* param) {
    MemoryEntry* newItem = (MemoryEntry*)param;
    if (!e->ptr) {
        *e = *newItem;
        return EB_TRUE;
    } else if (e->file == newItem->file && e->line == newItem->line) {
        e->count += newItem->count;
        return EB_TRUE;
    }
    //to next position.
    return EB_FALSE;
}

static EB_BOOL CollectMem(MemoryEntry* e, void* param) {
    EB_PTRType type = *(EB_PTRType*)param;
    if (e->ptr && e->type == type) {
        ForEachHashEntry(gProfileEntry, 0, AddLocation, e);
    }
    //Loop entire bucket.
    return EB_FALSE;
}

static int CompareCount(const void* a,const void* b)
{
    const MemoryEntry* pa = (const MemoryEntry*)a;
    const MemoryEntry* pb = (const MemoryEntry*)b;
    if (pb->count < pa->count) return -1;
    if (pb->count == pa->count) return 0;
    return 1;
}

static void PrintTop10Llocations() {
    EB_HANDLE m = GetMallocMutex();
    EB_PTRType type = EB_N_PTR;
    EbBlockOnMutex(m);
    gProfileEntry = (MemoryEntry*)calloc(MEM_ENTRY_SIZE, sizeof(MemoryEntry));
    if (!gProfileEntry) {
        fprintf(stderr, "not enough memory for memory profile");
        EbReleaseMutex(m);
        return;
    }

    ForEachHashEntry(gMemEntry, 0, CollectMem, &type);
    qsort(gProfileEntry, MEM_ENTRY_SIZE, sizeof(MemoryEntry), CompareCount);

    printf("top 10 %s locations:\r\n", ResourceTypeName(type));
    for (int i = 0; i < 10; i++) {
        double usage;
        char scale;
        MemoryEntry* e = gProfileEntry + i;
        GetMemoryUsageAndScale(e->count, &usage, &scale);
        printf("(%.2lf %cB): %s:%d\r\n", usage, scale, e->file, e->line);
    }
    free(gProfileEntry);
    EbReleaseMutex(m);
}
#endif //PROFILE_MEMORY_USAGE

static int gComponentCount;

#endif //DEBUG_MEMORY_USAGE

void EbPrintMemoryUsage()
{
#ifdef DEBUG_MEMORY_USAGE
    MemSummary sum;
    double fulless;
    double usage;
    char scale;
    memset(&sum, 0, sizeof(MemSummary));

    ForEachMemEntry(0, CountMemEntry, &sum);
    printf("SVT Memory Usage:\r\n");
    GetMemoryUsageAndScale(sum.amount[EB_N_PTR] + sum.amount[EB_C_PTR] + sum.amount[EB_A_PTR], &usage, &scale);
    printf("    total allocated memory:       %.2lf %cB\r\n", usage, scale);
    GetMemoryUsageAndScale(sum.amount[EB_N_PTR], &usage, &scale);
    printf("        malloced memory:          %.2lf %cB\r\n", usage, scale);
    GetMemoryUsageAndScale(sum.amount[EB_C_PTR], &usage, &scale);
    printf("        callocated memory:        %.2lf %cB\r\n", usage, scale);
    GetMemoryUsageAndScale(sum.amount[EB_A_PTR], &usage, &scale);
    printf("        allocated aligned memory: %.2lf %cB\r\n", usage, scale);

    printf("    mutex count: %d\r\n", (int)sum.amount[EB_MUTEX]);
    printf("    semaphore count: %d\r\n", (int)sum.amount[EB_SEMAPHORE]);
    printf("    thread count: %d\r\n", (int)sum.amount[EB_THREAD]);
    fulless = (double)sum.occupied / MEM_ENTRY_SIZE;
    printf("    hash table fulless: %f, hash bucket is %s\r\n", fulless, fulless < .3 ? "healthy":"too full" );
#ifdef PROFILE_MEMORY_USAGE
    PrintTop10Llocations();
#endif
#endif
}


void EbIncreaseComponentCount()
{
#ifdef DEBUG_MEMORY_USAGE
    EB_HANDLE m = GetMallocMutex();
    EbBlockOnMutex(m);
    gComponentCount++;
    EbReleaseMutex(m);
#endif
}

#ifdef DEBUG_MEMORY_USAGE
static EB_BOOL PrintLeak(MemoryEntry* e, void* param)
{
    if (e->ptr) {
        EB_BOOL* leaked = (EB_BOOL*)param;
        *leaked = EB_TRUE;
        fprintf(stderr, "SVT: %s leaked at %s:L%d\r\n", ResourceTypeName(e->type), e->file, e->line);
    }
    //loop through all items
    return EB_FALSE;
}
#endif

void EbDecreaseComponentCount()
{
#ifdef DEBUG_MEMORY_USAGE
    EB_HANDLE m = GetMallocMutex();
    EbBlockOnMutex(m);
    gComponentCount--;
    if (!gComponentCount) {
        EB_BOOL leaked = EB_FALSE;
        ForEachHashEntry(gMemEntry, 0, PrintLeak, &leaked);
        if (!leaked) {
            printf("SVT: you have no memory leak\r\n");
        }
    }
    EbReleaseMutex(m);
#endif
}

#ifdef DEBUG_TIMESTAMP

static EB_HANDLE g_time_mutex;

#ifdef _WIN32

#include <windows.h>

static INIT_ONCE g_time_once = INIT_ONCE_STATIC_INIT;

BOOL CALLBACK create_time_mutex (
    PINIT_ONCE InitOnce,
    PVOID Parameter,
    PVOID *lpContext)
{
    (void)InitOnce;
    (void)Parameter;
    (void)lpContext;
    g_time_mutex = EbCreateMutex();
    return TRUE;
}

static EB_HANDLE get_time_mutex()
{
    InitOnceExecuteOnce(&g_time_once, create_time_mutex, NULL, NULL);
    return g_time_mutex;
}
#else
#include <pthread.h>
static void create_time_mutex()
{
    g_time_mutex = EbCreateMutex();
}

static pthread_once_t g_time_once = 0;

static EB_HANDLE get_time_mutex()
{
    pthread_once(&g_time_once, create_time_mutex);
    return g_time_mutex;
}
#endif // _WIN32

EB_U32 hash_ti(EB_U64 p)
{
#define MASK32 ((((EB_U64)1)<<32)-1)

     EB_U64 low32 = p & MASK32;
    return (EB_U32)((p >> 32) + low32);
}

typedef struct TimeEntry{
    EB_U32 pic_num;
    EB_S8 seg_idx;
    EB_S8 tile_idx;
    EbTaskType in_type;
    EbTaskType out_type;
    EbProcessType proc_type;
    EB_U64 start_sTime;
    EB_U64 start_uTime;
    EB_U64 end_sTime;
    EB_U64 end_uTime;
} TimeEntry;

 //+1 to get a better hash result
#define TIME_ENTRY_SIZE (4 * 1024 * 1024 + 1)

TimeEntry g_time_entry[TIME_ENTRY_SIZE];

#define TO_INDEX_TI(v) ((v) % TIME_ENTRY_SIZE)
static EB_BOOL g_add_time_entry_warning = EB_TRUE;

typedef EB_BOOL (*Predicate2)(TimeEntry* e, void* param);

static EB_BOOL for_each_hash_entry_ti(TimeEntry* bucket, EB_U32 start, Predicate2 pred, void* param)
{

    EB_U32 s = TO_INDEX_TI(start);
    EB_U32 i = s;

    do {
        TimeEntry* e = bucket + i;
        if (pred(e, param)) {
            return EB_TRUE;
        }
        i++;
        i = TO_INDEX_TI(i);
    } while (i != s);
    return EB_FALSE;
}

static EB_BOOL for_each_time_entry(EB_U32 start, Predicate2 pred, void* param)
{
    EB_BOOL ret;
    EB_HANDLE m = get_time_mutex();
    EbBlockOnMutex(m);
    ret = for_each_hash_entry_ti(g_time_entry, start, pred, param);
    EbReleaseMutex(m);
    return ret;
}

static EB_BOOL add_time_entry(TimeEntry* e, void* param)
{
    TimeEntry* new_item = (TimeEntry*)param;
    if (!e->start_uTime) {
        EB_MEMCPY(e, new_item, sizeof(TimeEntry));
        return EB_TRUE;
    }
    return EB_FALSE;
}

static int compare_time(const void* a,const void* b)
{
    const TimeEntry* pa = (const TimeEntry*)a;
    const TimeEntry* pb = (const TimeEntry*)b;
    if (pa->start_sTime == 0 && pb->start_sTime != 0) return 1;
    if (pa->start_sTime != 0 && pb->start_sTime == 0) return -1;
    if (pa->start_sTime < pb->start_sTime) return -1;
    else if (pa->start_sTime == pb->start_sTime) {
        if (pa->start_uTime < pb->start_uTime) return -1;
        else if (pa->start_uTime > pb->start_uTime) return 1;
        else return 0;
    }
    return 1;
}

static const char *process_namelist[EB_PROCESS_TYPE_TOTAL] = {
    "RESOURCE", "PA", "PD",
    "ME", "IRC", "SRC",
    "PM", "RC", "MDC",
    "ENCDEC", "ENTROPY", "PAK"
};
static const char* process_name(EbProcessType type)
{
    return process_namelist[type];
}

#endif // DEBUG_TIMESTAMP
void eb_add_time_entry(EbProcessType proc_type, EbTaskType in_type, EbTaskType out_type,
                        EB_U32 pic_num, EB_S8 seg_idx, EB_S8 tile_idx,
                        EB_U64 start_sTime, EB_U64 start_uTime)
{
#ifdef DEBUG_TIMESTAMP
    TimeEntry item;
    item.pic_num = pic_num;
    item.seg_idx = seg_idx;
    item.tile_idx = tile_idx;
    item.in_type = in_type;
    item.out_type = out_type;
    item.proc_type = proc_type;
    item.start_sTime = start_sTime;
    item.start_uTime = start_uTime;
    EbHevcStartTime(&item.end_sTime, &item.end_uTime);
    if (for_each_time_entry(hash_ti(item.start_sTime), add_time_entry, &item))
        return;
    if (g_add_time_entry_warning) {
        fprintf(stderr, "SVT: can't add time entry.\r\n");
        fprintf(stderr, "SVT: You need to increase TIME_ENTRY_SIZE\r\n");
        g_add_time_entry_warning = EB_FALSE;
    }
#endif
}

void eb_print_time_usage(const char* profilePATH) {
#ifdef DEBUG_TIMESTAMP
    EB_HANDLE m = get_time_mutex();
    EbBlockOnMutex(m);
    FILE *fp = NULL; //*fp_raw = NULL;
    fp = fopen(profilePATH, "w+");
    // fp_raw = fopen("/tmp/profile_hevc_raw.csv", "w+");
    qsort(g_time_entry, TIME_ENTRY_SIZE, sizeof(TimeEntry), compare_time);
    int i = 0;
    double s_mtime, e_mtime, duration;
    while (g_time_entry[i].start_uTime) {
        EbHevcComputeOverallElapsedTimeRealMs(
            g_time_entry[0].start_sTime,
            g_time_entry[0].start_uTime,
            g_time_entry[i].start_sTime,
            g_time_entry[i].start_uTime,
            &s_mtime);
        EbHevcComputeOverallElapsedTimeRealMs(
            g_time_entry[0].start_sTime,
            g_time_entry[0].start_uTime,
            g_time_entry[i].end_sTime,
            g_time_entry[i].end_uTime,
            &e_mtime);
        EbHevcComputeOverallElapsedTimeRealMs(
            g_time_entry[i].start_sTime,
            g_time_entry[i].start_uTime,
            g_time_entry[i].end_sTime,
            g_time_entry[i].end_uTime,
            &duration);
        fprintf(fp, "%s, inType=%d, outType=%d, picNum=%u, segIdx=%d, tileIdx=%d, sTime=%.2f, eTime=%.2f, duration=%.2f\n",
            process_name(g_time_entry[i].proc_type), (int)g_time_entry[i].in_type, (int)g_time_entry[i].out_type,
             g_time_entry[i].pic_num, g_time_entry[i].seg_idx, g_time_entry[i].tile_idx, s_mtime, e_mtime, duration);
        // fprintf(fp_raw, "%d, %d, %d, %zu, %d, %.4f\n",
        //     (int)g_time_entry[i].Ptype, (int)g_time_entry[i].time_type, (int)g_time_entry[i].task_type,
        //      g_time_entry[i].pic_num, g_time_entry[i].seg_idx, mtime);
        ++i;
    }
    fclose(fp);
    // fclose(fp_raw);
    EbReleaseMutex(m);
#endif
}
