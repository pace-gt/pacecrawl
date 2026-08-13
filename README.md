# pacecrawl

Pacecrawl and its companion tools crawl a file tree and encode file/directory
metadata into an lmdb database for later processing (statistics, diffing,
permission application, etc.).

## pacecrawl
```
pacecrawl [-j NTHREADS] -o <out.db> <crawldir>
```

Pacecrawl is aimed at adding features to the existing pacedu tool. Pacecrawl walks a provided file tree and encodes file/directory metadata into a lmdb database for later processing with companion tools.

To run pacecrawl, specify an output database with `-o` and a directory to crawl as the last argument. Note that pacecrawl will also create a corresponding `<out.db>.dat` which stores some of the data to improve database performance. Change the number of threads used to crawl with `-j`.

### pacecrawl-diff
```
pacecrawl-diff <src.db> <dst.db>
```
Compares src.db and dst.db much like the unix `comm` command.
* Missing files (files that appear in src.db but not dst.db) are printed to stdout in the format `MISSING|filename`
* Deleted files (files that appear in dst.db but not src.db) are printed to stdout in the format `DELETED|filename`
* Files whose metadata differ are printed to stdout in the format
```c
fprintf(stdout, "DIFF|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s|%s\n", srcPath,
        STR(modeDifferent), STR(aclDifferent),
        STR(uidDifferent), STR(gidDifferent),
        STR(hashDifferent), STR(modtimeDifferent),
        STR(btimeDifferent), STR(sizeDifferent),
        STR(dstInodeWrong), STR(linksDifferent));
```
where
```c
#define STR(x) (x ? (#x) : "")
```

In other words, a file whose uid and modtime differs will be printed to stdout as so
```
DIFF|||uidDifferent|||modtimeDifferent|||||
```

Note that btime and ctime differences are never reported as these cannot be changed via the generic vfs interfaces in userspace on linux. Additionally, note that hardlink differences are reported through dstInodeWrong.

### pacecrawl-allpaths
```
pacecrawl-allpaths <src.db>
```
Prints all the paths in the database to stdout.

### pacecrawl-applyperms
```
cat files | pacecrawl-applyperms <src.db> <dstdir>
```

Takes in a list of newline separated files from stdin and applies the permissions found in `src.db` to their equivalents in `dstdir`.

### pacecrawl-dirsummaries
```
pacecrawl-dirsummaries <src.db>
```
Lists the size and file counts (recursive) of every directory.

### pacecrawl-findinode
```
pacecrawl-findinode <src.db> <inode>
```
Searches the database for all occurences of an inode.

### pacecrawl-findpath
```
pacecrawl-findpath <src.db> <path>
```
Searches the database for a path and prints information relating to it.

### pacecrawl-spacediff
```
pacecrawl-spacediff <src.db>
```
Print the difference between logical and physical size of every file.

### pacecrawl-stats
```
pacecrawl-stats <src.db>
```
Prints project statistics, including file count, logical size, and physical size of the input project.

### pacecrawl-stats2
```
pacecrawl-stats2 <src.db>
```
Prints project statistics, including file count, logical size, physical size, and various access/modification/change time histograms of the input project as well as directories 2 levels deep (subject to change in the future).

### pacecrawl-timetable
```
pacecrawl-timetable <src.db>
```
2 levels deep statistics on file time distribution by the month, somewhat like stats2.

## Installation
This project uses git submodules, so clone with `--recursive` (or run
`git submodule update --init --recursive` after cloning).

```bash
mkdir build && cd build
cmake .. -DCMAKE_BUILD_TYPE=RelWithDebInfo -DCMAKE_INSTALL_PREFIX=<install-dir>
make
make install
```

The dependencies of this project are as follows
* libacl
* libcrypto (openssl)
* pkgconfig

## General Usage Pattern
To crawl a source and destination directory and compare them, the process would
be as follows.
```bash
pacecrawl -o src.db -j128 src
pacecrawl -o dst.db -j128 dst
pacecrawl-diff src.db dst.db > diff.txt
```
This puts the resulting difference between the two directories into diff.txt.

## License
This project is licensed under the GNU General Public License v3.0. See
[LICENSE](LICENSE) for the full text.
