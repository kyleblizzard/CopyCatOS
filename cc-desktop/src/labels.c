// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// labels.c — File color label persistence via Linux extended attributes
//
// Extended attributes (xattrs) let you attach arbitrary key-value metadata
// to files without modifying the file contents. On Linux, user-defined
// xattrs live in the "user." namespace and are supported by ext4, btrfs,
// XFS, and most modern filesystems.
//
// We store one byte per file: the ASCII character '1'-'7' for the label
// color, or nothing (xattr absent) for no label.

#include "labels.h"

#include <sys/xattr.h>   // getxattr, setxattr, removexattr
#include <stdio.h>       // fprintf
#include <errno.h>       // errno

// Name of the xattr key we use to store the label.
// Using our own key (not com.apple.FinderInfo) keeps things simple.
#define LABEL_XATTR_KEY  "user.copycatos.label"

// ── Color table ─────────────────────────────────────────────────────

// RGB values measured from real Snow Leopard label colors.
// Index 0 is a sentinel (no label) and is never drawn.
// Indices 1-7 are Red, Orange, Yellow, Green, Blue, Purple, Grey.
const LabelColor label_colors[LABEL_COUNT] = {
    { 0.00f, 0.00f, 0.00f },   // 0: none (unused sentinel)
    { 1.00f, 0.42f, 0.42f },   // 1: Red     #FF6C6C
    { 1.00f, 0.70f, 0.28f },   // 2: Orange  #FFB347
    { 1.00f, 1.00f, 0.42f },   // 3: Yellow  #FFFF6C
    { 0.42f, 1.00f, 0.42f },   // 4: Green   #6CFF6C
    { 0.42f, 0.42f, 1.00f },   // 5: Blue    #6C6CFF
    { 0.81f, 0.42f, 1.00f },   // 6: Purple  #CF6CFF
    { 0.69f, 0.69f, 0.69f },   // 7: Grey    #B0B0B0
};

// Names match Finder's label menu order exactly.
const char *label_names[LABEL_COUNT] = {
    "None",     // 0
    "Red",      // 1
    "Orange",   // 2
    "Yellow",   // 3
    "Green",    // 4
    "Blue",     // 5
    "Purple",   // 6
    "Grey",     // 7
};

// ── label_get ────────────────────────────────────────────────────────

int label_get(const char *path)
{
    // Read the xattr into a small buffer.
    // We only need 1 byte ('0'-'7'), but request 4 to detect corrupt values.
    char buf[4] = {0};

    // getxattr() returns the number of bytes read, or -1 on error.
    // Common errors: ENODATA (no xattr set), ENOTSUP (filesystem limitation).
    // Both are fine — they just mean "no label".
    ssize_t n = getxattr(path, LABEL_XATTR_KEY, buf, sizeof(buf));
    if (n <= 0) {
        return LABEL_NONE;  // No label set, or read failed
    }

    // Parse the ASCII digit
    int label = (int)(buf[0] - '0');
    if (label < LABEL_NONE || label >= LABEL_COUNT) {
        return LABEL_NONE;  // Corrupt or out-of-range value
    }

    return label;
}

// ── label_set ────────────────────────────────────────────────────────

void label_set(const char *path, int label)
{
    if (label <= LABEL_NONE || label >= LABEL_COUNT) {
        // Remove the xattr entirely to clear the label.
        // removexattr() returns -1 if the xattr didn't exist — that's fine.
        removexattr(path, LABEL_XATTR_KEY);
        return;
    }

    // Store the label as a single ASCII digit ('1'-'7').
    // Using text instead of binary makes the xattr human-readable with
    // `getfattr -n user.copycatos.label <file>`.
    char buf[2];
    buf[0] = (char)('0' + label);
    buf[1] = '\0';

    // setxattr() flags:
    //   0 = create or replace (either is fine for us)
    int r = setxattr(path, LABEL_XATTR_KEY, buf, 1, 0);
    if (r != 0) {
        // Log but don't crash — the file might be on a read-only fs
        // or one that doesn't support xattrs. The label just won't persist.
        fprintf(stderr, "[labels] setxattr failed for '%s' (errno=%d)\n",
                path, errno);
    }
}
