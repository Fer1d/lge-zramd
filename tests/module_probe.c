#define _GNU_SOURCE
#include "runtime.h"
#include <errno.h>
#include <stdio.h>

struct probe { struct nzm_runtime *runtime; unsigned calls; };

static void execute(void *context, const struct nzm_runtime_job *job,
                     const struct nzm_runtime_control *control,
                     struct nzm_runtime_result *result)
{
    struct probe *probe = context;
    sigset_t mask;
    if (job->sequence != 1 || !(job->reasons & NZM_REASON_STARTUP) ||
        nzm_runtime_control_get_exec_mask(control, &mask) ||
        sigismember(&mask, SIGTERM) == 1)
        result->code = -EINVAL;
    probe->calls++;
    (void)nzm_runtime_request_stop(probe->runtime);
}

int main(void)
{
    struct nzm_runtime_options options;
    struct probe probe = {0};
    struct nzm_runtime_executor executor = { .owner_context = &probe, .execute = execute };
    struct nzm_runtime_stats stats;
    int rc;
    nzm_runtime_options_default(&options);
    options.psi.enabled = false; /* no real PSI registration or daemon startup */
    rc = nzm_runtime_create(&probe.runtime, &options, &executor);
    if (!rc)
        rc = nzm_runtime_run(probe.runtime);
    if (!rc)
        rc = nzm_runtime_get_stats(probe.runtime, &stats);
    if (!rc && (probe.calls != 1 || !stats.worker_joined || stats.last_result_code))
        rc = -EIO;
    if (nzm_runtime_destroy(probe.runtime) && !rc)
        rc = -EIO;
    if (rc) {
        fprintf(stderr, "module probe failed: %d\n", rc);
        return 1;
    }
    puts("module probe: PASS (single callback, cooperative stop, joined)");
    return 0;
}
