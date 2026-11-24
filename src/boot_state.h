#ifndef NZM_BOOT_STATE_H
#define NZM_BOOT_STATE_H
#include <stddef.h>
#include <sys/types.h>

#define NZM_BOOT_BINDING_MAX 512
#define NZM_BOOT_ENTRIES 11
#define NZM_BOOT_VALUE_MAX 256
/* Startup checkpoints only. No event log, execution intents, or wear records. */
struct nzm_boot_state {
  int dirfd, authority_fd, loaded, poisoned;
  dev_t authority_dev;
  ino_t authority_ino;
  char boot[37], binding[NZM_BOOT_BINDING_MAX];
  unsigned present;
  char value[NZM_BOOT_ENTRIES][NZM_BOOT_VALUE_MAX];
};
void nzm_boot_init(struct nzm_boot_state *s);
int nzm_boot_lock(struct nzm_boot_state *s, int dirfd);
/* Revalidates the immutable authority AND the directory entry's exact inode. */
int nzm_boot_assert(const struct nzm_boot_state *s);
void nzm_boot_close(struct nzm_boot_state *s);
/* Read-only; caller supplies a verified directory. Pending txn/new fails closed. */
int nzm_boot_load(struct nzm_boot_state *s, int dirfd);
/* Caller must first verify live cold/unowned state, including on missing state. */
int nzm_boot_reset(struct nzm_boot_state *s, const char *boot, const char *binding);
int nzm_boot_absent(const struct nzm_boot_state *s, const char *name);
int nzm_boot_text(const struct nzm_boot_state *s, const char *name, char *out, size_t cap);
/* Exclusive bounded entry creation; any uncertain transaction poisons the owner. */
int nzm_boot_mark(struct nzm_boot_state *s, const char *name, const char *value);
#endif
