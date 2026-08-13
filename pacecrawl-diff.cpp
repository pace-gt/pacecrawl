#include "gtl/phmap.hpp"
#include "lmdb-misc.h"
#include "pacecrawl-table.h"
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <lmdb.h>
#include <stdio.h>
#include <sys/mman.h>
#include <sys/stat.h>

int main(int argc, char *argv[]) {
    int err;

    DBISet srcDBs = {};
    if (openDB(argv[1], &srcDBs)) {
        return EXIT_FAILURE;
    }

    std::string datpath = std::string(argv[1]) + ".dat";
    int datfdSrc = open(datpath.c_str(), O_RDONLY);
    if (datfdSrc < 0) {
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

    void *datptrSrc =
        mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, datfdSrc, 0);
    if (!datptrSrc) {
        fprintf(stderr, "ERROR failed to map dat file %s: %s\n",
                datpath.c_str(), strerror(errno));
        return 1;
    }

    DBISet dstDBs = {};
    if (openDB(argv[2], &dstDBs)) {
        return EXIT_FAILURE;
    }

    datpath = std::string(argv[2]) + ".dat";
    int datfdDst = open(datpath.c_str(), O_RDONLY);
    if (datfdDst < 0) {
        fprintf(stderr, "ERROR: failed to open dat file %s: %s\n",
                datpath.c_str(), strerror(errno));
        return 1;
    }

    if (stat(datpath.c_str(), &st) < 0) {
        fprintf(stderr, "ERROR: failed to stat dat file %s: %s\n",
                datpath.c_str(), strerror(errno));
        return 1;
    }

    void *datptrDst =
        mmap(NULL, st.st_size, PROT_READ, MAP_PRIVATE, datfdDst, 0);
    if (!datptrDst) {
        fprintf(stderr, "ERROR failed to map dat file %s: %s\n",
                datpath.c_str(), strerror(errno));
        return 1;
    }

    MDB_txn *srcTxn;
    if ((err = mdb_txn_begin(srcDBs.env, NULL, MDB_RDONLY, &srcTxn))) {
        fprintf(stderr,
                "ERROR: failed to begin mdb data read: "
                "error %s",
                mdb_strerror(err));
        return EXIT_FAILURE;
    }

    MDB_txn *dstTxn;
    if ((err = mdb_txn_begin(dstDBs.env, NULL, MDB_RDONLY, &dstTxn))) {
        fprintf(stderr,
                "ERROR: failed to begin mdb data read: "
                "error %s",
                mdb_strerror(err));
        return EXIT_FAILURE;
    }

    Cursors srcCursors = {};
    Cursors dstCursors = {};

    if ((err = openCursors(&srcDBs, srcTxn, &srcCursors))) {
        return EXIT_FAILURE;
    }

    if ((err = openCursors(&dstDBs, dstTxn, &dstCursors))) {
        return EXIT_FAILURE;
    }

    MDB_val key = {};
    MDB_val data = {};

    const char *srcPath = "";
    uint64_t prevSrcInode = 0;
    uint64_t srcInode = 0;
    uint64_t prevDstInode = 0;
    const char *dstPath = "";
    uint64_t dstInode = 0;

    gtl::flat_hash_map<uint64_t, uint64_t> srcToDstInodeMap;
    gtl::flat_hash_map<uint64_t, uint64_t> dstToSrcInodeMap;

    size_t counter = 0;
    while (true) {
        if (counter && (counter % 1000) == 0) {
            fprintf(stderr, "INFO: checked %zu\n", counter);
        }
        counter++;

        int cmp = 0;
        if (srcPath && !dstPath) {
            cmp = -1;
        } else if (!srcPath && dstPath) {
            cmp = 1;
        } else {
            cmp = strcmp(srcPath, dstPath);
        }

        if (cmp <= 0) {
            if ((err = mdb_cursor_get(srcCursors.pathsCursor, &key, &data,
                                      MDB_NEXT))) {
                if (err != MDB_NOTFOUND) {
                    fprintf(stderr,
                            "ERROR: failed to read source pathsDB: %s\n",
                            mdb_strerror(err));
                    return EXIT_FAILURE;
                } else {
                    srcPath = NULL;
                }
            } else {
                prevSrcInode = srcInode;
                srcPath = (const char *)key.mv_data;
                srcInode = *((uint64_t *)data.mv_data);
            }
        }

        if (cmp >= 0) {
            if ((err = mdb_cursor_get(dstCursors.pathsCursor, &key, &data,
                                      MDB_NEXT))) {
                if (err != MDB_NOTFOUND) {
                    fprintf(stderr,
                            "ERROR: failed to read destination pathsDB: %s\n",
                            mdb_strerror(err));
                    return EXIT_FAILURE;
                } else {
                    dstPath = NULL;
                }
            } else {
                prevDstInode = dstInode;
                dstPath = (const char *)key.mv_data;
                dstInode = *((uint64_t *)data.mv_data);
            }
        }

        if (srcPath == NULL && dstPath != NULL) {
            fprintf(stdout, "DELETED|%s\n", dstPath);
        } else if (srcPath != NULL && dstPath == NULL) {
            fprintf(stdout, "MISSING|%s\n", srcPath);
        } else if (srcPath == NULL && dstPath == NULL) {
            break;
        } else {
            cmp = strcmp(srcPath, dstPath);
        }

        if (cmp < 0) {
            fprintf(stdout, "MISSING|%s\n", srcPath);
        } else if (cmp > 0) {
            fprintf(stdout, "DELETED|%s\n", dstPath);
        } else {
            MDB_val key = {};
            MDB_val data = {};

            key.mv_data = &srcInode;
            key.mv_size = sizeof(srcInode);
            InodeTableEntry srcInodeInfo;
            InodeTableEntry dstInodeInfo;

            if ((err = mdb_cursor_get(srcCursors.inodeInfoCursor, &key, &data,
                                      MDB_SET))) {
                fprintf(
                    stderr,
                    "ERROR: failed to find src inode entry for inode %zu\n ",
                    srcInode);
                continue;
            }
            srcInodeInfo = *((InodeTableEntry *)((uint8_t *)datptrSrc +
                                                 *((uint64_t *)data.mv_data)));

            key.mv_data = &dstInode;
            key.mv_size = sizeof(dstInode);
            if ((err = mdb_cursor_get(dstCursors.inodeInfoCursor, &key, &data,
                                      MDB_SET))) {
                fprintf(
                    stderr,
                    "ERROR: failed to find dst inode entry for inode %zu\n ",
                    dstInode);
                continue;
            }
            dstInodeInfo = *((InodeTableEntry *)((uint8_t *)datptrDst +
                                                 *((uint64_t *)data.mv_data)));

            FileTableEntry *srcFileInfo;
            FileTableEntry *dstFileInfo;

            key.mv_data = &srcInode;
            key.mv_size = sizeof(srcInode);
            if ((err = mdb_cursor_get(srcCursors.finfoCursor, &key, &data,
                                      MDB_SET))) {
                fprintf(
                    stderr,
                    "ERROR: failed to find src fileInfo entry for inode %zu\n ",
                    srcInode);
                continue;
            }
            srcFileInfo = (FileTableEntry *)((uint8_t *)datptrSrc +
                                             *(uint64_t *)data.mv_data);

            key.mv_data = &dstInode;
            key.mv_size = sizeof(dstInode);
            if ((err = mdb_cursor_get(dstCursors.finfoCursor, &key, &data,
                                      MDB_SET))) {
                fprintf(
                    stderr,
                    "ERROR: failed to find dst fileInfo entry for inode %zu\n ",
                    srcInode);
                continue;
            }
            dstFileInfo = (FileTableEntry *)((uint8_t *)datptrDst +
                                             *(uint64_t *)data.mv_data);

            bool modeDifferent = srcInodeInfo.mode != dstInodeInfo.mode;
            bool aclDifferent =
                memcmp(srcInodeInfo.aclHash, dstInodeInfo.aclHash, 32);
            bool uidDifferent = srcInodeInfo.uid != dstInodeInfo.uid;
            bool gidDifferent = srcInodeInfo.gid != dstInodeInfo.gid;
            bool hashDifferent =
                memcmp(srcInodeInfo.hash, dstInodeInfo.hash, 16);
            bool modtimeDifferent =
                memcmp(&srcInodeInfo.modtime.tv_sec,
                       &dstInodeInfo.modtime.tv_sec, sizeof(long));
            bool btimeDifferent =
                false; // memcmp(&srcInodeInfo.btime, &dstInodeInfo.btime,
                       // sizeof(timespec));
            bool sizeDifferent = S_ISREG(srcInodeInfo.mode) &&
                                 (srcInodeInfo.size != dstInodeInfo.size);

            auto [mapDstInode, seenSrcInode] =
                srcToDstInodeMap.insert(std::make_pair(srcInode, dstInode));
            auto [mapSrcInode, seenDstInode] =
                dstToSrcInodeMap.insert(std::make_pair(dstInode, srcInode));

            bool dstInodeWrong =
                (S_ISREG(srcInodeInfo.mode) || S_ISLNK(srcInodeInfo.mode)) &&
                ((seenSrcInode != seenDstInode) ||
                 (seenSrcInode && seenDstInode &&
                  (mapDstInode->second != dstInode ||
                   mapSrcInode->second != srcInode)));

            bool linksDifferent =
                S_ISLNK(srcInodeInfo.mode)
                    ? strcmp(srcFileInfo->pathAndLinkrefAndAbs +
                                 srcFileInfo->pathLen + 1,
                             dstFileInfo->pathAndLinkrefAndAbs +
                                 srcFileInfo->pathLen + 1)
                    : false;

#define STR(x) (x ? (#x) : "")

            if (modeDifferent || aclDifferent || uidDifferent || gidDifferent ||
                hashDifferent || modtimeDifferent || btimeDifferent ||
                sizeDifferent || dstInodeWrong) {
                // if (1) {
                // fprintf(stdout, "mode: %o vs %o, %d\n", srcInodeInfo.mode,
                //         dstInodeInfo.mode,
                //         srcInodeInfo.mode == dstInodeInfo.mode);
                // fprintf(stdout, "modtime: %zu vs %zu\n",
                //         srcInodeInfo.modtime.tv_sec,
                //         dstInodeInfo.modtime.tv_sec);
                // fprintf(stdout, "btime: %zu vs %zu\n",
                //         srcInodeInfo.btime.tv_sec, dstInodeInfo.btime.tv_sec);

                fprintf(stdout, "DIFF|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s\n", srcPath,
                        STR(modeDifferent), STR(aclDifferent),
                        STR(uidDifferent), STR(gidDifferent),
                        STR(hashDifferent), STR(modtimeDifferent),
                        STR(btimeDifferent), STR(sizeDifferent),
                        STR(dstInodeWrong), STR(linksDifferent));
            }
#undef STR
        }
    }

    return 0;
}
