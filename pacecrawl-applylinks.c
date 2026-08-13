#include "lmdb-misc.h"
#include "lmdb.h"
#include <asm-generic/errno-base.h>
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <linux/stat.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

int statx(int __dirfd, const char *__restrict __path, int __flags,
          unsigned int __mask, struct statx *__restrict __buf);

int main(int argc, char *argv[]) {
    if (argc < 3) {
        return EXIT_FAILURE;
    }

    int dirfd = open(argv[2], O_DIRECTORY);
    if (dirfd < 0) {
        fprintf(stderr, "FATAL: failed to open directory (%s), %s", argv[2],
                strerror(errno));
        exit(EXIT_FAILURE);
    }

    struct DBISet dbs = {};
    struct Cursors cursors = {};

    if (openDB(argv[1], &dbs)) {
        return EXIT_FAILURE;
    }

    int err;
    MDB_txn *txn;
    if ((err = mdb_txn_begin(dbs.env, NULL, MDB_RDONLY, &txn))) {
        fprintf(stderr, "ERROR: failed to begin mdb transaction: %s\n",
                mdb_strerror(err));
        return EXIT_FAILURE;
    }

    if (openCursors(&dbs, txn, &cursors)) {
        return EXIT_FAILURE;
    }

    int pathsetCapacity = 4096;
    char **pathset = calloc(pathsetCapacity, sizeof(char *));
    char *linkRoot = NULL;
    int pathsetSize = 0;

    MDB_val key = {};
    MDB_val val = {};

    int count = 0;
#pragma omp parallel
#pragma omp single
    while ((err = mdb_cursor_get(cursors.inodeInfoCursor, &key, &val,
                                 MDB_NEXT)) == 0) {
    retry:
        err = 0; // dummy nop for label

        int64_t inode = *((uint64_t *)key.mv_data);
        struct timespec destInodeModtime;
        int64_t
            destInode; // The inode for the group of files on the destination

        // char *col = (char *)sqlite3_column_text(stmt, 0);
        char *p;
        // if (col) {
        //     p = strdup(col);
        // } else {
        //     p = strdup("");
        // }
        if ((err = mdb_cursor_get(cursors.finfoCursor, MDB_val *key, MDB_val *data, MDB_cursor_op op)))

        if (faccessat(dirfd, p, F_OK, AT_SYMLINK_NOFOLLOW) == 0) {
            struct statx stx;
            statx(dirfd, p, AT_SYMLINK_NOFOLLOW, STATX_INO | STATX_MTIME, &stx);
            if (!linkRoot) {
                // fprintf(stderr, "INFO: linkroot is %s\n", p);
                linkRoot = p;

                destInode = stx.stx_ino;
                destInodeModtime.tv_sec = stx.stx_mtime.tv_sec;
                destInodeModtime.tv_nsec = stx.stx_mtime.tv_nsec;
            } else if (stx.stx_mtime.tv_sec > destInodeModtime.tv_sec ||
                       (stx.stx_mtime.tv_sec == destInodeModtime.tv_sec &&
                        stx.stx_mtime.tv_nsec > destInodeModtime.tv_nsec)) {
                linkRoot = p;

                destInode = stx.stx_ino;
                destInodeModtime.tv_sec = stx.stx_mtime.tv_sec;
                destInodeModtime.tv_nsec = stx.stx_mtime.tv_nsec;
            }
        } else {
            // fprintf(stderr, "INFO: faccessat failed for %s\n", p);
        }

        if (pathsetSize >= pathsetCapacity) {
            pathsetCapacity += 4096;
            pathset = realloc(pathset, pathsetCapacity * sizeof(char *));
        }
        pathset[pathsetSize++] = p;

        int64_t dstInode = -1; // surely negative inodes don't exist
        while ((err = sqlite3_step(stmt)) == SQLITE_ROW || err == SQLITE_DONE) {
            if (err != SQLITE_DONE)
                dstInode = sqlite3_column_int64(stmt, 1);

            if (dstInode != inode || err == SQLITE_DONE) {
                if (linkRoot) {
                    for (int i = 0; i < pathsetSize; i++) {
                        if (pathset[i] == linkRoot) {
                            continue;
                        }

                        count++;
                        if ((count % 10000) == 0) {
                            fprintf(stderr, "INFO: applied %d\n", count);
                        }

                        char *srcPath = strdup(linkRoot);
                        char *dstPath = pathset[i];
#pragma omp task
                        {
                            if (faccessat(dirfd, dstPath, F_OK,
                                          AT_SYMLINK_NOFOLLOW) == 0) {
                                struct statx stx;
                                statx(dirfd, dstPath, AT_SYMLINK_NOFOLLOW,
                                      STATX_INO, &stx);

                                if ((int64_t)stx.stx_ino != destInode) {
                                    unlinkat(dirfd, dstPath, 0);
                                }
                            }

                            fprintf(stderr, "INFO: performing link %s->%s\n",
                                    srcPath, dstPath);
                            if (linkat(dirfd, srcPath, dirfd, dstPath, 0) < 0 &&
                                errno != EEXIST) {
#pragma omp critical(PRINT)
                                fprintf(stderr,
                                        "ERROR: failed to perform link %s->%s "
                                        "(%s)\n",
                                        srcPath, dstPath, strerror(errno));
                            }

                            free(dstPath);
                            free(srcPath);
                        }
                    }

                    free(linkRoot);
                } else {
                    if (pathsetSize > 1)
                        fprintf(stderr, "failed to find a linkroot for %s\n",
                                pathset[0]);
                    for (int i = 0; i < pathsetSize; i++) {
                        free(pathset[i]);
                    }
                }

                pathsetSize = 0;
                linkRoot = NULL;

                break;
            }

            if (err == SQLITE_DONE) {
                break;
            }

            col = (char *)sqlite3_column_text(stmt, 0);
            if (col) {
                p = strdup(col);
            } else {
                p = strdup("");
            }

            if (pathsetSize >= pathsetCapacity) {
                pathsetCapacity += 4096;
                pathset = realloc(pathset, pathsetCapacity * sizeof(char *));
            }
            pathset[pathsetSize++] = p;

            if (faccessat(dirfd, p, F_OK, AT_SYMLINK_NOFOLLOW) == 0) {
                struct statx stx;
                statx(dirfd, p, AT_SYMLINK_NOFOLLOW, STATX_INO | STATX_MTIME,
                      &stx);
                if (!linkRoot) {
                    // fprintf(stderr, "INFO: linkroot is %s\n", p);
                    linkRoot = p;

                    destInode = stx.stx_ino;
                    destInodeModtime.tv_sec = stx.stx_mtime.tv_sec;
                    destInodeModtime.tv_nsec = stx.stx_mtime.tv_nsec;
                } else if (stx.stx_mtime.tv_sec > destInodeModtime.tv_sec ||
                           (stx.stx_mtime.tv_sec == destInodeModtime.tv_sec &&
                            stx.stx_mtime.tv_nsec > destInodeModtime.tv_nsec)) {
                    linkRoot = p;

                    destInode = stx.stx_ino;
                    destInodeModtime.tv_sec = stx.stx_mtime.tv_sec;
                    destInodeModtime.tv_nsec = stx.stx_mtime.tv_nsec;
                }
            } else {
                // fprintf(stderr, "INFO: faccessat failed for %s\n", p);
            }
        }

        if (err != SQLITE_ROW) {
            break;
        } else {
            goto retry;
        }
    }

    if (err != SQLITE_DONE) {
        fprintf(stderr, "FATAL: sqlite step failed (%s)\n",
                sqlite3_errmsg(conn));
        exit(1);
    }

    return 0;
}
