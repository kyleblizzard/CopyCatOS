// Copyright (c) 2026 Kyle Blizzard. All Rights Reserved.
// This code is publicly visible for portfolio purposes only.
// Unauthorized copying, forking, or distribution of this file,
// via any medium, is strictly prohibited.

// labels.h — File color labels (like Snow Leopard Finder labels)
//
// Snow Leopard lets you assign one of 7 colors to any file or folder.
// The color shows as a background tint behind the filename label on
// the desktop and in all Finder views. It's a visual organizing tool:
// red for urgent, blue for work, green for done, etc.
//
// Storage: we persist labels as an extended attribute ("xattr") on the
// file itself. This is exactly how the real Finder stores label data
// (in the com.apple.FinderInfo xattr on HFS+). We use our own
// "user.copycatos.label" key because Linux filesystems use the
// "user." namespace for application-defined xattrs.
//
// Format: a single ASCII digit '0'-'7', where 0 means no label.
//
// If the filesystem doesn't support xattrs (e.g., FAT32), get/set
// silently do nothing. Icons just show no label.

#ifndef CC_LABELS_H
#define CC_LABELS_H

// Label index values — 0 means no label, 1-7 are the seven colors.
// These match the Snow Leopard label ordering (Red, Orange, Yellow,
// Green, Blue, Purple, Grey — same order as in Finder's Label menu).
#define LABEL_NONE    0
#define LABEL_RED     1
#define LABEL_ORANGE  2
#define LABEL_YELLOW  3
#define LABEL_GREEN   4
#define LABEL_BLUE    5
#define LABEL_PURPLE  6
#define LABEL_GREY    7
#define LABEL_COUNT   8  // Total number of label slots (0=none, 1-7=colors)

// RGB color for each label index.
// Alpha is applied at draw time (the caller decides transparency).
// Values are 0.0–1.0 float, converted from Snow Leopard's actual label
// colors measured from real screenshots.
typedef struct {
    float r, g, b;
} LabelColor;

// Color table indexed by label index (0=unused/black, 1=red, …, 7=grey).
// Only indices 1-7 are drawn; index 0 means "no label."
extern const LabelColor label_colors[LABEL_COUNT];

// Human-readable name for each label (used in the Label submenu).
extern const char *label_names[LABEL_COUNT];

// Read the color label from a file's extended attributes.
// Returns 0 (LABEL_NONE) if the file has no label or the read fails.
// path must be an absolute filesystem path.
int label_get(const char *path);

// Write a color label to a file's extended attributes.
// Pass LABEL_NONE (0) to remove the label entirely.
// Silently ignores errors (e.g., filesystem doesn't support xattrs).
void label_set(const char *path, int label);

#endif // CC_LABELS_H
