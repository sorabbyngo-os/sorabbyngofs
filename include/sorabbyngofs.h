/* SPDX-License-Identifier: GPL-2.0 */
/* SorabbyngoFS — shared types and constants */

#ifndef _SORABBYNGOFS_H
#define _SORABBYNGOFS_H

#include <linux/types.h>
#include <linux/fs.h>
#include <linux/writeback.h>

/* ------------------------------------------------------------------ */
/*  Identity                                                            */
/* ------------------------------------------------------------------ */

#define SORABBYNGOFS_MAGIC        0x53594E47   /* SYNG */
#define SORABBYNGOFS_VERSION      "0.1.0"
#define SORABBYNGOFS_BLOCK_SIZE   4096

/* ------------------------------------------------------------------ */
/*  File op type constants (consent gate + journal)                    */
/* ------------------------------------------------------------------ */

#define BYNGO_OP_READ        1
#define BYNGO_OP_WRITE       2
#define BYNGO_OP_MODIFY      3
#define BYNGO_OP_DELETE      4
#define BYNGO_OP_INODE_SYNC  5

/* ------------------------------------------------------------------ */
/*  Permission bitmask                                                  */
/* ------------------------------------------------------------------ */

#define BYNGO_PERM_READ    (1 << 0)
#define BYNGO_PERM_WRITE   (1 << 1)
#define BYNGO_PERM_MODIFY  (1 << 2)
#define BYNGO_PERM_DELETE  (1 << 3)

/* ------------------------------------------------------------------ */
/*  Consent token — issued by CML, verified before every cross-zone op */
/* ------------------------------------------------------------------ */

struct byngo_consent_token {
    u64   user_id;
    u64   expires_ns;
    u8    signature[64];    /* Ed25519 — signed by sorabbyngo-security key */
    u32   permissions;      /* BYNGO_PERM_* bitmask */
    u64   agent_id;         /* which Byngox/Chameha agent holds this token */
};

/* ------------------------------------------------------------------ */
/*  Per-inode AI metadata                                               */
/* ------------------------------------------------------------------ */

struct sorabbyngofs_inode_info {
    /* VFS inode — MUST be first field */
    struct inode    vfs_inode;

    /* Consent audit trail */
    u64             ai_access_count;    /* total consented ops on this inode */
    u64             last_consent_ts;    /* nanosecond timestamp of last op    */

    /* ByngoPress compression state */
    u8              compressed;         /* 1 = compressed on disk             */
    u64             compressed_size;    /* size after ByngoPress compression  */

    /* Agent ownership */
    u64             ai_agent_id;        /* Byngox agent ID that owns the file */

    /* Journal dirty flag */
    u8              dirty;              /* 1 = pending journal flush          */
};

/* ------------------------------------------------------------------ */
/*  ioctl interface (used by ai-sandbox)                               */
/* ------------------------------------------------------------------ */

#define BYNGO_IOC_MAGIC      'B'
#define BYNGO_IOC_SET_TOKEN  _IOW(BYNGO_IOC_MAGIC, 1, struct byngo_consent_token)
#define BYNGO_IOC_GET_AUDIT  _IOR(BYNGO_IOC_MAGIC, 2, u64)

/* ------------------------------------------------------------------ */
/*  Function declarations                                               */
/* ------------------------------------------------------------------ */

/* consent gate — sorabbyngofs_consent.c */
int  sorabbyngofs_consent_check(struct inode *inode,
                                 struct byngo_consent_token *token,
                                 u32 required_perms);
void sorabbyngofs_consent_revoke(struct byngo_consent_token *token);

/* journal — sorabbyngofs_journal.c */
int  sorabbyngofs_journal_init(struct super_block *sb);
int  sorabbyngofs_journal_log(struct super_block *sb,
                               u64 user_id, u64 inode_no, u32 op_type);
void sorabbyngofs_journal_commit(struct super_block *sb);
void sorabbyngofs_journal_destroy(struct super_block *sb);

/* compression — sorabbyngofs_compress.c */
int  sorabbyngofs_compress_write(struct file *file,
                                  const char __user *buf, size_t len);

/* inode cache — sorabbyngofs_inode.c */
int  sorabbyngofs_inode_cache_init(void);
void sorabbyngofs_inode_cache_destroy(void);

/* operation tables — sorabbyngofs_inode.c + sorabbyngofs_file.c */
extern const struct inode_operations  sorabbyngofs_file_inode_ops;
extern const struct inode_operations  sorabbyngofs_dir_inode_ops;
extern const struct super_operations  sorabbyngofs_super_ops;
extern const struct file_operations   sorabbyngofs_file_ops;

#endif /* _SORABBYNGOFS_H */
