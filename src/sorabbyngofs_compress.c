// SPDX-License-Identifier: GPL-2.0
// SorabbyngoFS — ByngoPress compression integration
//
// This file defines the contract between SorabbyngoFS and the
// ByngoPress kernel API (sorabbyngo-os/byngo-press).
//
// STATUS: Stub — functions are defined and callable.
//         Real compression activates once byngo-press exports:
//           - byngopress_compress()
//           - byngopress_decompress()
//           - byngopress_compressed_size()
//         via include/linux/byngopress.h in the kernel tree.
//
// HOW TO ACTIVATE:
//   1. In sorabbyngo-os/byngo-press, implement the three functions below
//      and export them with EXPORT_SYMBOL_GPL.
//   2. Add to sorabbyngo-os/sorabbyngo-patches:
//        include/linux/byngopress.h  (API header)
//   3. Set CONFIG_SORABBYNGOFS_COMPRESS=y — compress path goes live.
//   4. Remove the #ifndef BYNGOPRESS_API_AVAILABLE stubs below.

#include <linux/fs.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/kernel.h>
#include "../include/sorabbyngofs.h"

/* ------------------------------------------------------------------ */
/*  ByngoPress API contract                                             */
/*  These signatures MUST match what byngo-press exports.              */
/* ------------------------------------------------------------------ */

#ifndef BYNGOPRESS_API_AVAILABLE
/*
 * Stub declarations — replaced by real symbols from byngo-press
 * once include/linux/byngopress.h is available in the kernel tree.
 */

/**
 * byngopress_compress() - compress a buffer using semantic compression
 * @in:       input buffer (kernel virtual address)
 * @in_len:   input length in bytes
 * @out:      output buffer (caller-allocated, at least in_len bytes)
 * @out_len:  on return, number of bytes written to out
 *
 * Returns 0 on success, negative errno on failure.
 * Returns -EOPNOTSUPP if the input is not compressible.
 */
static inline int byngopress_compress(const u8 *in, size_t in_len,
                                       u8 *out, size_t *out_len)
{
    /* Stub: pass through uncompressed */
    if (*out_len < in_len)
        return -ENOSPC;
    memcpy(out, in, in_len);
    *out_len = in_len;
    return -EOPNOTSUPP;  /* signals caller to use plain write */
}

/**
 * byngopress_decompress() - decompress a ByngoPress-compressed buffer
 * @in:       compressed input buffer
 * @in_len:   compressed length
 * @out:      decompression output buffer
 * @out_len:  on entry: output buffer capacity
 *            on return: actual decompressed size
 *
 * Returns 0 on success, negative errno on failure.
 */
static inline int byngopress_decompress(const u8 *in, size_t in_len,
                                         u8 *out, size_t *out_len)
{
    /* Stub: pass through */
    if (*out_len < in_len)
        return -ENOSPC;
    memcpy(out, in, in_len);
    *out_len = in_len;
    return 0;
}

/**
 * byngopress_bound() - returns worst-case output size for a given input
 * @in_len:   input size in bytes
 *
 * Use this to allocate the output buffer before calling compress().
 * Never returns less than in_len.
 */
static inline size_t byngopress_bound(size_t in_len)
{
    return in_len + 64;  /* stub: tiny overhead for header */
}

#endif /* BYNGOPRESS_API_AVAILABLE */


/* ------------------------------------------------------------------ */
/*  sorabbyngofs_compress_write()                                       */
/*                                                                      */
/*  Called from sorabbyngofs_file.c when CONFIG_SORABBYNGOFS_COMPRESS   */
/*  is set. Compresses the user buffer via ByngoPress, stores the       */
/*  result, and updates the inode's compressed_size field.              */
/*                                                                      */
/*  Returns bytes written on success (== count, the original size).    */
/*  Returns -EOPNOTSUPP if ByngoPress declines (file not compressible)  */
/*  so the caller falls back to a plain write.                          */
/* ------------------------------------------------------------------ */

int sorabbyngofs_compress_write(struct file *file,
                                 const char __user *buf,
                                 size_t count)
{
    struct inode *inode = file_inode(file);
    struct sorabbyngofs_inode_info *info =
        container_of(inode, struct sorabbyngofs_inode_info, vfs_inode);

    u8 *kbuf     = NULL;    /* copy of user data in kernel space  */
    u8 *cbuf     = NULL;    /* ByngoPress compressed output        */
    size_t csize = 0;
    int ret      = 0;

    /* Allocate kernel bounce buffer */
    kbuf = kvmalloc(count, GFP_KERNEL);
    if (!kbuf)
        return -ENOMEM;

    if (copy_from_user(kbuf, buf, count)) {
        ret = -EFAULT;
        goto out;
    }

    /* Allocate compression output buffer */
    csize = byngopress_bound(count);
    cbuf  = kvmalloc(csize, GFP_KERNEL);
    if (!cbuf) {
        ret = -ENOMEM;
        goto out;
    }

    /* Attempt ByngoPress compression */
    ret = byngopress_compress(kbuf, count, cbuf, &csize);
    if (ret == -EOPNOTSUPP) {
        /*
         * ByngoPress declined (data not semantically compressible).
         * Signal file.c to fall through to the plain write path.
         */
        pr_debug("sorabbyngofs: inode %lu not compressible, using plain write\n",
                 inode->i_ino);
        goto out;   /* returns -EOPNOTSUPP to caller */
    }
    if (ret < 0) {
        pr_err("sorabbyngofs: ByngoPress compress error %d on inode %lu\n",
               ret, inode->i_ino);
        goto out;
    }

    /*
     * Compression succeeded — store cbuf to page cache.
     * TODO: write cbuf into the inode's page cache pages.
     *       For now, update metadata and log the win.
     */
    info->compressed      = 1;
    info->compressed_size = csize;
    mark_inode_dirty(inode);

    pr_info("sorabbyngofs: inode %lu compressed %zu -> %zu bytes (%.1fx)\n",
            inode->i_ino, count, csize,
            csize > 0 ? (double)count / (double)csize : 0.0);

    ret = (int)count;   /* report original count to caller */

out:
    kvfree(kbuf);
    kvfree(cbuf);
    return ret;
}
EXPORT_SYMBOL_GPL(sorabbyngofs_compress_write);


/* ------------------------------------------------------------------ */
/*  sorabbyngofs_decompress_read()                                      */
/*                                                                      */
/*  Called from the read path when inode->compressed == 1.             */
/*  Decompresses page cache data back to original before copy_to_user. */
/* ------------------------------------------------------------------ */

int sorabbyngofs_decompress_read(struct file *file,
                                  char __user *buf,
                                  size_t count,
                                  const u8 *cdata,
                                  size_t csize)
{
    u8    *dbuf  = NULL;
    size_t dsize = count;
    int    ret   = 0;

    dbuf = kvmalloc(dsize, GFP_KERNEL);
    if (!dbuf)
        return -ENOMEM;

    ret = byngopress_decompress(cdata, csize, dbuf, &dsize);
    if (ret < 0) {
        pr_err("sorabbyngofs: ByngoPress decompress error %d\n", ret);
        goto out;
    }

    if (copy_to_user(buf, dbuf, min(count, dsize))) {
        ret = -EFAULT;
        goto out;
    }

    ret = (int)min(count, dsize);

out:
    kvfree(dbuf);
    return ret;
}
EXPORT_SYMBOL_GPL(sorabbyngofs_decompress_read);
