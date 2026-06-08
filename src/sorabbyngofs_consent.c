// SPDX-License-Identifier: GPL-2.0
// SorabbyngoFS — consent gate
// Verifies CML-issued Ed25519 tokens before any AI cross-zone file op

#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/time.h>
#include <linux/string.h>
#include <crypto/public_key.h>
#include "../include/sorabbyngofs.h"

/* CML public key — replace with real key bytes from sorabbyngo-security */
static const u8 cml_public_key[32] = {
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
};  /* PLACEHOLDER — inject from sorabbyngo-security at build time */

/*
 * sorabbyngofs_consent_check()
 *
 * Called before any AI-initiated file operation that crosses zone boundaries.
 * Three checks must all pass:
 *   1. Token not expired
 *   2. Token has the required permission bits
 *   3. Ed25519 signature is valid (signed by CML)
 *
 * Returns 0 on success, -EACCES on denial, -EINVAL on bad token.
 */
int sorabbyngofs_consent_check(struct inode *inode,
                                struct byngo_consent_token *token,
                                u32 required_perms)
{
    u64 now_ns;
    struct timespec64 ts;

    if (!token)
        return -EINVAL;

    /* --- Check 1: expiry --- */
    ktime_get_real_ts64(&ts);
    now_ns = (u64)ts.tv_sec * 1000000000ULL + ts.tv_nsec;

    if (now_ns > token->expires_ns) {
        pr_warn("sorabbyngofs: consent token expired for uid=%llu\n",
                token->user_id);
        return -EACCES;
    }

    /* --- Check 2: permission bits --- */
    if ((token->permissions & required_perms) != required_perms) {
        pr_warn("sorabbyngofs: insufficient permissions for uid=%llu "
                "(have=0x%x need=0x%x)\n",
                token->user_id, token->permissions, required_perms);
        return -EACCES;
    }

    /* --- Check 3: Ed25519 signature --- */
    /*
     * TODO: wire in kernel Ed25519 verify once sorabbyngo-security
     * exports its key material via the keyring subsystem.
     *
     * Stub path: always passes in dev builds.
     * Set CONFIG_SORABBYNGOFS_ENFORCE_CONSENT=y in Kconfig to hard-block.
     */
#ifdef CONFIG_SORABBYNGOFS_ENFORCE_CONSENT
    {
        int ret = sorabbyngofs_verify_ed25519(
            token->signature,
            sizeof(*token) - sizeof(token->signature),
            (u8 *)token,
            cml_public_key
        );
        if (ret) {
            pr_err("sorabbyngofs: invalid consent signature uid=%llu\n",
                   token->user_id);
            return -EACCES;
        }
    }
#else
    pr_debug("sorabbyngofs: consent sig check STUBBED (dev mode)\n");
#endif

    /* All checks passed — update inode audit trail */
    if (inode) {
        struct sorabbyngofs_inode_info *info =
            container_of(inode, struct sorabbyngofs_inode_info, vfs_inode);
        info->ai_access_count++;
        info->last_consent_ts = now_ns;
    }

    pr_info("sorabbyngofs: consent granted uid=%llu perms=0x%x\n",
            token->user_id, token->permissions);
    return 0;
}
EXPORT_SYMBOL_GPL(sorabbyngofs_consent_check);


/*
 * sorabbyngofs_consent_revoke()
 *
 * Called by CML or user-space when a user withdraws consent mid-session.
 * Zeros the token in-place so any in-flight ops immediately fail Check 1.
 */
void sorabbyngofs_consent_revoke(struct byngo_consent_token *token)
{
    if (!token)
        return;
    memzero_explicit(token, sizeof(*token));
    pr_info("sorabbyngofs: consent token revoked and zeroed\n");
}
EXPORT_SYMBOL_GPL(sorabbyngofs_consent_revoke);
