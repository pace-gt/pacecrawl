#include "lmdb-misc.h"
#include "pacecrawl-table.h"
#include <algorithm>
#include <cstring>
#include <ctime>
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
#include <vector>

#define FUZZY_MONTHS(t) ((t) / 60ULL / 60 / 24 / 30)

#define NMONTHS 12ULL

struct Stats {
    size_t count;
    size_t diskSize;
    size_t apparentSize;

    std::vector<size_t> atimeCountsByMonthsAgo;
    std::vector<size_t> atimeSizesByMonthsAgo;
    std::vector<size_t> mtimeCountsByMonthsAgo;
    std::vector<size_t> mtimeSizesByMonthsAgo;
};

inline void printHeader() {
    fprintf(stdout, "USERNAME\tCOUNT\tDISKSIZE\tLOGICALSIZE\t");

    for (int i = 0; i < NMONTHS; i++) {
        fprintf(stdout, "ATIME_-%dM_COUNT\t", i);
    }
    fprintf(stdout, "ATIME_GT_YEAR_COUNT\tATIME_FUTURE_COUNT\t");

    for (int i = 0; i < NMONTHS; i++) {
        fprintf(stdout, "MTIME_-%dM_COUNT\t", i);
    }
    fprintf(stdout, "MTIME_GT_YEAR_COUNT\tMTIME_FUTURE_COUNT\t");

    for (int i = 0; i < NMONTHS; i++) {
        fprintf(stdout, "ATIME_-%dM_SIZE\t", i);
    }
    fprintf(stdout, "ATIME_GT_-%lluM_SIZE\tATIME_FUTURE_SIZE\t", NMONTHS);

    for (int i = 0; i < NMONTHS; i++) {
        fprintf(stdout, "MTIME_-%dM_SIZE\t", i);
    }
    fprintf(stdout, "MTIME_GT_-%lluM_SIZE\tMTIME_FUTURE_SIZE\n", NMONTHS);
}

inline void printStats(const Stats &stats, const char *username) {
    fprintf(stdout, "%s\t%zu\t%zu\t%zu\t", username, stats.count,
            stats.diskSize, stats.apparentSize);

    for (int i = 0; i < stats.atimeCountsByMonthsAgo.size(); i++) {
        fprintf(stdout, "%zu\t", stats.atimeCountsByMonthsAgo[i]);
    }

    for (int i = 0; i < stats.mtimeCountsByMonthsAgo.size(); i++) {
        fprintf(stdout, "%zu\t", stats.mtimeCountsByMonthsAgo[i]);
    }

    for (int i = 0; i < stats.atimeSizesByMonthsAgo.size(); i++) {
        fprintf(stdout, "%zu\t", stats.atimeSizesByMonthsAgo[i]);
    }

    for (int i = 0; i < stats.mtimeSizesByMonthsAgo.size(); i++) {
        fprintf(stdout, "%zu\t", stats.mtimeSizesByMonthsAgo[i]);
    }

    fprintf(stdout, "\n");
}

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

    uint64_t now = time(NULL);

    std::unordered_map<std::string, Stats> userStats;

    // size_t count = 0;
    // size_t diskSize = 0;
    // size_t apparentSize = 0;
    Stats globalStats = {};
    globalStats.atimeCountsByMonthsAgo.resize(NMONTHS + 2);
    globalStats.mtimeCountsByMonthsAgo.resize(NMONTHS + 2);
    globalStats.atimeSizesByMonthsAgo.resize(NMONTHS + 2);
    globalStats.mtimeSizesByMonthsAgo.resize(NMONTHS + 2);
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
            globalStats.count++;
            globalStats.diskSize += inodeEntry.blocks * 512 / inodeEntry.nhlink;
            globalStats.apparentSize += inodeEntry.size / inodeEntry.nhlink;
        } else {
            globalStats.count++;
            globalStats.diskSize += inodeEntry.blocks * 512;
            globalStats.apparentSize += inodeEntry.size;
        }

        char *start = finfo->pathAndLinkrefAndAbs;
        char *end = strchr(start, '/');
        end = end ? strchr(end + 1, '/') : end;
        // if (end) {
        //     end = strchr(end + 1, '/');
        // }

        // fprintf(stderr, "%s, %s\n", start, end);

        std::string userName =
            std::string(start ? start : finfo->pathAndLinkrefAndAbs,
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

        if (stats.atimeCountsByMonthsAgo.size() != NMONTHS + 2) {
            stats.atimeCountsByMonthsAgo.resize(NMONTHS + 2);
            stats.atimeSizesByMonthsAgo.resize(NMONTHS + 2);
        }

        if (stats.mtimeCountsByMonthsAgo.size() != NMONTHS + 2) {
            stats.mtimeCountsByMonthsAgo.resize(NMONTHS + 2);
            stats.mtimeSizesByMonthsAgo.resize(NMONTHS + 2);
        }

        if (inodeEntry.modtime.tv_sec > now) {
            stats.mtimeCountsByMonthsAgo[NMONTHS + 1]++;
            globalStats.mtimeCountsByMonthsAgo[NMONTHS + 1]++;

            stats.mtimeSizesByMonthsAgo[NMONTHS + 1] += inodeEntry.size;
            globalStats.mtimeSizesByMonthsAgo[NMONTHS + 1] += inodeEntry.size;
        } else {
            uint64_t month = std::clamp(
                FUZZY_MONTHS(now - inodeEntry.modtime.tv_sec), 0ULL, NMONTHS);
            stats.mtimeCountsByMonthsAgo[month]++;
            globalStats.mtimeCountsByMonthsAgo[month]++;

            stats.mtimeSizesByMonthsAgo[month] += inodeEntry.size;
            globalStats.mtimeSizesByMonthsAgo[month] += inodeEntry.size;
        }

        if (inodeEntry.atime.tv_sec > now) {
            stats.atimeCountsByMonthsAgo[NMONTHS + 1]++;
            globalStats.atimeCountsByMonthsAgo[NMONTHS + 1]++;

            stats.atimeSizesByMonthsAgo[NMONTHS + 1] += inodeEntry.size;
            globalStats.atimeSizesByMonthsAgo[NMONTHS + 1] += inodeEntry.size;
        } else {
            uint64_t month = std::clamp(
                FUZZY_MONTHS(now - inodeEntry.atime.tv_sec), 0ULL, NMONTHS);
            stats.atimeCountsByMonthsAgo[month]++;
            globalStats.atimeCountsByMonthsAgo[month]++;

            stats.atimeSizesByMonthsAgo[month] += inodeEntry.size;
            globalStats.atimeSizesByMonthsAgo[month] += inodeEntry.size;
        }

        if ((globalStats.count % 10000) == 0) {
            fprintf(stderr, "INFO: count=%zu\n", globalStats.count);
        }

        // fprintf(stderr, "info: %zu, %zu, %zu\n", inodeEntry.blocks * 512,
        //         inodeEntry.size, *((uint64_t *)val.mv_data));
    }

    // fprintf(stdout, "count=%zu, diskSize=%zu, apparentSize=%zu\n", count,
    //         diskSize, apparentSize);
    printHeader();
    printStats(globalStats, "total");

    for (auto &[k, v] : userStats) {
        if (k == "") {
            continue;
        }

        printStats(v, k.c_str());
    }

    return EXIT_SUCCESS;
}
