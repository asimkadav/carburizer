//////////////////////////////////////////////////////////////////////////////
// TODO
//
//
///////////////////////////////////////////////////////////////////////////////

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/random.h>
#include <linux/version.h>
#include <asm/atomic.h>

#include <asm/cacheflush.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/highmem.h>
#include <asm/pgtable.h>
//#include <linux/mm.h>

///////////////////////////////////////////////////////////////////////////////
// For wrappers
///////////////////////////////////////////////////////////////////////////////
#include <linux/interrupt.h>
#include <linux/pci.h>

#include "fi_mod_control.h"

///////////////////////////////////////////////////////////////////////////////
// Prototypes for functions defined in this file.
///////////////////////////////////////////////////////////////////////////////
int init_module(void);
void cleanup_module(void);
int cr_ioctl (struct inode *, struct file *, unsigned int, unsigned long);

// Checking device activity
static pte_t *virt_to_pte (void *virtual);
static void clear_ref_bits (void);
static void crmod_check_function (void);

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,19)
static void crmod_work_thread (void *data);
#else
static void crmod_work_thread (struct work_struct *work);
#endif

// Module initialization
int cr_pci_register_driver (struct pci_driver *driver, struct module *this_mod);

// IRQ Handling
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
irqreturn_t cr_irq_handler(int irq, void *dev_instance, struct pt_regs *regs);
int cr_request_irq(unsigned int irq,
                   irqreturn_t (*handler)(int, void *, struct pt_regs *),
                   unsigned long flags, const char *dev_name, void *dev_id);
void cr_free_irq (unsigned int irq, void *dev_id);
#else
irqreturn_t cr_irq_handler(int irq, void *dev_instance);
int cr_request_irq(unsigned int irq,
                   irqreturn_t (*handler)(int, void *),
                   unsigned long flags, const char *dev_name, void *dev_id);
void cr_free_irq (unsigned int irq, void *dev_id);
#endif

///////////////////////////////////////////////////////////////////////////////
// Kernel/driver interaction
///////////////////////////////////////////////////////////////////////////////
MODULE_LICENSE("GPL");
#define CR_MINOR 47
static struct miscdevice cr_setup;
struct file_operations cr_fops = {
    .owner = THIS_MODULE,
    .ioctl = cr_ioctl,
};

///////////////////////////////////////////////////////////////////////////////
// Global variables
///////////////////////////////////////////////////////////////////////////////

// Constant that specifies how often we poll in jiffies
#define crmod_timer_length 1000

// Workqueue for checking device responsiveness
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
static struct workqueue_struct * crmod_workqueue_struct;
static struct work_struct crmod_work_struct;
#else
static struct workqueue_struct * crmod_workqueue_struct;
static struct delayed_work crmod_work_struct;
#endif

// Constant used in several contexts
#define CR_MAP_SIZE 256

// The base address and size of the module.
// Used for setting reference bits
static void *cr_base_address = NULL;
static unsigned long cr_module_size = 0;

// Array containing wrapped IRQ handlers.
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
static irqreturn_t (*cr_irq_handlers[CR_MAP_SIZE])(int, void *, struct pt_regs *);
static void *cr_irq_parameters[CR_MAP_SIZE];
#else
static irq_handler_t cr_irq_handlers[CR_MAP_SIZE];
static void *cr_irq_parameters[CR_MAP_SIZE];
#endif

// Used for checking driver activity
static atomic_t interrupt_handler_called;
static atomic_t problem_pending;

// Verbose mode?
#define DEBUG_MODE
#define uprintk(x, ...) if (DEBUG_MODE) { printk (x, __VA_ARGS__); }

///////////////////////////////////////////////////////////////////////////////
// Function implementations
///////////////////////////////////////////////////////////////////////////////
int init_module(void) {
    //struct module *cur_mod = NULL;

    // Set up our workqueue:
    crmod_workqueue_struct = create_singlethread_workqueue
        ("crmod_workqueue_struct");
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
    INIT_WORK(&crmod_work_struct, crmod_work_thread, NULL);
#else
    INIT_DELAYED_WORK(&crmod_work_struct, crmod_work_thread);
#endif
    queue_delayed_work (crmod_workqueue_struct, &crmod_work_struct, crmod_timer_length);
    
/*     list_for_each_entry(cur_mod, &THIS_MODULE->list, list) { */
/*         printk ("%s ", cur_mod->name); */
/*         printk ("0x%p ", cur_mod->module_core); */
/*         printk ("%lu\n", cur_mod->core_size); */
/*     } */

    atomic_set (&interrupt_handler_called, 0);
    atomic_set (&problem_pending, 0);
    
    // Initialize linux kernel junk
    cr_setup.minor = CR_MINOR;
    cr_setup.name = "runtime";
    cr_setup.fops = &cr_fops;
    return misc_register(&cr_setup);
}

void cleanup_module (void) {
    int number;

    cancel_delayed_work(&crmod_work_struct);
    flush_workqueue(crmod_workqueue_struct);
    destroy_workqueue(crmod_workqueue_struct);
    
    number = misc_deregister(&cr_setup);
    if (number < 0) {
        printk ("misc_deregister failed. %d\n", number);
    }
}

int cr_ioctl (struct inode *inode,
              struct file *fp,
              unsigned int cmd,
              unsigned long arg) {
    int rc = 0;
    switch (cmd) {
        case CR_DISABLE_IRQ:
            //local_irq_disable (arg);
            disable_irq (arg);
            break;

        case CR_ENABLE_IRQ:
            //local_irq_enable (arg);
            enable_irq (arg);
            break;
            
        default:
            printk ("Specify a valid cmd\n");
            break;
    }
    
    return rc;
}

// Translates a virtual address into a pte_t *
// Returns NULL if this isn't possible
// Call pte_unmap on the return value if it's not NULL
static pte_t *virt_to_pte (void *cur_addr) {
    pgd_t *pgd;
    pud_t *pud;
    pmd_t *pmd;
    pte_t *pte;
    unsigned long pg = (unsigned long) cur_addr & PAGE_MASK;
/*     struct page *page; */
/*     void *page_addr; */
/*     unsigned long phys_addr; */

    if (pg > TASK_SIZE)
        pgd = pgd_offset(current->active_mm, pg);
    else {
        printk ("pg < TASK_SIZE: 0x%lx\n", pg);
        return NULL;
    }
        
    if (pgd_none(*pgd)) {
        printk ("pgd_none(*pgd)\n");
        return NULL;
    }

    pud = pud_offset(pgd, pg);
    if (pud_none(*pud)) {
        printk ("pud_none(*pud)\n");
        return NULL;
    }

    pmd = pmd_offset(pud, pg);
    if (pmd_none(*pmd)) {
        printk ("pmd_none(*pmd) %p?\n", cur_addr);
        return NULL;
    }

    pte = pte_offset_map(pmd, pg);
    if (pte_none(*pte)) {
        pte_unmap(pte);
        printk ("pte_none(*pte) %p?\n", cur_addr);
        return NULL;
    }


/*     if (pte_present (*pte)) { */
/*         page = pte_page (*pte); */
/*     } else { */
/*         printk ("pte_present(*pte)\n"); */
/*         return NULL; */
/*     } */

/*     page_addr = kmap_atomic (page, KM_IRQ1); */
/*     phys_addr = virt_to_phys (cur_addr); */
    
/*     printk ("cur: %p flag: 0x%lx, addrs %p %lx, young: %d\n", */
/*             cur_addr, pte->pte_low, page_addr, phys_addr, pte_young(*pte)); */
/*     printk ("%x %x %x %x vs %x %x %x %x\n", */
/*             ((unsigned int *) page_addr)[0], */
/*             ((unsigned int *) page_addr)[1], */
/*             ((unsigned int *) page_addr)[2], */
/*             ((unsigned int *) page_addr)[3], */
/*             ((unsigned int *) cur_addr)[0], */
/*             ((unsigned int *) cur_addr)[1], */
/*             ((unsigned int *) cur_addr)[2], */
/*             ((unsigned int *) cur_addr)[3]); */
    return pte;
}

// Return true (!= 0) if any referenced bits are set.
static int ref_bits_set (void) {
    void *cur_addr;
    pte_t *pte;
    
    for (cur_addr = cr_base_address;
         cur_addr < cr_base_address + cr_module_size;
         cur_addr += PAGE_SIZE) {

        pte = virt_to_pte (cur_addr);
        if (pte != NULL) {
            if (pte_young(*pte) != 0) {
                // kunmap_atomic (page, KM_IRQ1);
                pte_unmap(pte);
                return 1;
            }

            // kunmap_atomic (page, KM_IRQ1);
            pte_unmap(pte);
        }
    }

    return 0;
}

// Reset all referenced bits to 0.
static void clear_ref_bits (void) {
    void *cur_addr;
    pte_t *pte;
    
    for (cur_addr = cr_base_address;
         cur_addr < cr_base_address + cr_module_size;
         cur_addr += PAGE_SIZE) {

        pte = virt_to_pte (cur_addr);
        if (pte != NULL) {
            *pte = pte_mkold(*pte);
            // kunmap_atomic (page, KM_IRQ1);
            pte_unmap(pte);
        }
    }
    
    global_flush_tlb();
}

static void call_all_interrupt_handlers (void) {
    int irq;
    irqreturn_t retval;
    
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,19)
    // This parameter is never used in practice,
    // so we just invent some nonsense.  It's so rarely
    // used that subsequent kernel versions removed it
    // entirely! :)
    static struct pt_regs bogus_regs;
#endif

    for (irq = 0; irq < CR_MAP_SIZE; irq++) {
        if (cr_irq_handlers[irq] != NULL) {
            void *dev_instance = cr_irq_parameters[irq];
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,19)
            retval = cr_irq_handlers[irq](irq, dev_instance, &bogus_regs);
#else
            retval = cr_irq_handlers[irq](irq, dev_instance, NULL);
#endif
        }
    }
}

/*
  http://lxr.linux.no/linux+v2.6.18.1/include/linux/page-flags.h
    31 * Note that the referenced bit, the page->lru list_head and the active,
    32 * inactive_dirty and inactive_clean lists are protected by the
    33 * zone->lru_lock, and *NOT* by the usual PG_locked bit!
*/  
static void crmod_check_function (void) {
    if (atomic_read (&problem_pending) != 0) {
        atomic_set (&problem_pending, 0);
        
        if (atomic_read (&interrupt_handler_called) == 0) {
            // In this case we've definitely got a problem
            // Call ALL IRQ handlers!  And hope for the best.
            printk ("Problem detected...\n");
            call_all_interrupt_handlers ();
        }
        else {
            // No problem since the interrupt handler is working
            // still.
        }
    } else {
        if (atomic_read (&interrupt_handler_called) != 0) {
            if (ref_bits_set ()) {
                // No big deal, this happens all the time
            }
            else {
                panic ("WTF, how is this possible?");
            }
        } else {
            // Interrupt handler not called
            if (ref_bits_set ()) {
                // We start the count down.
                // The interrupt handler was not called, but
                // referenced bits are set.
                atomic_inc(&problem_pending);
            }
            else {
                // Nothing, the driver is simply inactive.
            }
        }
    }
    
    atomic_set(&interrupt_handler_called, 0);
    clear_ref_bits ();
}

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,19)
void crmod_work_thread (void *param) {
#else
void crmod_work_thread (struct work_struct *work) {
#endif

    crmod_check_function ();
    
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,19)
    queue_delayed_work(crmod_workqueue_struct, &crmod_work_struct, crmod_timer_length);
#else
    // No need to reschedule in the new version of the kernel.
#endif
}

//
// Module initialization
//
int cr_pci_register_driver (struct pci_driver *driver, struct module *this_mod) {
    cr_base_address = this_mod->module_core;
    cr_module_size = this_mod->core_size;
    printk ("Module: %s, base address %p, size %lu\n",
            this_mod->name, this_mod->module_core, this_mod->core_size);
    return __pci_register_driver (driver, this_mod);
}

//
// Interrupt handler wrapper
//
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
irqreturn_t cr_irq_handler(int irq, void *dev_instance, struct pt_regs *regs)
#else
irqreturn_t cr_irq_handler(int irq, void *dev_instance)
#endif
{
    int ret;

#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
    ret = cr_irq_handlers[irq](irq, dev_instance, regs);
#else
    ret = cr_irq_handlers[irq](irq, dev_instance);
#endif

    //clear_ref_bits ();
    atomic_inc (&interrupt_handler_called);
    
    return ret;
}

//
// Request wrapper, keeps track of IRQ numbers requested
//
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
int cr_request_irq(unsigned int irq,
                   irqreturn_t (*handler)(int, void *, struct pt_regs *),
                   unsigned long flags, const char *dev_name, void *dev_id)
#else
int cr_request_irq(unsigned int irq,
                   irqreturn_t (*handler)(int, void *),
                   unsigned long flags, const char *dev_name, void *dev_id)
#endif
{
    //uprintk("%s", __FUNCTION__);
    if (cr_irq_handlers[irq] != NULL) {
        printk ("Already requested IRQ %d, overwriting...\n", irq);
    }

    if (irq >= CR_MAP_SIZE) {
        panic ("%s: map too small\n", __FUNCTION__); 
    }
    
    cr_irq_handlers[irq] = handler;
    cr_irq_parameters[irq] = dev_id;

    return request_irq(irq, cr_irq_handler, flags, dev_name, dev_id);
}


//
// Free IRQ wrapper, clears our state
//
void cr_free_irq (unsigned int irq, void *dev_id) {
    //uprintk("%s", __FUNCTION__);
    if (cr_irq_handlers[irq] == NULL) {
        printk ("%s: Already freed IRQ %d, ignoring...\n", __FUNCTION__, irq);
    }
    cr_irq_handlers[irq] = NULL;

    free_irq (irq, dev_id);
}

// Module initialization
EXPORT_SYMBOL (cr_pci_register_driver);

// IRQ
EXPORT_SYMBOL (cr_request_irq);
EXPORT_SYMBOL (cr_free_irq);
