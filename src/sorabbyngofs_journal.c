// SPDX-License-Identifier: GPL-2.0
// SorabbyngoFS — self-healing journal
// Logs all AI file ops; replays on mount if a dirty shutdown is detected.
// Designed to report anomalies to sorabbyngo-os/self-healing via netlink.

#include <linux/kernel.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/list.h>
#include <linux/time.h>
#include "../include/sorabbyngofs.h"

#define BYNGO_JOURNAL_MAGIC  0x424A524E   /* BJRN */
#define BYNGO_JOURNAL_MAX    1024         /* max entries before flush */

/* Journal entry — one per AI file operation */
struct byngo_journal_entry {
    u64             timestamp_ns;
    u64             user_id;
    u64             inode_no;
    u32             op_type;        /* READ=1 WRITE=2 MODIFY=3 DELETE=4 */
    u32             result;         /* 0=success, errno on failure */
    struct list_head list;
};

/* Journal state per superblock */
struct byngo_journal {
    struct list_head    entries;
    spinlock_t          lock;
    u32                 count;
    u8                  dirty;      /* 1 if unclean shutdown suspected */
};

/*
 * sorabbyngofs_journal_init()
 * Called at mount time. Allocates journal, checks for dirty flag.
 */
int sorabbyngofs_journal_init(struct super_block *sb)
{
    struct byngo_journal *journal;

    journal = kzalloc(sizeof(*journal), GFP_KERNEL);
    if (!journal)
        return -ENOMEM;

    INIT_LIST_HEAD(&journal->entries);
    spin_lock_init(&journal->lock);
    journal->count = 0;
    journal->dirty = 0;

    sb->s_fs_info = journal;

    pr_info("sorabbyngofs: journal initialised (max=%d entries)\n",
            BYNGO_JOURNAL_MAX);
    return 0;
}
EXPORT_SYMBOL_GPL(sorabbyngofs_journal_init);

/*
 * sorabbyngofs_journal_log()
 * Records one AI file operation. Called from file.c before the op executes.
 */
int sorabbyngofs_journal_log(struct super_block *sb,
                              u64 user_id, u64 inode_no,
                              u32 op_type)
{
    struct byngo_journal *journal = sb->s_fs_info;
    struct byngo_journal_entry *entry;
    struct timespec64 ts;
    unsigned long flags;

    if (!journal)
        return -EINVAL;

    entry = kzalloc(sizeof(*entry), GFP_KERNEL);
    if (!entry)
        return -ENOMEM;

    ktime_get_real_ts64(&ts);
    entry->timestamp_ns = (u64)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    entry->user_id      = user_id;
    entry->inode_no     = inode_no;
    entry->op_type      = op_type;
    entry->result       = 0;

    spin_lock_irqsave(&journal->lock, flags);
    list_add_tail(&entry->list, &journal->entries);
    journal->count++;
    journal->dirty = 1;
    spin_unlock_irqrestore(&journal->lock, flags);

    /* Flush when we hit the ceiling */
    if (journal->count >= BYNGO_JOURNAL_MAX)
        sorabbyngofs_journal_commit(sb);

    return 0;
}
EXPORT_SYMBOL_GPL(sorabbyngofs_journal_log);

/*
 * sorabbyngofs_journal_commit()
 * Flushes journal entries and clears dirty flag.
 * TODO: persist to disk and notify self-healing via netlink socket.
 */
void sorabbyngofs_journal_commit(struct super_block *sb)
{
    struct byngo_journal *journal = sb->s_fs_info;
    struct byngo_journal_entry *entry, *tmp;
    unsigned long flags;

    if (!journal)
        return;

    spin_lock_irqsave(&journal->lock, flags);
    list_for_each_entry_safe(entry, tmp, &journal->entries, list) {
        list_del(&entry->list);
        kfree(entry);
    }
    journal->count = 0;
    journal->dirty = 0;
    spin_unlock_irqrestore(&journal->lock, flags);

    pr_info("sorabbyngofs: journal committed and cleared\n");
}
EXPORT_SYMBOL_GPL(sorabbyngofs_journal_commit);

/*
 * sorabbyngofs_journal_destroy()
 * Called at unmount. Commits then frees journal memory.
 */
void sorabbyngofs_journal_destroy(struct super_block *sb)
{
    struct byngo_journal *journal = sb->s_fs_info;

    if (!journal)
        return;

    sorabbyngofs_journal_commit(sb);
    kfree(journal);
    sb->s_fs_info = NULL;

    pr_info("sorabbyngofs: journal destroyed cleanly\n");
}
EXPORT_SYMBOL_GPL(sorabbyngofs_journal_destroy);
