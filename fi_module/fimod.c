///////////////////////////////////////////////////////////////////////////////
// For our module
// TODO
//  - This module has many many race conditions, but most of the important ones
//    are dealt with :)  Most remaining race conditions stem from instances
//    in which an ioctl is taking place to change some settings.
///////////////////////////////////////////////////////////////////////////////

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/random.h>
#include <linux/version.h>
#include <asm/msr.h>

///////////////////////////////////////////////////////////////////////////////
// For wrappers
///////////////////////////////////////////////////////////////////////////////
#include <linux/interrupt.h>
#include <linux/pci.h>
#include <asm/io.h>

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,19)
#include <sound/driver.h>
#endif

#include <sound/core.h>
#include <sound/control.h>
#include <sound/pcm.h>
#include <sound/rawmidi.h>
#include <linux/slab.h>
#include <linux/usb.h>
///////////////////////////////////////////////////////////////////////////////

#include "fi_mod_control.h"

///////////////////////////////////////////////////////////////////////////////
// Prototypes for functions defined in this file.
///////////////////////////////////////////////////////////////////////////////
int init_module(void);
void cleanup_module(void);
static void fi_full_cleanup (void);
static void initialize_random_numbers (void);
inline static unsigned int get_random_number (void);
static void dump_diagnostics (void);
int fi_ioctl (struct inode *, struct file *, unsigned int, unsigned long);

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,19)
static void dma_work_thread (void *data);
#else
static void dma_work_thread (struct work_struct *work);
#endif

static void initialize_random_numbers (void);

///////////////////////////////////////////////////////////////////////////////
// Internal function prototypes -- not exported to the driver
///////////////////////////////////////////////////////////////////////////////

#define FI_READ  1
#define FI_WRITE 2

static int fi_find_iomem_map_exact (unsigned int base);
static int fi_find_iomem_map_range (unsigned int addr);
static int fi_find_empty_iomem_map (void);
static void fi_modify8 (unsigned int LINE, char rw, unsigned char *b, unsigned int addr);
static void fi_modify16 (unsigned int LINE, char rw, unsigned short *b, unsigned int addr);
static void fi_modify32 (unsigned int LINE, char rw, unsigned int *b, unsigned int addr);

static void fi_reset_iomem_stuckbits (unsigned int index);
static void fi_reset_all_iomem_stuckbits (void);
static void fi_init_iomem (unsigned int type, unsigned int base, unsigned int size);
static void fi_clear_iomem_index (unsigned int index, unsigned long flags);
static void fi_clear_iomem (unsigned int base);

static void fi_corrupt_urb (unsigned int LINE, struct urb *u, int device_to_host);
static void fi_corrupt_buffer (unsigned int LINE, unsigned char *buffer, unsigned int length);
static void fi_usb_completion (struct urb *, struct pt_regs *);

static int fi_verify_line (int line);
static void fi_track_line (int line);
static void fi_toggle_line (int line);
static void fi_force_line (int arg);
static int fi_contains_line (int line, const int *array);
static void fi_add_line (int line, int *array);
static void fi_clear_lines (int *array);
static void fi_print_line_force (int index);
static int fi_contains_line_force_generic (int line, unsigned int *value);
static int fi_contains_line_force_32 (int line, unsigned int *value);
static int fi_contains_line_force_16 (int line, unsigned short *value);
static int fi_contains_line_force_8 (int line, unsigned char *value);
static void fi_clear_line_force (void);

static void fi_add_line_affected (int line);
static void fi_clear_lines_affected (void);

///////////////////////////////////////////////////////////////////////////////
// Kernel/driver interaction
///////////////////////////////////////////////////////////////////////////////
MODULE_LICENSE("GPL");
#define FI_MINOR 46
static struct miscdevice fi_setup;
struct file_operations fi_fops = {
    .owner = THIS_MODULE,
    .ioctl = fi_ioctl,
};

///////////////////////////////////////////////////////////////////////////////
// Global variables
///////////////////////////////////////////////////////////////////////////////
static unsigned int fi_rnd_last;      // Last random number generated, used as seed
static spinlock_t fi_rnd_last_lock;   // Lock for fi_rnd_last

// Should be protected with lock--just don't update the parameters
// while the driver is running
static const char *fi_types_strings[FI_MAX_PARAMS]; // Descriptive names
static unsigned int fi_types[FI_MAX_PARAMS]; // What faults can we inject?
static unsigned int fi_stats[FI_MAX_PARAMS]; // Statistics about how many
                                             // faults have been injected.
#define FI_START()   {   int fi_total_count = 0;
#define FI_RESET(x)      fi_types_strings[x] = #x;                          \
                         fi_types[x] = 0;                                   \
                         fi_stats[x] = 0;                                   \
                         fi_total_count++;
#define FI_VERIFY()      if (fi_total_count != FI_TOTAL_COUNT) {            \
                             panic ("FI_VERIFY failed");                    \
                         }                                                  \
                     }

// Constant used in several contexts
#define FI_MAP_SIZE 256

// Array containing wrapped IRQ handlers.
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
static irqreturn_t (*fi_irq_handlers[FI_MAP_SIZE])(int, void *, struct pt_regs *);
#else
static irq_handler_t fi_irq_handlers[FI_MAP_SIZE];
#endif

// Memory types
#define MEMTYPE_KMALLOC 'k'
#define MEMTYPE_VMALLOC 'v'

// Array containing ranges of I/O memory
#define MAP_INVALID     0
#define MAP_IOMEMPORTS  1
#define MAP_DMA         2

static spinlock_t fi_iomem_map_lock;
struct iomem_map {
    // This indicates how stuckbitmask was allocated, e.g. vmalloc/kmalloc
    unsigned char memtype; // See above #defines

    // This indicates the type of memory we're tracking, see above #defines
    unsigned int type;

    // Base address
    unsigned int base;

    // Length in bytes
    unsigned long size;

    // Two pointers that maintain the bitmasks
    unsigned char *stuckbitmask[2]; // stuck at 0 or 1
};
static struct iomem_map fi_iomem_map[FI_MAP_SIZE];

// Workqueue for steadily corrupting DMA memory
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
static struct workqueue_struct * dma_workqueue_struct;
static struct work_struct dma_work_struct;
#else
static struct workqueue_struct * dma_workqueue_struct;
static struct delayed_work dma_work_struct;
#endif

//
// Lists of lines
//

struct fi_line_affected {
    unsigned int line;
    unsigned int count;
};

#define LINE_LIST_MAX 384
static int fi_line_list[LINE_LIST_MAX];     // Specified lines to track
static int fi_line_list_all[LINE_LIST_MAX]; // All possible lines to track
static struct fi_line_affected fi_line_list_affected[LINE_LIST_MAX];  // Lines we've already done FI on
static struct line_force fi_line_force[LINE_LIST_MAX];
static spinlock_t fi_line_lock;             // Lock for these arrays

// Verbose mode?
#define uprintk(x, ...) if (fi_types[FI_COMMAND_VERBOSE]) { printk (x, __VA_ARGS__); }

///////////////////////////////////////////////////////////////////////////////
// Function implementations
///////////////////////////////////////////////////////////////////////////////
int init_module(void){
    // Set up our workqueue:
    dma_workqueue_struct = create_singlethread_workqueue
        ("dma_workqueue_struct");
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
    INIT_WORK(&dma_work_struct, dma_work_thread, NULL);
#else
    INIT_DELAYED_WORK(&dma_work_struct, dma_work_thread);
#endif
    queue_delayed_work (dma_workqueue_struct, &dma_work_struct, fi_types[FI_DMA_TIMER]);

    // Set up spinlocks:
    spin_lock_init (&fi_iomem_map_lock);
    spin_lock_init (&fi_rnd_last_lock);
    spin_lock_init (&fi_line_lock);

    // Clear all data structures.
    fi_full_cleanup ();

    // Initialize linux kernel junk
    fi_setup.minor = FI_MINOR;
    fi_setup.name = "fimod";
    fi_setup.fops = &fi_fops;
    return misc_register(&fi_setup);
}

void cleanup_module (void) {
    int number;
    cancel_delayed_work(&dma_work_struct);
    flush_workqueue(dma_workqueue_struct);
    destroy_workqueue(dma_workqueue_struct);

    fi_full_cleanup();

    number = misc_deregister(&fi_setup);
    if (number < 0) {
        printk ("misc_deregister failed. %d\n", number);
    }
}

//
// Free all transient structures and clear all lists
// Called manually and on module load.
// In module init, be sure to call after creating the locks.
//
static void fi_full_cleanup (void) {
    int i;
    unsigned long flags;

    // Initialize the random number pool:
    initialize_random_numbers ();

    // Verify
    if (FI_TOTAL_COUNT > FI_MAX_PARAMS) {
        panic ("fimod.c:  FI_TOTAL_COUNT > FI_MAX_PARAMS\n");
    }

    // Clear all probabilities
    FI_START();
    FI_RESET(FI_BITFLIPS);
    FI_RESET(FI_STUCKBITS);
    FI_RESET(FI_DOMBITS);
    FI_RESET(FI_EXTRAIRQS);
    FI_RESET(FI_IGNOREDIRQS);
    FI_RESET(FI_RANDOMGARBAGE);
    
    FI_RESET(FI_CORRUPT_IOMEMPORTS);
    FI_RESET(FI_CORRUPT_DMA);
    FI_RESET(FI_CORRUPT_USB);
    
    FI_RESET(FI_SELECTIVE_LINES);
    FI_RESET(FI_TOGGLE_LINE);
    FI_RESET(FI_FORCE_LINE);
    FI_RESET(FI_DMA_TIMER);
    
    FI_RESET(FI_COMMAND_CLEAR_LINES);
    FI_RESET(FI_COMMAND_VERBOSE);
    FI_RESET(FI_COMMAND_IN_ONLY);
    FI_RESET(FI_COMMAND_DIAG);
    FI_VERIFY();

    // Set up line lists:
    fi_types[FI_SELECTIVE_LINES] = LINE_SELECTION_IGNORE;

    fi_clear_lines (fi_line_list);
    fi_clear_lines (fi_line_list_all);
    fi_clear_lines_affected ();
    fi_clear_line_force ();
    
    for (i = 0; i < FI_MAP_SIZE; i++) {
        spin_lock_irqsave(&fi_iomem_map_lock, flags);
        fi_clear_iomem_index (i, flags);
    }

    dump_diagnostics();
}

//
// Use a truly random number as the seed.
//
static void initialize_random_numbers (void) {
    unsigned int new_seed;
    unsigned long flags;

    get_random_bytes (&new_seed, sizeof (new_seed));
    
    spin_lock_irqsave(&fi_rnd_last_lock, flags);
    fi_rnd_last = new_seed;
    spin_unlock_irqrestore(&fi_rnd_last_lock, flags);
}

//
// Linear congruential random number generator:  BCPL parameters
// We don't need cryptographic-strength numbers.
//
inline static unsigned int get_random_number (void) {
    unsigned int a = 2147001325;
    unsigned int c = 715136305;
    unsigned int retval;
    unsigned long flags;
    spin_lock_irqsave(&fi_rnd_last_lock, flags);
    fi_rnd_last = a * fi_rnd_last + c;
    retval = fi_rnd_last;
    spin_unlock_irqrestore(&fi_rnd_last_lock, flags);
    return retval;
}

//
// Prints out information about the fault injection on demand.
//
static void dump_diagnostics (void) {
    int i, j, bit;
    char *str;
    
    for (i = 0; i < FI_MAX_PARAMS; i++) {
        printk ("Param %d %s: %u, stats: %u\n",
                i, fi_types_strings[i], fi_types[i], fi_stats[i]);
    }
    
    for (i = 0; i < FI_MAP_SIZE; i++) {
        if (fi_iomem_map[i].base != 0) {
            printk ("I/O memory map index %d, type %d\n", i, fi_iomem_map[i].type);
            printk ("Base: 0x%x\n", fi_iomem_map[i].base);
            printk ("Size: 0x%lx\n", fi_iomem_map[i].size);
            for (bit = 0; bit < 2; bit++) {
                const int MAX_NUMS = 100;
                int numbytes = fi_iomem_map[i].size > MAX_NUMS ?
                    MAX_NUMS : fi_iomem_map[i].size;
                printk ("Stuckbitmask %d:\n", bit);
                for (j = 0; j < numbytes; j++) {
                    printk ("0x%x ", fi_iomem_map[i].stuckbitmask[bit][j]);
                    if (j % 15 == 0) {
                        printk ("\n");
                    }
                }
                if (numbytes == MAX_NUMS) {
                    printk ("... [more omitted]\n");
                }
                
                printk ("\n\n");
            }
        }
    }

    if (fi_types[FI_SELECTIVE_LINES] == LINE_SELECTION_IGNORE) {
        str = "LINE_SELECTION_IGNORE";
    } else if (fi_types[FI_SELECTIVE_LINES] == LINE_SELECTION_INCLUDE) {
        str = "LINE_SELECTION_INCLUDE";
    } else if (fi_types[FI_SELECTIVE_LINES] == LINE_SELECTION_EXCLUDE) {
        str = "LINE_SELECTION_EXCLUDE";
    }
    
    printk ("Line tracking mode: %s\n", str);
    printk ("All tracked lines:\n");
    for (i = 0; i < LINE_LIST_MAX; i++) {
        if (fi_line_list_all[i] != -1) {
            printk ("%d ", fi_line_list_all[i]);
        }
    }
    printk ("\n");
    printk ("\n");

    printk ("All specified lines:\n");
    for (i = 0; i < LINE_LIST_MAX; i++) {
        if (fi_line_list[i] != -1) {
            printk ("%d ", fi_line_list[i]);
        }
    }
    printk ("\n");
    printk ("\n");

    printk ("All lines we've already done FI on:\n");
    for (i = 0; i < LINE_LIST_MAX; i++) {
        if (fi_line_list_affected[i].line != -1) {
            printk ("Line %d, count %d\n",
                    fi_line_list_affected[i].line,
                    fi_line_list_affected[i].count);
        }
    }
    printk ("\n");

    printk ("All forced line values.  These override everything else:\n");
    for (i = 0; i < LINE_LIST_MAX; i++) {
        fi_print_line_force (i);
    }
    printk ("\n");
}

//
// The value/purpose of "arg" is dependent on the value of "cmd".
//
int fi_ioctl (struct inode *inode,
              struct file *fp,
              unsigned int cmd,
              unsigned long arg) {
    int rc = 0;
    switch (cmd) {
        case FI_STUCKBITS: {
            unsigned long flags;
            spin_lock_irqsave (&fi_iomem_map_lock, flags);
            fi_types[cmd] = arg;
            fi_reset_all_iomem_stuckbits ();
            spin_unlock_irqrestore (&fi_iomem_map_lock, flags);
            
            // Fall through.  Only if we specify STUCK_BITS
            // do we recalculate which memory addresses are stuck.
        }
        
        case FI_BITFLIPS:
        case FI_EXTRAIRQS:
        case FI_IGNOREDIRQS:
        case FI_DOMBITS:
        case FI_RANDOMGARBAGE:
        case FI_CORRUPT_IOMEMPORTS:
        case FI_CORRUPT_DMA:
        case FI_CORRUPT_USB:
            // Specify which faults will be generated
            printk ("Param %u: %lu\n", cmd, arg);
            if (cmd < 0 || cmd >= FI_MAX_PARAMS) {
                panic ("Bug somewhere in fi_ioctl area\n");
            }
            fi_types[cmd] = arg;
            break;
        case FI_SELECTIVE_LINES:
            fi_types[FI_SELECTIVE_LINES] = arg;
            break;
        case FI_TOGGLE_LINE:
            fi_toggle_line (arg);
            break;
        case FI_FORCE_LINE:
            fi_force_line (arg);
            break;
        case FI_DMA_TIMER:
            fi_types[FI_DMA_TIMER] = arg;
            break;
        case FI_COMMAND_CLEAR_LINES:
            fi_clear_lines (fi_line_list);
            fi_clear_lines (fi_line_list_all);
            fi_clear_lines_affected ();
            fi_clear_line_force ();
            break;
        case FI_COMMAND_VERBOSE:
            fi_types[cmd] = !fi_types[cmd];
            printk ("Verbose: %d\n", fi_types[cmd]);
            break;
        case FI_COMMAND_IN_ONLY:
            fi_types[cmd] = !fi_types[cmd];
            break;
        case FI_COMMAND_DIAG:
            dump_diagnostics ();
            break;
        default:
            printk ("Error: specify a valid command %d.\n", cmd);
            break;
    }
    return rc;
}

///////////////////////////////////////////////////////////////////////////////
// Our workqueue
///////////////////////////////////////////////////////////////////////////////
void dma_corruption (void) {
    int i;
    unsigned int n;
    
    n = get_random_number ();
    if (fi_types[FI_CORRUPT_DMA] != 0) {
        for (i = 0; i < FI_MAP_SIZE; i++) {
            unsigned char *ptr = NULL;
            unsigned long flags;
            
            spin_lock_irqsave (&fi_iomem_map_lock, flags);
            if (fi_iomem_map[i].type == MAP_DMA) {
                unsigned int offset = n % fi_iomem_map[i].size;
                unsigned char *base = (unsigned char *) fi_iomem_map[i].base;
                ptr = &base[offset];
            }
            spin_unlock_irqrestore (&fi_iomem_map_lock, flags);

            if (ptr != NULL) {
                // We are only writing to DMA memory here.
                // Dealing with reads from DMA memory in general
                // doesn't seem to be easy, because there are no
                // wrappers already available in the code.
                fi_modify8 (-1, FI_WRITE, ptr, (unsigned int) ptr);
            }
        }
    }
}

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,19)
void dma_work_thread (void *param) {
#else
void dma_work_thread (struct work_struct *work) {
#endif

    dma_corruption ();
    
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,19)
    queue_delayed_work(dma_workqueue_struct, &dma_work_struct, fi_types[FI_DMA_TIMER]);
#else
    // No need to reschedule in the new version of the kernel.
#endif
}

///////////////////////////////////////////////////////////////////////////////
// Helper functions
///////////////////////////////////////////////////////////////////////////////

//
// Simple find:  must match exactly.
// Need to acquire the lock before executing this. 
//
static int fi_find_iomem_map_exact (unsigned int base) {
    int i;
    int retval = -1;
    
    for (i = 0; i < FI_MAP_SIZE; i++) {
        if (fi_iomem_map[i].base == base) {
            retval = i;
            break;
        }
    }

    return retval;
}

//
// Find the index in the fi_iomem_map array that holds the range
// that contains the specified port or address.  Calls panic if there
// is no such range.
//
// Need to acquire the lock before executing this. 
//
static int fi_find_iomem_map_range (unsigned int addr) {
    int i;
    int retval = -1;
    
    for (i = 0; i < FI_MAP_SIZE; i++) {
        if (fi_iomem_map[i].base <= addr &&
            fi_iomem_map[i].base + fi_iomem_map[i].size > addr) {
            retval = i;
            break;
        }
    }

    if (retval == -1) {
        fi_types[FI_STUCKBITS] = 0;
        dump_stack();
        printk ("%s Disabling stuck-at faults. No mapping (addr 0x%x)\n",
                __FUNCTION__, addr);
    }
    return retval;
}

static int fi_find_empty_iomem_map (void) {
    int i;
    int retval = -1;
    
    for (i = 0; i < FI_MAP_SIZE; i++) {
        if (fi_iomem_map[i].base == 0 &&
            fi_iomem_map[i].size == 0) {
            retval = i;
            break;
        }
    }

    if (retval == -1) {
        dump_stack();
        panic ("%s Do we need so many I/O maps?\n", __FUNCTION__);
    }

    return retval;
}
//
// See if we want to inject a random transient bit flip.
// This bit flip occurs just once.
//
#define FLIP_HELPER(type)                                                     \
    if (fi_types[FI_BITFLIPS] > 0) {                                          \
        int i;                                                                \
        int bits = sizeof (type) * 8;                                         \
        type before = *b;                                                     \
        for (i = 0; i < bits; i++) {                                          \
            unsigned int n = get_random_number ();                            \
            if (n < fi_types[FI_BITFLIPS]) {                                  \
                *b = (*b) ^ (type) (1 << (n & (bits - 1)));                   \
            }                                                                 \
        }                                                                     \
        if (*b != before) {                                                   \
            uprintk ("Injecting tr, before %d, after %d\n", before, *b);      \
            fi_stats[FI_BITFLIPS]++;                                          \
            fi_add_line_affected (LINE);                                      \
        }                                                                     \
    }

//
// See if we want to inject a stuck-at fault.  This fault
// is permanent.  TODO we currently support only "stuck at 1" faults.
//
#define STUCK_HELPER(type)                                                    \
    if (fi_types[FI_STUCKBITS] > 0) {                                         \
        int i;                                                                \
        unsigned long offset;                                                 \
        unsigned long flags;                                                  \
        type before = *b;                                                     \
        spin_lock_irqsave(&fi_iomem_map_lock, flags);                         \
        i = fi_find_iomem_map_range (addr);                                   \
        if (i != -1) {                                                        \
            offset = addr - fi_iomem_map[i].base;                             \
            *b = *b | *((type *)&fi_iomem_map[i].stuckbitmask[1][offset]);    \
            *b = *b & *((type *)&fi_iomem_map[i].stuckbitmask[0][offset]);    \
            if (*b != before) {                                               \
                uprintk ("Injecting st, before %d, after %d\n", before, *b);  \
                fi_stats[FI_STUCKBITS]++;                                     \
                fi_add_line_affected (LINE);                                  \
            }                                                                 \
        }                                                                     \
        spin_unlock_irqrestore(&fi_iomem_map_lock, flags);                    \
    }

//
// Current implementation is that garbage is either all 0s or all 1s
// Alternative implementation is that garbage is random 0s and 1s. 
// 
#define GARBAGE_HELPER(type)                                                  \
    if (fi_types[FI_RANDOMGARBAGE] > 0) {                                     \
        unsigned int n = get_random_number ();                                \
        type before = *b;                                                     \
        if (n < fi_types[FI_RANDOMGARBAGE]) {                                 \
            /**b = (type) n;*/                                                \
            *b = (type) ((n & 1) ? -1 : 0);                                   \
        }                                                                     \
        if (*b != before) {                                                   \
            uprintk ("Injecting gb, before %d, after %d\n", before, *b);      \
            fi_add_line_affected (LINE);                                      \
            fi_stats[FI_RANDOMGARBAGE]++;                                     \
        }                                                                     \
    }

#define FORCE_HELPER(function)                                                \
        function (LINE, b);
 
//
// The next three functions randomly modify the input
// to include a fault.  They do not do anything
// if fault injection is disabled or the dice come up
// wrong.
//
static void fi_modify8 (unsigned int LINE,
                        char rw,
                        unsigned char *b,
                        unsigned int addr) {
    if ((rw == FI_WRITE && fi_types[FI_COMMAND_IN_ONLY] == 0) ||
        rw == FI_READ) {
        unsigned char beforeall = *b;
        FLIP_HELPER(unsigned char);
        STUCK_HELPER(unsigned char);
        GARBAGE_HELPER(unsigned char);
        FORCE_HELPER(fi_contains_line_force_8);
        if (beforeall != *b) {
            uprintk (KERN_INFO "Injected fault of some kind: %d to %d\n",
                 beforeall, *b);
        }
    } else {
        // In this case, we are doing a write, and we've specified
        // that we only want to corrupt incoming data.
    }
}

static void fi_modify16 (unsigned int LINE,
                         char rw,
                         unsigned short *b,
                         unsigned int addr) {
    if ((rw == FI_WRITE && fi_types[FI_COMMAND_IN_ONLY] == 0) ||
        rw == FI_READ) {
        unsigned short beforeall = *b;
        FLIP_HELPER(unsigned short);
        STUCK_HELPER(unsigned short);
        GARBAGE_HELPER(unsigned short);
        FORCE_HELPER(fi_contains_line_force_16);
        if (beforeall != *b) {
            uprintk (KERN_INFO "Injected fault of some kind: %d to %d\n",
                     beforeall, *b);
        }
    } else {
        // See fi_modify8
    }
}
 
static void fi_modify32 (unsigned int LINE,
                         char rw,
                         unsigned int *b,
                         unsigned int addr) {
    if ((rw == FI_WRITE && fi_types[FI_COMMAND_IN_ONLY] == 0) ||
        rw == FI_READ) {
        unsigned int beforeall = *b;
        FLIP_HELPER(unsigned int);
        STUCK_HELPER(unsigned int);
        GARBAGE_HELPER(unsigned int);
        FORCE_HELPER(fi_contains_line_force_32);
        if (beforeall != *b) {
            uprintk (KERN_INFO "Injected fault of some kind: %d to %d\n",
                 beforeall, *b);
        }
    } else {
        // See fi_modify8
    }
}

///////////////////////////////////////////////////////////////////////////////
// Wrapped kernel functions
///////////////////////////////////////////////////////////////////////////////

//
// Old I/O memory functions
// TODO Add LINE
// TODO Deal with fi_verify_line 
//

#define VERIFY_START                  \
    if (fi_verify_line (LINE) == 1) {
    
#define VERIFY_END                    \
    }
 
unsigned int fi_readl (unsigned int LINE, void const volatile *addr) {
    unsigned int result = ((unsigned int)
                           *((unsigned int volatile *) addr));
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify32 (LINE, FI_READ, &result, (unsigned int) addr);
    }
    VERIFY_END;
    return result;
}

unsigned short fi_readw (unsigned int LINE, void const volatile *addr) {
    unsigned short result = ((unsigned short)
                             *((unsigned short volatile *) addr));
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify16 (LINE, FI_READ, &result, (unsigned int) addr);
    }
    VERIFY_END;
    return result;
}

unsigned char fi_readb (unsigned int LINE, void const volatile *addr) {
    unsigned char result = ((unsigned char) *
                            ((unsigned char volatile *) addr));
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify8 (LINE, FI_READ, &result, (unsigned int) addr);
    }
    VERIFY_END;
    return result;
}

void fi_writel (unsigned int LINE, unsigned int b, void volatile *addr) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify32 (LINE, FI_WRITE, &b, (unsigned int) addr);
    }
    VERIFY_END;
    
    *((unsigned int volatile *) addr) = (unsigned int volatile) b;
}

void fi_writew (unsigned int LINE, unsigned short b, void volatile *addr) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify16 (LINE, FI_WRITE, &b, (unsigned int) addr);
    }
    VERIFY_END;
        
    *((unsigned short volatile *) addr) = (unsigned short volatile) b;
}

void fi_writeb (unsigned int LINE, unsigned char b, void volatile *addr) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify8 (LINE, FI_WRITE, &b, (unsigned int) addr);
    }
    VERIFY_END;
    
    *((unsigned char volatile *) addr) = (unsigned char volatile) b;
}

//
// Old port I/O functions
// Set 1
//

unsigned char fi_inb (unsigned int LINE, int port) {
    unsigned char result = inb (port);
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify8 (LINE, FI_READ, &result, port);
    }
    VERIFY_END;
    return result;
}

unsigned short fi_inw (unsigned int LINE, int port) {
    unsigned short result = inw (port);
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify16 (LINE, FI_READ, &result, port);
    }
    VERIFY_END;
    return result;
}

unsigned int fi_inl (unsigned int LINE, int port) {
    unsigned int result = inl (port);
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify32 (LINE, FI_READ, &result, port);
    }
    VERIFY_END;
    return result;
}

void fi_outb(unsigned int LINE, unsigned char value, int port) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify8 (LINE, FI_WRITE, &value, port);
    }
    VERIFY_END;
    outb (value, port);
}

void fi_outw(unsigned int LINE, unsigned short value, int port) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify16 (LINE, FI_WRITE, &value, port);
    }
    VERIFY_END;
    outw (value, port);
}

void fi_outl(unsigned int LINE, unsigned int value, int port) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify32 (LINE, FI_WRITE, &value, port);
    }
    VERIFY_END;
    outl (value, port);
}

//
// Old port I/O functions
// Set 2
//

unsigned char fi_inb_p (unsigned int LINE, int port) {
    unsigned char result = inb_p (port);
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify8 (LINE, FI_READ, &result, port);
    }
    VERIFY_END;
    return result;
}

unsigned short fi_inw_p (unsigned int LINE, int port) {
    unsigned short result = inw_p (port);
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify16 (LINE, FI_READ, &result, port);
    }
    VERIFY_END;
    return result;
}

unsigned int fi_inl_p (unsigned int LINE, int port) {
    unsigned int result = inl_p (port);
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify32 (LINE, FI_READ, &result, port);
    }
    VERIFY_END;
    return result;
}

void fi_outb_p(unsigned int LINE, unsigned char value, int port) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify8 (LINE, FI_WRITE, &value, port);
    }
    VERIFY_END;
    outb_p (value, port);
}

void fi_outw_p(unsigned int LINE, unsigned short value, int port) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify16 (LINE, FI_WRITE, &value, port);
    }
    VERIFY_END;
    outw_p (value, port);
}

void fi_outl_p(unsigned int LINE, unsigned int value, int port) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify32 (LINE, FI_WRITE, &value, port);
    }
    VERIFY_END;
    outl_p (value, port);
}

//
// Old port I/O functions
// Set 3
//

unsigned char fi_inb_local (unsigned int LINE, int port) {
    unsigned char result = inb_local (port);
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify8 (LINE, FI_READ, &result, port);
    }
    VERIFY_END;
    return result;
}

unsigned short fi_inw_local (unsigned int LINE, int port) {
    unsigned short result = inw_local (port);
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify16 (LINE, FI_READ, &result, port);
    }
    VERIFY_END;
    return result;
}

unsigned int fi_inl_local (unsigned int LINE, int port) {
    unsigned int result = inl_local (port);
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify32 (LINE, FI_READ, &result, port);
    }
    VERIFY_END;
    return result;
}

void fi_outb_local(unsigned int LINE, unsigned char value, int port) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify8 (LINE, FI_WRITE, &value, port);
    }
    VERIFY_END;
    outb_local (value, port);
}

void fi_outw_local(unsigned int LINE, unsigned short value, int port) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify16 (LINE, FI_WRITE, &value, port);
    }
    VERIFY_END;
    outw_local (value, port);
}

void fi_outl_local(unsigned int LINE, unsigned int value, int port) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify32 (LINE, FI_WRITE, &value, port);
    }
    VERIFY_END;
    outl_local (value, port);
}

//
// Old port I/O functions
// Set 4
//

unsigned char fi_inb_local_p (unsigned int LINE, int port) {
    unsigned char result = inb_local_p (port);
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify8 (LINE, FI_READ, &result, port);
    }
    VERIFY_END;
    return result;
}

unsigned short fi_inw_local_p (unsigned int LINE, int port) {
    unsigned short result = inw_local_p (port);
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify16 (LINE, FI_READ, &result, port);
    }
    VERIFY_END;
    return result;
}

unsigned int fi_inl_local_p (unsigned int LINE, int port) {
    unsigned int result = inl_local_p (port);
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify32 (LINE, FI_READ, &result, port);
    }
    VERIFY_END;
    return result;
}

void fi_outb_local_p(unsigned int LINE, unsigned char value, int port) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify8 (LINE, FI_WRITE, &value, port);
    }
    VERIFY_END;
    outb_local_p (value, port);
}

void fi_outw_local_p(unsigned int LINE, unsigned short value, int port) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify16 (LINE, FI_WRITE, &value, port);
    }
    VERIFY_END;
    outw_local_p (value, port);
}

void fi_outl_local_p(unsigned int LINE, unsigned int value, int port) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify32 (LINE, FI_WRITE, &value, port);
    }
    VERIFY_END;
    outl_local_p (value, port);
}

//
// New mixed I/O memory/port I/O functions
//
unsigned int fi_ioread8(unsigned int LINE, void __iomem *addr) {
    unsigned char retval = ioread8 (addr);
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify8 (LINE, FI_READ, &retval, (unsigned int) addr);
    }
    VERIFY_END;
    return retval;
}

unsigned int fi_ioread16(unsigned int LINE, void __iomem *addr) {
    unsigned short retval = ioread16 (addr);
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify16 (LINE, FI_READ, &retval, (unsigned int) addr);
    }
    VERIFY_END;
    return retval;
}

unsigned int fi_ioread16be(unsigned int LINE, void __iomem *addr) {
    panic ("%s need to implement BE support\n", __FUNCTION__);
    // TODO
    // Need to do some bit twiddling in the case of stuck-at faults
    // to ensure the proper bit is set regardless of whether we're doing
    // big-endian or little-endian access.
    //
    // No need to bother with this feature unless we actually see it come up.
}

unsigned int fi_ioread32(unsigned int LINE, void __iomem *addr) {
    unsigned int retval = ioread32 (addr);
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify32 (LINE, FI_READ, &retval, (unsigned int) addr);
    }
    VERIFY_END;
    return retval;
}

unsigned int fi_ioread32be(unsigned int LINE, void __iomem *addr) {
    panic ("%s need to implement BE support\n", __FUNCTION__);
}

void fi_iowrite8(unsigned int LINE, u8 value, void __iomem *addr) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify8 (LINE, FI_WRITE, &value, (unsigned int) addr);
    }
    VERIFY_END;
    iowrite8(value, addr);
}

void fi_iowrite16(unsigned int LINE, u16 value, void __iomem *addr) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify16 (LINE, FI_WRITE, &value, (unsigned int) addr);
    }
    VERIFY_END;
    iowrite16(value, addr);
}

void fi_iowrite16be(unsigned int LINE, u16 value, void __iomem *addr) {
    panic ("%s need to implement BE support\n", __FUNCTION__);
}

void fi_iowrite32(unsigned int LINE, u32 value, void __iomem *addr) {
    VERIFY_START;
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {
        fi_modify32 (LINE, FI_WRITE, &value, (unsigned int) addr);
    }
    VERIFY_END;
    iowrite32(value, addr);
}

void fi_iowrite32be(unsigned int LINE, u32 value, void __iomem *addr) {
    panic ("%s need to implement BE support\n", __FUNCTION__);
}

//
// New mixed I/O memory/port I/O functions, specifically for repeating
// reads/writes from the same port/I/O memory address.
//
#define IOREAD_REP_HELPER(IOREAD_FUNC, MODIFY_FUNC, TYPE)                     \
    int i;                                                                    \
    IOREAD_FUNC (addr, buf, count);                                           \
    VERIFY_START;                                                             \
    if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {                               \
        for (i = 0; i < count; i++) {                                         \
            MODIFY_FUNC(LINE,                                                 \
                        FI_READ,                                              \
                        &((TYPE *) buf)[i],                                   \
                        (unsigned int) addr);                                 \
        }                                                                     \
    }                                                                         \
    VERIFY_END; 

void fi_ioread8_rep(unsigned int LINE, void __iomem *addr, void *buf, unsigned long count) {
    IOREAD_REP_HELPER (ioread8_rep, fi_modify8, unsigned char);
}

void fi_ioread16_rep(unsigned int LINE, void __iomem *addr, void *buf, unsigned long count) {
    IOREAD_REP_HELPER (ioread16_rep, fi_modify16, unsigned short);
}

void fi_ioread32_rep(unsigned int LINE, void __iomem *addr, void *buf, unsigned long count) {
    IOREAD_REP_HELPER (ioread32_rep, fi_modify32, unsigned int);
}

#define IOWRITE_REP_HELPER(IOWRITE_FUNC, MODIFY_FUNC, TYPE)                   \
    int i;                                                                    \
    TYPE *temp = kmalloc (count * sizeof (TYPE), GFP_ATOMIC);                 \
    for (i = 0; i < count; i++) {                                             \
        temp[i] = ((TYPE *) buf)[i];                                          \
        VERIFY_START;                                                         \
        if (fi_types[FI_CORRUPT_IOMEMPORTS] != 0) {                           \
            MODIFY_FUNC(LINE,                                                 \
                        FI_WRITE,                                             \
                        &temp[i],                                             \
                        (unsigned int) addr);                                 \
        }                                                                     \
        VERIFY_END;                                                           \
    }                                                                         \
    IOWRITE_FUNC (addr, temp, count);                                         \
    kfree (temp);


void fi_iowrite8_rep(unsigned int LINE, 
                     void __iomem *addr,
                     const void *buf,
                     unsigned long count) {
    IOWRITE_REP_HELPER (iowrite8_rep, fi_modify8, unsigned char);
}

void fi_iowrite16_rep(unsigned int LINE,
                      void __iomem *addr,
                      const void *buf,
                      unsigned long count) {
    IOWRITE_REP_HELPER (iowrite16_rep, fi_modify16, unsigned short);
}

void fi_iowrite32_rep(unsigned int LINE,
                      void __iomem *addr,
                      const void *buf,
                      unsigned long count) {
    IOWRITE_REP_HELPER (iowrite32_rep, fi_modify32, unsigned int);
}

///////////////////////////////////////////////////////////////////////////////
// Wrapped the interrupt handler.  This wrapper either drops
// interrupts or repeats interrupts.
///////////////////////////////////////////////////////////////////////////////
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
irqreturn_t fi_irq_handler(int irq, void *dev_instance, struct pt_regs *regs)
#else
irqreturn_t fi_irq_handler(int irq, void *dev_instance)
#endif
{
    int i;
    int nCalls = 1;
    irqreturn_t ret = IRQ_HANDLED;
    int n = get_random_number ();

    //uprintk("%s", __FUNCTION__);

    dma_corruption ();

    if ((n < fi_types[FI_EXTRAIRQS]) && (n & 0x1)) {
        nCalls = 2;
    }
    
    if ((n < fi_types[FI_IGNOREDIRQS]) && !(n & 0x1)) {
        nCalls = 0;
    }

    for (i = 0; i < nCalls; ++i) {
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
        ret = fi_irq_handlers[irq](irq, dev_instance, regs);
#else
        ret = fi_irq_handlers[irq](irq, dev_instance);
#endif
    }

    return ret;
}

//
// Request wrapper, keeps track of IRQ numbers requested
//
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
int fi_request_irq(unsigned int irq,
                     irqreturn_t (*handler)(int, void *, struct pt_regs *),
                     unsigned long flags, const char *dev_name, void *dev_id)
#else
int fi_request_irq(unsigned int irq,
                   irqreturn_t (*handler)(int, void *),
                   unsigned long flags, const char *dev_name, void *dev_id)
#endif
{
    //uprintk("%s", __FUNCTION__);
    if (fi_irq_handlers[irq] != NULL) {
        printk ("Already requested IRQ %d, overwriting...\n", irq);
    }

    if (irq >= FI_MAP_SIZE) {
        panic ("%s: map too small\n", __FUNCTION__); 
    }
    
    fi_irq_handlers[irq] = handler;

    return request_irq(irq, fi_irq_handler, flags, dev_name, dev_id);
}

//
// Free IRQ wrapper, clears our state
//
void fi_free_irq (unsigned int irq, void *dev_id) {
    //uprintk("%s", __FUNCTION__);
    if (fi_irq_handlers[irq] == NULL) {
        printk ("%s: Already freed IRQ %d, ignoring...\n", __FUNCTION__, irq);
    }
    fi_irq_handlers[irq] = NULL;

    free_irq (irq, dev_id);
}

///////////////////////////////////////////////////////////////////////////////
// I/O memory wrappers
///////////////////////////////////////////////////////////////////////////////

// Does not acquire lock
static void fi_reset_iomem_stuckbits (unsigned int i) {
    unsigned int j, k, n;
    
    for (j = 0; j < fi_iomem_map[i].size; j++) {
        // Initialize everything to 0xFF or 0x00 by default
        fi_iomem_map[i].stuckbitmask[0][j] = 0xFF;
        fi_iomem_map[i].stuckbitmask[1][j] = 0x00;

        // Flip some bits.
        for (k = 0; k < 8; k++) {
            // Set the bit in question
            n = get_random_number ();
            if (n < fi_types[FI_STUCKBITS]) {
                fi_iomem_map[i].stuckbitmask[1][j] |= (1 << k);
            }
            
            // Clear the bit in question
            n = get_random_number ();
            if (n < fi_types[FI_STUCKBITS]) {
                fi_iomem_map[i].stuckbitmask[0][j] &= ~(1 << k);
            }
        }
    }
}

// Does not acquire lock
static void fi_reset_all_iomem_stuckbits (void) {
    int i;
    for (i = 0; i < FI_MAP_SIZE; i++) {
        fi_reset_iomem_stuckbits (i);
    }
}

// Acquires lock
static void fi_init_iomem (unsigned int type,
                           unsigned int base,
                           unsigned int size) {
    int i;
    unsigned long flags;
    unsigned char memtype = MEMTYPE_KMALLOC;

    // Sometimes this is called from interrupt context
    // Very rare, e.g. USB driver.
    void *buf1 = kmalloc (size, GFP_ATOMIC | __GFP_NOWARN);
    void *buf2 = kmalloc (size, GFP_ATOMIC | __GFP_NOWARN);

    // Remalloc using vmalloc.  If GFP_ATOMIC fails,
    // and we're in interrupt context, we're screwed.
    // Hopefully that won't happen! :-o
    if (buf1 == NULL || buf2 == NULL) {
        kfree (buf1);
        kfree (buf2);
        buf1 = vmalloc (size);
        buf2 = vmalloc (size);
        memtype = MEMTYPE_VMALLOC;
    }
    
    spin_lock_irqsave(&fi_iomem_map_lock, flags);
    i = fi_find_iomem_map_exact(base);
    
    if (i == -1) {
        i = fi_find_empty_iomem_map ();

        fi_iomem_map[i].memtype = memtype;
        fi_iomem_map[i].type = type;
        fi_iomem_map[i].base = base;
        fi_iomem_map[i].size = size;
    
        fi_iomem_map[i].stuckbitmask[0] = buf1;
        fi_iomem_map[i].stuckbitmask[1] = buf2;
        fi_reset_iomem_stuckbits (i);
        
        uprintk ("Tracking range %d, type %d, base: 0x%x size: %d\n",
                 i, type, base, size);
    }

    spin_unlock_irqrestore(&fi_iomem_map_lock, flags);
}

// Given an index, clear the data structures associated with it.
// Weird lock setup: just be sure the lock is acquired before calling.
// This function frees the lock.  Yeah, deal with it.
//
// The problem is that we need to free some memory, but we can't do
// that while holding a spinlock if the memory was allocated using
// vmalloc.  Since we may need to use vmalloc some times, we can't
// free the memory while holding the spinlock in general.
//
// If we used the atomic kmalloc, then we wouldn't need to do this, but
// we can't always do that because the amount of memory required is sometimes
// large.
//
static void fi_clear_iomem_index (unsigned int index, unsigned long flags) {
    void *buf1, *buf2;
    unsigned char memtype;
    
    memtype = fi_iomem_map[index].memtype;
    fi_iomem_map[index].type = MAP_INVALID;
    fi_iomem_map[index].base = 0;
    fi_iomem_map[index].size = 0;
    buf1 = fi_iomem_map[index].stuckbitmask[0];
    buf2 = fi_iomem_map[index].stuckbitmask[1];
    fi_iomem_map[index].stuckbitmask[0] = 0;
    fi_iomem_map[index].stuckbitmask[1] = 0;
    spin_unlock_irqrestore (&fi_iomem_map_lock, flags);

    if (memtype == MEMTYPE_KMALLOC) {
        kfree (buf1);
        kfree (buf2);
    } else if (memtype == MEMTYPE_VMALLOC) {
        vfree (buf1);
        vfree (buf2);
    } else {
        if (buf1 != 0 || buf2 != 0) {
            panic ("Error in fi_clear_iomem");
        }
    }
}

// Acquires lock--probably some races in here anyway
static void fi_clear_iomem (unsigned int base) {
    int i;
    unsigned long flags;

    spin_lock_irqsave (&fi_iomem_map_lock, flags);
    i = fi_find_iomem_map_exact (base);
    
    if (i != -1) {
        fi_clear_iomem_index (i, flags);
    } else {
        dump_stack ();
        printk ("%s Maybe we need to add port I/O support? 0x%x",
                __FUNCTION__, base);
    }
}

//
// I/O memory wrapper.  Sets up stuck-at faults.
//
void __iomem * fi_ioremap(unsigned long offset, unsigned long size) {
    int index;
    unsigned long flags;
    void __iomem *retval = ioremap (offset, size);

    spin_lock_irqsave (&fi_iomem_map_lock, flags);
    index = fi_find_iomem_map_exact ((unsigned int) retval);
    spin_unlock_irqrestore (&fi_iomem_map_lock, flags);
    
    if (index == -1) {
        // In this case, the driver may have called pci_resource_start
        // to get a range of ports.  So, this condition means it's I/O
        // memory instead.  We'll just add another range, and delete the
        // existing one.
        
        fi_clear_iomem (offset);
        fi_init_iomem (MAP_IOMEMPORTS, (unsigned int) retval, size);
    }
    
    return retval;
}

//
// I/O memory wrapper.  Clears our stuck-at fault state.
// TODO unmapping and remapping the same region will
// produce a different set of stuck-at faults each time,
// since we fix the set of stuck at faults when we map the
// region.
//
void fi_iounmap (void __iomem *addr) {
    fi_clear_iomem ((unsigned int) addr);
    iounmap (addr);
}

//
// We'll assume this function is called before any MMIO or port I/O
//
unsigned int fi_pci_resource_start (struct pci_dev *pdev, int bar) {
    unsigned int retval = pci_resource_start (pdev, bar);
    unsigned int size = pci_resource_len(pdev, bar);
    fi_init_iomem (MAP_IOMEMPORTS, retval, size);
    return retval;
}

struct resource *fi___request_region(struct resource *r, resource_size_t start,
                                     resource_size_t n, const char *name) {
    struct resource *retval = __request_region (r, start, n, name);
    fi_init_iomem (MAP_IOMEMPORTS, start, n);
    return retval;
}

void fi___release_region(struct resource *r, resource_size_t start,
                         resource_size_t n) {
    fi_clear_iomem ((unsigned int) start);
    __release_region (r, start, n);
}

void __iomem *fi_ioport_map(unsigned long port, unsigned int nr) {
    void __iomem* retval;
    retval = ioport_map (port, nr);
    fi_init_iomem (MAP_IOMEMPORTS, (unsigned int) retval, nr);
    return retval;
}

void fi_ioport_unmap(void __iomem *addr) {
    fi_clear_iomem ((unsigned int) addr);
    ioport_unmap (addr);
}

///////////////////////////////////////////////////////////////////////////////
// DMA memory wrappers
///////////////////////////////////////////////////////////////////////////////
void *fi_pci_alloc_consistent(int LINE, struct pci_dev *hwdev, size_t size,
                              dma_addr_t *dma_handle) {
    void *retval = pci_alloc_consistent (hwdev, size, dma_handle);
    uprintk ("%s\n", __FUNCTION__);

    // TODO size = 364 for PCNET32 private structure
    // The trouble is that pcnet32_private ends up ALL in DMA
    // memory, so it makes no sense to do fault injection there.
    // We'd end up corrupting the structure, and that's not
    // particularly realistic.
    if (size != 364
        ) {
        fi_init_iomem (MAP_DMA, (unsigned int) retval, size);
    } else {
        printk ("Ignoring I/O memory range because of hardcoded exception\n");
    }

    fi_contains_line_force_32 (LINE, (unsigned int *) &retval);
    return retval;
}

void fi_pci_free_consistent(struct pci_dev *hwdev, size_t size,
                            void *vaddr, dma_addr_t dma_handle) {
    uprintk ("%s\n", __FUNCTION__);
    fi_clear_iomem ((unsigned int) vaddr);
    pci_free_consistent (hwdev, size, vaddr, dma_handle);
}

void *fi_dma_alloc_coherent(int LINE, void *dev, size_t size,
                            dma_addr_t *dma_handle, gfp_t flag) {
    void *retval = dma_alloc_coherent (dev, size, dma_handle, flag);
    uprintk ("%s\n", __FUNCTION__);
    fi_init_iomem(MAP_DMA, (unsigned int) retval, size);

    fi_contains_line_force_32 (LINE, (unsigned int *) &retval);
    return retval;
}

void fi_dma_free_coherent(void *dev, size_t size,
                       void *vaddr, dma_addr_t dma_handle) {
    uprintk ("%s\n", __FUNCTION__);
    fi_clear_iomem ((unsigned int) vaddr);
    dma_free_coherent (dev, size, vaddr, dma_handle);
}

int fi_snd_dma_alloc_pages(int type, struct device *device, size_t size,
                           struct snd_dma_buffer *dmab) {
    int retval = snd_dma_alloc_pages(type, device, size, dmab);
    unsigned int base = (unsigned int) dmab->area;
    uprintk ("%s\n", __FUNCTION__);
    fi_init_iomem (MAP_DMA, base, size);
    return retval;
}

void fi_snd_dma_free_pages(struct snd_dma_buffer *dmab) {
    unsigned int base = (unsigned int) dmab->area;
    uprintk ("%s\n", __FUNCTION__);
    fi_clear_iomem (base);
    snd_dma_free_pages (dmab);
}

int fi_snd_pcm_lib_malloc_pages(struct snd_pcm_substream *substream,
                                size_t size) {
    int retval = snd_pcm_lib_malloc_pages (substream, size);
    unsigned int base = 0;
    uprintk ("%s\n", __FUNCTION__);

    if (substream != NULL) {
        if (substream->runtime != NULL) {
            base = (unsigned int) substream->runtime->dma_area;
        }
    }

    if (base != 0) {
        fi_init_iomem (MAP_DMA, base, substream->runtime->dma_bytes);
    }
    
    return retval;
}

int fi_snd_pcm_lib_free_pages(struct snd_pcm_substream *substream) {
    unsigned int base;
    int retval;

    uprintk ("%s\n", __FUNCTION__);

    if (substream != NULL) {
        if (substream->runtime != NULL) {
            base = (unsigned int) substream->runtime->dma_area;
        }
    }

    fi_clear_iomem (base);
    retval = snd_pcm_lib_free_pages (substream);
    return retval;
}

unsigned long fi___get_free_pages(gfp_t gfp_mask, unsigned int order) {
    // This function can be used to allocate DMA memory, apparently.
    unsigned long retval = __get_free_pages (gfp_mask, order);
    uprintk ("%s\n", __FUNCTION__);
    if (gfp_mask & GFP_DMA) {
        fi_init_iomem(MAP_DMA, retval, 1 << order);
    }
    return retval;
}

void fi_free_pages(unsigned long addr, unsigned int order) {
    uprintk ("%s\n", __FUNCTION__);
    fi_clear_iomem (addr);
    free_pages (addr, order);
}

///////////////////////////////////////////////////////////////////////////////
// DMA memory wrappers
///////////////////////////////////////////////////////////////////////////////

// This is an opaque kernel structure, not available to drivers.
// This structure changes from .18 to .28
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,19)
struct dma_pool {       /* the pool */
    struct list_head        page_list;
    spinlock_t              lock;
    size_t                  blocks_per_page;
    size_t                  size;
    struct device           *dev;
    size_t                  allocation;
    char                    name [32];
    wait_queue_head_t       waitq;
    struct list_head        pools;
};
#else
struct dma_pool {               /* the pool */
    struct list_head page_list;
    spinlock_t lock;
    size_t size;
    struct device *dev;
    size_t allocation;
    size_t boundary;
    char name[32];
    wait_queue_head_t waitq;
    struct list_head pools;
};
#endif

struct dma_pool *fi_dma_pool_create(const char *name, struct device *dev,
                                    size_t size, size_t align,
                                    size_t allocation) {
    //panic ("Implement %s\n", __FUNCTION__);
    return dma_pool_create (name, dev, size, align, allocation);
}

void fi_dma_pool_destroy(struct dma_pool *pool) {
    //panic ("Implement %s\n", __FUNCTION__);
    dma_pool_destroy (pool);
}

void *fi_dma_pool_alloc(struct dma_pool *pool, gfp_t mem_flags,
                        dma_addr_t *handle) {
    //panic ("Implement %s\n", __FUNCTION__);
    void *retval = dma_pool_alloc (pool, mem_flags, handle);
    fi_init_iomem (MAP_DMA, (unsigned int) retval, pool->size);
    return retval;
}

void fi_dma_pool_free(struct dma_pool *pool, void *vaddr, dma_addr_t addr) {
    //panic ("Implement %s\n", __FUNCTION__);
    fi_clear_iomem ((unsigned int) vaddr);
    dma_pool_free (pool, vaddr, addr);
}

///////////////////////////////////////////////////////////////////////////////
// USB functions
///////////////////////////////////////////////////////////////////////////////
struct fi_urb_context {
    usb_complete_t original_completion_function;
    void *original_context;
};

int fi_usb_submit_urb(unsigned int LINE,
                      struct urb *u,
                      gfp_t mem_flags) {
    if (fi_types[FI_CORRUPT_USB] != 0) {
        if (u->pipe & USB_DIR_IN) {
            // From device to host
            // Ensure we intercept the completion routine.
            // We can corrupt the data in the completion routine.
            struct fi_urb_context *new_context =
                kmalloc (sizeof (struct fi_urb_context), GFP_ATOMIC);
            new_context->original_completion_function = u->complete;
            new_context->original_context = u->context;

            u->complete = fi_usb_completion;
            u->context = new_context;
        
            return usb_submit_urb (u, mem_flags);
        } else {
            // From host to device
            // In this case, we can corrupt the data now.
            // No need to intercept the completion routine.
            fi_corrupt_urb (LINE, u, 0);
            return usb_submit_urb (u, mem_flags);
        }
    } else {
        return usb_submit_urb (u, mem_flags);
    }
}

static void fi_corrupt_urb (unsigned int LINE,
                            struct urb *u,
                            int device_to_host) {
    unsigned char *buffer;
    unsigned int length;
    
    if (device_to_host) {
        // From device to host
        buffer = u->transfer_buffer;
        length = u->transfer_buffer_length;
    } else {
        // From host to device
        buffer = u->transfer_buffer;
        length = u->transfer_buffer_length;
    }

    fi_corrupt_buffer (LINE, buffer, length);
}

// Inject transient bit flips and garbage.
// Does not do stuck bits since this doesn't seem
// to make sense in the context of USB transfer
// mechanisms.
static void fi_corrupt_buffer (unsigned int LINE,
                               unsigned char *buffer,
                               unsigned int length) {
    unsigned int i;
    
    for (i = 0; i < length; i++) {
        unsigned char beforeall = buffer[i];
        unsigned char *b = &buffer[i];
        FLIP_HELPER (unsigned char);
        GARBAGE_HELPER (unsigned char);
        if (beforeall != *b) {
            uprintk (KERN_INFO "Inject bit flip: %d to %d\n",
                    beforeall, *b);
        }
    }
}

// Intercept the URB completion routine, with the expectation that
// we corrupt the return data.  Note that we only want to intercept
// the completion routine for URBs travelling from the device to
// the host, not the other way around.
static void fi_usb_completion (struct urb *u,
                               struct pt_regs *regs) {
    struct fi_urb_context *new_context;

    if (!(u->pipe & USB_DIR_IN)) {
        panic ("URB completion routine is being called incorrectly.");
    }
    
    new_context = u->context;

    u->context = new_context->original_context;
    u->complete = new_context->original_completion_function;

    // Note:  First parameter should be "LINE", but we don't know where
    // this function is called from--it's from in the kernel.
    // There's no correspondence between a line in the driver
    // and this location, so we can't record the source of any
    // faults we inject because they're from in the kernel.
    // Thus, we use 0 to indicate "somewhere else."
    fi_corrupt_urb (0, u, 1);

    new_context->original_completion_function (u, regs);
}

int fi_usb_control_msg(unsigned int LINE,
                       struct usb_device *dev, unsigned int pipe,
                       __u8 request, __u8 requesttype, __u16 value, __u16 index,
                       void *data, __u16 size, int timeout) {
    int retval;
    if (pipe & USB_DIR_IN) {
        // Device to host
        retval = usb_control_msg(dev, pipe, request, requesttype,
                                 value, index, data, size, timeout);
        fi_corrupt_buffer (LINE, data, size);
    } else {
        // Host to device
        fi_corrupt_buffer (LINE, data, size);
        retval = usb_control_msg(dev, pipe, request, requesttype,
                                 value, index, data, size, timeout);
    }
    
    return retval;
}

int fi_usb_interrupt_msg(unsigned int LINE,
                         struct usb_device *usb_dev, unsigned int pipe,
                         void *data, int len, int *actual_length, int timeout) {
    int retval;
    if (pipe & USB_DIR_IN) {
        // Device to host
        retval = usb_interrupt_msg(usb_dev, pipe, data,
                                   len, actual_length, timeout);
        fi_corrupt_buffer (LINE, data, *actual_length);
    } else {
        // Host to device
        fi_corrupt_buffer (LINE, data, len);
        retval = usb_interrupt_msg(usb_dev, pipe, data,
                                   len, actual_length, timeout);
    }

    return retval;
}

int fi_usb_bulk_msg (unsigned int LINE,
                     struct usb_device *usb_dev, unsigned int pipe,
                     void *data, int len, int *actual_length,
                     int timeout) {
    int retval;
    if (pipe & USB_DIR_IN) {
        // Device to host
        retval = usb_bulk_msg(usb_dev, pipe, data,
                              len, actual_length, timeout);
        fi_corrupt_buffer (LINE, data, *actual_length);
    } else {
        // Host to device
        fi_corrupt_buffer (LINE, data, len);
        retval = usb_bulk_msg(usb_dev, pipe, data,
                              len, actual_length, timeout);
    }

    return retval;
}

///////////////////////////////////////////////////////////////////////////////
// Miscellaneous functions
///////////////////////////////////////////////////////////////////////////////

// Return 1 if fault injection is OK to do here,
// Return 0 if no fault injection is allowed here.
// Acquires lock
int fi_verify_line (int line) {
    fi_track_line (line);
    
    if (fi_types[FI_SELECTIVE_LINES] == LINE_SELECTION_IGNORE) {
        return 1;
    } else {
        unsigned long flags;
        int contains;

        spin_lock_irqsave (&fi_line_lock, flags);
        contains = fi_contains_line (line, fi_line_list);
        spin_unlock_irqrestore (&fi_line_lock, flags);

        if (contains) {
            if (fi_types[FI_SELECTIVE_LINES] == LINE_SELECTION_INCLUDE) {
                return 1;
            } else if (fi_types[FI_SELECTIVE_LINES] == LINE_SELECTION_EXCLUDE) {
                return 0;
            }
        } else {
            if (fi_types[FI_SELECTIVE_LINES] == LINE_SELECTION_INCLUDE) {
                return 0;
            } else if (fi_types[FI_SELECTIVE_LINES] == LINE_SELECTION_EXCLUDE) {
                return 1;
            }   
        }
    }
    
    panic ("Uh oh");
}

// Acquires lock
static void fi_track_line (int line) {
    int contains;
    unsigned long flags;
    
    spin_lock_irqsave (&fi_line_lock, flags);
    contains = fi_contains_line (line, fi_line_list_all);
    if (contains) {
        spin_unlock_irqrestore (&fi_line_lock, flags);
        return;
    }

    fi_add_line (line, fi_line_list_all);
    spin_unlock_irqrestore (&fi_line_lock, flags);
}

// Acquires lock
static void fi_toggle_line (int line) {
    int i;
    int found = -1;
    int minus_one_index = -1;
    unsigned long flags;

    spin_lock_irqsave (&fi_line_lock, flags);
    for (i = 0; i < LINE_LIST_MAX; i++) {
        if (fi_line_list[i] == -1 && minus_one_index == -1) {
            minus_one_index = i;
        }
        
        if (fi_line_list[i] == line) {
            found = i;
        }
    }

    if (found != -1) {
        printk ("Removed line[%d]:  %d\n", found, line);
        fi_line_list[found] = -1;
    } else {
        if (minus_one_index != -1) {
            printk ("Added line[%d]:  %d\n", minus_one_index, line);
            fi_line_list[minus_one_index] = line;
        } else {
            panic ("Out of line indices.  Don't go wild now.\n");
        }
    }

    spin_unlock_irqrestore (&fi_line_lock, flags);
}

static void fi_force_line (int arg) {
    struct line_force kernel_map;
    struct line_force *user_map = (struct line_force *) arg;
    int existing_index;
    unsigned int flags;
    
    if (copy_from_user (&kernel_map, user_map, sizeof (struct line_force)) != 0) {
        printk ("I'm sorry, Dave. I'm afraid I can't do that.\n");
    }

    spin_lock_irqsave (&fi_line_lock, flags);
    existing_index = fi_contains_line_force_generic (kernel_map.line, NULL);
    if (existing_index != -1) {
        // In this case, the user has already specified that they want
        // this line forced to some value.  So, we simply overwrite
        // their existing request with their new request.
        if (fi_line_force[existing_index].line != kernel_map.line) {
            panic ("These should be the same by definition\n");
        }

        fi_line_force[existing_index].value = kernel_map.value;
        fi_line_force[existing_index].operation = kernel_map.operation;
        fi_line_force[existing_index].odds = kernel_map.odds;
        fi_line_force[existing_index].total_faults = kernel_map.total_faults;

        // Used in driver only:
        fi_line_force[existing_index].num_faults = 0;

        // Print out that we added it
        fi_print_line_force (existing_index);
    }
    else {
        // In this case, the user is specifying a new line to force to
        // a specific value, so we add it to the list.

        int i;
        for (i = 0; i < LINE_LIST_MAX; i++) {
            if (fi_line_force[i].line == -1) {
                fi_line_force[i].line = kernel_map.line;
                fi_line_force[i].value = kernel_map.value;
                fi_line_force[i].operation = kernel_map.operation;
                fi_line_force[i].odds = kernel_map.odds;
                fi_line_force[i].total_faults = kernel_map.total_faults;

                // Used in driver only:
                fi_line_force[i].num_faults = 0;

                // Print out that we added it
                fi_print_line_force (i);
                break;
            }
        }

        if (i == LINE_LIST_MAX) {
            panic ("fi_force_line out of space.  Get a better flaky device.\n");
        }
    }
    spin_unlock_irqrestore (&fi_line_lock, flags);
}

// Does not acquire lock
static int fi_contains_line (int line, const int *array) {
    int i;
    int contains = 0;
    for (i = 0; i < LINE_LIST_MAX; i++) {
        if (array[i] == line) {
            contains = 1;
            break;
        }
    }

    return contains;
}

// Does not acquire lock
static void fi_add_line (int line, int *array) {
    int i;
    
    for (i = 0; i < LINE_LIST_MAX; i++) {
        if (array[i] == -1) {
            array[i] = line;
            return;
        }
    }

    panic ("Tracking too many lines--overflowed, increase LINE_LIST_MAX\n");
}

// Acquires lock
static void fi_clear_lines (int *array) {
    unsigned long flags;
    int i;
    
    spin_lock_irqsave (&fi_line_lock, flags);
    for (i = 0; i < LINE_LIST_MAX; i++) {
        array[i] = -1;
    }
    spin_unlock_irqrestore (&fi_line_lock, flags);
}

// The parameter is the index into the fi_line_force array.
// This function simply prints out the data at the index
// specified.  Used for diagnostics and when a new line
// is added.
static void fi_print_line_force (int i) {
    const char *str;
    
    if (fi_line_force[i].line != -1) {
        switch (fi_line_force[i].operation) {
            case LINE_FORCE_SET: str = "set"; break;
            case LINE_FORCE_AND: str = "and"; break;
            case LINE_FORCE_OR: str = "or"; break;
            default: panic ("Be sure to set the forced line operation appropriately\n");
        }
        
        printk ("Line %d -> Value 0x%x, Operation %s, Odds %u, Total faults: %u, Num so far: %d\n",
                fi_line_force[i].line,
                fi_line_force[i].value,
                str,
                fi_line_force[i].odds,
                fi_line_force[i].total_faults,
                fi_line_force[i].num_faults);
    }
}

// Does not acquire lock
// Returns 0 if the map does not contain the line, or the index if it does.
// Stores the value for the specified line in "value", does not change
// "value" if the specified line is not mentioned.
static int fi_contains_line_force_generic (int line, unsigned int *value) {
    int i;
    int contains = -1;
    for (i = 0; i < LINE_LIST_MAX; i++) {
        if (fi_line_force[i].line != line) {
            continue;
        }
        
        contains = i;
        if (value == NULL) {
            break;
        }
        
        if (get_random_number () >= fi_line_force[i].odds) {
            break;
        }

        if (fi_line_force[i].num_faults > fi_line_force[i].total_faults) {
            break;
        }
        
        //printk ("Forcing fault injection before 0x%x after 0x%x\n", *value, fi_line_force[i].value);
        switch (fi_line_force[i].operation) {
            case LINE_FORCE_SET: *value = fi_line_force[i].value; break;
            case LINE_FORCE_AND: *value &= fi_line_force[i].value; break;
            case LINE_FORCE_OR: *value |= fi_line_force[i].value; break;
            default: panic ("fi_contains_line_force_generic");
        }

        fi_line_force[i].num_faults++;
        fi_stats[FI_FORCE_LINE]++;
        fi_add_line_affected (line);
        break;
    }

    return contains;
}

static int fi_contains_line_force_32 (int line, unsigned int *value_32) {
    return fi_contains_line_force_generic (line, value_32);
}

static int fi_contains_line_force_16 (int line, unsigned short *value_16) {
    unsigned int value_32 = *value_16;
    int ret;
    ret = fi_contains_line_force_generic (line, &value_32);
    *value_16 = (unsigned short) value_32;
    return ret;
}

static int fi_contains_line_force_8 (int line, unsigned char *value_8) {
    unsigned int value_32 = *value_8;
    int ret;
    ret = fi_contains_line_force_generic (line, &value_32);
    *value_8 = (unsigned char) value_32;
    return ret;
}

// Acquires lock
static void fi_clear_line_force (void) {
    int i;
    unsigned long flags;
    
    spin_lock_irqsave (&fi_line_lock, flags);
    for (i = 0; i < LINE_LIST_MAX; i++) {
        fi_line_force[i].line = -1;
        fi_line_force[i].value = -1;
        fi_line_force[i].operation = -1;
        fi_line_force[i].odds = -1;
        fi_line_force[i].total_faults = -1;
        fi_line_force[i].num_faults = -1;
    }
    spin_unlock_irqrestore (&fi_line_lock, flags);
}

///////////////////////////////////////////////////////////////////////////////
// Acquires lock
// Given that we're injecting a fault on the line specified, then keep track of
// this.  Associate each line with a count of the number of times faults have
// been injected.
static void fi_add_line_affected (int line) {
    int i;
    unsigned long flags;
    int first_minus_one = -1;
    
    spin_lock_irqsave (&fi_line_lock, flags);
    for (i = 0; i < LINE_LIST_MAX; i++) {
        if (first_minus_one == -1 &&
            fi_line_list_affected[i].line == -1) {
            first_minus_one = i;
        }
        
        if (fi_line_list_affected[i].line == line) {
            fi_line_list_affected[i].count++;
            first_minus_one = -1;
            break;
        }
    }

    if (first_minus_one != -1) {
        fi_line_list_affected[first_minus_one].line = line;
        fi_line_list_affected[first_minus_one].count = 1;
    }
    spin_unlock_irqrestore (&fi_line_lock, flags);
}

// Acquires lock
static void fi_clear_lines_affected (void) {
    int i;
    unsigned long flags;
    
    spin_lock_irqsave (&fi_line_lock, flags);
    for (i = 0; i < LINE_LIST_MAX; i++) {
        fi_line_list_affected[i].line = -1;
        fi_line_list_affected[i].count = -1;
    }
    spin_unlock_irqrestore (&fi_line_lock, flags);
}


// Old I/O memory functions
EXPORT_SYMBOL(fi_readl);
EXPORT_SYMBOL(fi_readw);
EXPORT_SYMBOL(fi_readb);
EXPORT_SYMBOL(fi_writel);
EXPORT_SYMBOL(fi_writew);
EXPORT_SYMBOL(fi_writeb);

// Port I/O, Set 1
EXPORT_SYMBOL(fi_inb);
EXPORT_SYMBOL(fi_inw);
EXPORT_SYMBOL(fi_inl);
EXPORT_SYMBOL(fi_outb);
EXPORT_SYMBOL(fi_outw);
EXPORT_SYMBOL(fi_outl);

// Set 2
EXPORT_SYMBOL(fi_inb_p);
EXPORT_SYMBOL(fi_inw_p);
EXPORT_SYMBOL(fi_inl_p);
EXPORT_SYMBOL(fi_outb_p);
EXPORT_SYMBOL(fi_outw_p);
EXPORT_SYMBOL(fi_outl_p);

// Set 3
EXPORT_SYMBOL(fi_inb_local);
EXPORT_SYMBOL(fi_inw_local);
EXPORT_SYMBOL(fi_inl_local);
EXPORT_SYMBOL(fi_outb_local);
EXPORT_SYMBOL(fi_outw_local);
EXPORT_SYMBOL(fi_outl_local);

// Set 4
EXPORT_SYMBOL(fi_inb_local_p);
EXPORT_SYMBOL(fi_inw_local_p);
EXPORT_SYMBOL(fi_inl_local_p);
EXPORT_SYMBOL(fi_outb_local_p);
EXPORT_SYMBOL(fi_outw_local_p);
EXPORT_SYMBOL(fi_outl_local_p);

// New I/O memory + port accessors
EXPORT_SYMBOL(fi_ioread8);
EXPORT_SYMBOL(fi_ioread16);
EXPORT_SYMBOL(fi_ioread16be);
EXPORT_SYMBOL(fi_ioread32);
EXPORT_SYMBOL(fi_ioread32be);

EXPORT_SYMBOL(fi_iowrite8);
EXPORT_SYMBOL(fi_iowrite16);
EXPORT_SYMBOL(fi_iowrite16be);
EXPORT_SYMBOL(fi_iowrite32);
EXPORT_SYMBOL(fi_iowrite32be);

EXPORT_SYMBOL(fi_ioread8_rep);
EXPORT_SYMBOL(fi_ioread16_rep);
EXPORT_SYMBOL(fi_ioread32_rep);
EXPORT_SYMBOL(fi_iowrite8_rep);
EXPORT_SYMBOL(fi_iowrite16_rep);
EXPORT_SYMBOL(fi_iowrite32_rep);

// IRQ
EXPORT_SYMBOL(fi_irq_handler);
EXPORT_SYMBOL(fi_request_irq);
EXPORT_SYMBOL(fi_free_irq);

// Ports/IO memory
EXPORT_SYMBOL(fi_ioremap);
EXPORT_SYMBOL(fi_iounmap);
EXPORT_SYMBOL(fi_pci_resource_start);
EXPORT_SYMBOL(fi___request_region);
EXPORT_SYMBOL(fi___release_region);
EXPORT_SYMBOL(fi_ioport_map);
EXPORT_SYMBOL(fi_ioport_unmap);

// DMA memory
EXPORT_SYMBOL(fi_pci_alloc_consistent);
EXPORT_SYMBOL(fi_pci_free_consistent);
EXPORT_SYMBOL(fi_dma_alloc_coherent);
EXPORT_SYMBOL(fi_dma_free_coherent);
EXPORT_SYMBOL(fi_snd_dma_alloc_pages);
EXPORT_SYMBOL(fi_snd_dma_free_pages);
EXPORT_SYMBOL(fi_snd_pcm_lib_malloc_pages);
EXPORT_SYMBOL(fi_snd_pcm_lib_free_pages);
EXPORT_SYMBOL(fi___get_free_pages);
EXPORT_SYMBOL(fi_free_pages);

// DMA pools
EXPORT_SYMBOL(fi_dma_pool_create);
EXPORT_SYMBOL(fi_dma_pool_destroy);
EXPORT_SYMBOL(fi_dma_pool_alloc);
EXPORT_SYMBOL(fi_dma_pool_free);

// USB functions
EXPORT_SYMBOL(fi_usb_submit_urb);
EXPORT_SYMBOL(fi_usb_control_msg);
EXPORT_SYMBOL(fi_usb_interrupt_msg);
EXPORT_SYMBOL(fi_usb_bulk_msg);
