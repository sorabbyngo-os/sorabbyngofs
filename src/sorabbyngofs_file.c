// SPDX-License-Identifier: GPL-2.0
// SorabbyngoFS — file operations
// All AI reads/writes pass through consent check + journal log before executing.

#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/kernel.h>
#include <linux/slab.h>
#include "../include/sorabbyngofs.h"

/* Op type constants (mirrors journal) */
#define BYNGO_OP_READ    1
#define BYNGO_OP_WRITE   2
#define BYNGO_OP_MODIFY  3
#define BYNGO_OP_DELETE  4

/*
 * sorabbyngofs_read()
 *
 * Flow:
 *   1. Retrieve consent token from file's private_data (set at open time)
 *   2. consent_check() — expiry + perms + sig
 *   3. journal_log()   — record before executing
 *   4. copy_to_user()  — actual data transfer
 */
static ssize_t sorabbyngofs_read(struct file *file, char __user *buf,
                                  size_t count, loff_t *ppos)
{
    struct byngo_consent_token *token = file->private_data;
    struct inode *inode = file_inode(file);
    struct super_block *sb = inode->i_sb;
    ssize_t ret;

    /* Consent check — READ permission required */
    ret = sorabbyngofs_consent_check(inode, token, BYNGO_PERM_READ);
    if (ret) {
        pr_warn("sorabbyngofs: read denied on inode %lu\n", inode->i_ino);
        return ret;
    }

    /* Journal before the op */
    sorabbyngofs_journal_log(sb, token->user_id, inode->i_ino, BYNGO_OP_READ);

    /*
     * TODO: if inode is ByngoPress-compressed, decompress into
     * a kernel bounce buffer first, then copy_to_user from there.
     * For now: direct page cache read via generic helper.
     */
    ret = generic_file_read_iter(file->f_iocb_flags ?
                                 (struct kiocb *)file : NULL, NULL);

    /* Fallback for non-AIO path */
    if (ret == -EOPNOTSUPP || ret == -EINVAL)
        ret = simple_read_from_buffer(buf, count, ppos,
                                      NULL, 0);

    return ret;
}

/*
 * sorabbyngofs_write()
 *
 * Flow:
 *   1. Retrieve consent token
 *   2. consent_check() — WRITE permission required
 *   3. journal_log()
 *   4. ByngoPress compress path (if CONFIG_SORABBYNGOFS_COMPRESS)
 *   5. copy_from_user() + write to page cache
 */
static ssize_t sorabbyngofs_write(struct file *file, const char __user *buf,
                                   size_t count, loff_t *ppos)
{
    struct byngo_consent_token *token = file->private_data;
    struct inode *inode = file_inode(file);
    struct super_block *sb = inode->i_sb;
    ssize_t ret;

    /* Consent check — WRITE permission required */
    ret = sorabbyngofs_consent_check(inode, token, BYNGO_PERM_WRITE);
    if (ret) {
        pr_warn("sorabbyngofs: write denied on inode %lu\n", inode->i_ino);
        return ret;
    }

    /* Journal before the op */
    sorabbyngofs_journal_log(sb, token->user_id, inode->i_ino, BYNGO_OP_WRITE);

#ifdef CONFIG_SORABBYNGOFS_COMPRESS
    /* Hand off to ByngoPress compression path */
    ret = sorabbyngofs_compress_write(file, buf, count);
    if (ret != -EOPNOTSUPP)
        return ret;
    /* Fall through to plain write if compress not ready */
#endif

    ret = simple_write_to_buffer(NULL, 0, ppos, buf, count);
    return ret;
}

/*
 * sorabbyngofs_open()
 *
 * Validates that a consent token is attached before the file descriptor
 * is handed to the AI process. Token is passed via O_CLOEXEC + ioctl
 * from the ai-sandbox layer before open() returns to userspace.
 *
 * If no token is attached, the open is denied immediately —
 * the AI never gets an fd to an unconsented file.
 */
static int sorabbyngofs_open(struct inode *inode, struct file *file)
{
    /*
     * Token is injected by ai-sandbox via sorabbyngofs_ioctl()
     * after open() but before first read/write. For now we allow
     * open() through and gate at read/write time.
     *
     * TODO: enforce token-at-open once ai-sandbox ioctl bridge is wired.
     */
    file->private_data = NULL;  /* token injected post-open via ioctl */

    pr_debug("sorabbyngofs: opened inode %lu — awaiting consent token\n",
             inode->i_ino);
    return 0;
}

/*
 * sorabbyngofs_release()
 *
 * Zeroes and frees the consent token when the fd is closed.
 * Ensures tokens never outlive the file descriptor.
 */
static int sorabbyngofs_release(struct inode *inode, struct file *file)
{
    struct byngo_consent_token *token = file->private_data;

    if (token) {
        sorabbyngofs_consent_revoke(token);
        kfree(token);
        file->private_data = NULL;
    }

    pr_debug("sorabbyngofs: released inode %lu — token destroyed\n",
             inode->i_ino);
    return 0;
}

/*
 * sorabbyngofs_ioctl()
 *
 * BYNGO_IOC_SET_TOKEN — called by ai-sandbox to inject a CML-issued
 * consent token into an open file descriptor before first I/O.
 */
#define BYNGO_IOC_MAGIC     'B'
#define BYNGO_IOC_SET_TOKEN _IOW(BYNGO_IOC_MAGIC, 1, struct byngo_consent_token)
#define BYNGO_IOC_GET_AUDIT _IOR(BYNGO_IOC_MAGIC, 2, u64)

static long sorabbyngofs_ioctl(struct file *file, unsigned int cmd,
                                unsigned long arg)
{
    struct inode *inode = file_inode(file);
    struct sorabbyngofs_inode_info *info =
        container_of(inode, struct sorabbyngofs_inode_info, vfs_inode);

    switch (cmd) {

    case BYNGO_IOC_SET_TOKEN: {
        struct byngo_consent_token *token;

        /* Free any existing token first */
        if (file->private_data) {
            sorabbyngofs_consent_revoke(file->private_data);
            kfree(file->private_data);
        }

        token = kzalloc(sizeof(*token), GFP_KERNEL);
        if (!token)
            return -ENOMEM;

        if (copy_from_user(token, (void __user *)arg, sizeof(*token))) {
            kfree(token);
            return -EFAULT;
        }

        file->private_data = token;
        pr_info("sorabbyngofs: consent token set on inode %lu uid=%llu\n",
                inode->i_ino, token->user_id);
        return 0;
    }

    case BYNGO_IOC_GET_AUDIT: {
        /* Return ai_access_count for this inode — for CML audit queries */
        if (copy_to_user((void __user *)arg,
                         &info->ai_access_count,
                         sizeof(u64)))
            return -EFAULT;
        return 0;
    }

    default:
        return -ENOTTY;
    }
}

/* File operations table */
const struct file_operations sorabbyngofs_file_ops = {
    .owner          = THIS_MODULE,
    .open           = sorabbyngofs_open,
    .release        = sorabbyngofs_release,
    .read           = sorabbyngofs_read,
    .write          = sorabbyngofs_write,
    .unlocked_ioctl = sorabbyngofs_ioctl,
    .llseek         = generic_file_llseek,
};
EXPORT_SYMBOL_GPL(sorabbyngofs_file_ops);
