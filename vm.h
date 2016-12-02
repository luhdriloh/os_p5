/*
 * vm.h
 */


/*
 * All processes use the same tag.
 */
#define TAG 0

#define NOT_ON_DISK         -1
#define PAGE_NOT_IN_FRAME   -1
#define SECTORS_IN_FRAME     8


#define TRACK_START          0

/*
    Different states for a page.
*/
#define UNUSED          500
#define INUSE           501


/*
    Different states for a frame.
*/

// For reference number
#define UNREFERENCED           0
#define REFERENCED             1

// For frame in use or not in use
#define NOT_USED        0
#define USED            1

// For frame state
#define CLEAN           502
#define DIRTY           503 
#define PAGER_OWNED     504


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
    int referenceNum;
    int pid;
    int pageNum;
    int timeStamp;
} FrameTableEntry;


#define CheckMode() assert(USLOSS_PsrGet() & USLOSS_PSR_CURRENT_MODE)
