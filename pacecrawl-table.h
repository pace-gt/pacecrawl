#pragma once

#include <grp.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <sys/types.h>

typedef struct __attribute__((packed)) {
    uint16_t mode;
    uint8_t __modepadding[6];

    uint8_t aclHash[32]; // sha256sum of the acl

    uint32_t uid;
    uint32_t gid;

    uint8_t hash[16]; // md5sum of the file itself

    struct timespec modtime;
    struct timespec chgtime;
    struct timespec btime;
    struct timespec atime;

    uint64_t size;
    uint64_t blocks;

    uint64_t inode; // maybe this is redundant
    uint64_t nhlink;
} InodeTableEntry;

typedef struct __attribute__((packed)) {
    uint64_t inode;
} InodeTableKey;

typedef struct __attribute__((packed)) {
    uint8_t aclHash[32]; // sha256sum of the acl
} ACLTableKey;

typedef const char *ACLTableEntry;

typedef struct __attribute__((packed)) {
    uint64_t inode;
    uint32_t linkrefLen;
    uint32_t absLinkrefLen;
    uint32_t pathLen;
    uint32_t linkrefValid;
    char pathAndLinkrefAndAbs[]; // Not ideal, but the format of this is
                                 // path\0linkref\0abslinkref\0 where abslinkref
                                 // starts at byte pathLen + 1 + linkrefLen + 1
} FileTableEntry;

