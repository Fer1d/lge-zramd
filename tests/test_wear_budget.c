#include "wear_budget.h"
#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char boot_a[]="01234567-89ab-cdef-0123-456789abcdef";
static const char boot_b[]="fedcba98-7654-3210-fedc-ba9876543210";
static unsigned checks;
#define CHECK(x) do { ++checks; if (!(x)) { fprintf(stderr,"FAIL %d: %s\n",__LINE__,#x); abort(); } } while (0)

static struct nzm_budget_sample sample(uint64_t ns, int reboot)
{
    struct nzm_budget_sample s={0};
    memcpy(s.boot,reboot ? boot_b:boot_a,sizeof s.boot);
    s.boottime_ns=ns;
    return s;
}

static void reserve_pending(struct nzm_budget *s, struct nzm_budget_sample at,
                    uint64_t amount, uint64_t daily)
{
    struct nzm_budget next, parsed;
    char *text=NULL;
    size_t len=0;
    int migrated=-1;
    uint64_t request=strcmp(s->boot,at.boot) ? 1:s->next_request_id;
    CHECK(!nzm_budget_reserve_text(s,&at,amount,daily,request,&next,&text,&len));
    CHECK(text && len==strlen(text) && len<NZM_WEAR_MAX_BYTES);
    CHECK(!nzm_budget_parse(text,len,&at,&parsed,&migrated) && !migrated);
    CHECK(!memcmp(&next,&parsed,sizeof next));
    free(text);
    *s=next;
}

static void settle(struct nzm_budget *s, struct nzm_budget_sample at, uint64_t amount)
{
    struct nzm_budget next, parsed, before=*s;
    struct nzm_budget_reservation identity=s->pending;
    char *text=NULL;
    size_t len=0;
    int migrated=-1;
    CHECK(!nzm_budget_settle_text(s,&at,&identity,amount,&next,&text,&len));
    CHECK(!memcmp(s,&before,sizeof before));
    CHECK(text && len==strlen(text) && len<NZM_WEAR_MAX_BYTES);
    CHECK(!nzm_budget_parse(text,len,NULL,&parsed,&migrated) && !migrated);
    CHECK(!memcmp(&next,&parsed,sizeof next));
    CHECK(next.cumulative_kib==s->cumulative_kib && next.generation==s->generation);
    free(text);
    *s=next;
}

static void reserve(struct nzm_budget *s, struct nzm_budget_sample at,
                    uint64_t amount, uint64_t daily)
{
    reserve_pending(s,at,amount,daily);
    settle(s,at,amount);
}

static void budget(struct nzm_budget *s, struct nzm_budget_sample at,
                   uint64_t daily, uint64_t expected, uint64_t expiry)
{
    uint64_t used=123, available=456, next=789;
    CHECK(!nzm_budget_budget(s,&at,daily,&used,&available,&next));
    CHECK(used==expected && available==(expected>=daily ? 0:daily-expected));
    CHECK(next==expiry);
}

static void boundaries(void)
{
    struct nzm_budget s;
    struct nzm_budget_sample at=sample(0,0);
    uint64_t b=NZM_WEAR_BUCKET_NS, w=NZM_WEAR_WINDOW_NS;
    CHECK(!nzm_budget_init(&at,&s));
    reserve(&s,at,10,100);
    at.boottime_ns=b-1; reserve(&s,at,20,100);
    CHECK(s.count==1 && s.buckets[0].kib==30);
    at.boottime_ns=b; reserve(&s,at,40,100);
    CHECK(s.count==2);
    budget(&s,sample(w-1,0),100,70,w+b);
    budget(&s,sample(w,0),100,70,w+b); /* exact 24h must not age bucket early */
    budget(&s,sample(w+b-1,0),100,70,w+b);
    budget(&s,sample(w+b,0),100,40,w+2*b);
    budget(&s,sample(w+2*b,0),100,0,0);
    /* Last nanosecond of bucket0 survives at least a full 24 hours. */
    CHECK((w+b)-(b-1)>w);
    CHECK(s.cumulative_kib==70 && s.generation==3); /* no query refunds */
    struct nzm_budget pruned;
    int due=0;
    at=sample(w+b,0);
    CHECK(!nzm_budget_checkpoint_due(&s,&at,UINT64_MAX,&due) && due);
    CHECK(!nzm_budget_reanchor(&s,&at,&pruned));
    CHECK(pruned.count==1 && pruned.buckets[0].index==1);
    CHECK(pruned.cumulative_kib==70 && pruned.generation==3);
}

static void projection(void)
{
    struct nzm_budget s, next, parsed;
    struct nzm_budget_sample at=sample(100,0);
    uint64_t now;
    int due, migrated;
    char *text;
    size_t len;
    CHECK(!nzm_budget_init(&at,&s));
    reserve(&s,at,5,100);
    at.boottime_ns=150;
    for (int i=0;i<20;i++) {
        CHECK(!nzm_budget_project(&s,&at,&now) && now==50);
        CHECK(s.logical_ns==0 && s.boottime_ns==100);
    }
    CHECK(!nzm_budget_checkpoint_due(&s,&at,51,&due) && !due);
    CHECK(!nzm_budget_checkpoint_due(&s,&at,50,&due) && due);
    CHECK(!nzm_budget_checkpoint_text(&s,&at,&next,&text,&len));
    CHECK(next.logical_ns==50 && next.boottime_ns==150 && next.generation==1);
    CHECK(!nzm_budget_parse(text,len,&at,&parsed,&migrated));
    free(text); s=parsed;
    CHECK(!nzm_budget_project(&s,&at,&now) && now==50);
    at.boottime_ns=200;
    CHECK(!nzm_budget_project(&s,&at,&now) && now==100);
    /* Crash loses unpersisted old-boot 50ns; even huge new-boot uptime and
     * arbitrary real-time changes are absent from pure clock inputs. */
    at=sample(UINT64_C(9000000000000000),1);
    for (int i=0;i<20;i++) CHECK(!nzm_budget_project(&s,&at,&now) && now==50);
    CHECK(!nzm_budget_checkpoint_due(&s,&at,UINT64_MAX,&due) && due);
    CHECK(!nzm_budget_reanchor(&s,&at,&next));
    CHECK(next.logical_ns==50 && next.next_request_id==1);
    CHECK(next.cumulative_kib==s.cumulative_kib && next.generation==s.generation);
    at.boottime_ns+=200;
    CHECK(!nzm_budget_project(&next,&at,&now) && now==250);
    reserve(&next,at,5,100);
    CHECK(next.next_request_id==2 && next.cumulative_kib==10);
    budget(&next,at,100,10,NZM_WEAR_WINDOW_NS+NZM_WEAR_BUCKET_NS);
}

static void bounded_growth(void)
{
    struct nzm_budget s;
    struct nzm_budget_sample at=sample(0,0);
    const uint64_t b=NZM_WEAR_BUCKET_NS;
    size_t maximum=0;
    CHECK(!nzm_budget_init(&at,&s));
    /* 100 days, four reservations per bucket, bounded independently of total
     * requests/generation. Every helper serializes and strict-parses each step. */
    for (uint64_t i=0;i<100*96*4;i++) {
        at.boottime_ns=i*(b/4);
        reserve(&s,at,1,1000);
        CHECK(s.count<=97);
        if (s.count>maximum) maximum=s.count;
    }
    CHECK(maximum==97 && s.generation==38400 && s.cumulative_kib==38400);
    CHECK(s.next_request_id==38401 && s.count==97);
    at.boottime_ns+=2*NZM_WEAR_WINDOW_NS;
    struct nzm_budget pruned;
    CHECK(!nzm_budget_reanchor(&s,&at,&pruned) && !pruned.count);
    CHECK(pruned.generation==38400 && pruned.cumulative_kib==38400);
    budget(&pruned,at,1000,0,0);
    reserve(&pruned,at,1,1000);
    CHECK(pruned.generation==38401 && pruned.count==1);
}

static void migration(void)
{
    const char *v1="wear-v1 3 18446744073709551615 600\n1 100\n100000 200\n18446744073709551615 300\n";
    struct nzm_budget s, next;
    struct nzm_budget_sample at=sample(UINT64_C(999000000000000),0);
    char *text;
    size_t len;
    int migrated=-1;
    CHECK(!nzm_budget_parse(v1,strlen(v1),&at,&s,&migrated) && migrated==1);
    CHECK(s.generation==3 && s.cumulative_kib==600 && s.count==1);
    CHECK(s.buckets[0].index==0 && s.buckets[0].kib==600 && s.logical_ns==0);
    budget(&s,at,500,600,NZM_WEAR_WINDOW_NS+NZM_WEAR_BUCKET_NS);
    CHECK(nzm_budget_reserve_text(&s,&at,1,500,1,&next,&text,&len)==-EDQUOT);
    CHECK(!text && !len);
    CHECK(!nzm_budget_checkpoint_text(&s,&at,&next,&text,&len));
    CHECK(!strncmp(text,"wear-v3 3 600 ",14));
    CHECK(!nzm_budget_parse(text,len,&at,&s,&migrated) && !migrated);
    free(text);
    at.boottime_ns+=NZM_WEAR_WINDOW_NS;
    budget(&s,at,500,600,NZM_WEAR_WINDOW_NS+NZM_WEAR_BUCKET_NS);
    at.boottime_ns+=NZM_WEAR_BUCKET_NS;
    budget(&s,at,500,0,0);
    reserve(&s,at,1,500);
    CHECK(s.generation==4 && s.cumulative_kib==601);
    v1="wear-v1 0 0 0\n";
    CHECK(!nzm_budget_parse(v1,strlen(v1),&at,&s,&migrated));
    CHECK(migrated && !s.count && s.next_request_id==1);
}

static void bad(const char *text)
{
    struct nzm_budget s, before;
    struct nzm_budget_sample at=sample(0,0);
    int migrated=42;
    memset(&s,0x5a,sizeof s); before=s;
    CHECK(nzm_budget_parse(text,strlen(text),&at,&s,&migrated)<0);
    CHECK(!memcmp(&s,&before,sizeof s) && migrated==42);
}

static void corrupt_and_overflow(void)
{
    char text[512];
    const char *bad_headers[]={
        "wear-v2 01 1 %s 0 0 2 1\n0 1\n",
        "wear-v2 1 1 %s 0 0 2 98\n",
        "wear-v2 1 1 %s 0 0 2 1\n0 0\n",
        "wear-v2 1 1 %s 0 0 2 1\n1 1\n",
        "wear-v2 2 2 %s 0 0 3 2\n0 1\n0 1\n",
        "wear-v2 2 2 %s 900000000000 0 3 2\n1 1\n0 1\n",
        "wear-v2 1 1 %s 87300000000000 0 2 1\n0 1\n",
        "wear-v2 1 1 %s 0 0 2 1\n0 2\n",
        "wear-v2 0 1 %s 0 0 2 0\n",
        "wear-v2 2 1 %s 0 0 2 0\n",
        "wear-v2 2 18446744073709551615 %s 900000000000 0 3 2\n0 18446744073709551615\n1 1\n",
        "wear-v2 1 1 %s 0 0 2 0\nextra\n",
        "wear-v2 1 1 %s 18446744073709551616 0 2 0\n",
        "wear-v2 1 1 %s -1 0 2 0\n",
        "wear-v2 1 1 %s 0 0 2 0\r\n"
    };
    for (size_t i=0;i<sizeof bad_headers/sizeof *bad_headers;i++) {
        snprintf(text,sizeof text,bad_headers[i],boot_a); bad(text);
    }
    bad(""); bad("wear-v2"); bad("wear-v3 0\n");
    bad("wear-v1 2 2 2\n1 1\n");
    bad("wear-v1 1 2 9\n2 8\n");
    snprintf(text,sizeof text,"wear-v2 1 1 %s 0 0 2 1\n0 1\n",boot_a);
    text[25]='A'; bad(text); /* Uppercase UUID is not canonical. */
    snprintf(text,sizeof text,"wear-v2 1 1 %s 0 0 2 1\n0 1\n",boot_a);
    text[20]='_'; bad(text);
    snprintf(text,sizeof text,"wear-v2 1 1 %s 0 0 2 1\n0 1\n",boot_a);
    text[strlen(text)-1]=0; bad(text); /* Required final newline. */
}

static int fake_clock(void *ctx, uint64_t *out)
{
    *out=*(uint64_t *)ctx;
    return 0;
}

static int failed_clock(void *ctx, uint64_t *out)
{
    (void)ctx; (void)out;
    return -EIO;
}

static void failures(void)
{
    struct nzm_budget s, before, out;
    struct nzm_budget_sample at=sample(10,0), sampled;
    uint64_t now=999, fake=123456;
    int migrated=17, due=17;
    char *text=(char *)1;
    size_t len=17;
    CHECK(!nzm_budget_init(&at,&s));
    before=s;
    at.boottime_ns=9;
    CHECK(nzm_budget_project(&s,&at,&now)==-ERANGE && now==999);
    CHECK(nzm_budget_checkpoint_due(&s,&at,1,&due)==-ERANGE && due==17);
    CHECK(nzm_budget_reserve_text(&s,&at,1,10,1,&out,&text,&len)==-ERANGE);
    CHECK(!text && !len && !memcmp(&s,&before,sizeof s));
    at.boottime_ns=10;
    CHECK(nzm_budget_reserve_text(&s,&at,0,10,1,&out,&text,&len)==-EINVAL);
    CHECK(nzm_budget_reserve_text(&s,&at,1,0,1,&out,&text,&len)==-EINVAL);
    CHECK(nzm_budget_reserve_text(&s,&at,1,10,2,&out,&text,&len)==-EINVAL);
    CHECK(nzm_budget_reserve_text(&s,&at,11,10,1,&out,&text,&len)==-EDQUOT);
    CHECK(!memcmp(&s,&before,sizeof s));
    s.logical_ns=UINT64_MAX;
    at.boottime_ns=11;
    CHECK(nzm_budget_project(&s,&at,&now)==-ERANGE);
    at.boottime_ns=10;
    reserve(&s,at,1,10);
    budget(&s,at,10,1,UINT64_MAX);
    s.generation=s.cumulative_kib=UINT64_MAX;
    s.receipt.reservation.generation=UINT64_MAX;
    CHECK(nzm_budget_reserve_text(&s,&at,1,10,s.next_request_id,&out,&text,&len)==-ERANGE);
    CHECK(!text && !len);
    CHECK(!nzm_budget_init(&at,&s));
    s.next_request_id=UINT64_MAX;
    reserve(&s,at,1,10);
    CHECK(!s.next_request_id);
    CHECK(nzm_budget_reserve_text(&s,&at,1,10,1,&out,&text,&len)==-EINVAL);
    at=sample(100,1);
    reserve(&s,at,1,10);
    CHECK(s.next_request_id==2);
    CHECK(!nzm_budget_sample_clock(boot_a,fake_clock,&fake,&sampled));
    CHECK(sampled.boottime_ns==fake && !strcmp(sampled.boot,boot_a));
    struct nzm_budget_sample original=sampled;
    CHECK(nzm_budget_sample_clock(boot_a,failed_clock,NULL,&sampled)==-EIO);
    CHECK(!memcmp(&sampled,&original,sizeof sampled));
    CHECK(!nzm_budget_clock_boottime(NULL,&now));
    CHECK(nzm_budget_sample_clock("01234567-89aB-cdef-0123-456789abcdef",fake_clock,&fake,&sampled)==-EINVAL);
    CHECK(nzm_budget_sample_clock("01234567-89ab-cdef-0123-456789abcde",fake_clock,&fake,&sampled)==-EINVAL);
    char invalid_boot[37]; memcpy(invalid_boot,boot_a,sizeof invalid_boot); invalid_boot[8]='_';
    CHECK(nzm_budget_sample_clock(invalid_boot,fake_clock,&fake,&sampled)==-EINVAL);
    CHECK(nzm_budget_sample_clock("",fake_clock,&fake,&sampled)==-EINVAL);
    CHECK(nzm_budget_parse("wear-v2\0\n",9,&at,&s,&migrated)==-EINVAL);
    CHECK(migrated==17);
}

static void pending_and_receipts(void)
{
    struct nzm_budget s, next, before, parsed;
    struct nzm_budget_sample at=sample(0,0);
    struct nzm_budget_reservation id, wrong;
    char *text=NULL, *receipt_text=NULL;
    size_t len=0, receipt_len=0;
    int migrated;
    CHECK(!nzm_budget_init(&at,&s));
    reserve_pending(&s,at,80,100);
    id=s.pending;
    CHECK(!s.count && s.generation==1 && s.cumulative_kib==80 &&
          !s.known_submitted_kib && id.reserved_kib==80);
    budget(&s,at,100,80,0);
    at.boottime_ns=3*NZM_WEAR_WINDOW_NS;
    budget(&s,at,100,80,0); /* Unknown result never ages and cannot retry. */
    before=s;
    CHECK(nzm_budget_reserve_text(&s,&at,1,100,2,&next,&text,&len)==-EBUSY);
    CHECK(!text && !len && !memcmp(&before,&s,sizeof s));
    CHECK(!nzm_budget_checkpoint_text(&s,&at,&next,&text,&len));
    CHECK(!memcmp(&next.pending,&id,sizeof id) && !next.count);
    CHECK(!nzm_budget_parse(text,len,NULL,&parsed,&migrated) && !migrated);
    CHECK(!memcmp(&next,&parsed,sizeof next)); free(text); s=parsed;
    struct nzm_budget oldboot=s;
    at=sample(100,1);
    CHECK(nzm_budget_settle_text(&s,&at,&id,20,&next,&text,&len)==-EINVAL);
    CHECK(!nzm_budget_checkpoint_text(&s,&at,&next,&text,&len));
    CHECK(next.next_request_id==1 && !memcmp(&next.pending,&id,sizeof id));
    free(text); s=next;
    CHECK(nzm_budget_reserve_text(&s,&at,1,100,1,&next,&text,&len)==-EBUSY);
    CHECK(nzm_budget_settle_text(&s,&at,&id,20,&next,&text,&len)==-EINVAL);
    at=sample(3*NZM_WEAR_WINDOW_NS,0);
    CHECK(nzm_budget_settle_text(&s,&at,&id,20,&next,&text,&len)==-EINVAL);
    s=oldboot; /* Remaining cases use the original still-running old boot. */
    before=s;
    for (unsigned i=0;i<4;i++) {
        wrong=id;
        if (i==0) memcpy(wrong.boot,boot_b,sizeof wrong.boot);
        if (i==1) wrong.request_id++;
        if (i==2) wrong.generation++;
        if (i==3) wrong.reserved_kib++;
        memset(&next,0x5a,sizeof next); parsed=next;
        CHECK(nzm_budget_settle_text(&s,&at,&wrong,20,&next,&text,&len)==-EINVAL);
        CHECK(!text && !len && !memcmp(&next,&parsed,sizeof next));
        CHECK(!memcmp(&s,&before,sizeof s));
    }
    CHECK(nzm_budget_settle_text(&s,&at,&id,81,&next,&text,&len)==-EINVAL);
    CHECK(!text && !len && !memcmp(&s,&before,sizeof s));
    at.boottime_ns+=NZM_WEAR_BUCKET_NS;
    settle(&s,at,20); /* A failed execution with known partial progress. */
    CHECK(!s.pending.request_id && s.known_submitted_kib==20 && s.cumulative_kib==80);
    CHECK(s.count==1 && s.buckets[0].index==3*96+1);
    budget(&s,at,100,20,(3*96+1+97)*NZM_WEAR_BUCKET_NS);
    CHECK(!nzm_budget_checkpoint_text(&s,&at,&next,&receipt_text,&receipt_len));
    CHECK(!memcmp(&s,&next,sizeof s));
    before=s;
    struct nzm_budget_sample retry=sample(UINT64_MAX,0);
    CHECK(!nzm_budget_settle_text(&s,&retry,&id,20,&next,&text,&len));
    CHECK(!memcmp(&before,&next,sizeof next) && len==receipt_len &&
          !memcmp(text,receipt_text,len)); free(text); free(receipt_text);
    CHECK(nzm_budget_settle_text(&s,&at,&id,19,&next,&text,&len)==-EINVAL);
    reserve_pending(&s,at,30,100);
    before=s;
    CHECK(!nzm_budget_settle_text(&s,&retry,&id,20,&next,&text,&len));
    CHECK(!memcmp(&before,&next,sizeof next)); free(text);
    struct nzm_budget_reservation second=s.pending;
    settle(&s,at,0); /* Known zero even on an error releases only pending. */
    CHECK(s.known_submitted_kib==20 && s.cumulative_kib==110 && s.generation==2);
    CHECK(!memcmp(&s.receipt.reservation,&second,sizeof second));
    CHECK(nzm_budget_settle_text(&s,&at,&id,20,&next,&text,&len)==-EINVAL);
    reserve_pending(&s,at,50,100);
    settle(&s,at,50);
    CHECK(s.known_submitted_kib==70 && s.cumulative_kib==160 && s.generation==3);
    budget(&s,at,100,70,(3*96+1+97)*NZM_WEAR_BUCKET_NS);
    at.boottime_ns+=NZM_WEAR_WINDOW_NS+NZM_WEAR_BUCKET_NS;
    budget(&s,at,100,0,0);
    CHECK(s.known_submitted_kib==70 && s.cumulative_kib==160);
}

static void v2_preservation(void)
{
    char v2[512];
    struct nzm_budget s, next, parsed;
    struct nzm_budget_sample at=sample(UINT64_MAX,1), anchor;
    uint64_t now;
    int migrated=-1;
    char *text;
    size_t len;
    snprintf(v2,sizeof v2,"wear-v2 20 400 %s %" PRIu64 " 123 42 2\n0 10\n96 30\n",
             boot_a,NZM_WEAR_WINDOW_NS);
    CHECK(!nzm_budget_parse(v2,strlen(v2),&at,&s,&migrated) && migrated==2);
    CHECK(s.generation==20 && s.cumulative_kib==400 && s.next_request_id==42 &&
          s.logical_ns==NZM_WEAR_WINDOW_NS && s.boottime_ns==123 && s.count==2);
    CHECK(!s.known_submitted_kib && !s.pending.request_id && !s.receipt.reservation.request_id);
    CHECK(!nzm_budget_project(&s,&at,&now) && now==NZM_WEAR_WINDOW_NS);
    memcpy(anchor.boot,s.boot,sizeof anchor.boot); anchor.boottime_ns=s.boottime_ns;
    CHECK(!nzm_budget_checkpoint_text(&s,&anchor,&next,&text,&len));
    CHECK(!memcmp(&s,&next,sizeof s));
    CHECK(!nzm_budget_parse(text,len,NULL,&parsed,&migrated) && !migrated);
    CHECK(!memcmp(&s,&parsed,sizeof s)); free(text);
    anchor.boottime_ns+=NZM_WEAR_BUCKET_NS-1;
    budget(&s,anchor,100,40,NZM_WEAR_WINDOW_NS+NZM_WEAR_BUCKET_NS);
    anchor.boottime_ns++;
    budget(&s,anchor,100,30,(96+97)*NZM_WEAR_BUCKET_NS);
}

static void v3_strict_and_limits(void)
{
    struct nzm_budget s, next;
    struct nzm_budget_sample at=sample(0,0);
    char text[1024];
    char *serialized;
    size_t len;
    const char *bad_rows[]={
        "pending - 0 0 1\nreceipt - 0 0 0 0\n",
        "pending - 0 0 0\nreceipt - 0 0 0 1\n",
        "pending - 0 0 0\nreceipt - 0 0 0 0\nextra\n",
        "pending - 00 0 0\nreceipt - 0 0 0 0\n",
        "pending - 0 0 0\nreceipt - 0 0 0 18446744073709551616\n"
    };
    for (size_t i=0;i<sizeof bad_rows/sizeof *bad_rows;i++) {
        snprintf(text,sizeof text,"wear-v3 0 0 %s 0 0 1 0 0\n%s",boot_a,bad_rows[i]);
        bad(text);
    }
    snprintf(text,sizeof text,"wear-v3 1 1 %s 0 0 2 0 0\npending %s 1 2 1\nreceipt - 0 0 0 0\n",boot_a,boot_a);
    bad(text);
    snprintf(text,sizeof text,"wear-v3 1 1 %s 0 0 2 0 0\npending - 0 0 0\nreceipt %s 1 1 1 1\n",boot_a,boot_a);
    bad(text); /* Receipt cannot exceed known submitted. */
    snprintf(text,sizeof text,"wear-v3 1 1 %s 0 0 2 98 0\n",boot_a); bad(text);
    CHECK(!nzm_budget_init(&at,&s));
    reserve_pending(&s,at,UINT64_MAX,UINT64_MAX);
    settle(&s,at,UINT64_MAX);
    CHECK(s.known_submitted_kib==UINT64_MAX && s.cumulative_kib==UINT64_MAX);
    budget(&s,at,UINT64_MAX,UINT64_MAX,97*NZM_WEAR_BUCKET_NS);
    CHECK(nzm_budget_reserve_text(&s,&at,1,UINT64_MAX,2,&next,&serialized,&len)==-EDQUOT);
    at.boottime_ns=97*NZM_WEAR_BUCKET_NS;
    CHECK(nzm_budget_reserve_text(&s,&at,1,UINT64_MAX,2,&next,&serialized,&len)==-ERANGE);
    CHECK(!serialized && !len);
}

int main(void)
{
    boundaries(); projection(); bounded_growth(); migration();
    corrupt_and_overflow(); failures();
    pending_and_receipts(); v2_preservation(); v3_strict_and_limits();
    printf("PASS wear-v3: %u checks, nonaging pending, exact idempotent settlement, bounded completion history, v1/v2 migration and corruption\n",checks);
    return 0;
}
