/*
 * vm.h
 */

#include <usloss.h>
/*
 * All processes use the same tag.
 */
#define TAG 0

#define NOT_ON_DISK         -1
#define PAGE_NOT_IN_FRAME   -1
#define SECTORS_IN_FRAME     8
#define PAGER_PAGE           0

#define TRACK_START          0
#define NUM_TRACKS           16

/* Mailbox status */
#define MAILBOX_RELEASED -3


/* VM started macro */
#define VM_STARTED        1
#define VM_STOPPED        0

/* Different states for a frame */

// For reference number
#define UNREFERENCED           0
#define REFERENCED             USLOSS_MMU_REF
#define DIRTY                  USLOSS_MMU_DIRTY
#define PAGER_OWNED            3


// For frame in use or not in use
#define NOT_USED        0
#define USED            1

/* You'll probably want more states */

/* typedefs */
typedef struct PageTableEntry *PageTableEntryPtr;
typedef struct FrameTableEntry *FrameTableEntryPtr;
typedef struct FaultMsg *FaultMsgPtr;

/*
 * Page table entry.
 */
typedef struct PageTableEntry {
    int  state;      // See above.
    int  frame;      // Frame that stores the page (if any). -1 if none.
    int  diskBlock;  // Disk block that stores the page (if any). -1 if none.
    // Add more stuff here
} PageTableEntry;

/*
 * Per-process information.
 */
typedef struct Process {
    int  numPages;   // Size of the page table.
    int  pagesInUse;
    PageTableEntryPtr PageTable; // The page table for the process.
} Process;

/*
 * Information about page faults. This message is sent by the faulting
 * process to the pager to request that the fault be handled.
 */
typedef struct FaultMsg {
    int  pid;        // Process with the problem.
    void *addr;      // Address that caused the fault.
    int  replyMbox;  // Mailbox to send reply.
    // Add more stuff here.
} FaultMsg;

/*
 *  Frames structure that keeps track of the frames created
 */
typedef struct FrameTableEntry {
    int used;
    int state;
    int pid;
    int pageNum;
} FrameTableEntry;


#define CheckMode() assert(USLOSS_PsrGet() & USLOSS_PSR_CURRENT_MODE)
