#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <linux/limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

int main(int argc, char *argv[]) {
    if (argc < 4) {
        return EXIT_FAILURE;
    }

    char *srcRoot = argv[3];
    if (!strlen(srcRoot)) {
        fprintf(stderr, "FATAL: please enter valid srcRoot\n");
        exit(EXIT_FAILURE);
    }
    if (srcRoot[0] != '/') {
        fprintf(stderr, "FATAL: please enter absolute path for srcRoot\n");
        exit(EXIT_FAILURE);
    }
    if (strlen(srcRoot) > 1 && srcRoot[strlen(srcRoot) - 1] == '/') {
        srcRoot[strlen(srcRoot) - 1] = '\0';
    }

    char *dstRoot = argv[2];
    if (!strlen(dstRoot)) {
        fprintf(stderr, "FATAL: please enter valid dstRoot\n");
        exit(EXIT_FAILURE);
    }
    if (dstRoot[0] != '/') {
        fprintf(stderr, "FATAL: please enter absolute path for dstRoot\n");
        exit(EXIT_FAILURE);
    }
    if (strlen(dstRoot) > 1 && dstRoot[strlen(dstRoot) - 1] == '/') {
        dstRoot[strlen(dstRoot) - 1] = '\0';
    }

    int dirfd = open(argv[2], O_DIRECTORY);
    if (dirfd < 0) {
        fprintf(stderr, "FATAL: failed to open directory (%s), %s", argv[2],
                strerror(errno));
        exit(EXIT_FAILURE);
    }

    sqlite3 *conn;
    if (sqlite3_open_v2(argv[1], &conn,
                        SQLITE_OPEN_READONLY | SQLITE_OPEN_NOMUTEX,
                        NULL) != SQLITE_OK) {
        fprintf(stderr, "FATAL: failed to open sqlite database (%s): %s\n",
                argv[1], sqlite3_errmsg(conn));
        exit(EXIT_FAILURE);
    }

    sqlite3_stmt *stmt;
    const char *sql =
        "SELECT path,linkref,linkatroot FROM files WHERE type = 'SYMLINK';";
    if (sqlite3_prepare_v2(conn, sql, -1, &stmt, NULL) != SQLITE_OK) {
        fprintf(stderr, "FATAL: failed to compile sql statement: %s\n",
                sqlite3_errmsg(conn));
        exit(EXIT_FAILURE);
    }

    chdir(dstRoot);

    int err;
    int count = 0;
#pragma omp parallel
#pragma omp single
    while ((err = sqlite3_step(stmt)) == SQLITE_ROW) {
        char *colPath = (char *)sqlite3_column_text(stmt, 0);
        char *colLinkref = (char *)sqlite3_column_text(stmt, 1);
        char *colLinkatroot = (char *)sqlite3_column_text(stmt, 2);

        char *path;
        char *linkref;
        char *linkatroot;

#define NULLABLE_STRDUP(x, y)                                                  \
    if (y) {                                                                   \
        x = strdup(y);                                                         \
    } else {                                                                   \
        x = strdup("");                                                        \
    }

        NULLABLE_STRDUP(path, colPath);
        NULLABLE_STRDUP(linkref, colLinkref);
        NULLABLE_STRDUP(linkatroot, colLinkatroot);

        count++;
        if ((count % 10000) == 0) {
            fprintf(stderr, "INFO: applied %d\n", count);
        }

#pragma omp task
        {
            char *newLink;
            if (linkref[0] == '/' &&
                strncmp(linkref, srcRoot, strlen(srcRoot)) == 0) {
                newLink = calloc(1, strlen(dstRoot) + strlen(linkref) + 2);
                strcpy(newLink, dstRoot);
                strcat(newLink, "/");
                strcat(newLink, linkref + strlen(srcRoot));
            } else {
                newLink = strdup(linkref);
            }

            if (faccessat(dirfd, path, F_OK, AT_SYMLINK_NOFOLLOW) == 0) {
                remove(path);
            }

            fprintf(stderr, "INFO: performing symlink %s -> %s\n", newLink,
                    path);
            if (symlink(newLink, path) < 0) {
                fprintf(stderr,
                        "ERROR: failed to apply symlink %s -> %s (%s)\n", path,
                        newLink, strerror(errno));
            }

            free(path);
            free(linkref);
            free(linkatroot);
            free(newLink);
        }
    }
}
