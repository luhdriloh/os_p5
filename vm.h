#include <usloss.h>
/*
 * All processes use the same tag.
 */
#define TAG 0

#define NOT_ON_DISK         -1
#define PAGE_NOT_IN_FRAME   -1
#define SECTORS_IN_FRAME     8
#define PAGER_PAGE           0
#define NO_PID              -1

#define TRACK_START          0

/* Mailbox status */
#define MAILBOX_RELEASED     -3


/* VM started macro */
#define VM_STARTED        1
#define VM_STOPPED        0

/* Different states for a frame */

// For reference number
#define UNREFERENCED           0
#define REFERENCED             USLOSS_MMU_REF
#define DIRTY                  USLOSS_MMU_DIRTY
#define CLEAN                  0
#define PAGER_OWNED            1
#define NOT_PAGER_OWNED        0


// For frame in use or not in use
#define NOT_USED        0
#define USED            1

/* You'll probably want more states */

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
    int pagesInUse;
    PageTableEntry *PageTable; // The page table for the process.
} Process;

/*
 * Information about page faults. This message is sent by the faulting
 * process to the pager to request that the fault be handled.
 */
typedef struct FaultMsg {
    int  pid;        // Process with the problem.
    int  offset;      // Address that caused the fault.
    int  replyMbox;  // Mailbox to send reply.
    // Add more stuff here.
} FaultMsg;

/*
 *  Frames structure that keeps track of the frames created
 */
typedef struct FrameTableEntry {
    int pagerOwned;
    int used;
    int state;
    int dirty;
    int pid;
    int pageNum;
} FrameTableEntry;


/* typedefs */
typedef struct PageTableEntry *PageTableEntryPtr;
typedef struct FrameTableEntry *FrameTableEntryPtr;
typedef struct FaultMsg *FaultMsgPtr;


#define CheckMode() assert(USLOSS_PsrGet() & USLOSS_PSR_CURRENT_MODE)
