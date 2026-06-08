# SorabbyngoFS

AI-native VFS filesystem module for Sorabbyngo OS.
Provides **Zone 3** — the AI sandbox filesystem — with consent-gated
file access, ByngoPress compression, and a self-healing journal.

---

## Architecture

Sorabbyngo OS uses a 3-zone filesystem strategy:

| Zone | Filesystem | Path | AI access |
|------|-----------|------|-----------|
| 1 — System | ext4 / btrfs | `/boot` `/bin` `/lib` `/etc` | Never |
| 2 — User | ext4 / btrfs | `/home` `/documents` | Consent-gated only |
| 3 — AI sandbox | **SorabbyngoFS** | `/ai-workspace` `/models` `/byngox-state` | Native |

All three zones are visible through the Linux VFS layer.
SorabbyngoFS registers as a standard kernel filesystem type
(`register_filesystem`) and mounts at Zone 3 paths only.

---

## Source layout
sorabbyngofs/
├── src/
│   ├── sorabbyngofs_main.c      # VFS registration + module init/exit
│   ├── sorabbyngofs_file.c      # read/write/open/release/ioctl
│   ├── sorabbyngofs_inode.c     # inode allocation + operations
│   ├── sorabbyngofs_consent.c   # consent gate (Ed25519 + CML tokens)
│   ├── sorabbyngofs_compress.c  # ByngoPress integration (stub)
│   └── sorabbyngofs_journal.c   # self-healing journal
├── include/
│   └── sorabbyngofs.h           # shared types, constants, declarations
├── Kconfig                      # kernel config entries
├── Makefile                     # kernel module build
└── README.md

---

## How consent works

Every AI file operation follows this flow:
AI process (ai-sandbox)
│
▼
open() ──► sorabbyngofs_open()       [fd returned, no token yet]
│
▼
ioctl(BYNGO_IOC_SET_TOKEN) ──► injects CML-issued Ed25519 token into fd
│
▼
read() / write()
│
├─► sorabbyngofs_consent_check()
│       1. Token not expired?
│       2. Permission bits match?
│       3. Ed25519 signature valid? (enforced in prod builds)
│       └── DENY → -EACCES returned, op never executes
│
├─► sorabbyngofs_journal_log()   [op recorded before execution]
│
└─► actual I/O
The consent token is issued by **CML** (`sorabbyngo-os/cml`) and
verified against the **sorabbyngo-security** Ed25519 public key.
Tokens are zeroed and freed on `close()` — they never outlive the fd.

---

## Kconfig options

| Option | Default | Description |
|--------|---------|-------------|
| `CONFIG_SORABBYNGOFS` | `m` | Build as kernel module (recommended) or `y` for built-in |
| `CONFIG_SORABBYNGOFS_ENFORCE_CONSENT` | `n` (dev) | **Must be `y` in production.** Enables Ed25519 signature verification on every AI file op. |
| `CONFIG_SORABBYNGOFS_COMPRESS` | `y` | Enable ByngoPress 10000:1 compression in Zone 3 |

> **Warning:** Never ship a production Sorabbyngo OS image with
> `CONFIG_SORABBYNGOFS_ENFORCE_CONSENT=n`. That flag exists only
> to unblock development before `sorabbyngo-security` key injection
> is wired up.

---

## Building

```bash
# Against your running kernel
make KERNEL_SRC=/lib/modules/$(uname -r)/build

# Against the Sorabbyngo OS kernel tree (Linux 6.19.10 + patches)
make KERNEL_SRC=/path/to/sorabbyngo-os/kernel

# Load the module
sudo insmod sorabbyngofs.ko

# Verify registration
cat /proc/filesystems | grep sorabbyngofs

# Mount Zone 3
sudo mkdir -p /ai-workspace
sudo mount -t sorabbyngofs none /ai-workspace
```

---

## ioctls

| ioctl | Direction | Description |
|-------|-----------|-------------|
| `BYNGO_IOC_SET_TOKEN` | write | Inject a CML consent token into an open fd |
| `BYNGO_IOC_GET_AUDIT` | read | Read `ai_access_count` for audit/CML queries |

Magic number: `'B'` (0x42)

---

## Related repos

| Repo | Relationship |
|------|-------------|
| `sorabbyngo-os/sorabbyngo-security` | Issues Ed25519 public key injected into consent gate |
| `sorabbyngo-os/cml` | Issues and signs consent tokens (CML layer) |
| `sorabbyngo-os/byngo-press` | Compression engine called from `sorabbyngofs_compress.c` |
| `sorabbyngo-os/self-healing` | Journal anomalies reported here via netlink |
| `sorabbyngo-os/ai-sandbox` | Injects consent tokens via `BYNGO_IOC_SET_TOKEN` ioctl |
| `sorabbyngo-os/sorabbyngo-patches` | Kernel patch set this module builds against |

---

## Status

| Component | Status |
|-----------|--------|
| VFS registration | Done |
| Consent gate (expiry + perms) | Done |
| Consent gate (Ed25519 sig) | Stub — pending sorabbyngo-security key export |
| Journal (in-memory) | Done |
| Journal (disk persist + netlink) | Done (netlink) — disk persist TODO |
| File read/write | Done (skeleton) |
| ByngoPress compress path | Contract defined — pending byngo-press export |
| Inode operations | Done |
| Production mount testing | TODO |

---

*Sorabbyngo OS — Africa-first AI OS*
*Linux 6.19.10 + patches | x86 | GPL v2*
