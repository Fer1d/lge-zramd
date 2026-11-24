#include "nzm_core.h"
#include "recovery.h"
#include "state_bootstrap.h"
#include <errno.h>
#include <string.h>
#include <unistd.h>
int main(int argc, char **argv) {
  /* Init launches this command in lge_zram_bootstrap, never the daemon domain. */
  if (argc >= 2 && !strcmp(argv[1], "bootstrap")) {
    if (argc != 3 || strcmp(argv[2], "--confirmed")) return 2;
    if (geteuid() != 0) { fprintf(stderr, "root required\n"); return 1; }
    int rc = nzm_state_bootstrap();
    if (rc) fprintf(stderr, "command=bootstrap errno=%d no_cleanup_no_retry\n", -rc);
    else puts("state_bootstrap=valid");
    return rc ? 1 : 0;
  }
  /* Read-only executable preflight, before all config/core/lock paths. */
  if (argc >= 2 && !strcmp(argv[1], "preflight-helper")) {
    if (argc != 2) return 2;
    if (geteuid() != 0) { fprintf(stderr, "root required\n"); return 1; }
    int rc = nzm_helper_preflight();
    if (rc) fprintf(stderr, "command=preflight-helper errno=%d helper_attempted=0 writeback_attempted=0\n", -rc);
    else puts("helper_preflight=valid helper_attempted=0 writeback_attempted=0");
    return rc ? 1 : 0;
  }
  /* Diagnostic only: no config, core init, lock, helper or persistent writes. */
  if (argc >= 2 && !strcmp(argv[1], "check-mounts")) {
    if (argc != 2) return 2;
    if (geteuid() != 0) { fprintf(stderr, "root required\n"); return 1; }
    int rc = nzm_data_verify();
    if (rc) fprintf(stderr, "command=check-mounts errno=%d no_cleanup_no_retry\n", -rc);
    else puts("mounts=valid");
    return rc ? 1 : 0;
  }
  if (argc < 2 ||
      !(!strcmp(argv[1], "status") || !strcmp(argv[1], "check-config") ||
        !strcmp(argv[1], "checkconfig") || !strcmp(argv[1], "boot") ||
        !strcmp(argv[1], "daemon") || !strcmp(argv[1], "pause"))) {
    fprintf(stderr, "usage: native-zram-manager status|check-config|check-mounts|preflight-helper|bootstrap --confirmed|boot "
                    "--confirmed|daemon --confirmed [--boot-completed]|pause --confirmed\n");
    return 2;
  }
  int readonly = !strcmp(argv[1], "status") ||
                 !strcmp(argv[1], "check-config") ||
                 !strcmp(argv[1], "checkconfig");
  int trusted_boot_completed = !strcmp(argv[1], "daemon") && argc == 4 &&
      !strcmp(argv[2], "--confirmed") && !strcmp(argv[3], "--boot-completed");
  if ((readonly && argc != 2) ||
      (!readonly && !trusted_boot_completed &&
       (argc != 3 || strcmp(argv[2], "--confirmed"))))
    return 2;
  if (!readonly && geteuid() != 0) {
    fprintf(stderr, "root required\n");
    return 1;
  }
  struct nzm_core c;
  nzm_core_init(&c);
  c.trusted_boot_completed = trusted_boot_completed;
  int rc = !strcmp(argv[1], "pause") ? 0 : nzm_core_config(&c);
  if (!rc) {
    if (!strcmp(argv[1], "status"))
      rc = nzm_core_status(&c, stdout);
    else if (!strcmp(argv[1], "boot") || !strcmp(argv[1], "daemon"))
      rc = nzm_phase1_start(&c, !strcmp(argv[1], "daemon"));
    else if (!strcmp(argv[1], "pause"))
      rc = nzm_core_pause(&c, "operator_pause");
    else {
      puts("config=valid");
      nzm_core_config_snapshot(&c, stdout);
    }
  }
  if (rc)
    fprintf(stderr, "command=%s errno=%d no_cleanup_no_retry\n", argv[1], -rc);
  nzm_core_close(&c);
  return rc ? 1 : 0;
}
