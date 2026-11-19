#include "nzm.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Tests replace only allocator boundaries at compile time. */
#ifdef NZM_TEST_ALLOC
extern void *nzm_test_calloc(size_t, size_t);
extern void *nzm_test_malloc(size_t);
#define calloc nzm_test_calloc
#define malloc nzm_test_malloc
#endif

static int number(const char **p, const char *end, uint64_t *out, char delim)
{
    uint64_t v=0;
    const char *s=*p;
    if (s==end || *s<'0' || *s>'9') return -EINVAL;
    while (s<end && *s>='0' && *s<='9') {
        unsigned d=(unsigned)(*s-'0');
        if (v>(UINT64_MAX-d)/10) return -ERANGE;
        v=v*10+d; ++s;
    }
    if (s==end || *s!=delim) return -EINVAL;
    *p=s+1; *out=v;
    return 0;
}

static int validate(const struct nzm_ledger *l, uint64_t now, uint64_t *used)
{
    uint64_t sum=0, prev=0, window=0;
    size_t i;
    if (!l || !now || now<l->highwater || now<l->validated_at) return -EINVAL;
    if (l->generation>NZM_LEDGER_MAX_RECORDS || l->count>NZM_LEDGER_MAX_RECORDS) return -E2BIG;
    if (l->generation!=l->count || (l->count && !l->records)) return -EINVAL;
    for (i=0;i<l->count;++i) {
        uint64_t t=l->records[i].timestamp, k=l->records[i].kib;
        if (!t || t<prev || t>l->highwater || !k) return -EINVAL;
        if (sum>UINT64_MAX-k) return -ERANGE;
        sum+=k; prev=t;
        if (now-t<NZM_LEDGER_WINDOW_SECONDS) {
            if (window>UINT64_MAX-k) return -ERANGE;
            window+=k;
        }
    }
    if (sum!=l->cumulative) return -EINVAL;
    *used=window;
    return 0;
}

void nzm_ledger_free(struct nzm_ledger *l)
{
    if (!l) return;
    free(l->records);
    memset(l,0,sizeof(*l));
}

int nzm_ledger_parse(const char *text, size_t len, uint64_t now, struct nzm_ledger *out)
{
    struct nzm_ledger l={0};
    const char *p, *end;
    size_t i;
    int rc;
    if (!text || !out || !len || !now) return -EINVAL;
    if (len>NZM_LEDGER_MAX_BYTES) return -E2BIG;
    if (text[len-1]!='\n' || memchr(text,0,len)) return -EINVAL;
    if (len<8 || memcmp(text,"wear-v1 ",8)) return -EINVAL;
    p=text+8; end=text+len;
    if ((rc=number(&p,end,&l.generation,' ')) ||
        (rc=number(&p,end,&l.highwater,' ')) ||
        (rc=number(&p,end,&l.cumulative,'\n'))) return rc;
    if (l.generation>NZM_LEDGER_MAX_RECORDS) return -E2BIG;
    /* Every row needs at least four bytes; reject impossible claims before alloc. */
    if (l.generation>(uint64_t)(end-p)/4) return -EINVAL;
    l.count=(size_t)l.generation;
    if (l.count) {
        l.records=calloc(l.count,sizeof(*l.records));
        if (!l.records) return -ENOMEM;
    }
    for (i=0;i<l.count;++i) {
        if ((rc=number(&p,end,&l.records[i].timestamp,' ')) ||
            (rc=number(&p,end,&l.records[i].kib,'\n'))) goto fail;
    }
    if (p!=end) { rc=-EINVAL; goto fail; }
    rc=validate(&l,now,&l.used_kib);
    if (rc) goto fail;
    l.validated_at=now;
    *out=l;
    return 0;
fail:
    nzm_ledger_free(&l);
    return rc;
}

int nzm_ledger_reserve_text(const struct nzm_ledger *l, uint64_t now,
                            uint64_t batch, uint64_t daily, char **out, size_t *len)
{
    uint64_t used;
    char *s;
    size_t cap, pos=0, i;
    int rc, n;
    if (out) *out=NULL;
    if (len) *len=0;
    if (!out || !len) return -EINVAL;
    if (!batch || !daily) return -EINVAL;
    rc=validate(l,now,&used);
    if (rc) return rc;
    if (l->generation>=NZM_LEDGER_MAX_RECORDS) return -E2BIG;
    if (l->cumulative>UINT64_MAX-batch) return -ERANGE;
    if (used>daily || batch>daily-used) return -EDQUOT;
    /* Header <= 72 bytes, each row <= 42 bytes; include new row and NUL. */
    if (l->count>(SIZE_MAX-176)/48) return -EOVERFLOW;
    cap=128+(l->count+1)*48;
    s=malloc(cap);
    if (!s) return -ENOMEM;
    n=snprintf(s,cap,"wear-v1 %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
               l->generation+1,now,l->cumulative+batch);
    if (n<0 || (size_t)n>=cap) { free(s); return -EOVERFLOW; }
    pos=(size_t)n;
    for (i=0;i<=l->count;++i) {
        uint64_t t=i==l->count ? now:l->records[i].timestamp;
        uint64_t k=i==l->count ? batch:l->records[i].kib;
        n=snprintf(s+pos,cap-pos,"%" PRIu64 " %" PRIu64 "\n",t,k);
        if (n<0 || (size_t)n>=cap-pos) { free(s); return -EOVERFLOW; }
        pos+=(size_t)n;
    }
    if (pos>NZM_LEDGER_MAX_BYTES) { free(s); return -E2BIG; }
    *out=s; *len=pos;
    return 0;
}
