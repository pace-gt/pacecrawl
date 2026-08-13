#include "lmdb-misc.h"
#include "pacecrawl-table.h"
#include <errno.h>
#include <fcntl.h>
#include <lmdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
    struct DBISet dbs;
    if (openDB(argv[1], &dbs)) {
        return EXIT_FAILURE;
    }

    int err;

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
    if (datptr == (void*)-1) {
        fprintf(stderr, "ERROR failed to map dat file %s: %s\n", datpath,
                strerror(errno));
        return 1;
    }

    MDB_val key = {};
    MDB_val val = {};

    size_t count = 0;
    while ((err = mdb_cursor_get(cursors.inodeInfoCursor, &key, &val,
                                 MDB_NEXT)) == 0) {
        InodeTableEntry inodeEntry =
            *((InodeTableEntry *)(datptr + *((uint64_t *)val.mv_data)));

        count++;

        if ((count % 10000) == 0) {
            fprintf(stderr, "INFO: scanned %zu\n", count);
        }
        // if (!S_ISDIR(inodeEntry.mode)) {
        //     continue;
        // }

        key.mv_data = &inodeEntry.inode;
        key.mv_size = sizeof(inodeEntry.inode);
        if ((err = mdb_cursor_get(cursors.finfoCursor, &key, &val, MDB_SET))) {
            fprintf(stderr, "ERROR: could not find path for inode %zu\n",
                    inodeEntry.inode);
            continue;
        }

        FileTableEntry *entry =
            (FileTableEntry *)(datptr + *((uint64_t *)val.mv_data));

        fprintf(stdout, "%s\n", entry->pathAndLinkrefAndAbs);
    }

    return EXIT_SUCCESS;
}
