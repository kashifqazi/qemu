/*
 * QEMU System Emulator
 *
 * Copyright (c) 2003-2008 Fabrice Bellard
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in
 * all copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
 * THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
 * THE SOFTWARE.
 */
#include <stdint.h>
#include <stdarg.h>
#include <stdlib.h>
#ifndef _WIN32
#include <sys/types.h>
#include <sys/mman.h>
#endif
#include "config.h"
#include "monitor/monitor.h"
#include "sysemu/sysemu.h"
#include "qemu/bitops.h"
#include "qemu/bitmap.h"
#include "sysemu/arch_init.h"
#include "audio/audio.h"
#include "hw/i386/pc.h"
#include "hw/pci/pci.h"
#include "hw/audio/audio.h"
#include "sysemu/kvm.h"
#include "migration/migration.h"
#include "migration/postcopy-ram.h"
#include "hw/i386/smbios.h"
#include "exec/address-spaces.h"
#include "hw/audio/pcspk.h"
#include "migration/page_cache.h"
#include "qemu/config-file.h"
#include "qemu/error-report.h"
#include "qmp-commands.h"
#include "trace.h"
#include "exec/cpu-all.h"
#include "exec/ram_addr.h"
#include "hw/acpi/acpi.h"
#include "qemu/host-utils.h"
#include "qemu/rcu_queue.h"

#ifdef DEBUG_ARCH_INIT
#define DPRINTF(fmt, ...) \
    do { fprintf(stdout, "arch_init: " fmt, ## __VA_ARGS__); } while (0)
#else
#define DPRINTF(fmt, ...) \
    do { } while (0)
#endif

#ifdef TARGET_SPARC
int graphic_width = 1024;
int graphic_height = 768;
int graphic_depth = 8;
#else
int graphic_width = 800;
int graphic_height = 600;
int graphic_depth = 32;
#endif

uint64_t __get_dirty_pages(ram_addr_t start, ram_addr_t length);

#if defined(TARGET_ALPHA)
#define QEMU_ARCH QEMU_ARCH_ALPHA
#elif defined(TARGET_ARM)
#define QEMU_ARCH QEMU_ARCH_ARM
#elif defined(TARGET_CRIS)
#define QEMU_ARCH QEMU_ARCH_CRIS
#elif defined(TARGET_I386)
#define QEMU_ARCH QEMU_ARCH_I386
#elif defined(TARGET_M68K)
#define QEMU_ARCH QEMU_ARCH_M68K
#elif defined(TARGET_LM32)
#define QEMU_ARCH QEMU_ARCH_LM32
#elif defined(TARGET_MICROBLAZE)
#define QEMU_ARCH QEMU_ARCH_MICROBLAZE
#elif defined(TARGET_MIPS)
#define QEMU_ARCH QEMU_ARCH_MIPS
#elif defined(TARGET_MOXIE)
#define QEMU_ARCH QEMU_ARCH_MOXIE
#elif defined(TARGET_OPENRISC)
#define QEMU_ARCH QEMU_ARCH_OPENRISC
#elif defined(TARGET_PPC)
#define QEMU_ARCH QEMU_ARCH_PPC
#elif defined(TARGET_S390X)
#define QEMU_ARCH QEMU_ARCH_S390X
#elif defined(TARGET_SH4)
#define QEMU_ARCH QEMU_ARCH_SH4
#elif defined(TARGET_SPARC)
#define QEMU_ARCH QEMU_ARCH_SPARC
#elif defined(TARGET_XTENSA)
#define QEMU_ARCH QEMU_ARCH_XTENSA
#elif defined(TARGET_UNICORE32)
#define QEMU_ARCH QEMU_ARCH_UNICORE32
#elif defined(TARGET_TRICORE)
#define QEMU_ARCH QEMU_ARCH_TRICORE
#endif

const uint32_t arch_type = QEMU_ARCH;
static bool mig_throttle_on;
static int dirty_rate_high_cnt;
static void check_guest_throttling(void);

static uint64_t bitmap_sync_count;

/***********************************************************/
/* ram save/restore */

#define RAM_SAVE_FLAG_FULL     0x01 /* Obsolete, not used anymore */
#define RAM_SAVE_FLAG_COMPRESS 0x02
#define RAM_SAVE_FLAG_MEM_SIZE 0x04
#define RAM_SAVE_FLAG_PAGE     0x08
#define RAM_SAVE_FLAG_EOS      0x10
#define RAM_SAVE_FLAG_CONTINUE 0x20
#define RAM_SAVE_FLAG_XBZRLE   0x40
/* 0x80 is reserved in migration.h start with 0x100 next */

static struct defconfig_file {
    const char *filename;
    /* Indicates it is an user config file (disabled by -no-user-config) */
    bool userconfig;
} default_config_files[] = {
    { CONFIG_QEMU_CONFDIR "/qemu.conf",                   true },
    { CONFIG_QEMU_CONFDIR "/target-" TARGET_NAME ".conf", true },
    { NULL }, /* end of list */
};

static const uint8_t ZERO_TARGET_PAGE[TARGET_PAGE_SIZE];

int qemu_read_default_config_files(bool userconfig)
{
    int ret;
    struct defconfig_file *f;

    for (f = default_config_files; f->filename; f++) {
        if (!userconfig && f->userconfig) {
            continue;
        }
        ret = qemu_read_config_file(f->filename);
        if (ret < 0 && ret != -ENOENT) {
            return ret;
        }
    }

    return 0;
}

static inline bool is_zero_range(uint8_t *p, uint64_t size)
{
    return buffer_find_nonzero_offset(p, size) == size;
}

/* struct contains XBZRLE cache and a static page
   used by the compression */
static struct {
    /* buffer used for XBZRLE encoding */
    uint8_t *encoded_buf;
    /* buffer for storing page content */
    uint8_t *current_buf;
    /* Cache for XBZRLE, Protected by lock. */
    PageCache *cache;
    QemuMutex lock;
} XBZRLE;

/* buffer used for XBZRLE decoding */
static uint8_t *xbzrle_decoded_buf;

static void XBZRLE_cache_lock(void)
{
    if (migrate_use_xbzrle())
        qemu_mutex_lock(&XBZRLE.lock);
}

static void XBZRLE_cache_unlock(void)
{
    if (migrate_use_xbzrle())
        qemu_mutex_unlock(&XBZRLE.lock);
}

/*
 * called from qmp_migrate_set_cache_size in main thread, possibly while
 * a migration is in progress.
 * A running migration maybe using the cache and might finish during this
 * call, hence changes to the cache are protected by XBZRLE.lock().
 */
int64_t xbzrle_cache_resize(int64_t new_size)
{
    PageCache *new_cache;
    int64_t ret;

    if (new_size < TARGET_PAGE_SIZE) {
        return -1;
    }

    XBZRLE_cache_lock();

    if (XBZRLE.cache != NULL) {
        if (pow2floor(new_size) == migrate_xbzrle_cache_size()) {
            goto out_new_size;
        }
        new_cache = cache_init(new_size / TARGET_PAGE_SIZE,
                                        TARGET_PAGE_SIZE);
        if (!new_cache) {
            error_report("Error creating cache");
            ret = -1;
            goto out;
        }

        cache_fini(XBZRLE.cache);
        XBZRLE.cache = new_cache;
    }

out_new_size:
    ret = pow2floor(new_size);
out:
    XBZRLE_cache_unlock();
    return ret;
}

/* accounting for migration statistics */
typedef struct AccountingInfo {
    uint64_t dup_pages;
    uint64_t skipped_pages;
    uint64_t norm_pages;
    uint64_t iterations;
    uint64_t xbzrle_bytes;
    uint64_t xbzrle_pages;
    uint64_t xbzrle_cache_miss;
    double xbzrle_cache_miss_rate;
    uint64_t xbzrle_overflows;
} AccountingInfo;

static AccountingInfo acct_info;

static void acct_clear(void)
{
    memset(&acct_info, 0, sizeof(acct_info));
}

uint64_t dup_mig_bytes_transferred(void)
{
    return acct_info.dup_pages * TARGET_PAGE_SIZE;
}

uint64_t dup_mig_pages_transferred(void)
{
    return acct_info.dup_pages;
}

uint64_t skipped_mig_bytes_transferred(void)
{
    return acct_info.skipped_pages * TARGET_PAGE_SIZE;
}

uint64_t skipped_mig_pages_transferred(void)
{
    return acct_info.skipped_pages;
}

uint64_t norm_mig_bytes_transferred(void)
{
    return acct_info.norm_pages * TARGET_PAGE_SIZE;
}

uint64_t norm_mig_pages_transferred(void)
{
    return acct_info.norm_pages;
}

uint64_t xbzrle_mig_bytes_transferred(void)
{
    return acct_info.xbzrle_bytes;
}

uint64_t xbzrle_mig_pages_transferred(void)
{
    return acct_info.xbzrle_pages;
}

uint64_t xbzrle_mig_pages_cache_miss(void)
{
    return acct_info.xbzrle_cache_miss;
}

double xbzrle_mig_cache_miss_rate(void)
{
    return acct_info.xbzrle_cache_miss_rate;
}

uint64_t xbzrle_mig_pages_overflow(void)
{
    return acct_info.xbzrle_overflows;
}

/* This is the last block that we have visited serching for dirty pages
 */
static RAMBlock *last_seen_block;
/* This is the last block from where we have sent data */
static RAMBlock *last_sent_block;
static ram_addr_t last_offset;
static bool last_was_from_queue;
static unsigned long *migration_bitmap;
static uint64_t migration_dirty_pages;
static uint32_t last_version;
static bool ram_bulk_stage;

/**
 * save_page_header: Write page header to wire
 *
 * If this is the 1st block, it also writes the block identification
 *
 * Returns: Number of bytes written
 *
 * @f: QEMUFile where to send the data
 * @block: block that contains the page we want to send
 * @offset: offset inside the block for the page
 *          in the lower bits, it contains flags
 */
static size_t save_page_header(QEMUFile *f, RAMBlock *block, ram_addr_t offset)
{
    size_t size;

    qemu_put_be64(f, offset);
    size = 8;

    if (!(offset & RAM_SAVE_FLAG_CONTINUE)) {
        qemu_put_byte(f, strlen(block->idstr));
        qemu_put_buffer(f, (uint8_t *)block->idstr,
                        strlen(block->idstr));
        size += 1 + strlen(block->idstr);
    }
    return size;
}

/* Update the xbzrle cache to reflect a page that's been sent as all 0.
 * The important thing is that a stale (not-yet-0'd) page be replaced
 * by the new data.
 * As a bonus, if the page wasn't in the cache it gets added so that
 * when a small write is made into the 0'd page it gets XBZRLE sent
 */
static void xbzrle_cache_zero_page(ram_addr_t current_addr)
{
    if (ram_bulk_stage || !migrate_use_xbzrle()) {
        return;
    }

    /* We don't care if this fails to allocate a new cache page
     * as long as it updated an old one */
    cache_insert(XBZRLE.cache, current_addr, ZERO_TARGET_PAGE,
                 bitmap_sync_count);
}

#define ENCODING_FLAG_XBZRLE 0x1

/**
 * save_xbzrle_page: compress and send current page
 *
 * Returns: 1 means that we wrote the page
 *          0 means that page is identical to the one already sent
 *          -1 means that xbzrle would be longer than normal
 *
 * @f: QEMUFile where to send the data
 * @current_data:
 * @current_addr:
 * @block: block that contains the page we want to send
 * @offset: offset inside the block for the page
 * @last_stage: if we are at the completion stage
 * @bytes_transferred: increase it with the number of transferred bytes
 */
static int save_xbzrle_page(QEMUFile *f, uint8_t **current_data,
                            ram_addr_t current_addr, RAMBlock *block,
                            ram_addr_t offset, bool last_stage,
                            uint64_t *bytes_transferred)
{
    int encoded_len = 0, bytes_xbzrle;
    uint8_t *prev_cached_page;

    if (!cache_is_cached(XBZRLE.cache, current_addr, bitmap_sync_count)) {
        acct_info.xbzrle_cache_miss++;
        if (!last_stage) {
            if (cache_insert(XBZRLE.cache, current_addr, *current_data,
                             bitmap_sync_count) == -1) {
                return -1;
            } else {
                /* update *current_data when the page has been
                   inserted into cache */
                *current_data = get_cached_data(XBZRLE.cache, current_addr);
            }
        }
        return -1;
    }

    prev_cached_page = get_cached_data(XBZRLE.cache, current_addr);

    /* save current buffer into memory */
    memcpy(XBZRLE.current_buf, *current_data, TARGET_PAGE_SIZE);

    /* XBZRLE encoding (if there is no overflow) */
    encoded_len = xbzrle_encode_buffer(prev_cached_page, XBZRLE.current_buf,
                                       TARGET_PAGE_SIZE, XBZRLE.encoded_buf,
                                       TARGET_PAGE_SIZE);
    if (encoded_len == 0) {
        DPRINTF("Skipping unmodified page\n");
        return 0;
    } else if (encoded_len == -1) {
        DPRINTF("Overflow\n");
        acct_info.xbzrle_overflows++;
        /* update data in the cache */
        if (!last_stage) {
            memcpy(prev_cached_page, *current_data, TARGET_PAGE_SIZE);
            *current_data = prev_cached_page;
        }
        return -1;
    }

    /* we need to update the data in the cache, in order to get the same data */
    if (!last_stage) {
        memcpy(prev_cached_page, XBZRLE.current_buf, TARGET_PAGE_SIZE);
    }

    /* Send XBZRLE based compressed page */
    bytes_xbzrle = save_page_header(f, block, offset | RAM_SAVE_FLAG_XBZRLE);
    qemu_put_byte(f, ENCODING_FLAG_XBZRLE);
    qemu_put_be16(f, encoded_len);
    qemu_put_buffer(f, XBZRLE.encoded_buf, encoded_len);
    bytes_xbzrle += encoded_len + 1 + 2;
    acct_info.xbzrle_pages++;
    acct_info.xbzrle_bytes += bytes_xbzrle;
    *bytes_transferred += bytes_xbzrle;

    return 1;
}

/* mr: The region to search for dirty pages in
 * start: Start address (typically so we can continue from previous page)
 * ram_addr_abs: Pointer into which to store the address of the dirty page
 *               within the global ram_addr space
 *
 * Returns: byte offset within memory region of the start of a dirty page
 */
static inline
ram_addr_t migration_bitmap_find_and_reset_dirty(MemoryRegion *mr,
                                                 ram_addr_t start,
                                                 ram_addr_t *ram_addr_abs)
{
    unsigned long base = mr->ram_addr >> TARGET_PAGE_BITS;
    unsigned long nr = base + (start >> TARGET_PAGE_BITS);
    uint64_t mr_size = TARGET_PAGE_ALIGN(memory_region_size(mr));
    unsigned long size = base + (mr_size >> TARGET_PAGE_BITS);

    unsigned long next;

    if (ram_bulk_stage && nr > base) {
        next = nr + 1;
    } else {
        next = find_next_bit(migration_bitmap, size, nr);
    }

    if (next < size) {
        clear_bit(next, migration_bitmap);
        migration_dirty_pages--;
    }
    *ram_addr_abs = next << TARGET_PAGE_BITS;
    return (next - base) << TARGET_PAGE_BITS;
}

static inline bool migration_bitmap_set_dirty(ram_addr_t addr)
{
    bool ret;
    int nr = addr >> TARGET_PAGE_BITS;

    ret = test_and_set_bit(nr, migration_bitmap);

    if (!ret) {
        migration_dirty_pages++;
    }
    return ret;
}

static inline bool migration_bitmap_clear_dirty(ram_addr_t addr)
{
    bool ret;
    int nr = addr >> TARGET_PAGE_BITS;

    ret = test_and_clear_bit(nr, migration_bitmap);

    if (ret) {
        migration_dirty_pages--;
    }
    return ret;
}

static void migration_bitmap_sync_range(ram_addr_t start, ram_addr_t length)
{
    ram_addr_t addr;
    unsigned long page = BIT_WORD(start >> TARGET_PAGE_BITS);

    /* start address is aligned at the start of a word? */
    if (((page * BITS_PER_LONG) << TARGET_PAGE_BITS) == start) {
        int k;
        int nr = BITS_TO_LONGS(length >> TARGET_PAGE_BITS);
        unsigned long *src = ram_list.dirty_memory[DIRTY_MEMORY_MIGRATION];

        for (k = page; k < page + nr; k++) {
            if (src[k]) {
                unsigned long new_dirty;
                new_dirty = ~migration_bitmap[k];
                migration_bitmap[k] |= src[k];
                new_dirty &= src[k];
                migration_dirty_pages += ctpopl(new_dirty);
                src[k] = 0;
            }
        }
    } else {
        for (addr = 0; addr < length; addr += TARGET_PAGE_SIZE) {
            if (cpu_physical_memory_get_dirty(start + addr,
                                              TARGET_PAGE_SIZE,
                                              DIRTY_MEMORY_MIGRATION)) {
                cpu_physical_memory_reset_dirty(start + addr,
                                                TARGET_PAGE_SIZE,
                                                DIRTY_MEMORY_MIGRATION);
                migration_bitmap_set_dirty(start + addr);
            }
        }
    }
}


//Added @Kashif

uint64_t __get_dirty_pages(ram_addr_t start, ram_addr_t length) 
{
    ram_addr_t addr;
    unsigned long page = BIT_WORD(start >> TARGET_PAGE_BITS);
    uint64_t count = 0;
    /* start address is aligned at the start of a word? */
    if (((page * BITS_PER_LONG) << TARGET_PAGE_BITS) == start) {
        int k;
        int nr = BITS_TO_LONGS(length >> TARGET_PAGE_BITS);
        unsigned long *src = ram_list.dirty_memory[DIRTY_MEMORY_CODE];

        for (k = page; k < page + nr; k++) {
            if (src[k]) {
                count++;
                src[k] = 0;
            }
        }
    } else {
        for (addr = 0; addr < length; addr += TARGET_PAGE_SIZE) {
            if (cpu_physical_memory_get_dirty(start + addr,
                                              TARGET_PAGE_SIZE,
                                              DIRTY_MEMORY_CODE)) {
                cpu_physical_memory_reset_dirty(start + addr,
                                                TARGET_PAGE_SIZE,
                                                DIRTY_MEMORY_CODE);
                count++;
            }
        }
    }

    return count;
}

uint64_t get_dirty_pages(void)
{

	RAMBlock *block;
    	uint64_t retcount = 0;
    
    	//qemu_mutex_lock_iothread();
    	qemu_mutex_lock_ramlist();
	rcu_read_lock();
    	memory_global_dirty_log_start();
	address_space_sync_dirty_bitmap(&address_space_memory);
    	QLIST_FOREACH_RCU(block, &ram_list.blocks, next) {
        	retcount += __get_dirty_pages(block->mr->ram_addr, block->used_length);
    	}
    
    	memory_global_dirty_log_stop();
	rcu_read_unlock();
    	qemu_mutex_unlock_ramlist();
    	//qemu_mutex_unlock_iothread();
    	
	
	return retcount;
}

/*
uint64_t get_dirty_pages(void)
{

    RAMBlock *block;
    uint64_t retcount = 0;
    
    qemu_mutex_lock_iothread();
    qemu_mutex_lock_ramlist();
    rcu_read_lock();
    memory_global_dirty_log_start();
    
    QLIST_FOREACH_RCU(block, &ram_list.blocks, next) {
        retcount += __get_dirty_pages(block->mr->ram_addr, block->used_length);
    }
    
    memory_global_dirty_log_stop();
    qemu_mutex_unlock_ramlist();
    qemu_mutex_unlock_iothread();
    rcu_read_unlock();
    return retcount;

}
*/

/* Fix me: there are too many global variables used in migration process. */
static int64_t start_time;
static int64_t bytes_xfer_prev;
static int64_t num_dirty_pages_period;

static void migration_bitmap_sync_init(void)
{
    start_time = 0;
    bytes_xfer_prev = 0;
    num_dirty_pages_period = 0;
}

/* Called with iothread lock held, to protect ram_list.dirty_memory[] */
static void migration_bitmap_sync(void)
{
    RAMBlock *block;
    uint64_t num_dirty_pages_init = migration_dirty_pages;
    MigrationState *s = migrate_get_current();
    int64_t end_time;
    int64_t bytes_xfer_now;
    static uint64_t xbzrle_cache_miss_prev;
    static uint64_t iterations_prev;

    bitmap_sync_count++;

    if (!bytes_xfer_prev) {
        bytes_xfer_prev = ram_bytes_transferred();
    }

    if (!start_time) {
        start_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    }

    trace_migration_bitmap_sync_start();
    address_space_sync_dirty_bitmap(&address_space_memory);

    rcu_read_lock();
    QLIST_FOREACH_RCU(block, &ram_list.blocks, next) {
        migration_bitmap_sync_range(block->mr->ram_addr, block->used_length);
    }
    rcu_read_unlock();

    trace_migration_bitmap_sync_end(migration_dirty_pages
                                    - num_dirty_pages_init);
    num_dirty_pages_period += migration_dirty_pages - num_dirty_pages_init;
    end_time = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);

    /* more than 1 second = 1000 millisecons */
    if (end_time > start_time + 1000) {
        if (migrate_auto_converge()) {
            /* The following detection logic can be refined later. For now:
               Check to see if the dirtied bytes is 50% more than the approx.
               amount of bytes that just got transferred since the last time we
               were in this routine. If that happens >N times (for now N==4)
               we turn on the throttle down logic */
            bytes_xfer_now = ram_bytes_transferred();
            if (s->dirty_pages_rate &&
               (num_dirty_pages_period * TARGET_PAGE_SIZE >
                   (bytes_xfer_now - bytes_xfer_prev)/2) &&
               (dirty_rate_high_cnt++ > 4)) {
                    trace_migration_throttle();
                    mig_throttle_on = true;
                    dirty_rate_high_cnt = 0;
             }
             bytes_xfer_prev = bytes_xfer_now;
        } else {
             mig_throttle_on = false;
        }
        if (migrate_use_xbzrle()) {
            if (iterations_prev != 0) {
                acct_info.xbzrle_cache_miss_rate =
                   (double)(acct_info.xbzrle_cache_miss -
                            xbzrle_cache_miss_prev) /
                   (acct_info.iterations - iterations_prev);
            }
            iterations_prev = acct_info.iterations;
            xbzrle_cache_miss_prev = acct_info.xbzrle_cache_miss;
        }
        s->dirty_pages_rate = num_dirty_pages_period * 1000
            / (end_time - start_time);
        s->dirty_bytes_rate = s->dirty_pages_rate * TARGET_PAGE_SIZE;
        start_time = end_time;
        num_dirty_pages_period = 0;
        s->dirty_sync_count = bitmap_sync_count;
    }
}

static RAMBlock *ram_find_block(const char *id)
{
    RAMBlock *block;

    QLIST_FOREACH_RCU(block, &ram_list.blocks, next) {
        if (!strcmp(id, block->idstr)) {
            return block;
        }
    }

    return NULL;
}

/**
 * ram_save_page: Send the given page to the stream
 *
 * Returns: Number of pages written.
 *
 * @f: QEMUFile where to send the data
 * @block: block that contains the page we want to send
 * @offset: offset inside the block for the page
 * @last_stage: if we are at the completion stage
 * @bytes_transferred: increase it with the number of transferred bytes
 */
static int ram_save_page(QEMUFile *f, RAMBlock* block, ram_addr_t offset,
                         bool last_stage, uint64_t *bytes_transferred)
{
    int pages = -1;
    uint64_t bytes_xmit;
    ram_addr_t current_addr;
    MemoryRegion *mr = block->mr;
    uint8_t *p;
    int ret;
    bool send_async = true;

    p = memory_region_get_ram_ptr(mr) + offset;

    /* In doubt sent page as normal */
    bytes_xmit = 0;
    ret = ram_control_save_page(f, block->offset,
                           offset, TARGET_PAGE_SIZE, &bytes_xmit);
    if (bytes_xmit) {
        *bytes_transferred += bytes_xmit;
        pages = 1;
    }

    XBZRLE_cache_lock();

    current_addr = block->offset + offset;

    if (block == last_sent_block) {
        offset |= RAM_SAVE_FLAG_CONTINUE;
    }
    if (ret != RAM_SAVE_CONTROL_NOT_SUPP) {
        if (ret != RAM_SAVE_CONTROL_DELAYED) {
            if (bytes_xmit > 0) {
                acct_info.norm_pages++;
            } else if (bytes_xmit == 0) {
                acct_info.dup_pages++;
            }
        }
    } else if (is_zero_range(p, TARGET_PAGE_SIZE)) {
        acct_info.dup_pages++;
        *bytes_transferred += save_page_header(f, block,
                                               offset | RAM_SAVE_FLAG_COMPRESS);
        qemu_put_byte(f, 0);
        *bytes_transferred += 1;
        pages = 1;
        /* Must let xbzrle know, otherwise a previous (now 0'd) cached
         * page would be stale
         */
        xbzrle_cache_zero_page(current_addr);
    } else if (!ram_bulk_stage && migrate_use_xbzrle()) {
        pages = save_xbzrle_page(f, &p, current_addr, block,
                                 offset, last_stage, bytes_transferred);
        if (!last_stage) {
            /* Can't send this cached data async, since the cache page
             * might get updated before it gets to the wire
             */
            send_async = false;
        }
    }

    /* XBZRLE overflow or normal page */
    if (pages == -1) {
        *bytes_transferred += save_page_header(f, block,
                                               offset | RAM_SAVE_FLAG_PAGE);
        if (send_async) {
            qemu_put_buffer_async(f, p, TARGET_PAGE_SIZE);
        } else {
            qemu_put_buffer(f, p, TARGET_PAGE_SIZE);
        }
        *bytes_transferred += TARGET_PAGE_SIZE;
        pages = 1;
        acct_info.norm_pages++;
    }

    XBZRLE_cache_unlock();

    return pages;
}

/*
 * Unqueue a page from the queue fed by postcopy page requests
 *
 * Returns:      The RAMBlock* to transmit from (or NULL if the queue is empty)
 *      ms:      MigrationState in
 *  offset:      the byte offset within the RAMBlock for the start of the page
 * ram_addr_abs: global offset in the dirty/sent bitmaps
 */
static RAMBlock *ram_save_unqueue_page(MigrationState *ms, ram_addr_t *offset,
                                       ram_addr_t *ram_addr_abs)
{
    RAMBlock *result = NULL;
    qemu_mutex_lock(&ms->src_page_req_mutex);
    if (!QSIMPLEQ_EMPTY(&ms->src_page_requests)) {
        struct MigrationSrcPageRequest *entry =
                                    QSIMPLEQ_FIRST(&ms->src_page_requests);
        result = entry->rb;
        *offset = entry->offset;
        *ram_addr_abs = (entry->offset + entry->rb->offset) & TARGET_PAGE_MASK;

        if (entry->len > TARGET_PAGE_SIZE) {
            entry->len -= TARGET_PAGE_SIZE;
            entry->offset += TARGET_PAGE_SIZE;
        } else {
            memory_region_unref(result->mr);
            QSIMPLEQ_REMOVE_HEAD(&ms->src_page_requests, next_req);
            g_free(entry);
        }
    }
    qemu_mutex_unlock(&ms->src_page_req_mutex);

    return result;
}

/*
 * Queue the pages for transmission, e.g. a request from postcopy destination
 *   ms: MigrationStatus in which the queue is held
 *   rbname: The RAMBlock the request is for - may be NULL (to mean reuse last)
 *   start: Offset from the start of the RAMBlock
 *   len: Length (in bytes) to send
 *   Return: 0 on success
 */
int ram_save_queue_pages(MigrationState *ms, const char *rbname,
                         ram_addr_t start, ram_addr_t len)
{
    RAMBlock *ramblock;

    rcu_read_lock();
    if (!rbname) {
        /* Reuse last RAMBlock */
        ramblock = ms->last_req_rb;

        if (!ramblock) {
            /*
             * Shouldn't happen, we can't reuse the last RAMBlock if
             * it's the 1st request.
             */
            error_report("ram_save_queue_pages no previous block");
            goto err;
        }
    } else {
        ramblock = ram_find_block(rbname);

        if (!ramblock) {
            /* We shouldn't be asked for a non-existent RAMBlock */
            error_report("ram_save_queue_pages no block '%s'", rbname);
            goto err;
        }
    }
    trace_ram_save_queue_pages(ramblock->idstr, start, len);
    if (start+len > ramblock->used_length) {
        error_report("%s request overrun start=%zx len=%zx blocklen=%zx",
                     __func__, start, len, ramblock->used_length);
        goto err;
    }

    struct MigrationSrcPageRequest *new_entry =
        g_malloc0(sizeof(struct MigrationSrcPageRequest));
    new_entry->rb = ramblock;
    new_entry->offset = start;
    new_entry->len = len;
    ms->last_req_rb = ramblock;

    qemu_mutex_lock(&ms->src_page_req_mutex);
    memory_region_ref(ramblock->mr);
    QSIMPLEQ_INSERT_TAIL(&ms->src_page_requests, new_entry, next_req);
    qemu_mutex_unlock(&ms->src_page_req_mutex);
    rcu_read_unlock();

    return 0;

err:
    rcu_read_unlock();
    return -1;
}

/*
 * ram_find_and_save_block: Finds a dirty page and sends it to f
 *
 * Called within an RCU critical section.
 *
 * Returns:  The number of pages written
 *           0 means no dirty pages
 *
 * @f: QEMUFile where to send the data
 * @last_stage: if we are at the completion stage
 * @bytes_transferred: increase it with the number of transferred bytes
 */

static int ram_find_and_save_block(QEMUFile *f, bool last_stage,
                                   uint64_t *bytes_transferred)
{
    MigrationState *ms = migrate_get_current();
    RAMBlock *block = last_seen_block;
    RAMBlock *tmpblock;
    ram_addr_t offset = last_offset;
    ram_addr_t tmpoffset;
    bool complete_round = false;
    int pages = 0;
    ram_addr_t dirty_ram_abs; /* Address of the start of the dirty page in
                                 ram_addr_t space */

    if (!block) {
        block = QLIST_FIRST_RCU(&ram_list.blocks);
        last_was_from_queue = false;
    }

    while (true) { /* Until we send a block or run out of stuff to send */
        tmpblock = NULL;

        /*
         * Don't break host-page chunks up with queue items
         * so only unqueue if,
         *   a) The last item came from the queue anyway
         *   b) The last sent item was the last target-page in a host page
         */
        if (last_was_from_queue || !last_sent_block ||
            ((last_offset & ~qemu_host_page_mask) ==
             (qemu_host_page_size - TARGET_PAGE_SIZE))) {
            tmpblock = ram_save_unqueue_page(ms, &tmpoffset, &dirty_ram_abs);
        }

        if (tmpblock) {
            /* We've got a block from the postcopy queue */
            trace_ram_find_and_save_block_postcopy(tmpblock->idstr,
                                                   (uint64_t)tmpoffset,
                                                   (uint64_t)dirty_ram_abs);
            /*
             * We're sending this page, and since it's postcopy nothing else
             * will dirty it, and we must make sure it doesn't get sent again.
             */
            if (!migration_bitmap_clear_dirty(dirty_ram_abs)) {
                trace_ram_find_and_save_block_postcopy_not_dirty(
                    tmpblock->idstr, (uint64_t)tmpoffset,
                    (uint64_t)dirty_ram_abs,
                    test_bit(dirty_ram_abs >> TARGET_PAGE_BITS, ms->sentmap));

                continue;
            }
            /*
             * As soon as we start servicing pages out of order, then we have
             * to kill the bulk stage, since the bulk stage assumes
             * in (migration_bitmap_find_and_reset_dirty) that every page is
             * dirty, that's no longer true.
             */
            ram_bulk_stage = false;
            /*
             * We mustn't change block/offset unless it's to a valid one
             * otherwise we can go down some of the exit cases in the normal
             * path.
             */
            block = tmpblock;
            offset = tmpoffset;
            last_was_from_queue = true;
        } else {
            MemoryRegion *mr;
            /* priority queue empty, so just search for something dirty */
            mr = block->mr;
            offset = migration_bitmap_find_and_reset_dirty(mr, offset,
                                                           &dirty_ram_abs);
            if (complete_round && block == last_seen_block &&
                offset >= last_offset) {
                break;
            }
            if (offset >= block->used_length) {
                offset = 0;
                block = QLIST_NEXT_RCU(block, next);
                if (!block) {
                    block = QLIST_FIRST_RCU(&ram_list.blocks);
                    complete_round = true;
                    ram_bulk_stage = false;
                }
                continue; /* pick an offset in the new block */
            }
            last_was_from_queue = false;
        }

        /* We have a page to send, so send it */
        pages = ram_save_page(f, block, offset, last_stage,
                              bytes_transferred);

        /* if page is unmodified, continue to the next */
        if (pages > 0) {
            if (ms->sentmap) {
                set_bit(dirty_ram_abs >> TARGET_PAGE_BITS, ms->sentmap);
            }

            break;
        }
    }

    last_seen_block = block;
    last_offset = offset;

    return pages;
}

static uint64_t bytes_transferred;

void acct_update_position(QEMUFile *f, size_t size, bool zero)
{
    uint64_t pages = size / TARGET_PAGE_SIZE;
    if (zero) {
        acct_info.dup_pages += pages;
    } else {
        acct_info.norm_pages += pages;
        bytes_transferred += size;
        qemu_update_position(f, size);
    }
}

static ram_addr_t ram_save_remaining(void)
{
    return migration_dirty_pages;
}

uint64_t ram_bytes_remaining(void)
{
    return ram_save_remaining() * TARGET_PAGE_SIZE;
}

uint64_t ram_bytes_transferred(void)
{
    return bytes_transferred;
}

uint64_t ram_bytes_total(void)
{
    RAMBlock *block;
    uint64_t total = 0;

    rcu_read_lock();
    QLIST_FOREACH_RCU(block, &ram_list.blocks, next)
        total += block->used_length;
    rcu_read_unlock();
    return total;
}

void free_xbzrle_decoded_buf(void)
{
    g_free(xbzrle_decoded_buf);
    xbzrle_decoded_buf = NULL;
}

static void migration_end(void)
{
    MigrationState *s = migrate_get_current();

    if (migration_bitmap) {
        memory_global_dirty_log_stop();
        g_free(migration_bitmap);
        migration_bitmap = NULL;
    }

    if (s->sentmap) {
        g_free(s->sentmap);
        s->sentmap = NULL;
    }

    XBZRLE_cache_lock();
    if (XBZRLE.cache) {
        cache_fini(XBZRLE.cache);
        g_free(XBZRLE.encoded_buf);
        g_free(XBZRLE.current_buf);
        XBZRLE.cache = NULL;
        XBZRLE.encoded_buf = NULL;
        XBZRLE.current_buf = NULL;
    }
    XBZRLE_cache_unlock();
}

static void ram_migration_cancel(void *opaque)
{
    migration_end();
}

static void reset_ram_globals(void)
{
    last_seen_block = NULL;
    last_sent_block = NULL;
    last_offset = 0;
    last_version = ram_list.version;
    ram_bulk_stage = true;
    last_was_from_queue = false;
}

#define MAX_WAIT 50 /* ms, half buffered_file limit */

/* Each of ram_save_setup, ram_save_iterate and ram_save_complete has
 * long-running RCU critical section.  When rcu-reclaims in the code
 * start to become numerous it will be necessary to reduce the
 * granularity of these critical sections.
 */

/*
 * 'expected' is the value you expect the bitmap mostly to be full
 * of and it won't bother printing lines that are all this value
 * if 'todump' is null the migration bitmap is dumped.
 */
void ram_debug_dump_bitmap(unsigned long *todump, bool expected)
{
    int64_t ram_pages = last_ram_offset() >> TARGET_PAGE_BITS;

    int64_t cur;
    int64_t linelen = 128;
    char linebuf[129];

    if (!todump) {
        todump = migration_bitmap;
    }

    for (cur = 0; cur < ram_pages; cur += linelen) {
        int64_t curb;
        bool found = false;
        /*
         * Last line; catch the case where the line length
         * is longer than remaining ram
         */
        if (cur+linelen > ram_pages) {
            linelen = ram_pages - cur;
        }
        for (curb = 0; curb < linelen; curb++) {
            bool thisbit = test_bit(cur+curb, todump);
            linebuf[curb] = thisbit ? '1' : '.';
            found = found || (thisbit != expected);
        }
        if (found) {
            linebuf[curb] = '\0';
            fprintf(stderr,  "0x%08" PRIx64 " : %s\n", cur, linebuf);
        }
    }
}

/* **** functions for postcopy ***** */

/*
 * Callback from postcopy_each_ram_send_discard for each RAMBlock
 * start,end: Indexes into the bitmap for the first and last bit
 *            representing the named block
 */
static int postcopy_send_discard_bm_ram(MigrationState *ms,
                                        PostcopyDiscardState *pds,
                                        unsigned long start, unsigned long end)
{
    unsigned long current;

    for (current = start; current <= end; ) {
        unsigned long set = find_next_bit(ms->sentmap, end + 1, current);

        if (set <= end) {
            unsigned long zero = find_next_zero_bit(ms->sentmap,
                                                    end + 1, set + 1);

            if (zero > end) {
                zero = end + 1;
            }
            postcopy_discard_send_range(ms, pds, set, zero - 1);
            current = zero + 1;
        } else {
            current = set;
        }
    }

    return 0;
}

/*
 * Utility for the outgoing postcopy code.
 *   Calls postcopy_send_discard_bm_ram for each RAMBlock
 *   passing it bitmap indexes and name.
 * Returns: 0 on success
 * (qemu_ram_foreach_block ends up passing unscaled lengths
 *  which would mean postcopy code would have to deal with target page)
 */
static int postcopy_each_ram_send_discard(MigrationState *ms)
{
    struct RAMBlock *block;
    int ret;

    QLIST_FOREACH_RCU(block, &ram_list.blocks, next) {
        unsigned long first = block->offset >> TARGET_PAGE_BITS;
        unsigned long last = (block->offset + (block->max_length-1))
                                >> TARGET_PAGE_BITS;
        PostcopyDiscardState *pds = postcopy_discard_send_init(ms,
                                                               first,
                                                               block->idstr);

        /*
         * Postcopy sends chunks of bitmap over the wire, but it
         * just needs indexes at this point, avoids it having
         * target page specific code.
         */
        ret = postcopy_send_discard_bm_ram(ms, pds, first, last);
        postcopy_discard_send_finish(ms, pds);
        if (ret) {
            return ret;
        }
    }

    return 0;
}

/*
 * Helper for postcopy_chunk_hostpages where HPS/TPS >= bits-in-long
 *
 * !! Untested !!
 */
static int hostpage_big_chunk_helper(const char *block_name, void *host_addr,
                                     ram_addr_t offset, ram_addr_t length,
                                     void *opaque)
{
    MigrationState *ms = opaque;
    unsigned long long_bits = sizeof(long) * 8;
    unsigned int host_len = (qemu_host_page_size / TARGET_PAGE_SIZE) /
                            long_bits;
    unsigned long first_long, last_long, cur_long, current_hp;
    unsigned long first = offset >> TARGET_PAGE_BITS;
    unsigned long last = (offset + (length - 1)) >> TARGET_PAGE_BITS;

    PostcopyDiscardState *pds = postcopy_discard_send_init(ms,
                                                           first,
                                                           block_name);
    first_long = first / long_bits;
    last_long = last / long_bits;

    /*
     * I'm assuming RAMBlocks must start at the start of host pages,
     * but I guess they might not use the whole of the host page
     */

    /* Work along one host page at a time */
    for (current_hp = first_long; current_hp <= last_long;
         current_hp += host_len) {
        bool discard = 0;
        bool redirty = 0;
        bool has_some_dirty = false;
        bool has_some_undirty = false;
        bool has_some_sent = false;
        bool has_some_unsent = false;

        /*
         * Check each long of mask for this hp, and see if anything
         * needs updating.
         */
        for (cur_long = current_hp; cur_long < (current_hp + host_len);
             cur_long++) {
            /* a chunk of sent pages */
            unsigned long sdata = ms->sentmap[cur_long];
            /* a chunk of dirty pages */
            unsigned long ddata = migration_bitmap[cur_long];

            if (sdata) {
                has_some_sent = true;
            }
            if (sdata != ~0ul) {
                has_some_unsent = true;
            }
            if (ddata) {
                has_some_dirty = true;
            }
            if (ddata != ~0ul) {
                has_some_undirty = true;
            }

        }

        if (has_some_sent && has_some_unsent) {
            /* Partially sent host page */
            discard = true;
            redirty = true;
        }

        if (has_some_dirty && has_some_undirty) {
            /* Partially dirty host page */
            redirty = true;
        }

        if (!discard && !redirty) {
            /* All consistent - next host page */
            continue;
        }


        /* Now walk the chunks again, sending discards etc */
        for (cur_long = current_hp; cur_long < (current_hp + host_len);
             cur_long++) {
            unsigned long cur_bits = cur_long * long_bits;

            /* a chunk of sent pages */
            unsigned long sdata = ms->sentmap[cur_long];
            /* a chunk of dirty pages */
            unsigned long ddata = migration_bitmap[cur_long];

            if (discard && sdata) {
                /* Tell the destination to discard these pages */
                postcopy_discard_send_range(ms, pds, cur_bits,
                                            cur_bits + long_bits - 1);
                /* And clear them in the sent data structure */
                ms->sentmap[cur_long] = 0;
            }

            if (redirty) {
                migration_bitmap[cur_long] = ~0ul;
                /* Inc the count of dirty pages */
                migration_dirty_pages += ctpopl(~ddata);
            }
        }
    }

    postcopy_discard_send_finish(ms, pds);

    return 0;
}

/*
 * When working on long chunks of a bitmap where the only valid section
 * is between start..end (inclusive), generate a mask with only those
 * valid bits set for the current long word within that bitmask.
 */
static unsigned long make_long_mask(unsigned long start, unsigned long end,
                                    unsigned long cur_long)
{
    unsigned long long_bits = sizeof(long) * 8;
    unsigned long long_bits_mask = long_bits - 1;
    unsigned long first_long, last_long;
    unsigned long mask = ~(unsigned long)0;
    first_long = start / long_bits ;
    last_long = end / long_bits;

    if ((cur_long == first_long) && (start & long_bits_mask)) {
        /* e.g. (start & 31) = 3
         *         1 << .    -> 2^3
         *         . - 1     -> 2^3 - 1 i.e. mask 2..0
         *         ~.        -> mask 31..3
         */
        mask &= ~((((unsigned long)1) << (start & long_bits_mask)) - 1);
    }

    if ((cur_long == last_long) && ((end & long_bits_mask) != long_bits_mask)) {
        /* e.g. (end & 31) = 3
         *            .   +1 -> 4
         *         1 << .    -> 2^4
         *         . -1      -> 2^4 - 1
         *                   = mask set 3..0
         */
        mask &= (((unsigned long)1) << ((end & long_bits_mask) + 1)) - 1;
    }

    return mask;
}

/*
 * Utility for the outgoing postcopy code.
 *
 * Discard any partially sent host-page size chunks, mark any partially
 * dirty host-page size chunks as all dirty.
 *
 * Returns: 0 on success
 */
static int postcopy_chunk_hostpages(MigrationState *ms)
{
    struct RAMBlock *block;
    unsigned int host_bits = qemu_host_page_size / TARGET_PAGE_SIZE;
    unsigned long long_bits = sizeof(long) * 8;
    unsigned long host_mask;

    assert(is_power_of_2(host_bits));

    if (qemu_host_page_size == TARGET_PAGE_SIZE) {
        /* Easy case - TPS==HPS - nothing to be done */
        return 0;
    }

    /* Easiest way to make sure we don't resume in the middle of a host-page */
    last_seen_block = NULL;
    last_sent_block = NULL;

    /*
     * The currently worst known ratio is ARM that has 1kB target pages, and
     * can have 64kB host pages, which is thus inconveniently larger than a long
     * on ARM (32bits), and a long is the underlying element of the migration
     * bitmaps.
     */
    if (host_bits >= long_bits) {
        /* Deal with the odd case separately */
        return qemu_ram_foreach_block(hostpage_big_chunk_helper, ms);
    } else {
        host_mask =  (1ul << host_bits) - 1;
    }

    rcu_read_lock();
    QLIST_FOREACH_RCU(block, &ram_list.blocks, next) {
        unsigned long first_long, last_long, cur_long;
        unsigned long first = block->offset >> TARGET_PAGE_BITS;
        unsigned long last = (block->offset + (block->used_length - 1))
                                >> TARGET_PAGE_BITS;
        PostcopyDiscardState *pds = postcopy_discard_send_init(ms,
                                                               first,
                                                               block->idstr);

        first_long = first / long_bits;
        last_long = last / long_bits;
        for (cur_long = first_long; cur_long <= last_long; cur_long++) {
            unsigned long current_hp;
            /* Deal with start/end not on alignment */
            unsigned long mask = make_long_mask(first, last, cur_long);

            /* a chunk of sent pages */
            unsigned long sdata = ms->sentmap[cur_long];
            /* a chunk of dirty pages */
            unsigned long ddata = migration_bitmap[cur_long];
            unsigned long discard = 0;
            unsigned long redirty = 0;
            sdata &= mask;
            ddata &= mask;

            for (current_hp = 0; current_hp < long_bits;
                 current_hp += host_bits) {
                unsigned long host_sent = (sdata >> current_hp) & host_mask;
                unsigned long host_dirty = (ddata >> current_hp) & host_mask;

                if (host_sent && (host_sent != host_mask)) {
                    /* Partially sent host page */
                    redirty |= host_mask << current_hp;
                    discard |= host_mask << current_hp;

                    /* Tell the destination to discard this page */
                    postcopy_discard_send_range(ms, pds,
                             cur_long * long_bits + current_hp /* start */,
                             cur_long * long_bits + current_hp +
                                 host_bits - 1 /* end */);
                } else if (host_dirty && (host_dirty != host_mask)) {
                    /* Partially dirty host page */
                    redirty |= host_mask << current_hp;
                }
            }
            if (discard) {
                /* clear the page in the sentmap */
                ms->sentmap[cur_long] &= ~discard;
            }
            if (redirty) {
                /*
                 * Reread original dirty bits and OR in ones we clear; we
                 * must reread since we might be at the start or end of
                 * a RAMBlock that the original 'mask' discarded some
                 * bits from
                */
                ddata = migration_bitmap[cur_long];
                migration_bitmap[cur_long] = ddata | redirty;
                /* Inc the count of dirty pages */
                migration_dirty_pages += ctpopl(redirty - (ddata & redirty));
            }
        }

        postcopy_discard_send_finish(ms, pds);
    }

    rcu_read_unlock();
    return 0;
}

/*
 * Transmit the set of pages to be discarded after precopy to the target
 * these are pages that have been sent previously but have been dirtied
 * Hopefully this is pretty sparse
 */
int ram_postcopy_send_discard_bitmap(MigrationState *ms)
{
    int ret;

    rcu_read_lock();

    /* This should be our last sync, the src is now paused */
    migration_bitmap_sync();

    /* Deal with TPS != HPS */
    ret = postcopy_chunk_hostpages(ms);
    if (ret) {
        rcu_read_unlock();
        return ret;
    }

    /*
     * Update the sentmap to be  sentmap&=dirty
     */
    bitmap_and(ms->sentmap, ms->sentmap, migration_bitmap,
               last_ram_offset() >> TARGET_PAGE_BITS);


    trace_ram_postcopy_send_discard_bitmap();
#ifdef DEBUG_POSTCOPY
    ram_debug_dump_bitmap(ms->sentmap, false);
#endif

    ret = postcopy_each_ram_send_discard(ms);
    rcu_read_unlock();

    return ret;
}

/*
 * At the start of the postcopy phase of migration, any now-dirty
 * precopied pages are discarded.
 *
 * start..end is an inclusive byte address range within the RAMBlock
 *
 * Returns 0 on success.
 */
int ram_discard_range(MigrationIncomingState *mis,
                      const char *block_name,
                      uint64_t start, uint64_t end)
{
    int ret = -1;

    assert(end >= start);

    rcu_read_lock();
    RAMBlock *rb = ram_find_block(block_name);

    if (!rb) {
        error_report("ram_discard_range: Failed to find block '%s'",
                     block_name);
        goto err;
    }

    uint8_t *host_startaddr = rb->host + start;
    uint8_t *host_endaddr;

    if ((uintptr_t)host_startaddr & (qemu_host_page_size - 1)) {
        error_report("ram_discard_range: Unaligned start address: %p",
                     host_startaddr);
        goto err;
    }

    if (end <= rb->used_length) {
        host_endaddr   = rb->host + end;
        if (((uintptr_t)host_endaddr + 1) & (qemu_host_page_size - 1)) {
            error_report("ram_discard_range: Unaligned end address: %p",
                         host_endaddr);
            goto err;
        }
        ret = postcopy_ram_discard_range(mis, host_startaddr, host_endaddr);
    } else {
        error_report("ram_discard_range: Overrun block '%s' (%" PRIu64
                     "/%" PRIu64 "/%zu)",
                     block_name, start, end, rb->used_length);
    }

err:
    rcu_read_unlock();

    return ret;
}

static int ram_save_setup(QEMUFile *f, void *opaque)
{
    RAMBlock *block;
    int64_t ram_bitmap_pages; /* Size of bitmap in pages, including gaps */

    mig_throttle_on = false;
    dirty_rate_high_cnt = 0;
    bitmap_sync_count = 0;
    migration_bitmap_sync_init();

    if (migrate_use_xbzrle()) {
        XBZRLE_cache_lock();
        XBZRLE.cache = cache_init(migrate_xbzrle_cache_size() /
                                  TARGET_PAGE_SIZE,
                                  TARGET_PAGE_SIZE);
        if (!XBZRLE.cache) {
            XBZRLE_cache_unlock();
            error_report("Error creating cache");
            return -1;
        }
        XBZRLE_cache_unlock();

        /* We prefer not to abort if there is no memory */
        XBZRLE.encoded_buf = g_try_malloc0(TARGET_PAGE_SIZE);
        if (!XBZRLE.encoded_buf) {
            error_report("Error allocating encoded_buf");
            return -1;
        }

        XBZRLE.current_buf = g_try_malloc(TARGET_PAGE_SIZE);
        if (!XBZRLE.current_buf) {
            error_report("Error allocating current_buf");
            g_free(XBZRLE.encoded_buf);
            XBZRLE.encoded_buf = NULL;
            return -1;
        }

        acct_clear();
    }

    /* iothread lock needed for ram_list.dirty_memory[] */
    qemu_mutex_lock_iothread();
    qemu_mutex_lock_ramlist();
    rcu_read_lock();
    bytes_transferred = 0;
    reset_ram_globals();

    ram_bitmap_pages = last_ram_offset() >> TARGET_PAGE_BITS;
    migration_bitmap = bitmap_new(ram_bitmap_pages);
    bitmap_set(migration_bitmap, 0, ram_bitmap_pages);

    if (migrate_postcopy_ram()) {
        MigrationState *s = migrate_get_current();
        s->sentmap = bitmap_new(ram_bitmap_pages);
        bitmap_clear(s->sentmap, 0, ram_bitmap_pages);
    }

    /*
     * Count the total number of pages used by ram blocks not including any
     * gaps due to alignment or unplugs.
     */
    migration_dirty_pages = ram_bytes_total() >> TARGET_PAGE_BITS;

    memory_global_dirty_log_start();
    migration_bitmap_sync();
    qemu_mutex_unlock_ramlist();
    qemu_mutex_unlock_iothread();

    qemu_put_be64(f, ram_bytes_total() | RAM_SAVE_FLAG_MEM_SIZE);

    QLIST_FOREACH_RCU(block, &ram_list.blocks, next) {
        qemu_put_byte(f, strlen(block->idstr));
        qemu_put_buffer(f, (uint8_t *)block->idstr, strlen(block->idstr));
        qemu_put_be64(f, block->used_length);
    }

    rcu_read_unlock();

    ram_control_before_iterate(f, RAM_CONTROL_SETUP);
    ram_control_after_iterate(f, RAM_CONTROL_SETUP);

    qemu_put_be64(f, RAM_SAVE_FLAG_EOS);

    return 0;
}

static int ram_save_iterate(QEMUFile *f, void *opaque)
{
    int ret;
    int i;
    int64_t t0;
    int pages_sent = 0;

    rcu_read_lock();
    if (ram_list.version != last_version) {
        reset_ram_globals();
    }

    /* Read version before ram_list.blocks */
    smp_rmb();

    ram_control_before_iterate(f, RAM_CONTROL_ROUND);

    t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
    i = 0;
    while ((ret = qemu_file_rate_limit(f)) == 0) {
        int pages;

        pages = ram_find_and_save_block(f, false, &bytes_transferred);
        /* no more pages to sent */
        if (pages == 0) {
            break;
        }
        pages_sent += pages;
        acct_info.iterations++;
        check_guest_throttling();
        /* we want to check in the 1st loop, just in case it was the 1st time
           and we had to sync the dirty bitmap.
           qemu_get_clock_ns() is a bit expensive, so we only check each some
           iterations
        */
        if ((i & 63) == 0) {
            uint64_t t1 = (qemu_clock_get_ns(QEMU_CLOCK_REALTIME) - t0) / 1000000;
            if (t1 > MAX_WAIT) {
                DPRINTF("big wait: %" PRIu64 " milliseconds, %d iterations\n",
                        t1, i);
                break;
            }
        }
        i++;
    }
    rcu_read_unlock();

    /*
     * Must occur before EOS (or any QEMUFile operation)
     * because of RDMA protocol.
     */
    ram_control_after_iterate(f, RAM_CONTROL_ROUND);

    qemu_put_be64(f, RAM_SAVE_FLAG_EOS);
    bytes_transferred += 8;

    ret = qemu_file_get_error(f);
    if (ret < 0) {
        return ret;
    }

    return pages_sent;
}

/* Called with iothread lock */
static int ram_save_complete(QEMUFile *f, void *opaque)
{
    rcu_read_lock();

    if (!migration_postcopy_phase(migrate_get_current())) {
        migration_bitmap_sync();
    }

    ram_control_before_iterate(f, RAM_CONTROL_FINISH);

    /* try transferring iterative blocks of memory */

    /* flush all remaining blocks regardless of rate limiting */
    while (true) {
        int pages;

        pages = ram_find_and_save_block(f, true, &bytes_transferred);
        /* no more blocks to sent */
        if (pages == 0) {
            break;
        }
    }

    ram_control_after_iterate(f, RAM_CONTROL_FINISH);
    migration_end();

    rcu_read_unlock();
    qemu_put_be64(f, RAM_SAVE_FLAG_EOS);

    return 0;
}

static void ram_save_pending(QEMUFile *f, void *opaque, uint64_t max_size,
                             uint64_t *non_postcopiable_pending,
                             uint64_t *postcopiable_pending)
{
    uint64_t remaining_size;

    remaining_size = ram_save_remaining() * TARGET_PAGE_SIZE;

    if (!migration_postcopy_phase(migrate_get_current()) &&
        remaining_size < max_size) {
        qemu_mutex_lock_iothread();
        rcu_read_lock();
        migration_bitmap_sync();
        rcu_read_unlock();
        qemu_mutex_unlock_iothread();
        remaining_size = ram_save_remaining() * TARGET_PAGE_SIZE;
    }

    *non_postcopiable_pending = 0;
    *postcopiable_pending = remaining_size;
}

static int load_xbzrle(QEMUFile *f, ram_addr_t addr, void *host)
{
    unsigned int xh_len;
    int xh_flags;

    if (!xbzrle_decoded_buf) {
        xbzrle_decoded_buf = g_malloc(TARGET_PAGE_SIZE);
    }

    /* extract RLE header */
    xh_flags = qemu_get_byte(f);
    xh_len = qemu_get_be16(f);

    if (xh_flags != ENCODING_FLAG_XBZRLE) {
        error_report("Failed to load XBZRLE page - wrong compression!");
        return -1;
    }

    if (xh_len > TARGET_PAGE_SIZE) {
        error_report("Failed to load XBZRLE page - len overflow!");
        return -1;
    }
    /* load data and decode */
    qemu_get_buffer(f, xbzrle_decoded_buf, xh_len);

    /* decode RLE */
    if (xbzrle_decode_buffer(xbzrle_decoded_buf, xh_len, host,
                             TARGET_PAGE_SIZE) == -1) {
        error_report("Failed to load XBZRLE page - decode error!");
        return -1;
    }

    return 0;
}

/* Must be called from within a rcu critical section.
 * Returns a pointer from within the RCU-protected ram_list.
 */
/*
 * Read a RAMBlock ID from the stream f, find the host address of the
 * start of that block and add on 'offset'
 *
 * f: Stream to read from
 * mis: MigrationIncomingState
 * offset: Offset within the block
 * flags: Page flags (mostly to see if it's a continuation of previous block)
 */
static inline void *host_from_stream_offset(QEMUFile *f,
                                            MigrationIncomingState *mis,
                                            ram_addr_t offset,
                                            int flags)
{
    static RAMBlock *block = NULL;
    char id[256];
    uint8_t len;

    if (flags & RAM_SAVE_FLAG_CONTINUE) {
        if (!block || block->max_length <= offset) {
            error_report("Ack, bad migration stream!");
            return NULL;
        }
        return memory_region_get_ram_ptr(block->mr) + offset;
    }

    len = qemu_get_byte(f);
    qemu_get_buffer(f, (uint8_t *)id, len);
    id[len] = 0;

    QLIST_FOREACH_RCU(block, &ram_list.blocks, next) {
        if (!strncmp(id, block->idstr, sizeof(id)) &&
            block->max_length > offset) {
            return memory_region_get_ram_ptr(block->mr) + offset;
        }
    }

    error_report("Can't find block %s!", id);
    return NULL;
}

/*
 * If a page (or a whole RDMA chunk) has been
 * determined to be zero, then zap it.
 */
void ram_handle_compressed(void *host, uint8_t ch, uint64_t size)
{
    if (ch != 0 || !is_zero_range(host, size)) {
        memset(host, ch, size);
    }
}

/*
 * Allocate data structures etc needed by incoming migration with postcopy-ram
 * postcopy-ram's similarly names postcopy_ram_incoming_init does the work
 */
int ram_postcopy_incoming_init(MigrationIncomingState *mis)
{
    size_t ram_pages = last_ram_offset() >> TARGET_PAGE_BITS;

    return postcopy_ram_incoming_init(mis, ram_pages);
}

static int ram_load(QEMUFile *f, void *opaque, int version_id)
{
    int flags = 0, ret = 0;
    static uint64_t seq_iter;
    /*
     * System is running in postcopy mode, page inserts to host memory must be
     * atomic
     */
    MigrationIncomingState *mis = migration_incoming_get_current();
    bool postcopy_running = postcopy_state_get(mis) >=
                            POSTCOPY_INCOMING_LISTENING;
    void *postcopy_host_page = NULL;
    bool postcopy_place_needed = false;
    bool matching_page_sizes = qemu_host_page_size == TARGET_PAGE_SIZE;

    seq_iter++;

    if (version_id != 4) {
        ret = -EINVAL;
    }

    /* This RCU critical section can be very long running.
     * When RCU reclaims in the code start to become numerous,
     * it will be necessary to reduce the granularity of this
     * critical section.
     */
    rcu_read_lock();
    while (!ret && !(flags & RAM_SAVE_FLAG_EOS)) {
        ram_addr_t addr, total_ram_bytes;
        void *host = 0;
        void *page_buffer = 0;
        void *postcopy_place_source = 0;
        uint8_t ch;
        bool all_zero = false;

        addr = qemu_get_be64(f);
        flags = addr & ~TARGET_PAGE_MASK;
        addr &= TARGET_PAGE_MASK;

        if (flags & (RAM_SAVE_FLAG_COMPRESS | RAM_SAVE_FLAG_PAGE |
                     RAM_SAVE_FLAG_XBZRLE)) {
            host = host_from_stream_offset(f, mis, addr, flags);
            if (!host) {
                error_report("Illegal RAM offset " RAM_ADDR_FMT, addr);
                ret = -EINVAL;
                break;
            }
            if (!postcopy_running) {
                page_buffer = host;
            } else {
                /*
                 * Postcopy requires that we place whole host pages atomically.
                 * To make it atomic, the data is read into a temporary page
                 * that's moved into place later.
                 * The migration protocol uses,  possibly smaller, target-pages
                 * however the source ensures it always sends all the components
                 * of a host page in order.
                 */
                if (!postcopy_host_page) {
                    postcopy_host_page = postcopy_get_tmp_page(mis);
                }
                page_buffer = postcopy_host_page +
                              ((uintptr_t)host & ~qemu_host_page_mask);
                /* If all TP are zero then we can optimise the place */
                if (!((uintptr_t)host & ~qemu_host_page_mask)) {
                    all_zero = true;
                }

                /*
                 * If it's the last part of a host page then we place the host
                 * page
                 */
                postcopy_place_needed = (((uintptr_t)host + TARGET_PAGE_SIZE) &
                                         ~qemu_host_page_mask) == 0;
                postcopy_place_source = postcopy_host_page;
            }
        } else {
            postcopy_place_needed = false;
        }

        switch (flags & ~RAM_SAVE_FLAG_CONTINUE) {
        case RAM_SAVE_FLAG_MEM_SIZE:
            /* Synchronize RAM block list */
            total_ram_bytes = addr;
            while (!ret && total_ram_bytes) {
                RAMBlock *block;
                char id[256];
                ram_addr_t length;

                qemu_get_counted_string(f, id);
                length = qemu_get_be64(f);

                QLIST_FOREACH_RCU(block, &ram_list.blocks, next) {
                    if (!strncmp(id, block->idstr, sizeof(id))) {
                        if (length != block->used_length) {
                            Error *local_err = NULL;

                            ret = qemu_ram_resize(block->offset, length, &local_err);
                            if (local_err) {
                                error_report_err(local_err);
                            }
                        }
                        break;
                    }
                }

                if (!block) {
                    error_report("Unknown ramblock \"%s\", cannot "
                                 "accept migration", id);
                    ret = -EINVAL;
                }

                total_ram_bytes -= length;
            }
            break;
        case RAM_SAVE_FLAG_COMPRESS:
            ch = qemu_get_byte(f);
            if (!postcopy_running) {
                ram_handle_compressed(host, ch, TARGET_PAGE_SIZE);
            } else {
                memset(page_buffer, ch, TARGET_PAGE_SIZE);
                if (ch) {
                    all_zero = false;
                }
            }
            break;

        case RAM_SAVE_FLAG_PAGE:
            all_zero = false;
            if (!postcopy_place_needed || !matching_page_sizes) {
                qemu_get_buffer(f, page_buffer, TARGET_PAGE_SIZE);
            } else {
                /* Avoids the qemu_file copy during postcopy, which is
                 * going to do a copy later; can only do it when we
                 * do this read in one go (matching page sizes)
                 */
                qemu_get_buffer_less_copy(f, (uint8_t **)&postcopy_place_source,
                                          TARGET_PAGE_SIZE);
            }
            break;

        case RAM_SAVE_FLAG_XBZRLE:
            all_zero = false;
            if (postcopy_running) {
                error_report("XBZRLE RAM block in postcopy mode @%zx\n", addr);
                return -EINVAL;
            }
            if (load_xbzrle(f, addr, host) < 0) {
                error_report("Failed to decompress XBZRLE page at "
                             RAM_ADDR_FMT, addr);
                ret = -EINVAL;
                break;
            }
            break;
        case RAM_SAVE_FLAG_EOS:
            /* normal exit */
            break;
        default:
            if (flags & RAM_SAVE_FLAG_HOOK) {
                ram_control_load_hook(f, flags);
            } else {
                error_report("Unknown combination of migration flags: %#x",
                             flags);
                ret = -EINVAL;
            }
        }

        if (postcopy_place_needed) {
            /* This gets called at the last target page in the host page */
            ret = postcopy_place_page(mis, host + TARGET_PAGE_SIZE -
                                           qemu_host_page_size,
                                      postcopy_place_source,
                                      all_zero);
        }
        if (!ret) {
            ret = qemu_file_get_error(f);
        }
    }

    rcu_read_unlock();
    DPRINTF("Completed load of VM with exit code %d seq iteration "
            "%" PRIu64 "\n", ret, seq_iter);
    return ret;
}

static SaveVMHandlers savevm_ram_handlers = {
    .save_live_setup = ram_save_setup,
    .save_live_iterate = ram_save_iterate,
    .save_live_complete_postcopy = ram_save_complete,
    .save_live_complete_precopy = ram_save_complete,
    .save_live_pending = ram_save_pending,
    .load_state = ram_load,
    .cancel = ram_migration_cancel,
};

void ram_mig_init(void)
{
    qemu_mutex_init(&XBZRLE.lock);
    register_savevm_live(NULL, "ram", 0, 4, &savevm_ram_handlers, NULL);
}

struct soundhw {
    const char *name;
    const char *descr;
    int enabled;
    int isa;
    union {
        int (*init_isa) (ISABus *bus);
        int (*init_pci) (PCIBus *bus);
    } init;
};

static struct soundhw soundhw[9];
static int soundhw_count;

void isa_register_soundhw(const char *name, const char *descr,
                          int (*init_isa)(ISABus *bus))
{
    assert(soundhw_count < ARRAY_SIZE(soundhw) - 1);
    soundhw[soundhw_count].name = name;
    soundhw[soundhw_count].descr = descr;
    soundhw[soundhw_count].isa = 1;
    soundhw[soundhw_count].init.init_isa = init_isa;
    soundhw_count++;
}

void pci_register_soundhw(const char *name, const char *descr,
                          int (*init_pci)(PCIBus *bus))
{
    assert(soundhw_count < ARRAY_SIZE(soundhw) - 1);
    soundhw[soundhw_count].name = name;
    soundhw[soundhw_count].descr = descr;
    soundhw[soundhw_count].isa = 0;
    soundhw[soundhw_count].init.init_pci = init_pci;
    soundhw_count++;
}

void select_soundhw(const char *optarg)
{
    struct soundhw *c;

    if (is_help_option(optarg)) {
    show_valid_cards:

        if (soundhw_count) {
             printf("Valid sound card names (comma separated):\n");
             for (c = soundhw; c->name; ++c) {
                 printf ("%-11s %s\n", c->name, c->descr);
             }
             printf("\n-soundhw all will enable all of the above\n");
        } else {
             printf("Machine has no user-selectable audio hardware "
                    "(it may or may not have always-present audio hardware).\n");
        }
        exit(!is_help_option(optarg));
    }
    else {
        size_t l;
        const char *p;
        char *e;
        int bad_card = 0;

        if (!strcmp(optarg, "all")) {
            for (c = soundhw; c->name; ++c) {
                c->enabled = 1;
            }
            return;
        }

        p = optarg;
        while (*p) {
            e = strchr(p, ',');
            l = !e ? strlen(p) : (size_t) (e - p);

            for (c = soundhw; c->name; ++c) {
                if (!strncmp(c->name, p, l) && !c->name[l]) {
                    c->enabled = 1;
                    break;
                }
            }

            if (!c->name) {
                if (l > 80) {
                    error_report("Unknown sound card name (too big to show)");
                }
                else {
                    error_report("Unknown sound card name `%.*s'",
                                 (int) l, p);
                }
                bad_card = 1;
            }
            p += l + (e != NULL);
        }

        if (bad_card) {
            goto show_valid_cards;
        }
    }
}

void audio_init(void)
{
    struct soundhw *c;
    ISABus *isa_bus = (ISABus *) object_resolve_path_type("", TYPE_ISA_BUS, NULL);
    PCIBus *pci_bus = (PCIBus *) object_resolve_path_type("", TYPE_PCI_BUS, NULL);

    for (c = soundhw; c->name; ++c) {
        if (c->enabled) {
            if (c->isa) {
                if (!isa_bus) {
                    error_report("ISA bus not available for %s", c->name);
                    exit(1);
                }
                c->init.init_isa(isa_bus);
            } else {
                if (!pci_bus) {
                    error_report("PCI bus not available for %s", c->name);
                    exit(1);
                }
                c->init.init_pci(pci_bus);
            }
        }
    }
}

int qemu_uuid_parse(const char *str, uint8_t *uuid)
{
    int ret;

    if (strlen(str) != 36) {
        return -1;
    }

    ret = sscanf(str, UUID_FMT, &uuid[0], &uuid[1], &uuid[2], &uuid[3],
                 &uuid[4], &uuid[5], &uuid[6], &uuid[7], &uuid[8], &uuid[9],
                 &uuid[10], &uuid[11], &uuid[12], &uuid[13], &uuid[14],
                 &uuid[15]);

    if (ret != 16) {
        return -1;
    }
    return 0;
}

void do_acpitable_option(const QemuOpts *opts)
{
#ifdef TARGET_I386
    Error *err = NULL;

    acpi_table_add(opts, &err);
    if (err) {
        error_report("Wrong acpi table provided: %s",
                     error_get_pretty(err));
        error_free(err);
        exit(1);
    }
#endif
}

void do_smbios_option(QemuOpts *opts)
{
#ifdef TARGET_I386
    smbios_entry_add(opts);
#endif
}

void cpudef_init(void)
{
#if defined(cpudef_setup)
    cpudef_setup(); /* parse cpu definitions in target config file */
#endif
}

int kvm_available(void)
{
#ifdef CONFIG_KVM
    return 1;
#else
    return 0;
#endif
}

int xen_available(void)
{
#ifdef CONFIG_XEN
    return 1;
#else
    return 0;
#endif
}


TargetInfo *qmp_query_target(Error **errp)
{
    TargetInfo *info = g_malloc0(sizeof(*info));

    info->arch = g_strdup(TARGET_NAME);

    return info;
}

/* Stub function that's gets run on the vcpu when its brought out of the
   VM to run inside qemu via async_run_on_cpu()*/
static void mig_sleep_cpu(void *opq)
{
    qemu_mutex_unlock_iothread();
    g_usleep(30*1000);
    qemu_mutex_lock_iothread();
}

/* To reduce the dirty rate explicitly disallow the VCPUs from spending
   much time in the VM. The migration thread will try to catchup.
   Workload will experience a performance drop.
*/
static void mig_throttle_guest_down(void)
{
    CPUState *cpu;

    qemu_mutex_lock_iothread();
    CPU_FOREACH(cpu) {
        async_run_on_cpu(cpu, mig_sleep_cpu, NULL);
    }
    qemu_mutex_unlock_iothread();
}

static void check_guest_throttling(void)
{
    static int64_t t0;
    int64_t        t1;

    if (!mig_throttle_on) {
        return;
    }

    if (!t0)  {
        t0 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);
        return;
    }

    t1 = qemu_clock_get_ns(QEMU_CLOCK_REALTIME);

    /* If it has been more than 40 ms since the last time the guest
     * was throttled then do it again.
     */
    if (40 < (t1-t0)/1000000) {
        mig_throttle_guest_down();
        t0 = t1;
    }
}
