#define _POSIX_C_SOURCE 200809L
#include "wear_budget.h"
#include "nzm.h"
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static int boot_valid(const char *s)
{
    size_t i;
    if (!s) return 0;
    for (i=0;i<36;i++) {
        if (i==8 || i==13 || i==18 || i==23) {
            if (s[i]!='-') return 0;
        } else if (!((s[i]>='0' && s[i]<='9') || (s[i]>='a' && s[i]<='f')))
            return 0;
    }
    return s[36]=='\0';
}

/* The expiry of bucket i is (i+97)*15min. Division-based comparisons
 * avoid overflowing when logical nanoseconds approach UINT64_MAX. */
static int expired(uint64_t index, uint64_t now)
{
    uint64_t current=now/NZM_WEAR_BUCKET_NS;
    return current>=index && current-index>=NZM_WEAR_MAX_BUCKETS;
}

static uint64_t expiry(uint64_t index)
{
    if (index>UINT64_MAX/NZM_WEAR_BUCKET_NS-NZM_WEAR_MAX_BUCKETS)
        return UINT64_MAX;
    return (index+NZM_WEAR_MAX_BUCKETS)*NZM_WEAR_BUCKET_NS;
}

static int reservation_valid(const struct nzm_budget_reservation *r)
{
    size_t i;
    if (r->request_id)
        return boot_valid(r->boot) && r->generation && r->reserved_kib;
    for (i=0;i<sizeof r->boot;i++) if (r->boot[i]) return 0;
    return !r->generation && !r->reserved_kib;
}

static int same_reservation(const struct nzm_budget_reservation *a,
                            const struct nzm_budget_reservation *b)
{
    return a->request_id==b->request_id && a->generation==b->generation &&
           a->reserved_kib==b->reserved_kib && !strcmp(a->boot,b->boot);
}

static int valid(const struct nzm_budget *s)
{
    uint64_t sum;
    size_t i;
    if (!s || !boot_valid(s->boot) || s->count>NZM_WEAR_MAX_BUCKETS)
        return -EINVAL;
    if (s->count>s->generation || s->generation>s->cumulative_kib ||
        (!s->generation && s->cumulative_kib) ||
        !reservation_valid(&s->pending) ||
        !reservation_valid(&s->receipt.reservation)) return -EINVAL;
    if (s->pending.request_id && s->pending.generation!=s->generation)
        return -EINVAL;
    if (s->receipt.reservation.request_id) {
        if (s->receipt.reservation.generation>s->generation ||
            (s->pending.request_id &&
             s->receipt.reservation.generation>=s->pending.generation) ||
            (!s->pending.request_id &&
             s->receipt.reservation.generation!=s->generation) ||
            s->receipt.reservation.reserved_kib>s->cumulative_kib ||
            s->receipt.submitted_kib>s->receipt.reservation.reserved_kib ||
            s->receipt.submitted_kib>s->known_submitted_kib) return -EINVAL;
    } else if (s->receipt.submitted_kib || s->known_submitted_kib) return -EINVAL;
    if (s->pending.reserved_kib>s->cumulative_kib ||
        s->known_submitted_kib>s->cumulative_kib-s->pending.reserved_kib)
        return -EINVAL;
    sum=s->pending.reserved_kib;
    for (i=0;i<s->count;i++) {
        const struct nzm_budget_bucket *b=&s->buckets[i];
        if (!b->kib || b->index>s->logical_ns/NZM_WEAR_BUCKET_NS ||
            expired(b->index,s->logical_ns) ||
            (i && b->index<=s->buckets[i-1].index)) return -EINVAL;
        if (sum>UINT64_MAX-b->kib) return -ERANGE;
        sum+=b->kib;
    }
    return sum>s->cumulative_kib ? -EINVAL:0;
}

int nzm_budget_init(const struct nzm_budget_sample *sample, struct nzm_budget *out)
{
    struct nzm_budget s={0};
    if (!sample || !out || !boot_valid(sample->boot)) return -EINVAL;
    memcpy(s.boot,sample->boot,sizeof s.boot);
    s.boottime_ns=sample->boottime_ns;
    s.next_request_id=1;
    *out=s;
    return 0;
}

int nzm_budget_project(const struct nzm_budget *s,
                       const struct nzm_budget_sample *sample, uint64_t *now)
{
    uint64_t delta=0;
    int rc;
    if (!sample || !now || !boot_valid(sample->boot)) return -EINVAL;
    if ((rc=valid(s))) return rc;
    if (!strcmp(s->boot,sample->boot)) {
        if (sample->boottime_ns<s->boottime_ns) return -ERANGE;
        delta=sample->boottime_ns-s->boottime_ns;
    }
    if (s->logical_ns>UINT64_MAX-delta) return -ERANGE;
    *now=s->logical_ns+delta;
    return 0;
}

int nzm_budget_reanchor(const struct nzm_budget *s,
                        const struct nzm_budget_sample *sample,
                        struct nzm_budget *out)
{
    struct nzm_budget next;
    uint64_t now;
    size_t i, n=0;
    int rc;
    if (!out) return -EINVAL;
    if ((rc=nzm_budget_project(s,sample,&now))) return rc;
    next=*s;
    next.logical_ns=now;
    next.boottime_ns=sample->boottime_ns;
    if (strcmp(s->boot,sample->boot)) next.next_request_id=1;
    memcpy(next.boot,sample->boot,sizeof next.boot);
    for (i=0;i<s->count;i++)
        if (!expired(s->buckets[i].index,now)) next.buckets[n++]=s->buckets[i];
    memset(next.buckets+n,0,(NZM_WEAR_MAX_BUCKETS-n)*sizeof next.buckets[0]);
    next.count=n;
    *out=next;
    return 0;
}

int nzm_budget_budget(const struct nzm_budget *s,
                      const struct nzm_budget_sample *sample, uint64_t daily,
                      uint64_t *used, uint64_t *available, uint64_t *next_expiry)
{
    uint64_t now, sum=0, first=0;
    size_t i;
    int rc;
    if (!daily || !used || !available || !next_expiry) return -EINVAL;
    if ((rc=nzm_budget_project(s,sample,&now))) return rc;
    for (i=0;i<s->count;i++) {
        if (expired(s->buckets[i].index,now)) continue;
        /* valid() already proved the total sum cannot overflow. */
        sum+=s->buckets[i].kib;
        if (!first) first=expiry(s->buckets[i].index);
    }
    sum+=s->pending.reserved_kib; /* Never expires, even after reboot/day. */
    *used=sum;
    *available=sum>=daily ? 0:daily-sum;
    *next_expiry=first;
    return 0;
}

static int number(const char **p, const char *end, uint64_t *out, char delim)
{
    const char *s=*p, *start=s;
    uint64_t n=0;
    if (s==end || *s<'0' || *s>'9') return -EINVAL;
    while (s<end && *s>='0' && *s<='9') {
        unsigned d=(unsigned)(*s-'0');
        if (n>(UINT64_MAX-d)/10) return -ERANGE;
        n=n*10+d;
        s++;
    }
    if (s==end || *s!=delim || (s-start>1 && *start=='0')) return -EINVAL;
    *out=n;
    *p=s+1;
    return 0;
}

static int record(const char **p, const char *end, const char *tag,
                  struct nzm_budget_reservation *r, char delim)
{
    size_t n=strlen(tag);
    int rc;
    if ((size_t)(end-*p)<n || memcmp(*p,tag,n)) return -EINVAL;
    *p+=n;
    if ((size_t)(end-*p)>=2 && (*p)[0]=='-' && (*p)[1]==' ') *p+=2;
    else {
        if ((size_t)(end-*p)<37) return -EINVAL;
        memcpy(r->boot,*p,36); r->boot[36]=0; *p+=36;
        if (!boot_valid(r->boot) || *(*p)++!=' ') return -EINVAL;
    }
    if ((rc=number(p,end,&r->request_id,' ')) ||
        (rc=number(p,end,&r->generation,' ')) ||
        (rc=number(p,end,&r->reserved_kib,delim))) return rc;
    return reservation_valid(r) ? 0:-EINVAL;
}

int nzm_budget_parse(const char *text, size_t len,
                     const struct nzm_budget_sample *sample,
                     struct nzm_budget *out, int *migrated)
{
    struct nzm_budget s={0};
    const char *p, *end;
    uint64_t count;
    size_t i;
    int rc, version;
    if (!text || !out || !migrated || len<8) return -EINVAL;
    if (len>NZM_LEDGER_MAX_BYTES) return -E2BIG;
    if (text[len-1]!='\n' || memchr(text,0,len)) return -EINVAL;
    if (!memcmp(text,"wear-v1 ",8)) {
        struct nzm_ledger legacy={0};
        if ((rc=nzm_budget_init(sample,&s))) return rc;
        /* The legacy parser accepts padded decimals. Migration has the same
         * canonical-number contract as v2; leave legacy callers unchanged. */
        for (p=text+8;p<text+len-1;p++) {
            if ((p==text+8 || p[-1]==' ' || p[-1]=='\n') &&
                p[0]=='0' && p[1]>='0' && p[1]<='9') return -EINVAL;
        }
        /* Legacy highwater/timestamps are validated for internal consistency,
         * not against RTC or boottime and never used to age away lifetime wear. */
        rc=nzm_ledger_parse(text,len,UINT64_MAX,&legacy);
        if (rc) return rc;
        s.generation=legacy.generation;
        s.cumulative_kib=legacy.cumulative;
        if (legacy.cumulative) {
            s.count=1;
            s.buckets[0].index=s.logical_ns/NZM_WEAR_BUCKET_NS;
            s.buckets[0].kib=legacy.cumulative;
        }
        nzm_ledger_free(&legacy);
        *out=s;
        *migrated=1;
        return 0;
    }
    if (len>NZM_WEAR_MAX_BYTES) return -E2BIG;
    if (!memcmp(text,"wear-v2 ",8)) version=2;
    else if (!memcmp(text,"wear-v3 ",8)) version=3;
    else return -EINVAL;
    p=text+8; end=text+len;
    if ((rc=number(&p,end,&s.generation,' ')) ||
        (rc=number(&p,end,&s.cumulative_kib,' '))) return rc;
    if ((size_t)(end-p)<37) return -EINVAL;
    memcpy(s.boot,p,36); s.boot[36]=0; p+=36;
    if (!boot_valid(s.boot) || *p++!=' ') return -EINVAL;
    if ((rc=number(&p,end,&s.logical_ns,' ')) ||
        (rc=number(&p,end,&s.boottime_ns,' ')) ||
        (rc=number(&p,end,&s.next_request_id,' ')) ||
        (rc=number(&p,end,&count,version==3 ? ' ':'\n'))) return rc;
    if (version==3 && (rc=number(&p,end,&s.known_submitted_kib,'\n'))) return rc;
    if (count>NZM_WEAR_MAX_BUCKETS) return -E2BIG;
    s.count=(size_t)count;
    for (i=0;i<s.count;i++) {
        if ((rc=number(&p,end,&s.buckets[i].index,' ')) ||
            (rc=number(&p,end,&s.buckets[i].kib,'\n'))) return rc;
    }
    if (version==3) {
        if ((rc=record(&p,end,"pending ",&s.pending,'\n')) ||
            (rc=record(&p,end,"receipt ",&s.receipt.reservation,' ')) ||
            (rc=number(&p,end,&s.receipt.submitted_kib,'\n'))) return rc;
    }
    if (p!=end) return -EINVAL;
    if ((rc=valid(&s))) return rc;
    *out=s;
    *migrated=version==2 ? NZM_BUDGET_MIGRATED_V2:NZM_BUDGET_CURRENT;
    return 0;
}

static int serialize(const struct nzm_budget *s, char **out, size_t *len)
{
    char *text;
    size_t pos, i;
    int n;
    text=malloc(NZM_WEAR_MAX_BYTES);
    if (!text) return -ENOMEM;
    n=snprintf(text,NZM_WEAR_MAX_BYTES,
               "wear-v3 %" PRIu64 " %" PRIu64 " %s %" PRIu64 " %" PRIu64
               " %" PRIu64 " %zu %" PRIu64 "\n",s->generation,s->cumulative_kib,s->boot,
               s->logical_ns,s->boottime_ns,s->next_request_id,s->count,
               s->known_submitted_kib);
    if (n<0 || (size_t)n>=NZM_WEAR_MAX_BYTES) { free(text); return -EOVERFLOW; }
    pos=(size_t)n;
    for (i=0;i<s->count;i++) {
        n=snprintf(text+pos,NZM_WEAR_MAX_BYTES-pos,"%" PRIu64 " %" PRIu64 "\n",
                   s->buckets[i].index,s->buckets[i].kib);
        if (n<0 || (size_t)n>=NZM_WEAR_MAX_BYTES-pos) { free(text); return -EOVERFLOW; }
        pos+=(size_t)n;
    }
    n=snprintf(text+pos,NZM_WEAR_MAX_BYTES-pos,
               "pending %s %" PRIu64 " %" PRIu64 " %" PRIu64 "\n"
               "receipt %s %" PRIu64 " %" PRIu64 " %" PRIu64 " %" PRIu64 "\n",
               s->pending.request_id ? s->pending.boot:"-",
               s->pending.request_id,s->pending.generation,s->pending.reserved_kib,
               s->receipt.reservation.request_id ? s->receipt.reservation.boot:"-",
               s->receipt.reservation.request_id,s->receipt.reservation.generation,
               s->receipt.reservation.reserved_kib,s->receipt.submitted_kib);
    if (n<0 || (size_t)n>=NZM_WEAR_MAX_BYTES-pos) { free(text); return -EOVERFLOW; }
    pos+=(size_t)n;
    *out=text; *len=pos;
    return 0;
}

int nzm_budget_checkpoint_text(const struct nzm_budget *s,
                               const struct nzm_budget_sample *sample,
                               struct nzm_budget *next, char **text, size_t *len)
{
    struct nzm_budget candidate;
    int rc;
    if (text) *text=NULL;
    if (len) *len=0;
    if (!next || !text || !len) return -EINVAL;
    if ((rc=nzm_budget_reanchor(s,sample,&candidate))) return rc;
    if ((rc=serialize(&candidate,text,len))) return rc;
    *next=candidate;
    return 0;
}

int nzm_budget_reserve_text(const struct nzm_budget *s,
                            const struct nzm_budget_sample *sample,
                            uint64_t batch, uint64_t daily, uint64_t request,
                            struct nzm_budget *next, char **text, size_t *len)
{
    struct nzm_budget candidate;
    uint64_t used, available, when;
    int rc;
    if (text) *text=NULL;
    if (len) *len=0;
    if (!next || !text || !len || !batch || !daily || !request) return -EINVAL;
    if ((rc=nzm_budget_reanchor(s,sample,&candidate))) return rc;
    if (candidate.pending.request_id) return -EBUSY;
    if (request!=candidate.next_request_id) return -EINVAL;
    if ((rc=nzm_budget_budget(&candidate,sample,daily,&used,&available,&when))) return rc;
    if (batch>available) return -EDQUOT;
    if (candidate.generation==UINT64_MAX ||
        candidate.cumulative_kib>UINT64_MAX-batch) return -ERANGE;
    memcpy(candidate.pending.boot,sample->boot,sizeof candidate.pending.boot);
    candidate.pending.request_id=request;
    candidate.pending.generation=candidate.generation+1;
    candidate.pending.reserved_kib=batch;
    candidate.generation++;
    candidate.cumulative_kib+=batch;
    candidate.next_request_id=request==UINT64_MAX ? 0:request+1;
    if ((rc=serialize(&candidate,text,len))) return rc;
    *next=candidate;
    return 0;
}

int nzm_budget_settle_text(const struct nzm_budget *s,
                           const struct nzm_budget_sample *sample,
                           const struct nzm_budget_reservation *identity,
                           uint64_t submitted, struct nzm_budget *next,
                           char **text, size_t *len)
{
    struct nzm_budget candidate;
    uint64_t index;
    int rc;
    if (text) *text=NULL;
    if (len) *len=0;
    if (!next || !text || !len || !identity || !identity->request_id ||
        !reservation_valid(identity) || submitted>identity->reserved_kib ||
        !sample || !boot_valid(sample->boot)) return -EINVAL;
    if ((rc=valid(s))) return rc;
    /* A retry must not move anchors, expire buckets or alter a newer pending. */
    if (same_reservation(identity,&s->receipt.reservation)) {
        if (submitted!=s->receipt.submitted_kib) return -EINVAL;
        if ((rc=serialize(s,text,len))) return rc;
        *next=*s;
        return 0;
    }
    if (!same_reservation(identity,&s->pending) ||
        strcmp(sample->boot,identity->boot) || strcmp(s->boot,identity->boot))
        return -EINVAL;
    if ((rc=nzm_budget_reanchor(s,sample,&candidate))) return rc;
    if (candidate.known_submitted_kib>UINT64_MAX-submitted) return -ERANGE;
    index=candidate.logical_ns/NZM_WEAR_BUCKET_NS;
    if (submitted) {
        if (candidate.count && candidate.buckets[candidate.count-1].index==index) {
            uint64_t *kib=&candidate.buckets[candidate.count-1].kib;
            if (*kib>UINT64_MAX-submitted) return -ERANGE;
            *kib+=submitted;
        } else {
            if (candidate.count==NZM_WEAR_MAX_BUCKETS) return -E2BIG;
            candidate.buckets[candidate.count].index=index;
            candidate.buckets[candidate.count++].kib=submitted;
        }
    }
    candidate.receipt.reservation=candidate.pending;
    candidate.receipt.submitted_kib=submitted;
    memset(&candidate.pending,0,sizeof candidate.pending);
    candidate.known_submitted_kib+=submitted;
    if ((rc=valid(&candidate))) return rc;
    if ((rc=serialize(&candidate,text,len))) return rc;
    *next=candidate;
    return 0;
}

int nzm_budget_checkpoint_due(const struct nzm_budget *s,
                              const struct nzm_budget_sample *sample,
                              uint64_t interval, int *due)
{
    uint64_t now;
    int rc;
    if (!due || !interval) return -EINVAL;
    if ((rc=nzm_budget_project(s,sample,&now))) return rc;
    *due=strcmp(s->boot,sample->boot)!=0 || now-s->logical_ns>=interval ||
         (s->count && expired(s->buckets[0].index,now));
    return 0;
}

int nzm_budget_clock_boottime(void *unused, uint64_t *out)
{
    struct timespec ts;
    uint64_t sec;
    (void)unused;
    if (!out) return -EINVAL;
    if (clock_gettime(CLOCK_BOOTTIME,&ts)) return -errno;
    if (ts.tv_sec<0 || ts.tv_nsec<0 || ts.tv_nsec>=1000000000L) return -ERANGE;
    sec=(uint64_t)ts.tv_sec;
    if (sec>(UINT64_MAX-(uint64_t)ts.tv_nsec)/UINT64_C(1000000000)) return -ERANGE;
    *out=sec*UINT64_C(1000000000)+(uint64_t)ts.tv_nsec;
    return 0;
}

int nzm_budget_sample_clock(const char *boot, nzm_budget_sampler sampler,
                            void *ctx, struct nzm_budget_sample *out)
{
    struct nzm_budget_sample sample={0};
    int rc;
    if (!out || !boot_valid(boot)) return -EINVAL;
    memcpy(sample.boot,boot,sizeof sample.boot);
    if (!sampler) sampler=nzm_budget_clock_boottime;
    if ((rc=sampler(ctx,&sample.boottime_ns))) return rc<0 ? rc:-EIO;
    *out=sample;
    return 0;
}
