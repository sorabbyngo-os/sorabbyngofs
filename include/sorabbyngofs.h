/* SPDX-License-Identifier: GPL-2.0 */
/* SorabbyngoFS — shared types and constants */

#ifndef _SORABBYNGOFS_H
#define _SORABBYNGOFS_H

#include <linux/types.h>
#include <linux/fs.h>

#define SORABBYNGOFS_MAGIC       0x53594E47
#define SORABBYNGOFS_BLOCK_SIZE  4096

/* Consent token — issued by CML, verified before any cross-zone op */
struct byngo_consent_token {
    u64   user_id;
    u64   expires_ns;
    u8    signature[64];   /* Ed25519 signature from CML */
    u32   permissions;     /* bitmask: READ | WRITE | MODIFY | DELETE */
};

/* Per-inode AI metadata */
struct sorabbyngofs_inode_info {
    struct inode        vfs_inode;
    u64                 ai_access_count;
    u64                 last_consent_ts;
    u8                  compressed;        /* ByngoPress flag */
    u64                 compressed_size;
};

/* Permissions bitmask */
#define BYNGO_PERM_READ    (1 << 0)
#define BYNGO_PERM_WRITE   (1 << 1)
#define BYNGO_PERM_MODIFY  (1 << 2)
#define BYNGO_PERM_DELETE  (1 << 3)

/* Function declarations */
int  sorabbyngofs_consent_check(struct inode *inode, struct byngo_consent_token *token, u32 required_perms);
int  sorabbyngofs_compress_write(struct file *file, const char __user *buf, size_t len);
int  sorabbyngofs_journal_init(struct super_block *sb);
void sorabbyngofs_journal_commit(struct super_block *sb);

#endif /* _SORABBYNGOFS_H */
