#include "lmdb-misc.h"
#include "pacecrawl-table.h"
#include <cstring>
#include <fcntl.h>
#include <fcntl.h>
#include <lmdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <unordered_map>

struct Stats {
    size_t count;
    size_t diskSize;
    size_t apparentSize;
};

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

    MDB_val key = {};
    MDB_val val = {};

    std::string datpath = std::string(argv[1]) + ".dat";
    int datfd = open(datpath.c_str(), O_RDONLY);
    if (datfd < 0) {
        fprintf(stderr, "ERROR: failed to open dat file %s: %s\n",
                datpath.c_str(), strerror(errno));
        return 1;
    }

    struct stat st;
    if (stat(datpath.c_str(), &st) < 0) {
        fprintf(stderr, "ERROR: failed to stat dat file %s: %s\n",
                datpath.c_str(), strerror(errno));
        return 1;
    }

    void *datptr = mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, datfd, 0);
    if (!datptr) {
        fprintf(stderr, "ERROR failed to map dat file %s: %s\n",
                datpath.c_str(), strerror(errno));
        return 1;
    }

    std::unordered_map<std::string, Stats> userStats;

    size_t count = 0;
    size_t diskSize = 0;
    size_t apparentSize = 0;
    while ((err = mdb_cursor_get(cursors.finfoCursor, &key, &val, MDB_NEXT)) ==
           0) {
        FileTableEntry *finfo =
            (FileTableEntry *)((uint8_t *)datptr + *(uint64_t *)val.mv_data);
        // fprintf(stderr, "INFO: file is %zu %s\n", *(uint64_t *)val.mv_data,
        //         finfo->pathAndLinkrefAndAbs);

        key.mv_data = &finfo->inode;
        key.mv_size = sizeof(finfo->inode);
        err = mdb_cursor_get(cursors.inodeInfoCursor, &key, &val, MDB_SET);
        if (err) {
            fprintf(stderr,
                    "ERROR: failed to find inode entry for file %s: %s\n",
                    finfo->pathAndLinkrefAndAbs, mdb_strerror(err));
            continue;
        }

        InodeTableEntry inodeEntry = *((
            InodeTableEntry *)((uint8_t *)datptr + *((uint64_t *)val.mv_data)));

        if (S_ISREG(inodeEntry.mode)) {
            count++;
            diskSize += inodeEntry.blocks * 512 / inodeEntry.nhlink;
            apparentSize += inodeEntry.size / inodeEntry.nhlink;
        } else {
            count++;
            diskSize += inodeEntry.blocks * 512;
            apparentSize += inodeEntry.size;
        }

        char *end = strchr(finfo->pathAndLinkrefAndAbs, '/');
        std::string userName =
            std::string(finfo->pathAndLinkrefAndAbs,
                        end ? end : finfo->pathAndLinkrefAndAbs);

        auto &stats = userStats[userName];
        if (S_ISREG(inodeEntry.mode)) {
            stats.count++;
            stats.diskSize += inodeEntry.blocks * 512 / inodeEntry.nhlink;
            stats.apparentSize += inodeEntry.size / inodeEntry.nhlink;
        } else {
            stats.count++;
            stats.diskSize += inodeEntry.blocks * 512;
            stats.apparentSize += inodeEntry.size;
        }

        // if ((count % 10000) == 0) {
        //     fprintf(stderr, "INFO: count=%zu\n", count);
        // }

        // fprintf(stderr, "info: %zu, %zu, %zu\n", inodeEntry.blocks * 512,
        //         inodeEntry.size, *((uint64_t *)val.mv_data));
    }

    fprintf(stdout, "count=%zu, diskSize=%zu, apparentSize=%zu\n", count,
            diskSize, apparentSize);

    for (auto &[k, v] : userStats) {
        if (k == "") {
            continue;
        }

        fprintf(stdout, "user=%s count=%zu, diskSize=%zu, apparentSize=%zu\n",
                k.c_str(), v.count, v.diskSize, v.apparentSize);
    }

    return EXIT_SUCCESS;
}
