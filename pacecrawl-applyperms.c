#include "pacecrawl-table.h"
#include "sys/acl.h"
#include <bits/statx.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/stat.h>
#include <omp.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/statfs.h>
#include <sys/sysmacros.h>
#include <sys/xattr.h>
#include <time.h>
#include <unistd.h>

#include "lmdb-misc.h"
#include "lmdb.h"

int main(int argc, char *argv[]) {
    if (argc < 3) {
        return EXIT_FAILURE;
    }

    int err;
    struct DBISet dbs = {};
    if ((err = openDB(argv[1], &dbs))) {
        return EXIT_FAILURE;
    }

    MDB_txn *txn;
    if ((err = mdb_txn_begin(dbs.env, NULL, MDB_RDONLY, &txn))) {
        return EXIT_FAILURE;
    }

    struct Cursors cursors = {};
    if ((err = openCursors(&dbs, txn, &cursors))) {
        return EXIT_FAILURE;
    }

    char *datpath = calloc(1, strlen(argv[1]) + strlen(".dat") + 1);
    strcat(datpath, argv[1]);
    strcat(datpath, ".dat");

    int datfd = open(datpath, O_RDONLY);
    if (datfd < 0) {
        fprintf(stderr, "ERROR: failed to open dat file %s: %s\n", datpath,
                strerror(errno));
        return 1;
    }

    struct stat st;
    if (stat(datpath, &st) < 0) {
        fprintf(stderr, "ERROR: failed to stat dat file %s: %s\n", datpath,
                strerror(errno));
        return 1;
    }

    void *datptr = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, datfd, 0);
    if (!datptr) {
        fprintf(stderr, "ERROR failed to map dat file %s: %s\n", datpath,
                strerror(errno));
        return 1;
    }

    // int dirfd = open(argv[2], O_DIRECTORY);
    if (chdir(argv[2])) {
        fprintf(stderr, "FATAL: failed to chdir to directory (%s), %s\n",
                argv[2], strerror(errno));
        exit(EXIT_FAILURE);
    }

    MDB_val key = {};
    MDB_val val = {};

    int count = 0;

    size_t linebufLen = 0;
    ssize_t lineLen = 0;
    char *linebuf = NULL;
#pragma omp parallel
#pragma omp single
    while (linebuf = NULL,
           (lineLen = getline(&linebuf, &linebufLen, stdin)) != -1) {
        if (linebuf[lineLen - 1] == '\n') {
            linebuf[lineLen - 1] = '\0';
            lineLen--;
        }

        key.mv_data = linebuf;
        key.mv_size = lineLen + 1;
        int err;
        if ((err = mdb_cursor_get(cursors.pathsCursor, &key, &val, MDB_SET))) {
            fprintf(stderr, "ERROR: failed to get path %s: %s\n", linebuf,
                    mdb_strerror(err));
            continue;
        }

        ino_t inode = *((uint64_t *)val.mv_data);

        key.mv_data = &inode;
        key.mv_size = sizeof(inode);
        if ((err = mdb_cursor_get(cursors.inodeInfoCursor, &key, &val,
                                  MDB_SET))) {
            fprintf(stderr, "ERROR: failed to get inode %zu for %s: %s\n",
                    inode, linebuf, mdb_strerror(err));
            continue;
        }

        InodeTableEntry inodeEntry =
            *((InodeTableEntry *)(datptr + *((uint64_t *)val.mv_data)));

        // copy into parallel region, free at end
        char *path = linebuf;
#pragma omp task
        {
            mode_t mode = inodeEntry.mode;
            uid_t uid = inodeEntry.uid;
            uid_t gid = inodeEntry.gid;
            bool isLink = S_ISLNK(inodeEntry.mode);
            bool isFile = S_ISREG(inodeEntry.mode);

            fprintf(stderr, "islnk=%d chmod %o %s\n", isLink, mode, path);

            if (lchown(path, uid, gid)) {
                // #pragma omp critical(PRINT)
                fprintf(stderr, "ERROR: failed to chown path (%s), %s\n", path,
                        strerror(errno));
            }

            if (!isLink && fchmodat(AT_FDCWD, path, mode, 0)) {
                // #pragma omp critical(PRINT)
                fprintf(stderr, "ERROR: failed to chmod path (%s), %s\n", path,
                        strerror(errno));
            }

            if (S_ISDIR(inodeEntry.mode)) {
                struct timespec times[] = {
                    {.tv_nsec = UTIME_OMIT},
                    inodeEntry.modtime,
                };
                if (utimensat(AT_FDCWD, path, times, AT_SYMLINK_NOFOLLOW) < 0) {
                    fprintf(stderr, "failed to utimensat %s: %s\n", path,
                            strerror(errno));
                    exit(1);
                }
            }

            // if (isFile) {
            //     acl_t acl = acl_from_text(aclText);
            //     if (!acl) {
            //         // #pragma omp critical(PRINT)
            //         fprintf(
            //             stderr,
            //             "ERROR: failed to create acl for path (%s),
            //             %s\n", path, strerror(errno));
            //     } else if (acl_set_file(path, ACL_TYPE_ACCESS, acl)) {
            //         // #pragma omp critical(PRINT)
            //         fprintf(stderr,
            //                 "ERROR: failed to set acl for path (%s),
            //                 %s\n", path, strerror(errno));
            //     }
            //     acl_free(acl);
            // }
            free(path);
        }
    }

    return EXIT_SUCCESS;
}
