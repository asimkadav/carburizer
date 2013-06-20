//////////////////////////////////////////////////////////////////////////////
// TODO
// - This still crashes on real hardware on module unload.
//   Steps to reproduce:
//   Load crmod
//   Load driver
//   Unload driver
//   Unload crmod -- this may crash, but not with certainty.
//
///////////////////////////////////////////////////////////////////////////////

#include <linux/module.h>
#include <linux/fs.h>
#include <linux/miscdevice.h>
#include <linux/random.h>
#include <linux/version.h>
#include <asm/atomic.h>
#include <asm/system.h>
#include <linux/irq.h>

#include <asm/cacheflush.h>
#include <linux/rmap.h>
#include <linux/swap.h>
#include <linux/highmem.h>
#include <asm/pgtable.h>


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
static int ref_bits_set (int exclude_irqhandler);
static void clear_ref_bits (void);
static void crmod_check_function (void);

#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,19)
static void crmod_work_thread (void *data);
#else
static void crmod_work_thread (struct work_struct *work);
#endif

// IRQ Handling
static void cr_disable_irq (int irq);
static void cr_enable_irq (int irq);

// Diagnostics
static void print_diagnostics (void);

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
static void *cr_base_address[CR_MAP_SIZE];
static unsigned long cr_module_size[CR_MAP_SIZE];
static unsigned int cr_num_drivers;

// Array containing wrapped IRQ handlers.
#if LINUX_VERSION_CODE < KERNEL_VERSION(2,6,19)
static irqreturn_t (*cr_irq_handlers[CR_MAP_SIZE])(int, void *, struct pt_regs *);
#else
static irq_handler_t cr_irq_handlers[CR_MAP_SIZE];
#endif

static void *cr_irq_parameters[CR_MAP_SIZE];
static unsigned long cr_irq_flags[CR_MAP_SIZE];
static const char *cr_irq_name[CR_MAP_SIZE];

// Used for checking driver activity
// MAX_INTERRUPT_FREQUENCY is max # of interrupts per
// crmod_timer_length interval before we decide there's
// a problem.
#define MAX_INTERRUPT_FREQUENCY 40000000
static struct semaphore timer_semaphore;
static atomic_t interrupt_handler_called; // Count of interrupts
static int problem_pending;
static int unproductive_interrupts;
static const int problem_pending_inc = 1;
static const int max_unproductive_interrupts = 10;

// Constant that specifies how often we poll in jiffies
// One jiffy is probably either 1/100s or 1/250s.
// HZ = number of jiffies per second.
static const int default_timer_length = 4;
static int crmod_timer_length = 4;

// Verbose mode?
//#define DEBUG_MODE
#ifdef DEBUG_MODE
#define uprintk(...) printk (__VA_ARGS__)
#else
#define uprintk(x, ...)
#endif

///////////////////////////////////////////////////////////////////////////////
// Function implementations
///////////////////////////////////////////////////////////////////////////////
int init_module(void) {
    //struct module *cur_mod = NULL;
    int i;

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

    sema_init(&timer_semaphore, 1);
    atomic_set (&interrupt_handler_called, 0);
    problem_pending = 0;
    unproductive_interrupts = 0;
    
    for (i = 0; i < CR_MAP_SIZE; i++) {
        cr_base_address[i] = NULL;
        cr_module_size[i] = 0;
    }
    cr_num_drivers = 0;
    
    // Initialize linux kernel junk
    cr_setup.minor = CR_MINOR;
    cr_setup.name = "crmod";
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
            printk ("Disabling IRQ %lu\n", arg);
            //disable_irq (arg);
            cr_disable_irq (arg);
            break;

        case CR_ENABLE_IRQ:
            printk ("Enabling IRQ %lu\n", arg);
            //enable_irq (arg);
            cr_enable_irq (arg);
            break;

        case CR_COMMAND_DIAG:
            print_diagnostics ();
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

// TODO Major hack per Mike :)
static int addr_contains_irq_handler (void *addr) {
    int i;

    if (((unsigned long) addr) % PAGE_SIZE != 0) {
        panic ("addr_contains_irq_handler: Don't be stupid");
    }
    
    for (i = 0; i < CR_MAP_SIZE; i++) {
        if (cr_irq_handlers[i] != NULL)
            if ((unsigned long) cr_irq_handlers[i] >= (unsigned long) addr &&
                (unsigned long) cr_irq_handlers[i] < (unsigned long) addr + PAGE_SIZE) {
            return 1;
        }
    }
    
    return 0;
}

// Return true (!= 0) if any referenced bits are set.
static int ref_bits_set (int exclude_irqhandler) {
    void *cur_addr;
    pte_t *pte;
    int i;
    int ret_val = 0;

    for (i = 0; i < cr_num_drivers; i++) {
        if (exclude_irqhandler) uprintk ("i %d: ", i);
        for (cur_addr = cr_base_address[i];
             cur_addr < cr_base_address[i] + cr_module_size[i];
             cur_addr += PAGE_SIZE) {
            
            pte = virt_to_pte (cur_addr);
            if (pte != NULL) {
                // See if we're excluding the interrupt handler
                // from this check.
                if (exclude_irqhandler &&
                    addr_contains_irq_handler (cur_addr)) {
                    pte_unmap(pte);
                    if (exclude_irqhandler) uprintk ("X");
                    continue;
                }

                // See if the page was referenced lately.
                if (pte_young(*pte) != 0) {
                    // kunmap_atomic (page, KM_IRQ1);
                    pte_unmap(pte);
                    if (exclude_irqhandler) uprintk ("1");
                    ret_val = 1;
                    continue;
                }

                if (exclude_irqhandler) uprintk ("0");
                
                // kunmap_atomic (page, KM_IRQ1);
                pte_unmap(pte);
            }
        }

        if (exclude_irqhandler) uprintk ("\n");
    }

    return ret_val;
}

// Reset all referenced bits to 0.
static void clear_ref_bits (void) {
    void *cur_addr;
    pte_t *pte;
    int i;

    for (i = 0; i < cr_num_drivers; i++) {
        for (cur_addr = cr_base_address[i];
             cur_addr < cr_base_address[i] + cr_module_size[i];
             cur_addr += PAGE_SIZE) {
            
            pte = virt_to_pte (cur_addr);
            if (pte != NULL) {
                *pte = pte_mkold(*pte);
                // kunmap_atomic (page, KM_IRQ1);
                pte_unmap(pte);
            }
        }
    }
    
    global_flush_tlb();
}

static irqreturn_t call_all_interrupt_handlers (void) {
    int irq;
    irqreturn_t retval = 0;
    
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,19)
    // This parameter is never used in practice,
    // so we just invent some nonsense.  It's so rarely
    // used that subsequent kernel versions removed it
    // entirely! :)
    //
    // hope you're not reading this because you just
    // spent 5 hours trying to find out why it was crashing
    static struct pt_regs bogus_regs;
#endif

    for (irq = 0; irq < CR_MAP_SIZE; irq++) {
        if (cr_irq_handlers[irq] != NULL) {
            void *dev_instance = cr_irq_parameters[irq];
#if LINUX_VERSION_CODE <= KERNEL_VERSION(2,6,19)
            retval += cr_irq_handlers[irq](irq, dev_instance, &bogus_regs);
#else
            retval += cr_irq_handlers[irq](irq, dev_instance, NULL);
#endif
        }
    }

    return retval;
}

/*
  http://lxr.linux.no/linux+v2.6.18.1/include/linux/page-flags.h
    31 * Note that the referenced bit, the page->lru list_head and the active,
    32 * inactive_dirty and inactive_clean lists are protected by the
    33 * zone->lru_lock, and *NOT* by the usual PG_locked bit!
*/  
static void crmod_check_function (void) {
    int irq;

    // Disable the device's IRQ temporarily.
    // We'll be calling the interrupt handler ourselves potentially.
    // This avoids some race conditions
    for (irq = 0; irq < CR_MAP_SIZE; irq++) {
        if (cr_irq_handlers[irq] != NULL) {
            //        disable_irq (irq);
        }
    }

    down (&timer_semaphore);

    if (problem_pending != 0) {
        problem_pending--;
        
        if (atomic_read (&interrupt_handler_called) == 0) {
            // In this case we may have a problem.
            // A request was made of the driver, but the interrupt
            // handler has not been called.  This could be one of
            // two things:
            //
            // - The device is malfunctioning, and not generating
            //   interrupts.
            // - The driver simply did not need to invoke the device
            //   to service the request--e.g. E1000 ethtool functions
            //   are requests to the driver that do not result in
            //   interrupts being generated.
            //
            // Call ALL IRQ handlers!  And hope for the best.
            // Remember, this may take a long time!
            // More requests may come in while this executes. :(

            irqreturn_t retval;
            uprintk ("Problem detected...\n");
            retval = call_all_interrupt_handlers ();
            if (retval != 0) { // Productive
                uprintk ("Productive interrupt resolved.\n");

                if (unproductive_interrupts == 0) {
                    crmod_timer_length /= 2;
                    if (crmod_timer_length < 1) {
                        crmod_timer_length = 1;
                    }
                } else {
                    unproductive_interrupts--;
                }
            } else {
                // Reset
                unproductive_interrupts++;
                if (unproductive_interrupts > max_unproductive_interrupts) {
                    clear_ref_bits ();
                    atomic_set (&interrupt_handler_called, 0);
                    unproductive_interrupts = 0;
                }
                
                crmod_timer_length *= 2;
                if (crmod_timer_length > HZ) {
                    crmod_timer_length = HZ;
                }
                uprintk ("Unproductive interrupt resolved\n");
            }
        }
        else {
            // No problem since the interrupt handler is working
            // still.
            uprintk ("Situation #2\n");
            
            // Reset
            clear_ref_bits ();
            atomic_set(&interrupt_handler_called, 0);
        }
    } else {
        // In this case, there is no problem pending at the moment.
        // Let's see if that's still the case.
        
        if (atomic_read (&interrupt_handler_called) != 0) {
            // Interrupt handler was called recently.
            
            if (ref_bits_set (0)) {
                // This happens all the time
                // Interrupt handler was called, no big deal.
                uprintk ("Situation #3\n");
            }
            else {
                dump_stack();
                uprintk ("interrupt_handler_called: %d\n", atomic_read (&interrupt_handler_called));
                //panic ("%s how is this possible?", __FUNCTION__);
            }

            // Reset
            atomic_set(&interrupt_handler_called, 0);
            clear_ref_bits ();
            unproductive_interrupts = 0;
            crmod_timer_length = default_timer_length;
        } else {
            // Interrupt handler not called
            if (ref_bits_set (0)) {
                // We start the count down.
                // The interrupt handler was not called, but
                // referenced bits are set.
                problem_pending += problem_pending_inc;
                uprintk ("Situation #4\n");
            }
            else {
                // Nothing, the driver is simply inactive.
                //uprintk ("#5\n");
            }
            
            // Don't reset the reference bits since there'd be
            // a small race condition.  If we reset here,
            // then some request might come in before this
            // function finishes executing, and we'd miss it.
        }
    }

    up (&timer_semaphore);

    // Reset everything.
    for (irq = 0; irq < CR_MAP_SIZE; irq++) {
        if (cr_irq_handlers[irq] != NULL) {
            //enable_irq (irq);
        }
    }
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
void cr_force_register (struct module *this_mod) {
    unsigned int remainder;
    cr_base_address[cr_num_drivers] = this_mod->module_core;
    cr_module_size[cr_num_drivers] = this_mod->core_size;

    remainder = cr_module_size[cr_num_drivers] % PAGE_SIZE;

    if (remainder != 0) {
        cr_module_size[cr_num_drivers] += PAGE_SIZE - remainder;
    }

    remainder = cr_module_size[cr_num_drivers] % PAGE_SIZE;
    if (remainder != 0) {
        panic ("Fix your arithmetic 1.\n");
    }

    remainder = (unsigned int) cr_base_address[cr_num_drivers] % PAGE_SIZE;
    if (remainder != 0) {
        panic ("Fix your arithmetic 2.\n");
    }    
    
    printk ("Module: %s, base address %p, size %lu, pages: %lu\n",
            this_mod->name,
            this_mod->module_core,
            this_mod->core_size,
            cr_module_size[cr_num_drivers] / PAGE_SIZE);
    
    cr_num_drivers++;
}

int cr_pci_register_driver (struct pci_driver *driver, struct module *this_mod) {
    cr_force_register (this_mod);
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

    //printk ("Interrupt handler was just called");
    atomic_inc (&interrupt_handler_called);
    if (atomic_read (&interrupt_handler_called) > MAX_INTERRUPT_FREQUENCY) {
        printk ("Stuck interrupt, disabling IRQ %d\n", irq);
        cr_disable_irq (irq); // disable the interrupt just like the kernel

        // Be sure we don't keep disabling the interrupt handler repeatedly.
        atomic_set (&interrupt_handler_called, 0);

        // At this point, we will never execute any of this code again.
        // The polling recovery mechanism should eventually realize
        // no interrupts are being generated, and will take over.
    }
    
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
    printk("%s\n", __FUNCTION__);
    if (cr_irq_handlers[irq] != NULL) {
        printk ("Already requested IRQ %d, overwriting...\n", irq);
    }

    if (irq >= CR_MAP_SIZE) {
        panic ("%s: map too small\n", __FUNCTION__); 
    }
    
    cr_irq_handlers[irq] = handler;
    cr_irq_parameters[irq] = dev_id;
    cr_irq_flags[irq] = flags;
    cr_irq_name[irq] = dev_name;

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
    cr_irq_parameters[irq] = NULL;
    cr_irq_flags[irq] = 0;
    cr_irq_name[irq] = NULL;

    free_irq (irq, dev_id);
}

// Do not call free_irq from interrupt context
static void cr_disable_irq (int irq) {
/*     struct irq_desc *desc = &irq_desc[irq]; */
    
/*     printk(KERN_EMERG "Disabling IRQ #%d\n", irq); */
/*     desc->status |= IRQ_DISABLED; */
/*     desc->depth = 1; */
/*     if (desc->chip->shutdown) */
/*         desc->chip->shutdown(irq); */
/*     else */
/*         desc->chip->disable(irq); */
    
    disable_irq_nosync (irq);
    //free_irq (irq, cr_irq_parameters[irq]);
}

static void cr_enable_irq (int irq) {
    enable_irq (irq);
/*     request_irq (irq, */
/*                  cr_irq_handlers[irq], */
/*                  cr_irq_flags[irq], */
/*                  cr_irq_name[irq], */
/*                  cr_irq_parameters[irq]); */
}

static void print_diagnostics (void) {
    int i;
    printk ("Number of drivers: %d\n", cr_num_drivers);
    for (i = 0; i < cr_num_drivers; i++) {
        printk ("Driver %d:\n", i);
        printk ("Base address: %p\n", cr_base_address[i]);
        printk ("Module size: %lx\n", cr_module_size[i]);
        printk ("\n");
    }
    printk ("\n");

    printk ("Interrupt handlers:\n");
    for (i = 0; i < CR_MAP_SIZE; i++) {
        if (cr_irq_handlers[i] != NULL) {
            printk ("Address: %p\n", cr_irq_handlers[i]);
            printk ("Parameters: %p\n", cr_irq_parameters[i]);
            printk ("Flags: %lx\n", cr_irq_flags[i]);
            printk ("Name: %s\n", cr_irq_name[i]);
        }
    }

    printk ("\n");
}

// Module initialization
EXPORT_SYMBOL (cr_force_register);
EXPORT_SYMBOL (cr_pci_register_driver);

// IRQ
EXPORT_SYMBOL (cr_request_irq);
EXPORT_SYMBOL (cr_free_irq);
