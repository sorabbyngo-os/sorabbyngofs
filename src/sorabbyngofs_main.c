// SPDX-License-Identifier: GPL-2.0
// SorabbyngoFS — AI-native VFS filesystem module
// Sorabbyngo OS | sorabbyngo-os/sorabbyngofs

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/init.h>
#include "include/sorabbyngofs.h"

#define SORABBYNGOFS_MAGIC 0x53594E47   /* SYNG */
#define SORABBYNGOFS_VERSION "0.1.0"

static struct file_system_type sorabbyngofs_type = {
    .owner      = THIS_MODULE,
    .name       = "sorabbyngofs",
    .kill_sb    = kill_anon_super,
    .fs_flags   = FS_USERNS_MOUNT,
};

static int __init sorabbyngofs_init(void)
{
    int err;
    pr_info("sorabbyngofs: initialising v%s\n", SORABBYNGOFS_VERSION);

    err = register_filesystem(&sorabbyngofs_type);
    if (err) {
        pr_err("sorabbyngofs: failed to register filesystem: %d\n", err);
        return err;
    }

    pr_info("sorabbyngofs: registered — AI sandbox zone ready\n");
    return 0;
}

static void __exit sorabbyngofs_exit(void)
{
    unregister_filesystem(&sorabbyngofs_type);
    pr_info("sorabbyngofs: unregistered\n");
}

module_init(sorabbyngofs_init);
module_exit(sorabbyngofs_exit);

MODULE_LICENSE("GPL v2");
MODULE_AUTHOR("Sorabbyngo OS");
MODULE_DESCRIPTION("SorabbyngoFS — AI-native filesystem with consent gate and ByngoPress compression");
MODULE_VERSION(SORABBYNGOFS_VERSION);
