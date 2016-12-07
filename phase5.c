#include <assert.h>
#include <phase1.h>
#include <phase2.h>
#include <phase3.h>
#include <phase4.h>
#include <phase5.h>
#include <usyscall.h>
#include <libuser.h>
#include <providedPrototypes.h>
#include <string.h>

extern void mbox_create(systemArgs *args_ptr);
extern void mbox_release(systemArgs *args_ptr);
extern void mbox_send(systemArgs *args_ptr);
extern void mbox_receive(systemArgs *args_ptr);
extern void mbox_condsend(systemArgs *args_ptr);
extern void mbox_condreceive(systemArgs *args_ptr);

static void FaultHandler(int  type, void *arg);
static void vmInit(systemArgs *sysargsPtr);
static void vmDestroy(systemArgs *sysargsPtr);
static void *vmInitReal(int mappings, int pages, int frames, int pagers);
static void vmDestroyReal(void);
static int Pager(char *buf);
void setFrameEntryMembers(int pid, int frameIndex, int state, int dirty, 
                            int pageNum, int used, int pagerOwned);
void setPageEntryMembers(int pid, int pageNum, int state,
                            int frame, int diskBlock);
void checkDiskStatus(int status, char *name);
void readWriteToFrame(int frameIndex, void *dest, void *src);
int findOpenTrack();
void PrintStats();


PageTableEntryPtr pageTable[MAXPROC];
Process processes[MAXPROC];
FaultMsg faults[MAXPROC];
FrameTableEntryPtr frameTable;
int numFrames;
int numPages;
int numTracks;
int clockHand;
int clockHandMailbox;
int frameMailbox;
int pageSize;
int pagersMailbox;
int pagerPIDS[MAXPAGERS];
int *tracksInUse;
int vmStarted;
void *vmRegion; // start of virtual memory frames
VmStats  vmStats;

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
    
    PrintStats();
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
        sysargsPtr->arg4 = (void *) ERROR;
        return;
    }

    /* Check if vmInit has already been called */
    if (vmStarted == VM_STARTED) {
        sysargsPtr->arg4 = (void *) -2;
        return;
    }

    sysargsPtr->arg4 = OK;
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
    int status;
    char name[20];

    CheckMode();
    status = USLOSS_MmuInit(mappings, pages, frames);

    if (status != USLOSS_MMU_OK) {
      USLOSS_Console("vmInitReal: couldn't init MMU, status %d\n", status);
      abort();
    }

    USLOSS_IntVec[USLOSS_MMU_INT] = FaultHandler;
    numPages = pages;

    /* Initialize processes table, and page table */
    for (int process = 0; process < MAXPROC; process++) {
        pageTable[process] = (PageTableEntry *) malloc(sizeof(PageTableEntry) * pages);

        /* Set default values for page tables */
        for (int page = 0; page < numPages; page++) {
            pageTable[process][page].state = UNREFERENCED;
            pageTable[process][page].frame = PAGE_NOT_IN_FRAME;
            pageTable[process][page].diskBlock = NOT_ON_DISK;
        }

        processes[process].PageTable = pageTable[process];
        processes[process].numPages = pages;
        processes[process].pagesInUse = 0;
    }

    /* Initialize globals */
    frameTable = (FrameTableEntryPtr) malloc(sizeof(FrameTableEntry) * frames);
    numFrames = frames;
    clockHand = 0;

    for (int frame = 0; frame < numFrames; frame++) {
        setFrameEntryMembers(NO_PID, frame, UNREFERENCED, CLEAN, PAGE_NOT_IN_FRAME, NOT_USED, NOT_PAGER_OWNED);
    }

    /* Create the fault, and clockhand mailbox and fault mailboxes */
    pagersMailbox = MboxCreate(MAXPROC, sizeof(int));
    clockHandMailbox = MboxCreate(1, 0);
    frameMailbox = MboxCreate(1, 0);

    /* Initialize the fault mailboxes for each individual process */
    for (int process = 0; process < MAXPROC; process++) {
        faults[process].replyMbox = MboxCreate(0, 0);
    }

    /* Get size of a page and fork the pagers with buffer */
    pageSize = USLOSS_MmuPageSize();

    for (int unit = 0; unit < pagers; unit++) {        
        sprintf(name, "Pager unit %d", unit);
        pagerPIDS[unit] = fork1(name, Pager, NULL, USLOSS_MIN_STACK, PAGER_PRIORITY);
    }

    /* Zero out vmStat, then initialize */
    memset((char *) &vmStats, 0, sizeof(VmStats));
    vmStats.pages = pages;
    vmStats.frames = frames;
    vmStats.freeFrames = frames;
    vmStats.switches = 0;
    vmStats.faults = 0;
    vmStats.new = 0;
    vmStats.pageIns = 0;
    vmStats.pageOuts = 0;
    vmStats.replaced = 0;

    /* Initialize other vmStats fields */
    int sector, track, disk, blocks;
    diskSizeReal(DISK1, &sector, &track, &disk);

    blocks = ((sector * track) / pageSize) * disk;
    vmStats.diskBlocks = blocks;
    vmStats.freeDiskBlocks = blocks;
    tracksInUse = calloc(disk, sizeof(int));
    numTracks = disk;

    /* Create vm Region */
    vmRegion = USLOSS_MmuRegion(&numPages);

    vmStarted = VM_STARTED;
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

    vmDestroyReal();
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

    int joinStatus;
    
    /*  Kill the pagers here. */
    MboxRelease(pagersMailbox);
    for (int pager = 0; pager < MAXPAGERS; pager++) {
        if (pagerPIDS[pager] == -1) {
            break;
        }

        join(&joinStatus);
    }

    /* Free page table memory */
    for (int process = 0; process < MAXPROC; process++) {
        free(pageTable[process]);
    }

    /* Free frame table and track array */
    free(frameTable);
    free(tracksInUse);

    vmStarted = VM_STOPPED;
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
    USLOSS_Console("new:            %d\n", vmStats.new);
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
    int cause, pid;
    FaultMsgPtr faultMsg;
    pid = getpid();

    assert(type == USLOSS_MMU_INT);
    cause = USLOSS_MmuGetCause();
    assert(cause == USLOSS_MMU_FAULT);
    vmStats.faults++;

    faultMsg = &faults[pid % MAXPROC];
    faultMsg->pid = pid;
    faultMsg->offset = (long) arg;
    

    MboxSend(pagersMailbox, (void *) (&pid), sizeof(int));
    MboxReceive(faultMsg->replyMbox, NULL, 0);
} /* FaultHandler */



/*
 *----------------------------------------------------------------------
 *
 * clockAlgorithm
 *
 * 
 *
 * Results:
 * Returns the index of the next frame to use
 *
 * Side effects:
 * Increments the clockHand
 *
 *----------------------------------------------------------------------
 */
static int clockAlgorithm(PageTableEntryPtr pageToLoad) {
    FrameTableEntryPtr curFrame;
    int frameToReturn;

    frameToReturn = 0;

    /* Look for an unused frame */
    for (int frame = 0; frame < numFrames; frame++) {
        curFrame = &frameTable[frame];

        if (curFrame->used == NOT_USED &&
            curFrame->pagerOwned == NOT_PAGER_OWNED) {
            setFrameEntryMembers(curFrame->pid, frame, UNREFERENCED, curFrame->dirty, curFrame->pageNum, NOT_USED, PAGER_OWNED);
            return frame;
        }
    }


    /* Now using the clock hand look for the first unreferenced frame */
    MboxSend(clockHandMailbox, NULL, 0);

    while (1) {
        curFrame = &frameTable[clockHand];

        /* If the state is unreferenced and not pager owned take the frame */
        if (curFrame->state == UNREFERENCED &&
                curFrame->pagerOwned == NOT_PAGER_OWNED) {
            setFrameEntryMembers(curFrame->pid, clockHand, curFrame->state, curFrame->dirty, curFrame->pageNum, curFrame->used, PAGER_OWNED);
            frameToReturn = clockHand;
            clockHand = (clockHand+1) % numFrames;
            break;
        }

        if (curFrame->pagerOwned != PAGER_OWNED) {
            USLOSS_MmuSetAccess(clockHand, (0 | curFrame->dirty));
            curFrame->state = UNREFERENCED;            
        }

        clockHand = (clockHand+1) % numFrames;
    }

    MboxReceive(clockHandMailbox, NULL, 0);

    return frameToReturn;
}


/*
 *----------------------------------------------------------------------
 *
 * Pager 
 *
 * Kernel process that handles page faults and does page replacement.
 *
 * Results:
 * Maps a page to a frame
 *
 * Side effects:
 * Changes to the mmu, frameTable, and pageTable
 *
 *----------------------------------------------------------------------
 */
static int Pager(char *buf)
{
    int pid, frameIndex, pageNum, diskStatus, mailboxStatus;
    FaultMsgPtr faultPtr;
    FrameTableEntryPtr frameToUse;
    PageTableEntryPtr pageToLoad, pageToChange;

    /* Allocate memory for the buffer */
    buf = (char *) malloc(sizeof(char) * pageSize);
    pid = 0;

    while(1) {
        /* Mbox receive will tell us the pid */
        mailboxStatus = MboxReceive(pagersMailbox, (void *) &pid, sizeof(int));
        if (mailboxStatus == MAILBOX_RELEASED) {
            free(buf);
            return 0;
        }
        faultPtr = &faults[pid % MAXPROC];

        /* Find the page number based on the addr the fault happened */
        pageNum = faultPtr->offset / pageSize;
        pageToLoad = &pageTable[pid % MAXPROC][pageNum];
        
        /* Find frame to use with clockAlgorithm */  
        frameIndex = clockAlgorithm(pageToLoad);
        frameToUse = &frameTable[frameIndex];
        // USLOSS_Console("\npid %d: page %d set to frame %d\n", pid, pageNum, frameIndex);

        /* Update the page table of the process that owns the frame */
        if (frameToUse->used == USED) {
            int indexPageToSave, pidToSave;

            indexPageToSave = frameToUse->pageNum;
            pidToSave = frameToUse->pid;
            pageToChange = &pageTable[pidToSave % MAXPROC][indexPageToSave];

            /* Save frame into disk */
            if (frameToUse->dirty >= DIRTY) {

                /* See if this is our first time storing frame on disk */
                if (pageToChange->diskBlock == NOT_ON_DISK) {
                    pageToChange->diskBlock = findOpenTrack();
                }

                /* Increment page out, copy frame to buffer then to disk */
                vmStats.pageOuts++;

                readWriteToFrame(frameIndex, buf, vmRegion);
                diskStatus = diskWriteReal(DISK1, pageToChange->diskBlock,
                        TRACK_START, SECTORS_IN_FRAME, (void *) buf);
                checkDiskStatus(diskStatus, "Pager(): writing to disk");
            }
            
            /* Set page table entry on old process */
            setPageEntryMembers(pidToSave, indexPageToSave, REFERENCED,
                                    PAGE_NOT_IN_FRAME, pageToChange->diskBlock);
        }

        /* Check if we need to read from disk or zero out frame */
        if (pageToLoad->diskBlock != NOT_ON_DISK) {
            /* Copy page from disk into buffer then into frame */
            vmStats.pageIns++;
            diskStatus = diskReadReal(DISK1, pageToLoad->diskBlock,
                    TRACK_START, SECTORS_IN_FRAME, (void *) buf);
            checkDiskStatus(diskStatus, "Pager(): reading from disk");
            readWriteToFrame(frameIndex, vmRegion, buf);
        }
        else {
            memset(buf, 0, pageSize);
            readWriteToFrame(frameIndex, vmRegion, buf);
        }

        /* Set members inside frame entry and process page table */
        if (pageToLoad->state == UNREFERENCED) {
            vmStats.new++;
        }

        setFrameEntryMembers(pid, frameIndex, UNREFERENCED, CLEAN, pageNum, USED, NOT_PAGER_OWNED);
        setPageEntryMembers(pid, pageNum, REFERENCED, frameIndex,
                            pageToLoad->diskBlock);

        /* Set access bit */
        USLOSS_MmuSetAccess(frameIndex, 0);

        MboxSend(faultPtr->replyMbox, NULL, 0);
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

void setFrameEntryMembers(int pid, int frameIndex, int state, int dirty, 
                            int pageNum, int used, int pagerOwned)
{
    FrameTableEntryPtr frameToUpdate = &frameTable[frameIndex];

    frameToUpdate->pid = pid;
    frameToUpdate->state = state;
    frameToUpdate->dirty = dirty;
    frameToUpdate->pageNum = pageNum;
    frameToUpdate->used = used;
    frameToUpdate->pagerOwned = pagerOwned;
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
    PageTableEntryPtr pageToUpdate = &pageTable[pid % MAXPROC][pageNum];

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


/*
 *----------------------------------------------------------------------
 *
 * readWriteToFrame
 *
 * Helper function to check disk status
 *
 * Results:
 * None.
 *
 * Side effects:
 * Reads or writes to a frame
 *
 *----------------------------------------------------------------------
 */

void readWriteToFrame(int frameIndex, void *dest, void *src)
{
    /* Map frame to PAGER_PAGE( == 0) then write or read from frame */  
    USLOSS_MmuMap(TAG, PAGER_PAGE, frameIndex, USLOSS_MMU_PROT_RW);
    memcpy(dest, src, pageSize);
    USLOSS_MmuUnmap(TAG, PAGER_PAGE);
}


/*
 *----------------------------------------------------------------------
 *
 * findOpenTrack
 *
 * Helper function to find an open track
 *
 * Results:
 * None.
 *
 * Side effects:
 * Finds an open track
 *
 *----------------------------------------------------------------------
 */

int findOpenTrack()
{
    for (int track = 0; track < numTracks; track++) {
        if (tracksInUse[track] == NOT_USED) {
            vmStats.freeDiskBlocks--;
            tracksInUse[track] = USED;
            return track;
        }
    }

    USLOSS_Console("findOpenTrack(): Not enough tracks. Halting...\n");
    USLOSS_Halt(1);

    return -1;
}

