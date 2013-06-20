#ifndef FI_SHARED_H
#define FI_SHARED_H

///////////////////////////////////////////////////////////////////////////////
// IOCTL command numbers for fimod
// The meaning of the argument depends on the IOCTL number

#define FI_BITFLIPS             0
#define FI_STUCKBITS            1
#define FI_DOMBITS              2 /* Unused */
#define FI_EXTRAIRQS            3
#define FI_IGNOREDIRQS          4
#define FI_RANDOMGARBAGE        5

#define FI_CORRUPT_IOMEMPORTS   6
#define FI_CORRUPT_DMA          7
#define FI_CORRUPT_USB          8

#define FI_SELECTIVE_LINES      20
#define FI_TOGGLE_LINE          21
#define FI_FORCE_LINE           22
#define FI_DMA_TIMER            23

#define FI_COMMAND_CLEAR_LINES  28
#define FI_COMMAND_VERBOSE      29
#define FI_COMMAND_IN_ONLY      30
#define FI_COMMAND_DIAG         31

#define FI_MAX_PARAMS           32 /* Be sure:  FI_TOTAL_COUNT <= this */
#define FI_TOTAL_COUNT          17 /* Modify fi_full_cleanup too */

///////////////////////////////////////////////////////////////////////////////
// Constants that specify what to do with certain lines of code.
//
#define LINE_SELECTION_IGNORE  0  /* Apply FI to all lines equally */
#define LINE_SELECTION_INCLUDE 1  /* Include only specified lines */
#define LINE_SELECTION_EXCLUDE 2  /* Exclude only specified lines */
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// IOCTL command numbers for crmod
#define CR_DISABLE_IRQ             0
#define CR_ENABLE_IRQ              1

#define CR_COMMAND_DIAG           31
///////////////////////////////////////////////////////////////////////////////

///////////////////////////////////////////////////////////////////////////////
// Structure that maps a line to a specific value.
// Used for forcing a specific return value.
// If the line returns less than 32-bits, then the
// value specified is cast to the appropriate size.
struct line_force {
    int line;
    unsigned int value;
    unsigned int operation;
    unsigned int odds;
    unsigned int total_faults;

    // Used only in the driver
    unsigned int num_faults; // Number of faults injected so far.
};

#define LINE_FORCE_SET  0 /* Set the value as specified */
#define LINE_FORCE_AND  1 /* AND the value with the existing value */
#define LINE_FORCE_OR   2 /* OR the value with the existing value */
///////////////////////////////////////////////////////////////////////////////

#endif
