/*
 * skeleton.c
 *
 * This is a skeleton for phase5 of the programming assignment. It
 * doesn't do much -- it is just intended to get you started.
 */


#include <assert.h>
#include <phase1.h>
#include <phase2.h>
#include <phase3.h>
#include <phase4.h>
#include <phase5.h>
#include <usyscall.h>
#include <libuser.h>
#include <vm.h>
#include <string.h>

extern void mbox_create(systemArgs *args_ptr);
extern void mbox_release(systemArgs *args_ptr);
extern void mbox_send(systemArgs *args_ptr);
extern void mbox_receive(systemArgs *args_ptr);
extern void mbox_condsend(systemArgs *args_ptr);
extern void mbox_condreceive(systemArgs *args_ptr);

Process processes[MAXPROC];  // phase 5 process table
PageTableEntryPtr PageTable[MAXPROC];

void *vmRegion; // start of virtual memory frames

FaultMsg faults[MAXPROC]; /* Note that a process can have only
                           * one fault at a time, so we can
                           * allocate the messages statically
                           * and index them by pid. */
VmStats  vmStats;
int pagersMailbox;
int pagerPIDS[MAXPAGERS];

static void FaultHandler(int  type,  // USLOSS_MMU_INT
             void *arg); // Offset within VM region

static void vmInit(systemArgs *sysargsPtr);
static void vmDestroy(systemArgs *sysargsPtr);
static int Pager(char *buf);
static void *vmInitReal(int mappings, int pages, int frames, int pagers);
static void vmDestroyReal(void);
/*
 *----------------------------------------------------------------------
 *
 * start4 --
 *
 * Initializes the VM system call handlers. 
 *
 * Results:
 *      MMU return status
 *
 * Side effects:
 *      The MMU is initialized.
 *
 *----------------------------------------------------------------------
 */
int start4(char *arg)
{
    int pid;
    int result;
    int status;

    /* to get user-process access to mailbox functions */
    systemCallVec[SYS_MBOXCREATE]      = mbox_create;
    systemCallVec[SYS_MBOXRELEASE]     = mbox_release;
    systemCallVec[SYS_MBOXSEND]        = mbox_send;
    systemCallVec[SYS_MBOXRECEIVE]     = mbox_receive;
    systemCallVec[SYS_MBOXCONDSEND]    = mbox_condsend;
    systemCallVec[SYS_MBOXCONDRECEIVE] = mbox_condreceive;

    /* user-process access to VM functions */
    systemCallVec[SYS_VMINIT]    = vmInit;
    systemCallVec[SYS_VMDESTROY] = vmDestroy;

    /* Initialize the fault mailboxes for each individual process */
    for (int process = 0; process < MAXPROC; process++) {
        faults[process].replyMbox = MboxCreate(0, 0);
    }

    /* Set pagerPIDS to -1 */
    for (int pager = 0; pager < MAXPAGERS; pager++) {
        pagerPIDS[pager] = -1;
    }

    result = Spawn("Start5", start5, NULL, 8*USLOSS_MIN_STACK, 2, &pid);
    if (result != 0) {
        USLOSS_Console("start4(): Error spawning start5\n");
        Terminate(1);
    }

    // Wait for start5 to terminate
    result = Wait(&pid, &status);
    if (result != 0) {
        USLOSS_Console("start4(): Error waiting for start5\n");
        Terminate(1);
    }

    Terminate(0);
    return 0; // not reached

} /* start4 */


/*
 *----------------------------------------------------------------------
 *
 * VmInit --
 *
 * Stub for the VmInit system call.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      VM system is initialized.
 *
 *----------------------------------------------------------------------
 */
static void vmInit(systemArgs *sysargsPtr)
{
    CheckMode();

    long mappings, pages, frames, pagers, status;

    mappings = (long) sysargsPtr->arg1;
    pages = (long) sysargsPtr->arg2;
    frames = (long) sysargsPtr->arg3;
    pagers = (long) sysargsPtr->arg4;
    status = OK;

    /* Error checking */
    if (pagers > MAXPAGERS) {
        status = ERROR;
    }

    if (mappings < 1 || mappings != pages) {
        status = ERROR;
    }

    if (frames < 1) {
        status = ERROR;
    }

    /* Check error value */
    if (status == ERROR) {
        sysargsPtr->arg1 = (void *) ERROR;
        return;
    }

    sysargsPtr->arg1 = vmInitReal(mappings, pages, frames, pagers);
} /* vmInit */


/*
 *----------------------------------------------------------------------
 *
 * vmInitReal --
 *
 * Called by vmInit.
 * Initializes the VM system by configuring the MMU and setting
 * up the page tables.
 *
 * Results:
 *      Address of the VM region.
 *
 * Side effects:
 *      The MMU is initialized.
 *
 *----------------------------------------------------------------------
 */
void *vmInitReal(int mappings, int pages, int frames, int pagers)
{
    int status, errorMMU, pageSize;
    char name[20];

    CheckMode();
    status = USLOSS_MmuInit(mappings, pages, frames);
    if (status != USLOSS_MMU_OK) {
      USLOSS_Console("vmInitReal: couldn't init MMU, status %d\n", status);
      abort();
    }
    USLOSS_IntVec[USLOSS_MMU_INT] = FaultHandler;

    /* Initialize page tables */
    for (int process = 0; process < MAXPROC; process++) {
        PageTable[process] = (PageTableEntry *) malloc(sizeof(PageTableEntry) * pages);
        processes[process].PageTable = PageTable[process];
        processes[process].numPages = pages;
        processes[process].pagesInUse = 0;
    }

    /* Create the fault mailbox */
    pagersMailbox = MboxCreate(MAXPROC, sizeof(int));

    /* Get size of a page and fork the pagers with buffer */
    pageSize = USLOSS_MmuPageSize();

    for (int unit = 0; unit < pagers; unit++) {
        char *buf = (char *) malloc(sizeof(char) * pageSize);
        
        sprintf(name, "Pager unit %d", unit);
        pagerPIDS[unit] = fork1(name, Pager, buf, USLOSS_MIN_STACK, PAGER_PRIORITY);
    }

    /* Zero out vmStat, then initialize */
    memset((char *) &vmStats, 0, sizeof(VmStats));
    vmStats.pages = pages;
    vmStats.frames = frames;
    vmStats.freeFrames = frames;
    vmStats.switches = 0;
    vmStats.faults = 0;
    vmStats.newFaults = 0;
    vmStats.pageIns = 0;
    vmStats.pageOuts = 0;
    vmStats.replaced = 0;

    /* Initialize other vmStats fields */
    int sector, track, disk;
    DiskSize(DISK1, &sector, &track, &disk);

    vmStats.diskBlocks = disk;
    vmStats.freeDiskBlocks = disk;

    /* Create vm Region */
    errorMMU = USLOSS_MmuInit(mappings, pages, frames);
    if (errorMMU != USLOSS_MMU_OK) {
        USLOSS_Console("vmInitReal(): Error creating mmu unit. Halting...\n");
        USLOSS_Halt(1);
    }

    vmRegion = USLOSS_MmuRegion(NULL);

    return vmRegion;
} /* vmInitReal */


/*
 *----------------------------------------------------------------------
 *
 * vmDestroy --
 *
 * Stub for the VmDestroy system call.
 *
 * Results:
 *      None.
 *
 * Side effects:
 *      VM system is cleaned up.
 *
 *----------------------------------------------------------------------
 */

static void vmDestroy(systemArgs *sysargsPtr)
{
    CheckMode();
} /* vmDestroy */


/*
 *----------------------------------------------------------------------
 *
 * vmDestroyReal --
 *
 * Called by vmDestroy.
 * Frees all of the global data structures
 *
 * Results:
 *      None
 *
 * Side effects:
 *      The MMU is turned off.
 *
 *----------------------------------------------------------------------
 */
void vmDestroyReal(void)
{

    CheckMode();
    USLOSS_MmuDone();
    /*
    * Kill the pagers here.
    */
    /* 
    * Print vm statistics.
    */
    USLOSS_Console("vmStats:\n");
    USLOSS_Console("pages: %d\n", vmStats.pages);
    USLOSS_Console("frames: %d\n", vmStats.frames);
    USLOSS_Console("blocks: %d\n", vmStats.blocks);
    /* and so on... */

} /* vmDestroyReal */


/*
 *----------------------------------------------------------------------
 *
 * PrintStats --
 *
 *      Print out VM statistics.
 *
 * Results:
 *      None
 *
 * Side effects:
 *      Stuff is printed to the USLOSS_Console.
 *
 *----------------------------------------------------------------------
 */
void PrintStats(void)
{
    USLOSS_Console("VmStats\n");
    USLOSS_Console("pages:          %d\n", vmStats.pages);
    USLOSS_Console("frames:         %d\n", vmStats.frames);
    USLOSS_Console("diskBlocks:     %d\n", vmStats.diskBlocks);
    USLOSS_Console("freeFrames:     %d\n", vmStats.freeFrames);
    USLOSS_Console("freeDiskBlocks: %d\n", vmStats.freeDiskBlocks);
    USLOSS_Console("switches:       %d\n", vmStats.switches);
    USLOSS_Console("faults:         %d\n", vmStats.faults);
    USLOSS_Console("new:            %d\n", vmStats.newFaults);
    USLOSS_Console("pageIns:        %d\n", vmStats.pageIns);
    USLOSS_Console("pageOuts:       %d\n", vmStats.pageOuts);
    USLOSS_Console("replaced:       %d\n", vmStats.replaced);
} /* PrintStats */


/*
 *----------------------------------------------------------------------
 *
 * FaultHandler
 *
 * Handles an MMU interrupt. Simply stores information about the
 * fault in a queue, wakes a waiting pager, and blocks until
 * the fault has been handled.
 *
 * Results:
 * None.
 *
 * Side effects:
 * The current process is blocked until the fault is handled.
 *
 *----------------------------------------------------------------------
 */
static void FaultHandler(int  type /* USLOSS_MMU_INT */,
             void *arg  /* Offset within VM region */)
{
    int cause, pid, offset;
    FaultMsgPtr faultMsg;

    offset = (int) (long) arg;
    pid = getpid();

    assert(type == USLOSS_MMU_INT);
    cause = USLOSS_MmuGetCause();
    assert(cause == USLOSS_MMU_FAULT);
    vmStats.faults++;

    faultMsg = &faults[pid % MAXPROC];
    faultMsg->pid = pid;
    faultMsg->addr = (void *) ((char *) vmRegion + offset);

    Mbox_Send(pagersMailbox, (void *) (long) pid, sizeof(int));
    Mbox_Receive(faultMsg->replyMbox, NULL, 0);
} /* FaultHandler */

/*
 *----------------------------------------------------------------------
 *
 * Pager 
 *
 * Kernel process that handles page faults and does page replacement.
 *
 * Results:
 * None.
 *
 * Side effects:
 * None.
 *
 *----------------------------------------------------------------------
 */
static int Pager(char *buf)
{
    while(1) {
        /* Wait for fault to occur (receive from mailbox) */
        /* Look for free frame */
        /* If there isn't one then use clock algorithm to
         * replace a page (perhaps write to disk) */
        /* Load page into frame from disk, if necessary */
        /* Unblock waiting (faulting) process */
    }
    return 0;
} /* Pager */
































