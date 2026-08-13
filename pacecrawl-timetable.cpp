#include "lmdb-misc.h"
#include "pacecrawl-table.h"
#include <algorithm>
#include <array>
#include <ctime>
#include <errno.h>
#include <fcntl.h>
#include <lmdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <string>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unordered_map>

struct HistInfo {
    size_t count = 0;
    size_t size = 0;
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

    char *datpath = (char *)calloc(1, strlen(argv[1]) + strlen(".dat") + 1);
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
    if (datptr == (void *)-1) {
        fprintf(stderr, "ERROR failed to map dat file %s: %s\n", datpath,
                strerror(errno));
        return 1;
    }

    fprintf(stdout, "project|metric|timetype|0|1|2|3|4|5|6|7|8|9|10|11|12\n");

    MDB_val key = {};
    MDB_val val = {};

    int64_t now = time(NULL);

    std::unordered_map<std::string, std::array<std::array<HistInfo, 13>, 4>>
        infoMap;

    int64_t count = 0;
    while ((err = mdb_cursor_get(cursors.inodeInfoCursor, &key, &val,
                                 MDB_NEXT)) == 0) {
        InodeTableEntry inodeEntry = *((
            InodeTableEntry *)((uint8_t *)datptr + *((uint64_t *)val.mv_data)));

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
            (FileTableEntry *)((uint8_t *)datptr + *((uint64_t *)val.mv_data));

        const char *path = strdup(entry->pathAndLinkrefAndAbs);
        const char *pathEnd = strchr(path, '/');

        if (!pathEnd) {
            free((char *)path);
            continue;
        }
        pathEnd = strchr(pathEnd + 1, '/');

        if (!pathEnd) {
            free((char *)path);
            continue;
        }

        *((char *)pathEnd) = 0;

        const int64_t MONTHS = (60 * 60 * 24 * 30);

        if (!S_ISDIR(inodeEntry.mode)) {
            // fprintf(stdout, "%s|%zu|%zu|%zu|%zu\n", path,
            //         (now - inodeEntry.btime.tv_sec) / MONTHS,
            //         (now - inodeEntry.modtime.tv_sec) / MONTHS,
            //         (now - inodeEntry.atime.tv_sec) / MONTHS,
            //         (now - inodeEntry.chgtime.tv_sec) / MONTHS);
            auto &btimeInfo =
                infoMap[path][0]
                       [std::clamp((now - inodeEntry.btime.tv_sec) / MONTHS,
                                   (int64_t)0, (int64_t)12)];
            btimeInfo.count++;
            btimeInfo.size += inodeEntry.blocks * 512;

            auto &mtimeInfo =
                infoMap[path][1]
                       [std::clamp((now - inodeEntry.modtime.tv_sec) / MONTHS,
                                   (int64_t)0, (int64_t)12)];
            mtimeInfo.count++;
            mtimeInfo.size += inodeEntry.blocks * 512;

            auto &atimeInfo =
                infoMap[path][2]
                       [std::clamp((now - inodeEntry.atime.tv_sec) / MONTHS,
                                   (int64_t)0, (int64_t)12)];
            // fprintf(stderr, "path is %s\n", entry->pathAndLinkrefAndAbs);
            // fprintf(stderr, "atime unix is %zu\n", inodeEntry.atime.tv_sec);
            // fprintf(stderr, "atime is %zu\n",
            //         /*std::clamp(*/(now - inodeEntry.atime.tv_sec) / MONTHS/*,
            //                    (int64_t)0, (int64_t)12)*/);
            atimeInfo.count++;
            atimeInfo.size += inodeEntry.blocks * 512;

            auto &ctimeInfo =
                infoMap[path][3]
                       [std::clamp((now - inodeEntry.chgtime.tv_sec) / MONTHS,
                                   (int64_t)0, (int64_t)12)];
            ctimeInfo.count++;
            ctimeInfo.size += inodeEntry.blocks * 512;
        }
        free((char *)path);
    }

    for (const auto &[project, pinfo] : infoMap) {
        const char *metrics[] = {"size", "count"};
        const char *timetypes[] = {"btime", "mtime", "atime", "ctime"};

        for (size_t ttypeIdx = 0; ttypeIdx < 4; ttypeIdx++) {
            fprintf(stdout,
                    "%s|%s|%s|%zu|%zu|%zu|%zu|%zu|%zu|%zu|%zu|%zu|%zu|%zu|%zu|%"
                    "zu\n",
                    project.c_str(), "size", timetypes[ttypeIdx],
                    pinfo[ttypeIdx][0].size, pinfo[ttypeIdx][1].size,
                    pinfo[ttypeIdx][2].size, pinfo[ttypeIdx][3].size,
                    pinfo[ttypeIdx][4].size, pinfo[ttypeIdx][5].size,
                    pinfo[ttypeIdx][6].size, pinfo[ttypeIdx][7].size,
                    pinfo[ttypeIdx][8].size, pinfo[ttypeIdx][9].size,
                    pinfo[ttypeIdx][10].size, pinfo[ttypeIdx][11].size,
                    pinfo[ttypeIdx][12].size);
            fprintf(stdout,
                    "%s|%s|%s|%zu|%zu|%zu|%zu|%zu|%zu|%zu|%zu|%zu|%zu|%zu|%zu|%"
                    "zu\n",
                    project.c_str(), "count", timetypes[ttypeIdx],
                    pinfo[ttypeIdx][0].count, pinfo[ttypeIdx][1].count,
                    pinfo[ttypeIdx][2].count, pinfo[ttypeIdx][3].count,
                    pinfo[ttypeIdx][4].count, pinfo[ttypeIdx][5].count,
                    pinfo[ttypeIdx][6].count, pinfo[ttypeIdx][7].count,
                    pinfo[ttypeIdx][8].count, pinfo[ttypeIdx][9].count,
                    pinfo[ttypeIdx][10].count, pinfo[ttypeIdx][11].count,
                    pinfo[ttypeIdx][12].count);
        }
    }

    return EXIT_SUCCESS;
}
