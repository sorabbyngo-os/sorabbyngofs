// SPDX-License-Identifier: GPL-2.0
// SorabbyngoFS — inode operations
// Allocates and manages inodes with embedded AI metadata.
// Every file in Zone 3 carries: consent audit trail, compression flag,
// ByngoPress size, and a link back to the owning AI agent (Byngox/Chameha).

#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/time.h>
#include <linux/stat.h>
#include <linux/kernel.h>
#include <linux/mm.h>
#include "../include/sorabbyngofs.h"

/* Inode cache — one slab per mount, avoids GFP_KERNEL on every alloc */
static struct kmem_cache *sorabbyngofs_inode_cache;

/* ------------------------------------------------------------------ */
/*  Inode cache callbacks                                               */
/* ------------------------------------------------------------------ */

/*
 * sorabbyngofs_inode_init_once()
 * Called by the slab allocator when a new inode object is carved out.
 * Initialises the VFS inode portion — slab reuse is safe because
 * inode_init_once() is idempotent.
 */
static void sorabbyngofs_inode_init_once(void *obj)
{
    struct sorabbyngofs_inode_info *info = obj;
    inode_init_once(&info->vfs_inode);
}

/* ------------------------------------------------------------------ */
/*  Superblock callbacks                                                */
/* ------------------------------------------------------------------ */

/*
 * sorabbyngofs_alloc_inode()
 * Called by the VFS when it needs a new inode for this filesystem.
 * We allocate from our private slab so the sorabbyngofs_inode_info
 * wrapper is always contiguous with the vfs_inode it contains.
 */
static struct inode *sorabbyngofs_alloc_inode(struct super_block *sb)
{
    struct sorabbyngofs_inode_info *info;

    info = kmem_cache_alloc(sorabbyngofs_inode_cache, GFP_KERNEL);
    if (!info)
        return NULL;

    /* Zero our private fields — vfs_inode already clean from init_once */
    info->ai_access_count  = 0;
    info->last_consent_ts  = 0;
    info->compressed       = 0;
    info->compressed_size  = 0;
    info->ai_agent_id      = 0;
    info->dirty            = 0;

    return &info->vfs_inode;
}

/*
 * sorabbyngofs_free_inode()
 * Called by the VFS after the inode refcount drops to zero.
 * Zeros sensitive AI metadata before returning to slab.
 */
static void sorabbyngofs_free_inode(struct inode *inode)
{
    struct sorabbyngofs_inode_info *info =
        container_of(inode, struct sorabbyngofs_inode_info, vfs_inode);

    /* Scrub AI metadata — consent timestamps must not leak via slab reuse */
    memzero_explicit(&info->ai_access_count,
                     sizeof(*info) - offsetof(struct sorabbyngofs_inode_info,
                                              ai_access_count));

    kmem_cache_free(sorabbyngofs_inode_cache, info);
}

/*
 * sorabbyngofs_write_inode()
 * Called when the VFS wants to flush a dirty inode to backing store.
 * For now logs to journal; persistent inode table is a future milestone.
 */
static int sorabbyngofs_write_inode(struct inode *inode,
                                     struct writeback_control *wbc)
{
    struct sorabbyngofs_inode_info *info =
        container_of(inode, struct sorabbyngofs_inode_info, vfs_inode);

    sorabbyngofs_journal_log(inode->i_sb,
                              info->ai_agent_id,
                              inode->i_ino,
                              BYNGO_OP_INODE_SYNC);

    info->dirty = 0;
    return 0;
}

/*
 * sorabbyngofs_evict_inode()
 * Called just before an inode is evicted from the inode cache.
 * Truncates any page cache pages and marks the inode as freed.
 */
static void sorabbyngofs_evict_inode(struct inode *inode)
{
    truncate_inode_pages_final(&inode->i_data);
    clear_inode(inode);

    pr_debug("sorabbyngofs: evicted inode %lu\n", inode->i_ino);
}

/* ------------------------------------------------------------------ */
/*  Directory / lookup operations                                       */
/* ------------------------------------------------------------------ */

/*
 * sorabbyngofs_lookup()
 * Resolves a filename in a directory to an inode.
 * Zone 3 uses a flat namespace by default — all AI files live in
 * /ai-workspace directly. Subdirectories added in a future milestone.
 */
static struct dentry *sorabbyngofs_lookup(struct inode *dir,
                                           struct dentry *dentry,
                                           unsigned int flags)
{
    /*
     * Flat namespace: no children exist yet beyond what was explicitly
     * created. Return a negative dentry — VFS will call create() if
     * the caller has O_CREAT.
     */
    d_add(dentry, NULL);
    return NULL;
}

/*
 * sorabbyngofs_create()
 * Creates a new regular file inode in Zone 3.
 * Sets default AI metadata: uncompressed, no agent owner yet.
 */
static int sorabbyngofs_create(struct mnt_idmap *idmap,
                                struct inode *dir,
                                struct dentry *dentry,
                                umode_t mode, bool excl)
{
    struct inode *inode;
    struct sorabbyngofs_inode_info *info;
    struct timespec64 ts;

    inode = new_inode(dir->i_sb);
    if (!inode)
        return -ENOMEM;

    info = container_of(inode, struct sorabbyngofs_inode_info, vfs_inode);

    inode->i_ino   = get_next_ino();
    inode->i_mode  = mode | S_IFREG;
    inode->i_op    = &sorabbyngofs_file_inode_ops;
    inode->i_fop   = &sorabbyngofs_file_ops;

    ktime_get_real_ts64(&ts);
    inode->i_atime = inode->i_mtime = inode->i_ctime = ts;
    set_nlink(inode, 1);

    /* Default AI metadata */
    info->compressed      = 0;
    info->compressed_size = 0;
    info->ai_agent_id     = 0;   /* bound to agent on first BYNGO_IOC_SET_TOKEN */
    info->dirty           = 1;

    inode_init_owner(idmap, inode, dir, mode);
    d_instantiate(dentry, inode);
    mark_inode_dirty(inode);

    pr_info("sorabbyngofs: created inode %lu\n", inode->i_ino);
    return 0;
}

/*
 * sorabbyngofs_mkdir()
 * Creates a subdirectory inode. Sets link count to 2 (. and ..)
 */
static int sorabbyngofs_mkdir(struct mnt_idmap *idmap,
                               struct inode *dir,
                               struct dentry *dentry,
                               umode_t mode)
{
    struct inode *inode;
    struct timespec64 ts;

    inode = new_inode(dir->i_sb);
    if (!inode)
        return -ENOMEM;

    inode->i_ino  = get_next_ino();
    inode->i_mode = mode | S_IFDIR;
    inode->i_op   = &sorabbyngofs_dir_inode_ops;
    inode->i_fop  = &simple_dir_operations;

    ktime_get_real_ts64(&ts);
    inode->i_atime = inode->i_mtime = inode->i_ctime = ts;
    set_nlink(inode, 2);

    inode_init_owner(idmap, inode, dir, mode | S_IFDIR);
    inc_nlink(dir);
    d_instantiate(dentry, inode);
    mark_inode_dirty(inode);

    return 0;
}

/*
 * sorabbyngofs_unlink()
 * Removes a file. Journals the DELETE op before decrementing nlink.
 * If an AI agent tries to delete a file it does not own, consent_check
 * will have already blocked the unlink syscall upstream.
 */
static int sorabbyngofs_unlink(struct inode *dir, struct dentry *dentry)
{
    struct inode *inode = d_inode(dentry);
    struct sorabbyngofs_inode_info *info =
        container_of(inode, struct sorabbyngofs_inode_info, vfs_inode);
    struct timespec64 ts;

    sorabbyngofs_journal_log(inode->i_sb,
                              info->ai_agent_id,
                              inode->i_ino,
                              BYNGO_OP_DELETE);

    ktime_get_real_ts64(&ts);
    dir->i_mtime = dir->i_ctime = ts;
    drop_nlink(inode);
    dput(dentry);

    return 0;
}

/* ------------------------------------------------------------------ */
/*  Inode operation tables                                              */
/* ------------------------------------------------------------------ */

const struct inode_operations sorabbyngofs_file_inode_ops = {
    .setattr    = simple_setattr,
    .getattr    = simple_getattr,
};
EXPORT_SYMBOL_GPL(sorabbyngofs_file_inode_ops);

const struct inode_operations sorabbyngofs_dir_inode_ops = {
    .lookup     = sorabbyngofs_lookup,
    .create     = sorabbyngofs_create,
    .mkdir      = sorabbyngofs_mkdir,
    .unlink     = sorabbyngofs_unlink,
    .rmdir      = simple_rmdir,
    .rename     = simple_rename,
};
EXPORT_SYMBOL_GPL(sorabbyngofs_dir_inode_ops);

const struct super_operations sorabbyngofs_super_ops = {
    .alloc_inode    = sorabbyngofs_alloc_inode,
    .free_inode     = sorabbyngofs_free_inode,
    .write_inode    = sorabbyngofs_write_inode,
    .evict_inode    = sorabbyngofs_evict_inode,
    .statfs         = simple_statfs,
    .drop_inode     = generic_drop_inode,
};
EXPORT_SYMBOL_GPL(sorabbyngofs_super_ops);

/* ------------------------------------------------------------------ */
/*  Inode cache lifecycle — called from module init/exit               */
/* ------------------------------------------------------------------ */

int sorabbyngofs_inode_cache_init(void)
{
    sorabbyngofs_inode_cache = kmem_cache_create(
        "sorabbyngofs_inode",
        sizeof(struct sorabbyngofs_inode_info),
        0,
        SLAB_RECLAIM_ACCOUNT | SLAB_MEM_SPREAD | SLAB_ACCOUNT,
        sorabbyngofs_inode_init_once
    );

    if (!sorabbyngofs_inode_cache) {
        pr_err("sorabbyngofs: failed to create inode cache\n");
        return -ENOMEM;
    }

    pr_info("sorabbyngofs: inode cache created\n");
    return 0;
}
EXPORT_SYMBOL_GPL(sorabbyngofs_inode_cache_init);

void sorabbyngofs_inode_cache_destroy(void)
{
    rcu_barrier();  /* wait for all RCU-deferred frees */
    kmem_cache_destroy(sorabbyngofs_inode_cache);
    pr_info("sorabbyngofs: inode cache destroyed\n");
}
EXPORT_SYMBOL_GPL(sorabbyngofs_inode_cache_destroy);
