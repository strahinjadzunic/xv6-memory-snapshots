#ifndef KERNEL_STAT_H
#define KERNEL_STAT_H

#include "types.h"

#define T_DIR  1   // Directory
#define T_FILE 2   // File
#define T_DEV  3   // Device

struct stat {
	short type;  // Type of file
	int dev;     // File system's disk device
	uint ino;    // Inode number
	short nlink; // Number of links to file
	uint size;   // Size of file in bytes
};

// Informacije o jednom snimku - popunjava ih snap_status().
// Ime strukture i imena polja (size, num_pages) moraju biti tacno ovakva -
// drugacije nazivanje se po specifikaciji boduje 0
struct snap_info {
	int size;        // velicina snimka u bajtovima
	int num_pages;   // broj alociranih fizickih stranica
};

#endif // KERNEL_STAT_H
