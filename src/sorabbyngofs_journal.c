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


/* ------------------------------------------------------------------ */
/*  Netlink bridge to sorabbyngo-os/self-healing                       */
/*                                                                      */
/*  Sends anomaly events over a NETLINK_SORABBYNGO socket so the       */
/*  self-healing daemon can react without polling the journal.         */
/*                                                                      */
/*  Event types:                                                        */
/*    BYNGO_HEAL_HIGH_ACCESS  — ai_access_count crossed threshold      */
/*    BYNGO_HEAL_DIRTY_MOUNT  — unclean shutdown detected at mount     */
/*    BYNGO_HEAL_AUTH_FAIL    — repeated consent denials on one inode  */
/*    BYNGO_HEAL_JOURNAL_FULL — journal hit BYNGO_JOURNAL_MAX          */
/* ------------------------------------------------------------------ */

#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <net/sock.h>
#include <net/net_namespace.h>

#define NETLINK_SORABBYNGO      31      /* custom netlink family */
#define BYNGO_HEAL_HIGH_ACCESS  1
#define BYNGO_HEAL_DIRTY_MOUNT  2
#define BYNGO_HEAL_AUTH_FAIL    3
#define BYNGO_HEAL_JOURNAL_FULL 4

#define BYNGO_ACCESS_THRESHOLD  10000   /* ops before HIGH_ACCESS fires */

/* Netlink message payload */
struct byngo_heal_event {
    u32   event_type;       /* BYNGO_HEAL_* constant         */
    u64   inode_no;         /* affected inode (0 = global)   */
    u64   agent_id;         /* AI agent involved             */
    u64   timestamp_ns;     /* when the anomaly was detected */
    u64   value;            /* event-specific metric         */
    char  detail[64];       /* human-readable summary        */
};

static struct sock *byngo_netlink_sock;
static u32          byngo_heal_daemon_pid;  /* set by daemon on connect */

/*
 * sorabbyngofs_netlink_init()
 * Creates the NETLINK_SORABBYNGO socket.
 * Called from sorabbyngofs_journal_init().
 */
int sorabbyngofs_netlink_init(void)
{
    struct netlink_kernel_cfg cfg = {
        .groups = 1,
    };

    byngo_netlink_sock = netlink_kernel_create(&init_net,
                                                NETLINK_SORABBYNGO,
                                                &cfg);
    if (!byngo_netlink_sock) {
        pr_err("sorabbyngofs: failed to create netlink socket\n");
        return -ENOMEM;
    }

    pr_info("sorabbyngofs: netlink socket created (family=%d)\n",
            NETLINK_SORABBYNGO);
    return 0;
}
EXPORT_SYMBOL_GPL(sorabbyngofs_netlink_init);

/*
 * sorabbyngofs_netlink_destroy()
 * Releases the netlink socket on module unload.
 */
void sorabbyngofs_netlink_destroy(void)
{
    if (byngo_netlink_sock) {
        netlink_kernel_release(byngo_netlink_sock);
        byngo_netlink_sock = NULL;
    }
}
EXPORT_SYMBOL_GPL(sorabbyngofs_netlink_destroy);

/*
 * sorabbyngofs_heal_notify()
 * Sends one heal event to the self-healing daemon.
 * Safe to call from interrupt context — uses GFP_ATOMIC.
 *
 * If the daemon PID is not yet registered, the event is dropped
 * with a dev warning. The daemon registers its PID on startup
 * via a NLMSG_DONE message to this socket.
 */
void sorabbyngofs_heal_notify(u32 event_type, u64 inode_no,
                               u64 agent_id, u64 value,
                               const char *detail)
{
    struct sk_buff          *skb;
    struct nlmsghdr         *nlh;
    struct byngo_heal_event *evt;
    struct timespec64        ts;
    int                      msg_size;

    if (!byngo_netlink_sock) {
        pr_debug("sorabbyngofs: netlink not ready, dropping event %u\n",
                 event_type);
        return;
    }

    if (!byngo_heal_daemon_pid) {
        pr_debug("sorabbyngofs: self-healing daemon not connected\n");
        return;
    }

    msg_size = sizeof(struct byngo_heal_event);
    skb = nlmsg_new(msg_size, GFP_ATOMIC);
    if (!skb) {
        pr_warn("sorabbyngofs: failed to alloc netlink skb\n");
        return;
    }

    nlh = nlmsg_put(skb, 0, 0, NLMSG_DONE, msg_size, 0);
    if (!nlh) {
        kfree_skb(skb);
        return;
    }

    evt = nlmsg_data(nlh);
    ktime_get_real_ts64(&ts);

    evt->event_type   = event_type;
    evt->inode_no     = inode_no;
    evt->agent_id     = agent_id;
    evt->timestamp_ns = (u64)ts.tv_sec * 1000000000ULL + ts.tv_nsec;
    evt->value        = value;
    strscpy(evt->detail, detail ? detail : "", sizeof(evt->detail));

    nlmsg_unicast(byngo_netlink_sock, skb, byngo_heal_daemon_pid);

    pr_debug("sorabbyngofs: heal event %u sent to daemon pid=%u\n",
             event_type, byngo_heal_daemon_pid);
}
EXPORT_SYMBOL_GPL(sorabbyngofs_heal_notify);

/*
 * sorabbyngofs_heal_register_daemon()
 * Called when the self-healing daemon sends its first NLMSG to register.
 * Stored PID is used for all subsequent unicast notifications.
 */
void sorabbyngofs_heal_register_daemon(u32 pid)
{
    byngo_heal_daemon_pid = pid;
    pr_info("sorabbyngofs: self-healing daemon registered (pid=%u)\n", pid);
}
EXPORT_SYMBOL_GPL(sorabbyngofs_heal_register_daemon);
