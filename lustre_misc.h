#pragma once

#include <dirent.h>
#include <fcntl.h>
#include <linux/stat.h>
#include <stdint.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

// This is also the same as LOV_MAGIC_V0
// Why not LOV_MAGIC_V1?
// I have no idea. It's all SUPER MAGIC to me
#define LUSTRE_SUPER_MAGIC 198183888

typedef struct stat lstat_t;
typedef struct statx lstatx_t;

struct lu_fid {
    /**
     * FID sequence. Sequence is a unit of migration: all files (objects)
     * with FIDs from a given sequence are stored on the same server.
     * Lustre should support 2^64 objects, so even if each sequence
     * has only a single object we can still enumerate 2^64 objects.
     **/
    __u64 f_seq;
    /* FID number within sequence. */
    __u32 f_oid;
    /**
     * FID version, used to distinguish different versions (in the sense
     * of snapshots, etc.) of the same file system object. Not currently
     * used.
     **/
    __u32 f_ver;
} __attribute__((packed));

struct ost_id {
    union {
        struct {
            __u64 oi_id;
            __u64 oi_seq;
        } oi;
        struct lu_fid oi_fid;
    };
} __attribute__((packed));

#define lov_user_ost_data lov_user_ost_data_v1
struct lov_user_ost_data_v1 { /* per-stripe data structure */
    struct ost_id l_ost_oi;   /* OST object ID */
    union {
        __u32 l_ost_type; /* type of data stored in OST object */
        __u32 l_ost_gen;  /* generation of this OST index */
    };
    __u32 l_ost_idx; /* OST index in LOV */
} __attribute__((packed));

#define lov_user_md lov_user_md_v1
struct lov_user_md_v1 {     /* LOV EA user data (host-endian) */
    __u32 lmm_magic;        /* magic number = LOV_USER_MAGIC_V1 */
    __u32 lmm_pattern;      /* LOV_PATTERN_RAID0, LOV_PATTERN_RAID1 */
    struct ost_id lmm_oi;   /* MDT parent inode id/seq (id/0 for 1.x) */
    __u32 lmm_stripe_size;  /* size of stripe in bytes */
    __u16 lmm_stripe_count; /* num stripes in use for this object */
    union {
        __u16 lmm_stripe_offset; /* starting stripe offset in
                                  * lmm_objects, use when writing
                                  */
        __u16 lmm_layout_gen;    /* layout generation number
                                  * used when reading
                                  */
    };
    struct lov_user_ost_data_v1 lmm_objects[]; /* per-stripe data */
} __attribute__((packed, __may_alias__));

#define lov_user_mds_data lov_user_mds_data_v2
struct lov_user_mds_data_v1 {
    lstat_t lmd_st;                /* MDS stat struct */
    struct lov_user_md_v1 lmd_lmm; /* LOV EA V1 user data */
} __attribute__((packed));

struct lov_user_mds_data_v2 {
    struct lu_fid lmd_fid;         /* Lustre FID */
    lstatx_t lmd_stx;              /* MDS statx struct */
    __u64 lmd_flags;               /* MDS stat flags */
    __u32 lmd_lmmsize;             /* LOV EA size */
    __u32 lmd_padding;             /* unused */
    struct lov_user_md_v1 lmd_lmm; /* LOV EA user data */
} __attribute__((packed));

#define IOC_MDC_TYPE 'i'
#define LL_IOC_MDC_GETINFO_V2 _IOWR(IOC_MDC_TYPE, 23, struct lov_user_mds_data)
#define LL_IOC_MDC_GETINFO_V1                                                  \
    _IOWR(IOC_MDC_TYPE, 23, struct lov_user_mds_data_v1 *)
#define IOC_MDC_GETFILEINFO_V2 _IOWR(IOC_MDC_TYPE, 22, struct lov_user_mds_data)

