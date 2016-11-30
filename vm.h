/*
 * vm.h
 */


/*
 * All processes use the same tag.
 */
#define TAG 0

/*
 * Different states for a page.
 */
#define UNUSED 500
#define INCORE 501
/* You'll probably want more states */

/* typedefs */
typedef struct PageTableEntry *PageTableEntryPtr;
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
    We will also have a frames structure that keeps track of the frames created
 */


#define CheckMode() assert(USLOSS_PsrGet() & USLOSS_PSR_CURRENT_MODE)
