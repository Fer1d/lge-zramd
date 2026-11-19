#define _GNU_SOURCE
#include "nzm_io.h"
#include "nzm.h"
#include <sys/statvfs.h>
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/xattr.h>
/* Tests may inject syscall boundaries at compile time, never runtime paths. */
#ifdef NZM_IO_ADAPTER
#include NZM_IO_ADAPTER
#endif
static int component(const char *s) {
    return s && *s && strcmp(s,".") && strcmp(s,"..") && !strchr(s,'/');
}
int nzm_io_check(int fd, mode_t type, const struct nzm_meta *m) {
    struct stat st;
    if (!m) return -EINVAL;
    if (fstat(fd,&st)) return -errno;
    if ((st.st_mode&S_IFMT)!=type || st.st_uid!=m->uid || st.st_gid!=m->gid ||
        (st.st_mode&07777)!=m->mode || (m->links && st.st_nlink!=m->links)) return -EPERM;
    if (m->context) {
        char b[256]; size_t n=strlen(m->context); ssize_t r;
        if (n>=sizeof b) return -EINVAL;
        r=fgetxattr(fd,"security.selinux",b,sizeof b);
        if (r<0) return -errno;
        /* Kernel security labels include their trailing NUL. Accept only exact
         * label, optionally with that one terminator, never suffix/prefix. */
        if (!((size_t)r==n || ((size_t)r==n+1 && b[n]==0)) || memcmp(b,m->context,n)) return -EPERM;
    }
    return 0;
}
int nzm_io_dir_at(int p,const char *name,const struct nzm_meta *m) {
    int fd,rc;
    if (!component(name)) return -EINVAL;
    fd=openat(p,name,O_RDONLY|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC);
    if (fd<0) return -errno;
    rc=nzm_io_check(fd,S_IFDIR,m);
    if (rc) { close(fd); return rc; }
    return fd;
}
int nzm_io_data_dir(int rootfd) {
    const struct nzm_meta m={1000,1000,0771,0,NULL};
    int fd=openat(rootfd,"data",O_PATH|O_DIRECTORY|O_NOFOLLOW|O_CLOEXEC),rc;
    if(fd<0)return -errno;
    /* Parent traversal only: Android forbids read-open of /data. O_PATH
     * supports fstat/openat, not fgetxattr; child labels are checked on their
     * separate O_RDONLY descriptors by nzm_io_private_dir_at. */
    rc=nzm_io_check(fd,S_IFDIR,&m);
    if(rc){close(fd);return rc;}
    return fd;
}
int nzm_io_private_dir_at(int p,const char *name,const char *ctx) {
    const struct nzm_meta m={0,0,0700,0,ctx};
    if (!ctx || !*ctx) return -EINVAL;
    return nzm_io_dir_at(p,name,&m);
}
int nzm_io_read_fd(int fd,void *buf,size_t cap,size_t *len) {
    size_t n=0; ssize_t r; char extra;
    if (!buf || !len || !cap) return -EINVAL;
    *len=0;
    while (n<cap) {
        r=read(fd,(char *)buf+n,cap-n);
        if (r<0) { if (errno==EINTR) continue; return -errno; }
        if (!r) { *len=n; return 0; }
        n+=(size_t)r;
    }
    do { r=read(fd,&extra,1); } while (r<0 && errno==EINTR);
    if (r<0) return -errno;
    if (r) return -EFBIG;
    *len=n; return 0;
}
int nzm_io_read_at(int d,const char *name,const struct nzm_meta *m,void *b,size_t c,size_t *n) {
    int fd,rc;
    if (!component(name)) return -EINVAL;
    fd=openat(d,name,O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if (fd<0) return -errno;
    rc=nzm_io_check(fd,S_IFREG,m);
    if (!rc) rc=nzm_io_read_fd(fd,b,c,n);
    close(fd); return rc;
}
int nzm_io_sysfs_write(int fd,const void *b,size_t n) {
    ssize_t r;
    if (!b || !n || n>4096) return -EINVAL;
    r=write(fd,b,n);
    return r<0 ? -errno : ((size_t)r==n ? 0 : -EIO);
}
static int fullwrite(int fd,const char *s,size_t n) {
    while (n) {
        ssize_t r=write(fd,s,n);
        if (r<0) { if(errno==EINTR) continue; return -errno; }
        if (!r) return -EIO;
        s+=r; n-=(size_t)r;
    }
    return 0;
}
/* Keep the old authority allocated: rename needs a whole additional file, not
 * merely its growth. The reserve also covers intent/transaction metadata. */
int nzm_io_ledger_preflight(int d,size_t n) {
    struct statvfs st;
    const uintmax_t reserve=UINTMAX_C(1048576);
    uintmax_t blocks,unit,need;
    if(!n)return -EINVAL;
    if(n>NZM_LEDGER_MAX_BYTES)return -E2BIG;
    if((uintmax_t)n>UINTMAX_MAX-reserve)return -EOVERFLOW;
    need=(uintmax_t)n+reserve;
    if(fstatvfs(d,&st))return -errno;
    blocks=(uintmax_t)st.f_bavail;unit=(uintmax_t)st.f_frsize;
    if(!unit)return -EINVAL;
    if(blocks>UINTMAX_MAX/unit)return -EOVERFLOW;
    return blocks*unit<need ? -ENOSPC : 0;
}
/* A failed marker retirement poisons this process even if restoration fails.
 * After crash, an absent marker is safe: ledger rename was directory-synced
 * BEFORE retirement. A surviving marker always requires manual review. */
static int ledger_poisoned;
static int txn_marker(int d,const struct nzm_meta *m) {
    int fd=openat(d,"ledger.txn",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600),rc;
    if(fd<0)return -errno;
    rc=nzm_io_check(fd,S_IFREG,m);
    if(!rc)rc=fullwrite(fd,"nzm-ledger-txn-v1\n",18);
    if(!rc && fsync(fd))rc=-errno;
    if(close(fd)&&!rc)rc=-errno;
    if(!rc && fsync(d))rc=-errno;
    return rc;
}
int nzm_io_ledger_commit(int d,const char *s,size_t n,const char *dir_context,const char *file_context) {
    const struct nzm_meta dm={0,0,0700,0,dir_context}, fm={0,0,0600,1,file_context};
    int fd,rc;
    if (!s || !n || !dir_context || !*dir_context || !file_context || !*file_context) return -EINVAL;
    if(ledger_poisoned)return -EIO;
    rc=nzm_io_check(d,S_IFDIR,&dm); if(rc) return rc;
    { struct stat st;
      if(!fstatat(d,"ledger.txn",&st,AT_SYMLINK_NOFOLLOW))return -EEXIST;
      if(errno!=ENOENT)return -errno;
      if(!fstatat(d,"ledger.new",&st,AT_SYMLINK_NOFOLLOW))return -EEXIST;
      if(errno!=ENOENT)return -errno;
    }
    /* Never replace an unsafe existing authority, including a symlink/FIFO. */
    fd=openat(d,"ledger",O_RDONLY|O_NOFOLLOW|O_CLOEXEC|O_NONBLOCK);
    if(fd>=0) { rc=nzm_io_check(fd,S_IFREG,&fm);close(fd);if(rc)return rc; }
    else if(errno!=ENOENT) return -errno;
    rc=nzm_io_ledger_preflight(d,n);if(rc)return rc;
    rc=txn_marker(d,&fm);if(rc)return rc;
    fd=openat(d,"ledger.new",O_WRONLY|O_CREAT|O_EXCL|O_NOFOLLOW|O_CLOEXEC,0600);
    if(fd<0) return -errno;
    rc=nzm_io_check(fd,S_IFREG,&fm);
    if(!rc) rc=fullwrite(fd,s,n);
    if(!rc && fsync(fd)) rc=-errno;
    if(close(fd) && !rc) rc=-errno;
    if(!rc && renameat(d,"ledger.new",d,"ledger")) rc=-errno;
    if(!rc && fsync(d)) rc=-errno;
    if(!rc && unlinkat(d,"ledger.txn",0))rc=-errno;
    if(!rc && fsync(d)) {
        rc=-errno;ledger_poisoned=1;
        /* Best effort restore durable evidence. Never turn uncertainty into
         * success, even if this restoration happens to succeed. */
        (void)txn_marker(d,&fm);
    }
    return rc;
}
int nzm_io_lock(int d,const char *ctx,struct nzm_run_lock *l) {
    struct stat st; int rc,fd;
    if(!l || !ctx || !*ctx) return -EINVAL;
    l->fd=-1;
    { const struct nzm_meta m={0,0,0700,0,ctx};
      rc=nzm_io_check(d,S_IFDIR,&m); if(rc) return rc; }
    if(mkdirat(d,"lock",0700)) return -errno;
    fd=nzm_io_private_dir_at(d,"lock",ctx); if(fd<0) return fd;
    if(flock(fd,LOCK_EX|LOCK_NB) || fstat(fd,&st)) {rc=-errno;close(fd);return rc;}
    l->fd=fd; l->dev=st.st_dev; l->ino=st.st_ino; return 0;
}
int nzm_io_unlock(int d,struct nzm_run_lock *l) {
    struct stat st; int rc=0;
    if(!l || l->fd<0) return -EINVAL;
    if(fstatat(d,"lock",&st,AT_SYMLINK_NOFOLLOW)) rc=-errno;
    else if(!S_ISDIR(st.st_mode)||st.st_dev!=l->dev||st.st_ino!=l->ino) rc=-ESTALE;
    else if(unlinkat(d,"lock",AT_REMOVEDIR)) rc=-errno;
    close(l->fd); l->fd=-1; return rc;
}
