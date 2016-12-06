#include <usloss.h>
#include <phase5.h>

#define DEBUG 0
extern int debugflag;

void p1_fork(int pid)
{
    if (DEBUG && debugflag) {
        USLOSS_Console("p1_fork() called: pid = %d\n", pid);
    }

    if (vmStarted == VM_STOPPED) {
        return;
    }

} /* p1_fork */


/* As a reminder:
 * In phase 1, p1_switch is called by the dispatcher right before the
 * dispatcher does: enableInterrupts() followed by USLOSS_ContestSwitch()
 */
void p1_switch(int old, int newPID)
{
    if (DEBUG && debugflag) {
        USLOSS_Console("\np1_switch() called: old = %d, new = %d\n", old, newPID);
    }

    if (vmStarted == VM_STOPPED) {
        return;
    }

    int frame;
    PageTableEntryPtr pte;

    /* Go through the old processes page table and un map the pages */

    /*
    Load new valid mappings, else remove any page not associated with
    current process
    */
    for (int page = 0; page < numPages; page++) {
        pte = &pageTable[newPID % MAXPROC][page];
        frame = pte->frame;

        USLOSS_MmuUnmap(TAG, page);

        if (frame != PAGE_NOT_IN_FRAME) {
            USLOSS_MmuMap(TAG, page, frame, USLOSS_MMU_PROT_RW);            
        }
    }   

    vmStats.switches++;

} /* p1_switch */

void p1_quit(int pid)
{
    if (DEBUG || debugflag) {
        USLOSS_Console("p1_quit() called: pid = %d\n", pid);
    }

    if (vmStarted == VM_STOPPED) {
        return;
    }

    PageTableEntryPtr pte;
    FrameTableEntryPtr framePtr;

     for (int page = 0; page < numPages; page++) {
        pte = &pageTable[pid % MAXPROC][page];

        /* If page in a frame, set frame as NOT_USED and zero it out */
        if (pte->frame != PAGE_NOT_IN_FRAME && pte->state == USED) {
            framePtr = &frameTable[pte->frame];

            framePtr->state = UNREFERENCED;
            framePtr->dirty = CLEAN;
            framePtr->used = NOT_USED;

            USLOSS_MmuUnmap(TAG, page);
        }

        /* Set track as unused */
        if (pte->diskBlock != NOT_ON_DISK) {
            tracksInUse[pte->diskBlock] = NOT_USED;
        }

        /* Zero out the page table entry */
        pte->state = NOT_USED;
        pte->frame = PAGE_NOT_IN_FRAME;
        pte->diskBlock = NOT_ON_DISK;
    }   


} /* p1_quit */



























