// SPDX-License-Identifier: GPL-2.0
// SorabbyngoFS — VFS registration + mount + module lifecycle

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/pagemap.h>
#include "../include/sorabbyngofs.h"

/*
 * sorabbyngofs_fill_super()
 * Called by the VFS when the filesystem is mounted.
 * Initialises the superblock, creates the root inode and dentry.
 */
static int sorabbyngofs_fill_super(struct super_block *sb,
                                    void *data, int silent)
{
    struct inode *root;
    struct timespec64 ts;
    int ret;

    sb->s_magic      = SORABBYNGOFS_MAGIC;
    sb->s_op         = &sorabbyngofs_super_ops;
    sb->s_blocksize  = SORABBYNGOFS_BLOCK_SIZE;
    sb->s_blocksize_bits = PAGE_SHIFT;
    sb->s_maxbytes   = MAX_LFS_FILESIZE;
    sb->s_time_gran  = 1;

    /* Initialise self-healing journal */
    ret = sorabbyngofs_journal_init(sb);
    if (ret) {
        pr_err("sorabbyngofs: journal init failed: %d\n", ret);
        return ret;
    }

    /* Create root inode (directory) */
    root = new_inode(sb);
    if (!root) {
        sorabbyngofs_journal_destroy(sb);
        return -ENOMEM;
    }

    root->i_ino  = 1;
    root->i_mode = S_IFDIR | 0755;
    root->i_op   = &sorabbyngofs_dir_inode_ops;
    root->i_fop  = &simple_dir_operations;
    ktime_get_real_ts64(&ts);
    root->i_atime = root->i_mtime = root->i_ctime = ts;
    set_nlink(root, 2);

    sb->s_root = d_make_root(root);
    if (!sb->s_root) {
        sorabbyngofs_journal_destroy(sb);
        return -ENOMEM;
    }

    pr_info("sorabbyngofs: mounted — Zone 3 AI sandbox ready\n");
    return 0;
}

static struct dentry *sorabbyngofs_mount(struct file_system_type *fs_type,
                                          int flags, const char *dev_name,
                                          void *data)
{
    return mount_nodev(fs_type, flags, data, sorabbyngofs_fill_super);
}

static void sorabbyngofs_kill_sb(struct super_block *sb)
{
    sorabbyngofs_journal_destroy(sb);
    kill_anon_super(sb);
    pr_info("sorabbyngofs: unmounted cleanly\n");
}

static struct file_system_type sorabbyngofs_type = {
    .owner      = THIS_MODULE,
    .name       = "sorabbyngofs",
    .mount      = sorabbyngofs_mount,
    .kill_sb    = sorabbyngofs_kill_sb,
    .fs_flags   = FS_USERNS_MOUNT,
};

static int __init sorabbyngofs_init(void)
{
    int err;

    pr_info("sorabbyngofs: initialising v%s\n", SORABBYNGOFS_VERSION);

    err = sorabbyngofs_inode_cache_init();
    if (err)
        return err;

    err = register_filesystem(&sorabbyngofs_type);
    if (err) {
        sorabbyngofs_inode_cache_destroy();
        pr_err("sorabbyngofs: filesystem registration failed: %d\n", err);
        return err;
    }

    pr_info("sorabbyngofs: registered successfully\n");
    return 0;
}

static void __exit sorabbyngofs_exit(void)
{
    unregister_filesystem(&sorabbyngofs_type);
    sorabbyngofs_inode_cache_destroy();
    pr_info("sorabbyngofs: unregistered\n");
}

module_init(sorabbyngofs_init);
module_exit(sorabbyngofs_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Sorabbyngo OS");
MODULE_DESCRIPTION("SorabbyngoFS — AI-native filesystem, consent gate, ByngoPress, self-healing journal");
MODULE_VERSION(SORABBYNGOFS_VERSION);
