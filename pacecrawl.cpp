// GPFS scanning: /usr/lpp/mmfs/samples/util/tsreadfield
// dir.ccmakecache.pacecrawl.cpp fts find:
// https://git.savannah.gnu.org/cgit/findutils.git/tree/find/ftsfind.c

#include <algorithm>
#include <atomic>
#include <cassert>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <ctime>
#include <fcntl.h>
#include <iostream>
#include <iterator>
#include <linux/stat.h>
#include <memory>
#include <omp.h>
#include <optional>
#include <signal.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>
extern "C" {
#include "lustre_misc.h"
#include "pacecrawl_fts/pacecrawl_fts.h"
#include <dirent.h>
#include <grp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/types.h>
#include <unistd.h>

#include <acl/libacl.h>
#include <libgen.h>
#include <sys/acl.h>

#include <getopt.h>
#include <openssl/evp.h>

#include <linux/magic.h>
#include <lmdb.h>
// #include <xxhash.h>
}
#include <unistd.h>

#include "pacecrawl-table.h"
#include <pwd.h>
#include <sqlite3.h>
#include <sys/xattr.h>

char *opt_opath = nullptr;
char *opt_njobs = nullptr;
int opt_shouldPrintTansferCommands = 0;
char *opt_destStateDB = nullptr;
char *opt_destStatePath = nullptr;
int opt_no_lustre_opt = 1;
int njobs = 1;
size_t opt_max_db_memload_size = 5000000000;
bool loadDestStateFromMem = false;

bool loadDBIntoMem = true;

char *opt_hashalgo = nullptr;
enum HASH_ALGO { HASH_MD5SUM = 0, HASH_XXH128SUM };
HASH_ALGO hashalgo = HASH_MD5SUM;

#define INITIAL_READLINK_BUF_SIZE 4096

typedef struct {
    unsigned short fts_info;
    char path[PATH_MAX + 1];

    struct statx stx;

    bool attrMismatch;
    bool contentsMismatch;
} FileInfo;

#define INITIAL_FILE_BUF_COUNT 1e4

#define HASH_READ_SIZE 1048576 * 128
// #define HASH_READ_SIZE 1024 * 128
#define NUM_ENTRIES_PER_DB_FLUSH 1e4

#define MAX_DB_MEMLOAD_SIZE (500000000)

#define ISDOT(a) (a[0] == '.' && (!a[1] || (a[1] == '.' && !a[2])))

int md5sum(int dirfd, const char *path, uint8_t sum[16]);

void printHelp() {
    // clang-format off
    const char *HELP_STRING =
        "pacecrawl - recursively crawl a source directory and generate a contents report in the form of a sqlite database\n"
        "USAGE: pacecrawl [OPTIONS] -o OUTPUT.db SOURCE_DIRECTORY\n"
        "\n"
        "REQUIRED FLAGS\n"
        "  -o,--output <OUTPUT>\tSets output database to OUTPUT\n"
        "\n"
        "OPTIONS\n"
        "  -j,--jobs NJOBS                     Runs the crawler with NJOBS jobs\n"
        // "  -c,--checksum <md5>                 Enables checksumming with the provided algorithm (md5). If not specified, checksumming will not be performed\n"
        "  -p,--print-transfer-commands        Enables printing of paths with distinct inodes to stdout, could be a performance hit. Combined with a destination db/path, this will print files that need to be updated based on size and modtime.\n"
        "  --dest-state-db DB                  To be used along with --print-transfer-commands. Only print files where existance, modification time or size differ on source compared to the destination (read from the DB)\n"
        "  --dest-state-path PATH              Similar to --dest-state-db except state is read from the destination filesystem\n"
        "  -h,--help                           Prints this message\n"
        "\n"
        "AUTHOR\n"
        "  Aiden Lambert <alambert48@gatech.edu>\n"
        "\n";

    fprintf(stderr, "%s", HELP_STRING);
    // clang-format on
}

// void freeTableEntry(TableEntry *entry) {
// #define FREE_IF_NOTNULL(x) \
//     if ((x)) { \
//         free((x)); \
//         (x) = nullptr; \
//     }
//
//     FREE_IF_NOTNULL(entry->path);
//     FREE_IF_NOTNULL(entry->userName);
//     FREE_IF_NOTNULL(entry->groupName);
//
//     if (entry->acl) {
//         acl_free(entry->acl);
//         entry->acl = nullptr;
//     }
//
//     FREE_IF_NOTNULL(entry->linkref);
//     FREE_IF_NOTNULL(entry->linkatroot);
//     FREE_IF_NOTNULL(entry->hash);
//
// #undef FREE_IF_NOTNULL
// }

struct FileDescriptor {
    int fd;

    ~FileDescriptor() { close(fd); }
};

struct AttributeTimes {
    struct timespec modtime;
    struct timespec chgtime;

    char hash[17];

    ino_t _inode; // only valid when opt_changedinode_deletedir is set

    bool shouldDelete = true;
};

struct DestState {
    ino_t inode;

    struct timespec modtime;
    uint64_t size;

    uint8_t hash[16];
};

int getDestStateFromFS(int destdirfd, const char *path, DestState *state) {
    struct statx stx;
    if (statx(destdirfd, path, AT_SYMLINK_NOFOLLOW,
              STATX_TYPE | STATX_INO | STATX_SIZE | STATX_MTIME, &stx) != 0) {
        if (errno == ENOENT) {
            return ENOENT;
        }
        fprintf(stderr,
                "ERROR: get dest state from fs failed to statx %s, %s (%d)\n",
                path, strerror(errno), errno);
        return errno;
    }

    state->inode = stx.stx_ino;
    state->size = stx.stx_size;
    state->modtime.tv_sec = stx.stx_mtime.tv_sec;
    state->modtime.tv_nsec = stx.stx_mtime.tv_nsec;

    if (opt_hashalgo && hashalgo == HASH_MD5SUM && S_ISREG(stx.stx_mode)) {
        int ret = md5sum(destdirfd, path, state->hash);
        if (ret) {
            fprintf(stderr, "ERROR: getDestStateFromFS: %s failed to checksum",
                    path);
            return 1;
        }
    }

    return 0;
}

/**
 * NOTE: This is copied and slightly modified from GNOME's glib (permalinked
 * below). Source code/LICENSING here is informal as I (Aiden Lambert)
 * understand this to be a tool internal to PACE
 * https://gitlab.gnome.org/GNOME/glib/-/blob/549a966b46042081228fa4a276bb428e53b11f7c/glib/gfileutils.c?page=3#L2789-2931
 *
 * g_canonicalize_filename:
 * @filename: (type filename): the name of the file
 * @relative_to: (type filename) (nullable): the relative directory, or %NULL
 * to use the current working directory
 *
 * Gets the canonical file name from @filename. All triple slashes are turned
 * into single slashes, and all `..` and `.`s resolved against @relative_to.
 *
 * Symlinks are not followed, and the returned path is guaranteed to be
 * absolute.
 *
 * If @filename is an absolute path, @relative_to is ignored. Otherwise,
 * @relative_to will be prepended to @filename to make it absolute. @relative_to
 * must be an absolute path, or %NULL. If @relative_to is %NULL, it'll fallback
 * to g_get_current_dir().
 *
 * This function never fails, and will canonicalize file paths even if they
 * don't exist.
 *
 * No file system I/O is done.
 *
 * Returns: (type filename) (transfer full): a newly allocated string with the
 *   canonical file path
 *
 * Since: 2.58
 */
char *g_canonicalize_filename(const char *filename, const char *relative_to) {
    char *canon, *input, *output, *after_root, *output_start;

    if (relative_to[0] != '/') {
        return NULL;
    }

    canon = strdup(filename);

    after_root = canon;
    if (after_root[0] == '/') {
        while (after_root[0] == '/')
            after_root++;
    }

    /* Find the first dir separator and use the canonical dir separator. */
    for (output = after_root - 1; (output >= canon) && output[0] == '/';
         output--)
        *output = '/';

    /* 1 to re-increment after the final decrement above (so that output >=
     * canon), and 1 to skip the first `/`. There might not be a first `/` if
     * the @canon is a Windows `//server/share` style path with no
     * trailing directories. @after_root will be '\0' in that case. */
    output++;
    if (output[0] == '/')
        output++;

    /* POSIX allows double slashes at the start to mean something special
     * (as does windows too). So, "//" != "/", but more than two slashes
     * is treated as "/".
     */
    if (after_root - output == 1)
        output++;

    input = after_root;
    output_start = output;
    while (*input) {
        /* input points to the next non-separator to be processed. */
        /* output points to the next location to write to. */

        /* Ignore repeated dir separators. */
        while (input[0] == '/')
            input++;

        /* Ignore single dot directory components. */
        if (input[0] == '.' && (input[1] == 0 || input[1] == '/')) {
            if (input[1] == 0)
                break;
            input += 2;
        }
        /* Remove double-dot directory components along with the preceding
         * path component. */
        else if (input[0] == '.' && input[1] == '.' &&
                 (input[2] == 0 || input[2] == '/')) {
            if (output > output_start) {
                do {
                    output--;
                } while (output[-1] != '/' && output > output_start);
            }
            if (input[2] == 0)
                break;
            input += 3;
        }
        /* Copy the input to the output until the next separator,
         * while converting it to canonical separator */
        else {
            while (*input && input[0] != '/')
                *output++ = *input++;
            if (input[0] == 0)
                break;
            input++;
            *output++ = '/';
        }
    }

    /* Remove a potentially trailing dir separator */
    if (output > output_start && output[-1] == '/')
        output--;

    *output = '\0';

    return canon;
}

/**
 * Computes the MD5 checksum of the file pointed to by path. Returns the
 * checksum in hexadecimal string format (allocated with malloc)
 * @return NULL on error, a heap-allocated checksum string otherwise
 */
int md5sum(int dirfd, const char *path, uint8_t sum[16]) {
    int fd = openat(dirfd, path, O_RDONLY); /* reads in file */
    if (fd < 0) {
        fprintf(stderr, "ERROR: %s: failed to open for checksum\n", path);
        close(fd);
        return 1;
    }

    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();

    EVP_DigestInit_ex(mdctx, EVP_md5(), NULL);

    thread_local char *buf = (char *)calloc(1, HASH_READ_SIZE);

    int n = 0;
    while ((n = read(fd, buf, HASH_READ_SIZE)) > 0) {
        EVP_DigestUpdate(mdctx, buf, n);
    }

    if (n < 0) {
        fprintf(stderr, "ERROR %s: md5 read failed: %s\n", path,
                strerror(errno));
        EVP_MD_CTX_free(mdctx);
        close(fd);
        return 1;
    }

    unsigned char mdValue[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    EVP_DigestFinal_ex(mdctx, mdValue, &md_len); /* Finalize hash context */

    if (md_len != 16) {
        fprintf(stderr, "unexpected hash len: %d\n", md_len);
        close(fd);
        return 1;
    }

    memcpy(sum, mdValue, 16);

    EVP_MD_CTX_free(mdctx);
    close(fd);

    return 0;
}

int sha256sumFromData(void *data, size_t n, uint8_t sum[32]) {
    EVP_MD_CTX *mdctx = EVP_MD_CTX_new();

    EVP_DigestInit_ex(mdctx, EVP_sha256(), NULL);

    EVP_DigestUpdate(mdctx, data, n);

    unsigned char mdValue[EVP_MAX_MD_SIZE];
    unsigned int md_len = 0;
    EVP_DigestFinal_ex(mdctx, mdValue, &md_len); /* Finalize hash context */

    if (md_len != 32) {
        fprintf(stderr, "unexpected sha256 hash len: %d\n", md_len);
        EVP_MD_CTX_free(mdctx);
        return 1;
    }

    memcpy(sum, mdValue, 32);

    EVP_MD_CTX_free(mdctx);

    return 0; /* returns 0 */
}

// char *xxh128Sum(const char *path) {
//     int fd = open(path, O_RDONLY); /* reads in file */
//     if (fd < 0) {
//         fprintf(stderr, "%s: failed to open for checksum\n", path);
//         close(fd);
//         return nullptr;
//     }
//
//     XXH3_state_t *xxhState = XXH3_createState();
//     XXH3_128bits_reset_withSeed(xxhState, 0);
//
//     char *buf[HASH_READ_SIZE];
//
//     int n = 0;
//     while ((n = read(fd, buf, HASH_READ_SIZE)) > 0) {
//         XXH3_128bits_update(xxhState, buf, n);
//     }
//
//     if (n < 0) {
//         fprintf(stderr, "%s: xxh read failed: %s\n", path, strerror(errno));
//         XXH3_freeState(xxhState);
//         close(fd);
//         return nullptr;
//     }
//
//     XXH128_hash_t hash = XXH3_128bits_digest(xxhState);
//
//     char *hashString = (char *)malloc(33);
//     snprintf(hashString, 33, "%016llx%016llx", (unsigned long
//     long)hash.high64,
//              (unsigned long long)hash.low64);
//     hashString[32] = '\0';
//
//     XXH3_freeState(xxhState);
//
//     return hashString; /* returns 0 */
// }

// void printTableEntry(TableEntry *entry) {
//     printf("path: %s\n", entry->path);
//     printf("type: %s\n", entry->type);
//     printf("inode: %zu\n", entry->inode);
//     printf("nhlink: %zu\n", entry->nhlink);
//     printf("uid: %u\n", entry->uid);
//     printf("gid: %u\n", entry->gid);
//     printf("user: %s\n", entry->userName);
//     printf("group: %s\n", entry->groupName);
//     printf("perms: %u\n", entry->perms);
//     printf("acl: %s\n", entry->acl);
//     printf("linkref: %s\n", entry->linkref);
//     printf("linkatroot: %s\n", entry->linkatroot);
//     printf("linkvalid: %d\n", entry->linkValid);
//     printf("hash: %s\n", entry->hash);
//     printf("modtime: %zu\n",
//            entry->modtime.tv_sec * (unsigned long)1e9 +
//            entry->modtime.tv_nsec);
//     printf("chgtime: %zu\n",
//            entry->chgtime.tv_sec * (unsigned long)1e9 +
//            entry->chgtime.tv_nsec);
//     printf("size: %zu\n", entry->size);
// }
//
/**
 * Arbitrary length readlink
 * @param path Path to symlink
 * relative to that. Otherwise, set to -1
 * @return path (or NULL if error). Please free when done
 */
char *readSymlink(const char *path) {
    size_t size = INITIAL_READLINK_BUF_SIZE;
    char *buf = NULL;
    ssize_t nread = 0;
    while (1) {
        buf = (char *)reallocarray(buf, size, 1);

        nread = readlink(path, buf, size);

        if (nread < 0) {
            fprintf(stderr,
                    "ERROR: readlink failed to read symlink \"%s\" "
                    "(errorcode %ld)\n",
                    path, errno);
            free(buf);
            return nullptr;
        } else if (nread == 0) {
            buf[0] = '\0';
            break;
        }

        if (nread >= size) {
            size += INITIAL_READLINK_BUF_SIZE;
        } else {
            buf[nread] = '\0';
            break;
        }
    }

    return buf;
}

struct SharedState {
    MDB_env *env;

    MDB_dbi inodeTable;
    MDB_dbi aclTable;
    MDB_dbi finfoByInodeTable;
    MDB_dbi pathToInodeTable;

    MDB_txn *writeTxn;
    MDB_cursor *writeInodeCursor;

    MDB_cursor *writeACLCursor;

    MDB_cursor *writeFilesCursor;

    MDB_cursor *writePathsCursor;

    bool isLustre;

    std::unordered_map<std::string, AttributeTimes> &existingTimes;
    std::unordered_set<ino_t> &seenInodes;
    std::unordered_set<ino_t> &seenInodesDest;
    std::unordered_map<std::string, DestState> &destStates;

    std::atomic_uint64_t writeHead;
    int datfd;
};

double loadSLTotal = 0.0;
double loadGidUidTotal = 0.0;
double loadACLTotal = 0.0;

/**
 * Places the attributes (pretty much everything that doesn't involve the
 * contents of the file) of the input file into a table entry
 *
 * @param info information about the input file.
 * @param tableEntry output TableEntry describing the input file
 * @return 0 on success and anything else otherwise
 */
int loadAttributes(const char *path, SharedState *sharedState, FileInfo *info,
                   InodeTableEntry *entry, ACLTableEntry *aclEntry,
                   FileTableEntry **fileTableEntry) {
    double slLoadStart = omp_get_wtime();
    struct statx &stx = info->stx;

    *aclEntry = NULL;
    *fileTableEntry = NULL;

    if (info->fts_info == FTS_F || info->fts_info == FTS_D) {
    } else if (info->fts_info == FTS_SL || info->fts_info == FTS_SLNONE) {
        char *linkPath = readSymlink(info->path);

        if (linkPath == nullptr) {
            // we assume here that errors have already been printed
            return 1;
        }

        char *atrootLinkPath = nullptr;

        if (linkPath[0] != '/') {
            // extra character for / delimiter
            size_t totalLen = strlen(info->path) + strlen(linkPath) + 1;
            char *unsimplifiedAbsLinkpath = (char *)malloc(totalLen + 1);
            unsimplifiedAbsLinkpath[totalLen] = '\0';

            char *dirpathAlloc = (char *)malloc(strlen(info->path) + 1);
            strcpy(dirpathAlloc, info->path);
            char *dirpath = dirname(dirpathAlloc);

            strcpy(unsimplifiedAbsLinkpath, dirpath);

            strcat(unsimplifiedAbsLinkpath, "/");

            strcat(unsimplifiedAbsLinkpath, linkPath);

            atrootLinkPath =
                g_canonicalize_filename(unsimplifiedAbsLinkpath, "/");

            free(dirpathAlloc);
            free(unsimplifiedAbsLinkpath);
            if (!atrootLinkPath) {
                fprintf(stderr,
                        "ERROR: atrootLinkPath error for %s (%s): %s\n ",
                        info->path, unsimplifiedAbsLinkpath, strerror(errno));

                return 1;
            }

        } else {
            atrootLinkPath = (char *)malloc(strlen(linkPath) + 1);
            strcpy(atrootLinkPath, linkPath);
        }

        size_t pathLen = strlen(path);
        size_t linkrefLen = strlen(linkPath);
        size_t abslinkrefLen = strlen(atrootLinkPath);
        // fprintf(stderr, "INFO: path %s, %zu, %zu, %zu\n", path, pathLen,
        //         linkrefLen, abslinkrefLen);
        *fileTableEntry =
            (FileTableEntry *)calloc(1, sizeof(FileTableEntry) + pathLen + 1 +
                                            linkrefLen + 1 + abslinkrefLen + 1);

        memcpy((char *)((*fileTableEntry)->pathAndLinkrefAndAbs), path,
               pathLen);
        memcpy((char *)((*fileTableEntry)->pathAndLinkrefAndAbs) + pathLen + 1,
               linkPath, linkrefLen);
        memcpy((char *)((*fileTableEntry)->pathAndLinkrefAndAbs) + pathLen + 1 +
                   linkrefLen + 1,
               atrootLinkPath, abslinkrefLen);

        (*fileTableEntry)->pathLen = pathLen;
        (*fileTableEntry)->linkrefLen = linkrefLen;
        (*fileTableEntry)->absLinkrefLen = abslinkrefLen;

        (*fileTableEntry)->linkrefValid =
            faccessat(AT_FDCWD, atrootLinkPath, F_OK, AT_SYMLINK_NOFOLLOW) == 0;

        free(atrootLinkPath);
        free(linkPath);
    }

    if (!*fileTableEntry) {
        size_t pathLen = strlen(path);
        // three null terminators (1 per path)
        *fileTableEntry =
            (FileTableEntry *)calloc(1, sizeof(FileTableEntry) + pathLen + 3);
        (*fileTableEntry)->pathLen = pathLen;
        memcpy((char *)((*fileTableEntry)->pathAndLinkrefAndAbs), path,
               pathLen);
    }
    (*fileTableEntry)->inode = stx.stx_ino;

    loadSLTotal += omp_get_wtime() - slLoadStart;

    entry->inode = stx.stx_ino;
    entry->nhlink = stx.stx_nlink;
    entry->uid = stx.stx_uid;
    entry->gid = stx.stx_gid;

    entry->modtime.tv_nsec =
        stx.stx_mtime.tv_nsec; // This does a dreaded usigned to signed
                               // conversion, but surely this won't be in use
                               // past the signed 64bit time limit?
    entry->modtime.tv_sec = stx.stx_mtime.tv_sec;
    entry->chgtime.tv_nsec = stx.stx_ctime.tv_nsec;
    entry->chgtime.tv_sec = stx.stx_ctime.tv_sec;
    entry->btime.tv_nsec = stx.stx_btime.tv_nsec;
    entry->btime.tv_sec = stx.stx_btime.tv_sec;
    entry->atime.tv_nsec = stx.stx_atime.tv_nsec;
    entry->atime.tv_sec = stx.stx_atime.tv_sec;

    bool readpwuidSuccess = true;

    double gidUidStart = omp_get_wtime();

    entry->uid = stx.stx_uid;
    entry->gid = stx.stx_gid;

    loadGidUidTotal += omp_get_wtime() - gidUidStart;

    entry->mode = stx.stx_mode;

    double loadACLStart = omp_get_wtime();
    if (info->fts_info != FTS_SL && info->fts_info != FTS_SLNONE) {
        // We call getxattr here in order to avoid calling acl_get_file as
        // acl_get_file will stat a file if no system.posix_acl_access entry
        // exists. We don't want this behavior. Of course, there is a double
        // getxattr if an acl does exist, but ACLs *seem* to be pretty
        // infrequent on our system as of 2025
        // int retval = getxattr(info->path, "system.posix_acl_access", NULL,
        // 0);
        int retval = -1;
        if (retval <= 0 || errno == ENOTSUP || errno == ENODATA ||
            errno == E2BIG) {
            *aclEntry = (ACLTableEntry)calloc(1, 1);
            //
            //
            // thread_local char *buf = (char *)calloc(1, 8192 * 128);
            // memset(buf, 0, 8192 * 128);
            // int retval = llistxattr(info->path, buf, 8192 * 128);
            // if (retval <= 0 || errno == ENOTSUP ||
            // !strstr(buf, "system.posix_acl_access")) {
            *aclEntry = (ACLTableEntry)calloc(1, 1);
        } else {
            acl_t acl = acl_get_file(info->path, ACL_TYPE_ACCESS);
            errno = ENOSYS;

            // sketchy conversion
            *aclEntry = (ACLTableEntry)acl_to_text(acl, NULL);
            if (*aclEntry == NULL) {
                fprintf(stderr,
                        "ERROR: %s: failed to convert acl to text: %s\n",
                        info->path, strerror(errno));
                acl_free(acl);
                return 1;
            }
            acl_free(acl);
        }
    }

    if (!*aclEntry) {
        *aclEntry = (ACLTableEntry)calloc(1, 1);
    }

    size_t aclLen = strlen((const char *)*aclEntry);
    if (aclLen) {
        int ret = sha256sumFromData((void *)*aclEntry, aclLen, entry->aclHash);

        if (ret) {
            fprintf(stderr, "ERROR: failed to checksum acl for %s\n",
                    info->path);
        }
    }

    loadACLTotal += omp_get_wtime() - loadACLStart;

    entry->size = stx.stx_size;
    entry->blocks = stx.stx_blocks; // multiply by 512, block size used by stat

    // if (strcmp(path, "jliao74/jliao74/VertexReco/Vertex/LargeTC0.005/"
    //                  "lightning_logs/version_1/metrics.csv") == 0) {
    //     raise(SIGTRAP);
    // }

    return 0;
}

double statTotal = 0.0;

/**
 * Loads all information for a file. Designed to run concurrently with other
 * instances of this task.
 *
 * @param prefixLen The strlen of the directory passed in by the user. Useful as
 * we write paths relative to this directory instead of the cwd to the database
 * @param conns A vector of sqlite connections (one per thread)
 * @param fileInfo Input file information (need not contain any information but
 * the path)
 *
 * @return A TableEntry describing fileInfo if no fatal errors occurred.
 */
std::optional<std::tuple<InodeTableEntry, ACLTableEntry, FileTableEntry *>>
fileTask(size_t prefixLen, SharedState *sharedState, FileInfo *fileInfo,
         int destdirfd, std::shared_ptr<FileDescriptor> parentFD) {
    char *seenPath = fileInfo->path + prefixLen;
    if (seenPath[0] == '/') {
        seenPath++;
    }

    // if ((strcmp(
    //         seenPath,
    //         "bkreitz3/.conda/envs/atomic/lib/python3.13/site-packages/scipy/"
    //         "linalg/_matfuncs_expm.cpython-313-x86_64-linux-gnu.so")) == 0) {
    //     raise(SIGTRAP);
    // }

    {
        if (loadDBIntoMem) {
            auto it = sharedState->existingTimes.find(seenPath);
            if (it != sharedState->existingTimes.end()) {
                // fprintf(stderr, "got ptr %p\n", &(it->second));
                // fprintf(stderr, "INFO: seen %s\n", seenPath);
                it->second.shouldDelete = false;
            }
        }
    }

    int SIZE_AS_NEEDED = 0;
    if (opt_shouldPrintTansferCommands && opt_destStatePath) {
        SIZE_AS_NEEDED = STATX_SIZE;
    }

    double statStart = omp_get_wtime();
    struct statx stx;

    if (!sharedState->isLustre) {
        if (statx(AT_FDCWD, fileInfo->path, AT_SYMLINK_NOFOLLOW,
                  STATX_BASIC_STATS | STATX_BTIME, &stx) < 0) {
            fprintf(stderr, "ERROR: %s: failed to statx: errno %d, error %s\n",
                    fileInfo->path, errno, strerror(errno));
            return std::nullopt;
        }
    } else {
        const char *name = strrchr(seenPath, '/');
        if (!name) {
            name = seenPath;
        }

        if (name[0] == '/') {
            name++;
        }

        // This is just a guess at a ceiling for how much this ioctl takes
        // https://github.com/lustre/lustre-release/blob/0a0293fbd2b99507bc5b60dd24c81756b2b9436b/lustre/utils/liblustreapi.c#L489-L506
        thread_local lov_user_mds_data *lmd =
            (lov_user_mds_data *)calloc(1, 8192 * 128);

        sprintf((char *)lmd, "%s", name);

        int ret = ioctl(parentFD->fd, IOC_MDC_GETFILEINFO_V2, lmd);
        if (ret < 0) {
            fprintf(stderr, "ERROR: failed to ioctl stat (%s) %s: %d (%s)\n",
                    name, fileInfo->path, errno, strerror(errno));
            return std::nullopt;
        }

        stx = lmd->lmd_stx;

        // if (statx(parentFD->fd, name, AT_SYMLINK_NOFOLLOW,
        //           STATX_BASIC_STATS | STATX_BTIME, &stx) < 0) {
        //     fprintf(stderr, "ERROR: %s: failed to statx: errno %d, error
        //     %s\n",
        //             fileInfo->path, errno, strerror(errno));
        //     return std::nullopt;
        // }
    }

    statTotal += omp_get_wtime() - statStart;

    fileInfo->stx = stx;

    if (S_ISREG(fileInfo->stx.stx_mode)) {
        fileInfo->fts_info = FTS_F;
    } else if (S_ISDIR(fileInfo->stx.stx_mode)) {
        fileInfo->fts_info = FTS_D;
    } else if (S_ISLNK(fileInfo->stx.stx_mode)) {
        fileInfo->fts_info = FTS_SL;
    } else {
        fileInfo->fts_info = FTS_DEFAULT;
        // fprintf(stderr, "ERROR: %s: unkown file type\n", fileInfo->path);
        // return std::nullopt;
    }

    InodeTableEntry entry = {};
    ACLTableEntry aclEntry = NULL;
    FileTableEntry *fileEntry = NULL;
    int threadID = omp_get_thread_num();
    std::optional<AttributeTimes> attributeTime;
    {
        char *path = fileInfo->path + prefixLen;
        if (path[0] == '/') {
            path++;
        }

        fileInfo->attrMismatch = fileInfo->contentsMismatch = true;
    }

    if ((fileInfo->contentsMismatch && opt_hashalgo) ||
        fileInfo->attrMismatch) {
        const char *p = fileInfo->path + prefixLen;
        if (p[0] == '/') {
            p++;
        }

        if (fileInfo->attrMismatch) {
            // fprintf(stderr, "INFO: env: %p\n", sharedState);
            if (loadAttributes(seenPath, sharedState, fileInfo, &entry,
                               &aclEntry, &fileEntry)) {
                return std::nullopt;
            }
            // fprintf(stderr, "INFO: env: %p\n", sharedState);
        }

        int ret = 0;
        if (fileInfo->contentsMismatch && opt_hashalgo) {
            if (fileInfo->fts_info == FTS_F) {
                ret = md5sum(destdirfd, fileInfo->path, entry.hash);
            } else {
                // entry.hash = (char *)calloc(1, 1);
            }

            if (ret) {
                return std::nullopt;
            }
        }
    }

    if (opt_shouldPrintTansferCommands && fileInfo->fts_info == FTS_F) {
        char *path = fileInfo->path + prefixLen;
        if (path[0] == '/') {
            path++;
        }

        bool hasDestState = false;
        bool shouldPrint = false;

        DestState destState = {};
        if (opt_destStatePath) {
            int err;
            if ((err = getDestStateFromFS(destdirfd, path, &destState)) != 0) {
                shouldPrint = true;
            } else {
                hasDestState = true;
            }
        }

        if (!shouldPrint && hasDestState) {
            shouldPrint =
                (uint32_t)destState.modtime.tv_nsec != stx.stx_mtime.tv_nsec ||
                (uint32_t)destState.modtime.tv_sec != stx.stx_mtime.tv_sec ||
                destState.size != stx.stx_size;

            // if (shouldPrint) {
            //     printf("%s: %d vs %d, %d vs %lld, %lu vs %llu\n", path,
            //            (uint32_t)destState.modtime.tv_nsec,
            //            st.st_mtime.tv_nsec,
            //            (uint32_t)destState.modtime.tv_sec,
            //            st.st_mtime.tv_sec, destState.size, st.st_size);
            // }
            if (opt_hashalgo && S_ISREG(fileInfo->stx.stx_mode) &&
                !shouldPrint) {
                if (fileInfo->contentsMismatch) {
                    shouldPrint = memcmp(entry.hash, destState.hash, 16);
                } else {
                    if (!attributeTime) {
                        shouldPrint = true;
                    } else {
                        shouldPrint =
                            memcmp(attributeTime->hash, destState.hash, 16);
                    }
                }
            }
        }

#pragma omp critical(PRINT_UNIQUE)
        {

            // if (sharedState->seenInodes.find(stx.stx_ino) ==
            //     sharedState->seenInodes.end())
            {
                if (hasDestState &&
                    sharedState->seenInodesDest.find(destState.inode) !=
                        sharedState->seenInodesDest.end()) {
                    shouldPrint = true;
                }
                if (shouldPrint) {
                    fputs(path, stdout);
                    fputc('\n', stdout);
                }

                sharedState->seenInodes.insert(stx.stx_ino);
                sharedState->seenInodesDest.insert(destState.inode);
            }
        }
    }

    if (!((fileInfo->contentsMismatch && opt_hashalgo) ||
          fileInfo->attrMismatch)) {
        fprintf(stderr, "uneeded %s\n", fileInfo->path);
        return std::nullopt;
    }

    return std::make_tuple(entry, aclEntry, fileEntry);
}

/**
 * Writes entry to database described by conn
 */
void dbWrite(const char *path, SharedState *sharedState, FileInfo &fileInfo,
             InodeTableEntry *inodeEntry, ACLTableEntry aclEntry,
             FileTableEntry *fileEntry) {
    int err = 0;

    uint64_t datOffset = sharedState->writeHead.fetch_add(
        sizeof(InodeTableEntry), std::memory_order_relaxed);

    MDB_val keyVal;
    keyVal.mv_data = &inodeEntry->inode;
    keyVal.mv_size = sizeof(inodeEntry->inode);

    MDB_val dataVal;
    dataVal.mv_data = &datOffset;
    dataVal.mv_size = sizeof(uint64_t);

    // fprintf(stderr, "INFO: inode %zu\n", inodeEntry->inode);
    if ((err = mdb_cursor_put(sharedState->writeInodeCursor, &keyVal, &dataVal,
                              0))) {
        fprintf(stderr, "dbWrite failed to write inodeEntry: %s\n",
                mdb_strerror(err));
        return;
    }

    if (pwrite(sharedState->datfd, inodeEntry, sizeof(InodeTableEntry),
               datOffset) < 0) {
        fprintf(stderr, "ERROR: failed to write inode entry to dat file: %s\n",
                strerror(errno));
        return;
    }

// #pragma omp critical(PRINT)
//     fprintf(stderr, "offset=%zu\n", datOffset);

    // keyVal.mv_data = inodeEntry->aclHash;
    // keyVal.mv_size = 32;
    //
    // dataVal.mv_data = (void *)aclEntry;
    // dataVal.mv_size = strlen((const char *)aclEntry) + 1;
    //
    // if ((err = mdb_cursor_put(sharedState->writeACLCursor, &keyVal, &dataVal,
    //                           0)) &&
    //     err != MDB_KEYEXIST) {
    //     fprintf(stderr, "dbWrite failed to write aclEntry: %s\n",
    //             mdb_strerror(err));
    //     return;
    // }

    size_t fileTableEntrySize = sizeof(FileTableEntry) + fileEntry->pathLen +
                                1 + fileEntry->linkrefLen + 1 +
                                fileEntry->absLinkrefLen + 1;
    datOffset = sharedState->writeHead.fetch_add(fileTableEntrySize,
                                                 std::memory_order_relaxed);

    keyVal.mv_data = &inodeEntry->inode;
    keyVal.mv_size = sizeof(inodeEntry->inode);

    dataVal.mv_data = &datOffset;
    dataVal.mv_size = sizeof(datOffset);

    if ((err = mdb_cursor_put(sharedState->writeFilesCursor, &keyVal, &dataVal,
                              0)) &&
        err != MDB_KEYEXIST) {
        fprintf(stderr, "dbWrite failed to write fileEntry: %s\n",
                mdb_strerror(err));
        return;
    }

    if (pwrite(sharedState->datfd, fileEntry, fileTableEntrySize, datOffset) <
        0) {
        fprintf(stderr,
                "ERROR: failed to write fileTableEntry to dat file: %s\n",
                strerror(errno));
        return;
    }
// #pragma omp critical(PRINT)
//     fprintf(stderr, "offset=%zu\n", datOffset);

    keyVal.mv_data = (void *)path;
    keyVal.mv_size = strlen(path) + 1;

    dataVal.mv_data = &inodeEntry->inode;
    dataVal.mv_size = sizeof(inodeEntry->inode);

    if ((err = mdb_cursor_put(sharedState->writePathsCursor, &keyVal, &dataVal,
                              0)) &&
        err != MDB_KEYEXIST) {
        fprintf(stderr, "dbWrite failed to write pathsEntry for %s: %s\n", path,
                mdb_strerror(err));
        return;
    }
}

double startTime = 0.0;
double sqliteTotal = 0;
double sqliteNoLockTotal = 0;
double metadataTotal = 0;

int processDir(std::optional<dev_t> device, size_t prefixLen,
               size_t *currentTransactionTotal, size_t *total,
               SharedState *sharedState, FileInfo *fileInfo, int destdirfd) {
    const char *path = fileInfo->path;

    std::shared_ptr<FileDescriptor> parentDesc = nullptr;
    struct statx stx;
    if (!sharedState->isLustre) {
        if (statx(AT_FDCWD, fileInfo->path, AT_SYMLINK_NOFOLLOW, STATX_INO,
                  &stx)) {
            fprintf(stderr, "ERROR: %s: failed to statx: errno %d, error %s\n",
                    fileInfo->path, errno, strerror(errno));
            return 1;
        }
    } else {
        thread_local struct lov_user_mds_data *lmd =
            (lov_user_mds_data *)calloc(1, 8192 * 128);

        int fd = open(path, O_RDONLY);
        auto sharedFD = std::make_shared<FileDescriptor>();
        sharedFD->fd = fd;

        int ret = ioctl(fd, LL_IOC_MDC_GETINFO_V2, lmd);
        if (ret) {
            fprintf(stderr,
                    "ERROR: %s: failed to LL_IOC_MDC_GETINFO_V2: errno %d, "
                    "error %s\n",
                    fileInfo->path, errno, strerror(errno));
            return 1;
        }

        stx = lmd->lmd_stx;

        parentDesc = sharedFD;
    }

    dev_t thisDev = makedev(stx.stx_dev_major, stx.stx_dev_minor);
    if (device && device != thisDev) {
        return 0;
    } else if (!device) {
        device = thisDev;

        struct statfs64 stfs;
        if (statfs64(fileInfo->path, &stfs)) {
            fprintf(stderr, "ERROR: failed to statfs: %s\n", strerror(errno));
        } else {
            // This shouldn't be a race condition
            // This should run on the root dir only where 0 other jobs have
            // spawned
            sharedState->isLustre = stfs.f_type == LUSTRE_SUPER_MAGIC;
            if (opt_no_lustre_opt) {
                sharedState->isLustre = false;
            }
        }
    }

    if (sharedState->isLustre && !parentDesc) {
        int fd = open(path, O_RDONLY);
        parentDesc = std::make_shared<FileDescriptor>();
        parentDesc->fd = fd;
    }

    // if (0 == (strcmp(path, "/storage/vast/r-bkreitz3-0/bkreitz3/.conda/envs/"
    //                        "atomic/lib/python3.13/site-packages/"
    //                        "scipy/integrate/tests"))) {
    //     raise(SIGTRAP);
    // }

    DIR *dir = opendir(path);
    if (!dir) {
        fprintf(stderr, "ERROR: failed to opendir on %s: %d (%s)\n", path,
                errno, strerror(errno));
        return 1;
    }

    size_t pathLen = strlen(path);
    dirent *dp;

    while ((dp = readdir(dir))) {
        if (ISDOT(dp->d_name)) {
            continue;
        }

        FileInfo fi = {};
        size_t nameLen = strlen(dp->d_name);
        size_t totalLen = pathLen + 1 + nameLen;

        if (totalLen > PATH_MAX) {
            fprintf(stderr, "ERROR: path %s/%s exceeds pathmax\n", path,
                    dp->d_name);
            continue;
        }
        memcpy(fi.path, path, pathLen);
        fi.path[pathLen] = '/';
        memcpy(fi.path + pathLen + 1, (char *)dp->d_name, nameLen);

#pragma omp task
        {
            // fprintf(stderr, "INFO: env: %p\n", sharedState);
            double fileTaskStart = omp_get_wtime();
            auto entryTuple =
                fileTask(prefixLen, sharedState, &fi, destdirfd, parentDesc);
            metadataTotal += omp_get_wtime() - fileTaskStart;

            size_t currentCount = 0;
#pragma omp atomic capture
            currentCount = ++(*total);

            if ((currentCount % 10000) == 0) {
                fprintf(stderr, "INFO: crawled %zu\n", currentCount);
            }

            char *p = fi.path + prefixLen;
            if (p[0] == '/') {
                p++;
            }
            if (entryTuple) {
                double sqliteStart = omp_get_wtime();
                // fprintf(stderr, "INFO: write %s\n", p);

                auto &[inodeEntry, aclEntry, fileEntry] = entryTuple.value();
#pragma omp critical(DB_WRITE)
                {
                    double sqliteNoLockStart = omp_get_wtime();
                    if ((*currentTransactionTotal)++ == 0) {
                        // fprintf(stderr, "INFO: beginning\n");
                        int ret;
                        // fprintf(stderr, "INFO: env: %p\n", sharedState);
                        if ((ret = mdb_txn_begin(sharedState->env, NULL, 0,
                                                 &sharedState->writeTxn))) {
                            fprintf(
                                stderr,
                                "ERROR: LMDB failed to start transaction: %s\n",
                                mdb_strerror(ret));
                            goto exitWrite;
                        }

                        if ((ret = mdb_cursor_open(
                                 sharedState->writeTxn, sharedState->inodeTable,
                                 &sharedState->writeInodeCursor))) {
                            fprintf(stderr,
                                    "ERROR: LMDB failed to open cursor for "
                                    "inodeTable: %s\n",
                                    mdb_strerror(ret));
                            goto exitWrite;
                        }

                        if ((ret = mdb_cursor_open(
                                 sharedState->writeTxn, sharedState->aclTable,
                                 &sharedState->writeACLCursor))) {
                            fprintf(stderr,
                                    "ERROR: LMDB failed to open cursor for "
                                    "aclTable: %s\n",
                                    mdb_strerror(ret));
                            goto exitWrite;
                        }

                        if ((ret = mdb_cursor_open(
                                 sharedState->writeTxn,
                                 sharedState->finfoByInodeTable,
                                 &sharedState->writeFilesCursor))) {
                            fprintf(stderr,
                                    "ERROR: LMDB failed to open cursor for "
                                    "finfoByInodeTable: %s\n",
                                    mdb_strerror(ret));
                            goto exitWrite;
                        }

                        if ((ret = mdb_cursor_open(
                                 sharedState->writeTxn,
                                 sharedState->pathToInodeTable,
                                 &sharedState->writePathsCursor))) {
                            fprintf(stderr,
                                    "ERROR: LMDB failed to open cursor for "
                                    "pathToInodeTable: %s\n",
                                    mdb_strerror(ret));
                            goto exitWrite;
                        }
                        // if ((ret = mdb_cursor_open(, MDB_dbi dbi, MDB_cursor
                        // **cursor)))
                    }
                    dbWrite(p, sharedState, fi, &inodeEntry, aclEntry,
                            fileEntry);

                    if (aclEntry[0] == '\0') {
                        free((void *)aclEntry);
                    } else {
                        acl_free((void *)aclEntry);
                    }
                    free(fileEntry);

                    if ((*currentTransactionTotal % 1000000) == 0) {
                        // fprintf(stderr, "INFO: committing\n");
                        int ret;
                        mdb_cursor_close(sharedState->writeInodeCursor);
                        mdb_cursor_close(sharedState->writeACLCursor);
                        mdb_cursor_close(sharedState->writeFilesCursor);
                        mdb_cursor_close(sharedState->writePathsCursor);

                        sharedState->writeInodeCursor = NULL;
                        sharedState->writeACLCursor = NULL;
                        sharedState->writeFilesCursor = NULL;
                        sharedState->writePathsCursor = NULL;

                        if ((ret = mdb_txn_commit(sharedState->writeTxn))) {
                            fprintf(stderr,
                                    "ERROR: failed to commit lmdb transaction "
                                    ": %s\n",
                                    mdb_strerror(ret));
                        }

                        sharedState->writeTxn = NULL;

                        *currentTransactionTotal = 0;
                    }
                exitWrite:
                    assert(1);
                    sqliteNoLockTotal += omp_get_wtime() - sqliteNoLockStart;
                }
                sqliteTotal += omp_get_wtime() - sqliteStart;
            }
        }

        // Note: if your file system doesn't support dirent's
        // d_type, run statx with STATX_TYPE to get this information
        if (dp->d_type == DT_DIR) {
#pragma omp task
            {
                auto innerFi = fi;
                processDir(device, prefixLen, currentTransactionTotal, total,
                           sharedState, &innerFi, destdirfd);
            }
        }
    }

    closedir(dir);

    return 0;
}

int recursive_dir_checks(char *path) {
    FTS *dir;
    FTSENT *ent;
    size_t count = 0;
    size_t numFinished = 0;
    off_t size = 0;
    off_t fsize = 0;
    int err;

    int nThreads = omp_get_max_threads();
    fprintf(stderr, "INFO: max threads is %d\n", nThreads);

    static MDB_env *mdbEnv = NULL;
    if ((err = mdb_env_create(&mdbEnv))) {
        fprintf(stderr, "ERROR: failed to create mdb env: error %s\n",
                mdb_strerror(err));
        return EXIT_FAILURE;
    }
    mdb_env_set_maxdbs(mdbEnv, 8);
    mdb_env_set_mapsize(mdbEnv, (size_t)1048576 * (size_t)10000000);
    if ((err = mdb_env_open(mdbEnv, opt_opath,
                            MDB_NOTLS | MDB_NOLOCK | MDB_CREATE | MDB_NOSUBDIR |
                                MDB_WRITEMAP | MDB_NOSYNC,
                            0777))) {
        fprintf(stderr, "ERROR: failed to open mdb environment: error %s\n",
                mdb_strerror(err));
        return EXIT_FAILURE;
    }

    MDB_dbi inodeTable;
    MDB_dbi aclTable;
    MDB_dbi finfoByInodeTable;
    MDB_dbi pathToInodeTable;
    {
        MDB_txn *txn;
        if ((err = mdb_txn_begin(mdbEnv, NULL, 0, &txn))) {
            fprintf(stderr,
                    "ERROR: failed to begin mdb dbi aquisition transaction: "
                    "error %s",
                    mdb_strerror(err));
            return EXIT_FAILURE;
        }

        if ((err = mdb_dbi_open(txn, "INODES",
                                MDB_CREATE | MDB_INTEGERKEY | MDB_DUPSORT |
                                    MDB_DUPFIXED,
                                &inodeTable))) {
            fprintf(stderr, "ERROR: failed to get/create INODES table: %s\n",
                    mdb_strerror(err));
            return EXIT_FAILURE;
        }

        if ((err = mdb_dbi_open(txn, "ACLS",
                                MDB_CREATE | MDB_INTEGERKEY | MDB_DUPSORT |
                                    MDB_DUPFIXED,
                                &aclTable))) {
            fprintf(stderr, "ERROR: failed to get/create INODES table: %s\n",
                    mdb_strerror(err));
            return EXIT_FAILURE;
        }

        if ((err = mdb_dbi_open(txn, "FINFOBYINODE",
                                MDB_CREATE | MDB_INTEGERDUP | MDB_DUPSORT |
                                    MDB_DUPFIXED,
                                &finfoByInodeTable))) {
            fprintf(stderr, "ERROR: failed to get/create FILES table: %s\n",
                    mdb_strerror(err));
            return EXIT_FAILURE;
        }

        if ((err = mdb_dbi_open(txn, "PATHTOINODE",
                                MDB_CREATE | MDB_DUPFIXED | MDB_DUPSORT,
                                &pathToInodeTable))) {
            fprintf(stderr,
                    "ERROR: failed to get/create PATHTOINODE table: %s\n",
                    mdb_strerror(err));
            return EXIT_FAILURE;
        }

        if ((err = mdb_txn_commit(txn))) {
            fprintf(stderr,
                    "ERROR: failed to commit mdb dbi aquisition transaction: "
                    "error %s",
                    strerror(err));
            return EXIT_FAILURE;
        }
    }

    std::unordered_map<std::string, AttributeTimes> existingTimes;

    std::unordered_map<std::string, DestState> destStateMap;
    int destdirfd;

    if (opt_shouldPrintTansferCommands) {
        if (opt_destStateDB && opt_destStatePath) {
            fprintf(stderr, "FATAL: please only specify one of dest-state-db "
                            "or dest-state-path\n");
            return EXIT_FAILURE;
        } else if (opt_destStateDB) {

            struct stat st;
            if (stat(opt_destStateDB, &st)) {
                fprintf(stderr, "FATAL: failed to stat destStateDB (%s): %s\n",
                        opt_destStateDB, strerror(errno));
                return EXIT_FAILURE;
            }

        } else if (opt_destStatePath) {
            if ((destdirfd = open(opt_destStatePath, O_DIRECTORY)) == -1) {
                fprintf(stderr, "FATAL: failed to open dest state dir %s, %s\n",
                        opt_destStatePath, strerror(errno));
                return EXIT_FAILURE;
            }
        }
    }

    std::unordered_set<ino_t> seenInodes;
    std::unordered_set<ino_t> seenInodesDest;

    size_t prefixLen = strlen(path);
    size_t currentTransactionTotal = 0;

    std::string datPath = std::string(opt_opath) + ".dat";
    int datfd = open(datPath.c_str(), O_WRONLY | O_TRUNC | O_CREAT);

    if (datfd < 0) {
        fprintf(stderr, "ERROR: failed to open dat file %s: %s\n",
                datPath.c_str(), strerror(errno));
        return 1;
    }

    if (ftruncate(datfd, (size_t)1048576 * (size_t)10000000) < 0) {
        fprintf(stderr, "ERROR: failed to truncate dat file: %s\n",
                strerror(errno));
        return 0;
    }

    SharedState *sharedState = new SharedState{
        .env = mdbEnv,
        .inodeTable = inodeTable,
        .aclTable = aclTable,
        .finfoByInodeTable = finfoByInodeTable,
        .pathToInodeTable = pathToInodeTable,
        .existingTimes = existingTimes,
        .seenInodes = seenInodes,
        .seenInodesDest = seenInodesDest,
        .destStates = destStateMap,
        .writeHead = 0,
        .datfd = datfd,
    };

#pragma omp parallel
    {
#pragma omp single
        {
            FileInfo fi = {};
            strcpy(fi.path, path);
            // fprintf(stderr, "INFO: env: %p\n", sharedState);

            processDir(std::nullopt, prefixLen, &currentTransactionTotal,
                       &count, sharedState, &fi, destdirfd);
        }
    }

    if (sharedState->writeFilesCursor) {
        mdb_cursor_close(sharedState->writeFilesCursor);
        sharedState->writeFilesCursor = NULL;
    }

    if (sharedState->writeACLCursor) {
        mdb_cursor_close(sharedState->writeACLCursor);
        sharedState->writeACLCursor = NULL;
    }

    if (sharedState->writeInodeCursor) {
        mdb_cursor_close(sharedState->writeInodeCursor);
        sharedState->writeInodeCursor = NULL;
    }

    if (sharedState->writePathsCursor) {
        mdb_cursor_close(sharedState->writePathsCursor);
        sharedState->writePathsCursor = NULL;
    }

    // fprintf(stderr, "INFO: pre thing: %zu\n", currentTransactionTotal);
    if (sharedState->writeTxn) {
        fprintf(stderr, "INFO: final commit\n");
        int ret;
        if ((ret = mdb_txn_commit(sharedState->writeTxn))) {
            fprintf(stderr,
                    "ERROR: failed to commit lmdb transaction: "
                    "%s\n",
                    mdb_strerror(ret));
        }
        sharedState->writeTxn = NULL;
    }

    // fprintf(stderr, "INFO: total metadata time: %f\n", metadataTotal);
    // fprintf(stderr, "INFO: total sqlite time: %f\n", sqliteTotal);
    // fprintf(stderr, "INFO: total sqlite no lock time: %f\n", sqliteNoLockTotal);
    // fprintf(stderr, "INFO: load gid uid total: %f\n", loadGidUidTotal);
    // fprintf(stderr, "INFO: load acl total: %f\n", loadACLTotal);
    // fprintf(stderr, "INFO: load sl total: %f\n", loadSLTotal);
    // fprintf(stderr, "INFO: stat total: %f\n", statTotal);

    count = 0;
    return 0;
}

int main(int argc, char *argv[]) {
    opterr = 0;

    omp_set_nested(1);
    omp_set_max_active_levels(1024);

    static struct option long_options[] = {
        {"help", no_argument, 0, 'h'},
        {"print-transfer-commands", no_argument, 0, 'p'},
        {"jobs", required_argument, 0, 'j'},
        {"output", required_argument, 0, 'o'},
        // {"checksum", required_argument, 0, 'c'},
        {"dest-state-db", required_argument, 0, 0},
        {"dest-state-path", required_argument, 0, 0},
        {"max-memload", required_argument, 0, 0},
        {"lustre-opt", no_argument, 0, 0},
    };

    int c;
    int option_index;
    while ((c = getopt_long(argc, argv, "hpj:o:d:", long_options,
                            &option_index)) != -1) {
        switch (c) {
        case 0:
            if (strcmp(long_options[option_index].name, "dest-state-db") == 0) {
                opt_destStateDB = optarg;
            } else if (strcmp(long_options[option_index].name,
                              "dest-state-path") == 0) {
                opt_destStatePath = optarg;
            } else if (strcmp(long_options[option_index].name, "max-memload") ==
                       0) {
                char *raw = optarg;
                size_t rawlen = strlen(raw);
                size_t multiplier = 1;

                if (!rawlen) {
                    fprintf(stderr,
                            "FATAL: No max-memload argument specified.");
                    exit(EXIT_FAILURE);
                }

                char *endptr;
                size_t num = strtoull(raw, &endptr, 10);

                if ((endptr - raw) == (rawlen - 1)) {
                    switch (*endptr) {
                    case 'K':
                        multiplier = 1000;
                        break;
                    case 'M':
                        multiplier = 1000000;
                        break;
                    case 'G':
                        multiplier = 1000000000;
                        break;
                    default: {
                        fprintf(stderr, "FATAL: invalid magnitude order for "
                                        "max-memload. Options are K, M, G.\n");
                        exit(EXIT_FAILURE);
                    }
                    }
                } else if ((endptr - raw) < rawlen) {
                    fprintf(stderr, "FATAL: invalid argument to max-memload\n");
                    exit(EXIT_FAILURE);
                }

                opt_max_db_memload_size = multiplier * num;

                fprintf(stderr,
                        "INFO: memload is %zu, num=%zu, multiplier=%zu\n",
                        opt_max_db_memload_size, num, multiplier);
            } else if (strcmp(long_options[option_index].name, "lustre-opt") ==
                       0) {
                opt_no_lustre_opt = 0;
            } else {
                printHelp();
                exit(EXIT_FAILURE);
            }
            break;
        case 'h':
            printHelp();
            exit(EXIT_SUCCESS);
            break;
        // case 'c':
        //     opt_hashalgo = optarg;
        //     break;
        case 'j':
            opt_njobs = optarg;
            break;
        case 'o':
            opt_opath = optarg;
            break;
        case 'p':
            opt_shouldPrintTansferCommands = 1;
            break;
        case '?':
        default:
            printHelp();
            exit(EXIT_FAILURE);
            break;
        }
    }

    if (!opt_opath) {
        fprintf(stderr, "FATAL: please provide output database\n");
        exit(EXIT_FAILURE);
    }

    if (opt_hashalgo) {
        if ((strcmp(opt_hashalgo, "md5"))) {
            fprintf(stderr, "FATAL: hash algorithm must be md5!\n");
            exit(EXIT_FAILURE);
        } else if (!strcmp(opt_hashalgo, "md5")) {
            hashalgo = HASH_MD5SUM;
        }
    }

    if (opt_njobs) {
        njobs = atoi(opt_njobs);
        if (njobs <= 0) {
            fprintf(stderr, "FATAL: please enter valid number of jobs!\n");
            exit(EXIT_FAILURE);
        }
    }
    omp_set_num_threads(njobs);

    if (optind >= argc) {
        fprintf(stderr, "FATAL: specify directory to crawl\n");
        exit(EXIT_FAILURE);
    }

    return recursive_dir_checks(argv[optind]);
}
