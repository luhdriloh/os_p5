#include <assert.h>
#include <phase1.h>
#include <phase2.h>
#include <phase3.h>
#include <phase4.h>
#include <phase5.h>
#include <usyscall.h>
#include <libuser.h>
#include <vm.h>
#include <providedPrototypes.h>
#include <string.h>

extern void mbox_create(systemArgs *args_ptr);
extern void mbox_release(systemArgs *args_ptr);
extern void mbox_send(systemArgs *args_ptr);
extern void mbox_receive(systemArgs *args_ptr);
extern void mbox_condsend(systemArgs *args_ptr);
extern void mbox_condreceive(systemArgs *args_ptr);

Process processes[MAXPROC];  // phase 5 process table
PageTableEntryPtr PageTable[MAXPROC];
FrameTableEntryPtr frameTable;
int numFrames;
int clockHand;
int pageSize;
int indexForDiskWrite;

void *vmRegion; // start of virtual memory frames

FaultMsg faults[MAXPROC]; /* Note that a process can have only
                           * one fault at a time, so we can
                           * allocate the messages statically
                           * and index them by pid. */
VmStats  vmStats;
int pagersMailbox;
int pagerPIDS[MAXPAGERS];

static void FaultHandler(int  type, void *arg);
static void vmInit(systemArgs *sysargsPtr);
static void vmDestroy(systemArgs *sysargsPtr);
static void *vmInitReal(int mappings, int pages, int frames, int pagers);
static void vmDestroyReal(void);
static int Pager(char *buf);
void setFrameEntryMembers(int frameIndex, int state, int pid,
                            int pageNum, int reference, int used);
void setPageEntryMembers(int pid, int pageNum, int state,
                            int frame, int diskBlock);
void checkDiskStatus(int status, char *name);
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
    int status, errorMMU;
    char name[20];

    CheckMode();
    status = USLOSS_MmuInit(mappings, pages, frames);
    
    if (status != USLOSS_MMU_OK) {
      USLOSS_Console("vmInitReal: couldn't init MMU, status %d\n", status);
      abort();
    }

    USLOSS_IntVec[USLOSS_MMU_INT] = FaultHandler;

    /* Initialize processes table, and page table */
    for (int process = 0; process < MAXPROC; process++) {
        PageTable[process] = (PageTableEntry *) malloc(sizeof(PageTableEntry) * pages);
        processes[process].PageTable = PageTable[process];
        processes[process].numPages = pages;
        processes[process].pagesInUse = 0;
    }

    /* Initialize globals */
    frameTable = (FrameTableEntryPtr) malloc(sizeof(FrameTableEntry) * frames);
    numFrames = frames;
    clockHand = 0;
    indexForDiskWrite = 0;

    for (int frame = 0; frame < numFrames; frame++) {
        frameTable[frame].state = CLEAN;
        frameTable[frame].referenceNum = UNREFERENCED;
        frameTable[frame].used = NOT_USED;
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
    USLOSS_Console("blocks: %d\n", vmStats.diskBlocks);
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
    faultMsg->addr = (void *) (long) offset;

    Mbox_Send(pagersMailbox, (void *) (long) pid, sizeof(int));
    Mbox_Receive(faultMsg->replyMbox, NULL, 0);
} /* FaultHandler */


static int clockAlgorithm(PageTableEntryPtr pageToLoad) {
    FrameTableEntryPtr curFrame;

    /* Look for an unreferenced and clean frame */
    for (int frame = 0; frame < numFrames; frame++) {
        curFrame = &frameTable[frame];

        if (curFrame->used == NOT_USED) {
            return frame;
        }
    }

    /* CLOCK ALGORITHM */
    /* Now using the clock hand look for the first unreferenced frame */
    // TODO: Need mutual exclusion for clock hand
    while (1) {
        curFrame = &frameTable[clockHand];

        if (curFrame->referenceNum == UNREFERENCED) {
            break;
        }

        curFrame->referenceNum = UNREFERENCED;
        clockHand = (clockHand+1) % numFrames;
    }

    return clockHand;
}


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
    int pid, frameIndex, pageNum, diskStatus;
    void *framePtr;
    FaultMsgPtr faultPtr;
    FrameTableEntryPtr frameToUse;
    PageTableEntryPtr pageToLoad, pageToChange;

    while(1) {
        /* Mbox receive will tell us the */
        Mbox_Receive(pagersMailbox, &pid, sizeof(int));
        faultPtr = &faults[pid % MAXPROC];

        /* Find the page number based on the addr the fault happened */
        pageNum = ((long) faultPtr->addr - (long) vmRegion) / pageSize;
        pageToLoad = &PageTable[pid % MAXPROC][pageNum];

        /* Find frame to use with clockAlgorithm */       
        frameIndex = clockAlgorithm(pageToLoad);
        frameToUse = &frameTable[frameIndex];

        /* Update the page table of the process that owns the frame */
        if (frameToUse->used == USED) {
            int indexPageToSave, pidToSave;
                
            indexPageToSave = frameToUse->pageNum;
            pidToSave = frameToUse->pid;
            pageToChange = &PageTable[pidToSave % MAXPROC][indexPageToSave];

            /* Save frame into disk if necessary */
            if (frameToUse->state == DIRTY) {
                /* See if this is our first time storing frame on disk */
                if (pageToChange->diskBlock == NOT_ON_DISK) {
                    pageToChange->diskBlock = indexForDiskWrite;

                    indexForDiskWrite++;
                }

                /* Map pager to frame we have found to transfer */
                framePtr = (void *) ((char *) vmRegion +
                            (pageSize * frameIndex));
                USLOSS_MmuMap(TAG, 0, frameIndex, USLOSS_MMU_PROT_RW);
                memcpy(buf, framePtr, pageSize);

                diskStatus = diskWriteReal(DISK1, pageToChange->diskBlock,
                        TRACK_START, SECTORS_IN_FRAME, (void *) buf);
                checkDiskStatus(diskStatus, "Pager(): writing to disk");
            }

            setPageEntryMembers(pidToSave, indexPageToSave, NOT_USED,
                                    PAGE_NOT_IN_FRAME, pageToChange->diskBlock);
        }
       
        /* Check if we need to read from disk */
        if (pageToLoad->diskBlock != NOT_ON_DISK) {
            diskStatus = diskReadReal(DISK1, pageToLoad->diskBlock,
                    TRACK_START, SECTORS_IN_FRAME, (void *) buf);
            checkDiskStatus(diskStatus, "Pager(): reading from disk");

            framePtr = (void *) ((char *) vmRegion +
                            (pageSize * frameIndex));
            USLOSS_MmuMap(TAG, 0, frameIndex, USLOSS_MMU_PROT_RW);
            memcpy(framePtr, buf, pageSize);
        }

        /* Set members inside frame entry */
        setFrameEntryMembers(frameIndex, CLEAN, pid, pageNum, REFERENCED, USED);

        /* Set values in the processess page table */
        setPageEntryMembers(pid, pageNum, USED, frameIndex,
                pageToChange->diskBlock);

        /* Map the page to the empty frame found */
        USLOSS_MmuMap(TAG, pageNum, frameIndex, USLOSS_MMU_PROT_RW);

        /* Wake up fault handler which wakes up sleeping process */
        Mbox_Send(faultPtr->replyMbox, NULL, 0);
    }
    return 0;
} /* Pager */


/*
 *----------------------------------------------------------------------
 *
 * setFrameEntryMembers
 *
 * Helper function to set member of a FrameTableEntry struct.
 *
 * Results:
 * None.
 *
 * Side effects:
 * Will have changed the members of a FrameTableEntry struct
 *
 *----------------------------------------------------------------------
 */

void setFrameEntryMembers(int frameIndex, int state, int pid,
                            int pageNum, int reference, int used)
{
    FrameTableEntryPtr frameToUpdate = &frameTable[frameIndex];

    frameToUpdate->state = state;
    frameToUpdate->pid = pid;
    frameToUpdate->pageNum = pageNum;
    frameToUpdate->referenceNum = reference;
    frameToUpdate->used = used;
    frameToUpdate->timeStamp = USLOSS_Clock();
}


/*
 *----------------------------------------------------------------------
 *
 * setPageEntryMembers
 *
 * Helper function to set member of a PageTableEntry struct.
 *
 * Results:
 * None.
 *
 * Side effects:
 * Will have changed the members of a PageTableEntry struct
 *
 *----------------------------------------------------------------------
 */

void setPageEntryMembers(int pid, int pageNum, int state,
                            int frame, int diskBlock)
{
    PageTableEntryPtr pageToUpdate = &PageTable[pid % MAXPROC][pageNum];

    pageToUpdate->state = state;
    pageToUpdate->frame = frame;
    pageToUpdate->diskBlock = diskBlock;
}


/*
 *----------------------------------------------------------------------
 *
 * checkDiskStatus
 *
 * Helper function to check disk status
 *
 * Results:
 * None.
 *
 * Side effects:
 * Halt USLOSS
 *
 *----------------------------------------------------------------------
 */

void checkDiskStatus(int status, char *name)
{
    if (status != USLOSS_DEV_READY) {
        USLOSS_Console("Error %s. Halting...");
        USLOSS_Halt(1);
    }
}









































