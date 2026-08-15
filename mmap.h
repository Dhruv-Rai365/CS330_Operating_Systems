#ifndef XV6_MMAP_H
#define XV6_MMAP_H

// Simplified protection flags for anonymous mappings.
// x86 has no separate "read" PTE bit: a present user page is readable.
// PTE_W additionally makes it writable.
#define PROT_READ   0x1
#define PROT_WRITE  0x2

// Maximum number of simultaneously active mmap regions per process.
#define MAX_VMAS 16

#endif
