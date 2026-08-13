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

    if (argc < 3) {
        fprintf(stderr, "ERROR: please specify a path to search for\n");
        return EXIT_FAILURE;
    }

    // const char *path = argv[2];

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

    MDB_val key = {};
    MDB_val val = {};

    char *endptr;
    uint64_t inode = strtoull(argv[2], &endptr, 10);

    key.mv_data = &inode;
    key.mv_size = sizeof(uint64_t);

    MDB_val ftableVal = {};
    if ((err = mdb_get(txn, dbs.finfoByInodeTable, &key, &ftableVal)) !=
        MDB_SUCCESS) {
        fprintf(stderr, "ERROR: failed to find inode %zu in finfo table \n",
                inode);
        return EXIT_FAILURE;
    }

    FileTableEntry *ft =
        (FileTableEntry *)(datptr + *((uint64_t *)ftableVal.mv_data));

    MDB_val inodetableVal = {};
    if ((err = mdb_get(txn, dbs.inodeTable, &key, &inodetableVal)) !=
        MDB_SUCCESS) {
        fprintf(stderr, "ERROR: failed to find inode %zu in inode table \n",
                inode);
        return EXIT_FAILURE;
    }
    InodeTableEntry *it =
        ((InodeTableEntry *)(datptr + *((uint64_t *)inodetableVal.mv_data)));

    fprintf(
        stdout,
        "InodeTableEntry{ mode=%o, uid=%d, gid=%d, modtime=%zu, chgtime=%zu, "
        "btime=%zu, size=%zu, blocks=%zu, inode=%zu, nhlink=%zu}\n",
        it->mode, it->uid, it->gid, it->modtime.tv_sec, it->chgtime.tv_nsec,
        it->btime.tv_sec, it->size, it->blocks, it->inode, it->nhlink);

    err = mdb_cursor_get(cursors.finfoCursor, &key, &val, MDB_SET);
    while (err == MDB_SUCCESS) {
        uint64_t fileInfoOffset = *((uint64_t *)val.mv_data);
        FileTableEntry *ft = (FileTableEntry *)(datptr + fileInfoOffset);
        fprintf(stdout, "path: %s\n", ft->pathAndLinkrefAndAbs);

        err = mdb_cursor_get(cursors.finfoCursor, &key, &val, MDB_NEXT_DUP);
    }

    fprintf(stderr, "end with %s\n", mdb_strerror(err));

    return EXIT_SUCCESS;
}
