#ifndef FAULT_INJECTION_H
#define FAULT_INJECTION_H

///////////////////////////////////////////////////////////////////////////////
// Do not specify these in the modified driver code.
#ifdef ENABLE_FAULT_INJECTION
#error "ENABLE_FAULT_INJECTION defined somewhere"
#endif

#ifdef ENABLE_CARB_RUNTIME
#error "ENABLE_CARB_RUNTIME defined somewhere"
#endif

// Choose one of these, or neither.
// Do NOT select both at the same time
#define ENABLE_FAULT_INJECTION
//#define ENABLE_CARB_RUNTIME

// Define this to wrap only basic read/write operations,
// and not anything else.  This is useful for testing recovery,
// with forced fault injection paths.  Without this, we'd also
// wrap lots of other functions and would confuse
// shadow drivers
#define ENABLE_FAULT_INJECTION_BASICSONLY
///////////////////////////////////////////////////////////////////////////////

#ifdef ENABLE_FAULT_INJECTION
#ifdef ENABLE_CARB_RUNTIME
#error "Define only one of the two preprocessor macros!"
#endif
#endif


#ifdef ENABLE_FAULT_INJECTION

// Hack so that this header works with CIL-processed files.
//#define __fi_iomem        __attribute__((noderef, address_space(2)))
#define __fi_iomem

///////////////////////////////////////////////////////////////////////////////
// New function prototypes
///////////////////////////////////////////////////////////////////////////////
// Old I/O memory accessors
unsigned int fi_readl (unsigned int LINE, void const volatile *addr);
unsigned short fi_readw (unsigned int LINE, void const volatile *addr);
unsigned char fi_readb (unsigned int LINE, void const volatile *addr);
void fi_writel (unsigned int LINE, unsigned int b, void volatile *addr);
void fi_writew (unsigned int LINE, unsigned short b, void volatile *addr);
void fi_writeb (unsigned int LINE, unsigned char b, void volatile *addr);

// Set 1
unsigned char fi_inb (unsigned int LINE, int port);
unsigned short fi_inw (unsigned int LINE, int port);
unsigned int fi_inl (unsigned int LINE, int port);
void fi_outb(unsigned int LINE, unsigned char value, int port);
void fi_outw(unsigned int LINE, unsigned short value, int port);
void fi_outl(unsigned int LINE, unsigned int value, int port);

// Set 2
unsigned char fi_inb_p (unsigned int LINE, int port);
unsigned short fi_inw_p (unsigned int LINE, int port);
unsigned int fi_inl_p (unsigned int LINE, int port);
void fi_outb_p(unsigned int LINE, unsigned char value, int port);
void fi_outw_p(unsigned int LINE, unsigned short value, int port);
void fi_outl_p(unsigned int LINE, unsigned int value, int port);

// Set 3
unsigned char fi_inb_local (unsigned int LINE, int port);
unsigned short fi_inw_local (unsigned int LINE, int port);
unsigned int fi_inl_local (unsigned int LINE, int port);
void fi_outb_local(unsigned int LINE, unsigned char value, int port);
void fi_outw_local(unsigned int LINE, unsigned short value, int port);
void fi_outl_local(unsigned int LINE, unsigned int value, int port);

// Set 4
unsigned char fi_inb_local_p (unsigned int LINE, int port);
unsigned short fi_inw_local_p (unsigned int LINE, int port);
unsigned int fi_inl_local_p (unsigned int LINE, int port);
void fi_outb_local_p(unsigned int LINE, unsigned char value, int port);
void fi_outw_local_p(unsigned int LINE, unsigned short value, int port);
void fi_outl_local_p(unsigned int LINE, unsigned int value, int port);

// New I/O memory + port accessors
unsigned int fi_ioread8(unsigned int LINE, void __fi_iomem *);
unsigned int fi_ioread16(unsigned int LINE, void __fi_iomem *);
unsigned int fi_ioread16be(unsigned int LINE, void __fi_iomem *);
unsigned int fi_ioread32(unsigned int LINE, void __fi_iomem *);
unsigned int fi_ioread32be(unsigned int LINE, void __fi_iomem *);

void fi_iowrite8(unsigned int LINE, u8, void __fi_iomem *);
void fi_iowrite16(unsigned int LINE, u16, void __fi_iomem *);
void fi_iowrite16be(unsigned int LINE, u16, void __fi_iomem *);
void fi_iowrite32(unsigned int LINE, u32, void __fi_iomem *);
void fi_iowrite32be(unsigned int LINE, u32, void __fi_iomem *);

void fi_ioread8_rep(unsigned int LINE, void __fi_iomem *port, void *buf, unsigned long count);
void fi_ioread16_rep(unsigned int LINE, void __fi_iomem *port, void *buf, unsigned long count);
void fi_ioread32_rep(unsigned int LINE, void __fi_iomem *port, void *buf, unsigned long count);
void fi_iowrite8_rep(unsigned int LINE, void __fi_iomem *port, const void *buf, unsigned long count);
void fi_iowrite16_rep(unsigned int LINE, void __fi_iomem *port, const void *buf, unsigned long count);
void fi_iowrite32_rep(unsigned int LINE, void __fi_iomem *port, const void *buf, unsigned long count);

// IRQ
int fi_request_irq (unsigned int irq, void *handler, unsigned long flags,
                    const char *dev_name, void *dev_id);
void fi_free_irq (unsigned int irq, void *dev_id);

// Ports and I/O memory (Subsumed by pci_resource_start)
void __fi_iomem * fi_ioremap (unsigned long offset, unsigned long size);
void fi_iounmap (volatile void __fi_iomem *addr);
unsigned int fi_pci_resource_start (void *pdev, int bar);
void *fi___request_region(void *, resource_size_t start,
                          resource_size_t n, const char *name);
void fi___release_region(void *, resource_size_t, resource_size_t);
void __fi_iomem *fi_ioport_map(unsigned long port, unsigned int nr);
void fi_ioport_unmap(void __fi_iomem *);

// DMA memory
void *fi_pci_alloc_consistent(int LINE, void *hwdev, size_t size, dma_addr_t *dma_handle);
void fi_pci_free_consistent(void *hwdev, size_t size, void *vaddr,
                            dma_addr_t dma_handle);
void *fi_dma_alloc_coherent(int LINE, void *dev, size_t size,
                            dma_addr_t *dma_handle, gfp_t flag);
void fi_dma_free_coherent(void *dev, size_t size,
                          void *vaddr, dma_addr_t dma_handle);
int fi_snd_dma_alloc_pages(int type, void *device, size_t size, void *dmab);
void fi_snd_dma_free_pages(void *dmab);
int fi_snd_pcm_lib_malloc_pages(void *substream, size_t size);
int fi_snd_pcm_lib_free_pages(void *substream);
unsigned long fi___get_free_pages(gfp_t gfp_mask, unsigned int order);
void fi_free_pages(unsigned long addr, unsigned int order);

// DMA pools
void *fi_dma_pool_create(const char *name, void *dev, size_t size,
                         size_t align, size_t allocation);
void fi_dma_pool_destroy(void *pool);
void *fi_dma_pool_alloc(void *pool, gfp_t mem_flags, dma_addr_t *handle);
void fi_dma_pool_free(void *pool, void *vaddr, dma_addr_t addr);

// USB drivers
int fi_usb_submit_urb(unsigned int LINE, void *urb, gfp_t mem_flags);
int fi_usb_control_msg(unsigned int LINE, void *dev, unsigned int pipe,
                       unsigned char request, unsigned char requesttype,
                       unsigned short value, unsigned short index,
                       void *data, unsigned short size, int timeout);
int fi_usb_interrupt_msg(unsigned int LINE, void *usb_dev, unsigned int pipe,
                         void *data, int len, int *actual_length, int timeout);
int fi_usb_bulk_msg (unsigned int LINE, void *usb_dev, unsigned int pipe,
                     void *data, int len, int *actual_length,
                     int timeout);

// Used for checking if a line of code is OK to use fault injection on or not.
int fi_verify_line(int line);

///////////////////////////////////////////////////////////////////////////////
// Macros to replace original functions with new functions
///////////////////////////////////////////////////////////////////////////////
// Old I/O memory functions
#define readl(addr)                  fi_readl(__LINE__, addr)
#define readw(addr)                  fi_readw(__LINE__, addr)
#define readb(addr)                  fi_readb(__LINE__, addr)
#define writel(addr, b)              fi_writel(__LINE__, addr, b)
#define writew(addr, b)              fi_writew(__LINE__, addr, b)
#define writeb(addr, b)              fi_writeb(__LINE__, addr, b)

// Set 1
#define inb(port)                    fi_inb(__LINE__, port)
#define inw(port)                    fi_inw(__LINE__, port)
#define inl(port)                    fi_inl(__LINE__, port)
#define outb(port, b)                fi_outb(__LINE__, port, b)
#define outw(port, b)                fi_outw(__LINE__, port, b)
#define outl(port, b)                fi_outl(__LINE__, port, b)

// Set 2
#define inb_p(port)                  fi_inb_p(__LINE__, port)
#define inw_p(port)                  fi_inw_p(__LINE__, port)
#define inl_p(port)                  fi_inl_p(__LINE__, port)
#define outb_p(port, b)              fi_outb_p(__LINE__, port, b)
#define outw_p(port, b)              fi_outw_p(__LINE__, port, b)
#define outl_p(port, b)              fi_outl_p(__LINE__, port, b)

// Set 3
#define inb_local(port)              fi_inb_local(__LINE__, port)
#define inw_local(port)              fi_inw_local(__LINE__, port)
#define inl_local(port)              fi_inl_local(__LINE__, port)
#define outb_local(port, b)          fi_outb_local(__LINE__, port, b)
#define outw_local(port, b)          fi_outw_local(__LINE__, port, b)
#define outl_local(port, b)          fi_outl_local(__LINE__, port, b)

// Set 4
#define inb_local_p(port)            fi_inb_local_p(__LINE__, port)
#define inw_local_p(port)            fi_inw_local_p(__LINE__, port)
#define inl_local_p(port)            fi_inl_local_p(__LINE__, port)
#define outb_local_p(port, b)        fi_outb_local_p(__LINE__, port, b)
#define outw_local_p(port, b)        fi_outw_local_p(__LINE__, port, b)
#define outl_local_p(port, b)        fi_outl_local_p(__LINE__, port, b)

// New I/O mem + port accessors
#define ioread8(port)                fi_ioread8(__LINE__, port)
#define ioread16(port)               fi_ioread16(__LINE__, port)
#define ioread16be(port)             fi_ioread16be(__LINE__, port)
#define ioread32(port)               fi_ioread32(__LINE__, port)
#define ioread32be(port)             fi_ioread32be(__LINE__, port)

#define iowrite8(port, b)          fi_iowrite8(__LINE__, port, b)
#define iowrite16(port, b)         fi_iowrite16(__LINE__, port, b)
#define iowrite16be(port, b)       fi_iowrite16be(__LINE__, port, b)
#define iowrite32(port, b)         fi_iowrite32(__LINE__, port, b)
#define iowrite32be(port, b)       fi_iowrite32be(__LINE__, port, b)

#define ioread8_rep(port, b, count)      fi_ioread8_rep(__LINE__, port, b, count)
#define ioread16_rep(port, b, count)     fi_ioread16_rep(__LINE__, port, b, count)
#define ioread32_rep(port, b, count)     fi_ioread32_rep(__LINE__, port, b, count)
#define iowrite8_rep(port, b, count)     fi_iowrite8_rep(__LINE__, port, b, count)
#define iowrite16_rep(port, b, count)    fi_iowrite16_rep(__LINE__, port, b, count)
#define iowrite32_rep(port, b, count)    fi_iowrite32_rep(__LINE__, port, b, count)

#ifndef ENABLE_FAULT_INJECTION_BASICSONLY

// IRQ
#define request_irq(irq, handler, flags, dev_name, dev_id)  fi_request_irq(irq, handler, flags, dev_name, dev_id)
#define free_irq                 fi_free_irq

// Ports and I/O memory
#define ioremap                  fi_ioremap
#define iounmap                  fi_iounmap
#undef pci_resource_start // eliminate warnings about macro redefinition
#define pci_resource_start       fi_pci_resource_start
#define __request_region         fi___request_region
#define __release_region         fi___release_region
#define ioport_map               fi_ioport_map
#define ioport_unmap             fi_ioport_unmap

// DMA memory
#define pci_alloc_consistent(hwdev, size, dma_handle) \
    fi_pci_alloc_consistent (__LINE__, hwdev, size, dma_handle)
#define pci_free_consistent      fi_pci_free_consistent
#define dma_alloc_coherent(dev, size, dma_handle, flag) \
    fi_dma_alloc_coherent(__LINE__, dev, size, dma_handle, flag)
    
#define dma_free_coherent        fi_dma_free_coherent
#define snd_dma_alloc_pages      fi_snd_dma_alloc_pages
#define snd_dma_free_pages       fi_snd_dma_free_pages
#define snd_pcm_lib_malloc_pages fi_snd_pcm_lib_malloc_pages
#define snd_pcm_lib_free_pages   fi_snd_pcm_lib_free_pages
#define __get_free_pages         fi___get_free_pages
#define free_pages               fi_free_pages

// DMA pools
#define dma_pool_create          fi_dma_pool_create
#define dma_pool_destroy         fi_dma_pool_destroy
#define dma_pool_alloc           fi_dma_pool_alloc
#define dma_pool_free            fi_dma_pool_free

// USB Drivers
#define usb_submit_urb(urb, mem_flags) \
        fi_usb_submit_urb(__LINE__, urb, mem_flags)

#define usb_control_msg(dev, pipe, request, requesttype, value, index, data, size, timeout) \
        fi_usb_control_msg(__LINE__, dev, pipe, request, requesttype, value, index, data, size, timeout)

#define usb_interrupt_msg(usbdev, pipe, data, len, actual_length, timeout) \
        fi_usb_interrupt_msg(__LINE__, usbdev, pipe, data, len, actual_length, timeout)

#define usb_bulk_msg(usbdev, pipe, data, len, actual_length, timeout) \
        fi_usb_bulk_msg(__LINE__, usbdev, pipe, data, len, actual_length, timeout)

#endif // #ifndef ENABLE_FAULT_INJECTION_BASICSONLY

#endif // #ifdef ENABLE_FAULT_INJECTION




#ifdef ENABLE_CARB_RUNTIME

///////////////////////////////////////////////////////////////////////////////
// New function prototypes
///////////////////////////////////////////////////////////////////////////////

// Module initialization
void cr_force_register(void *this_mod);
int cr_pci_register_driver(void *driver, void *this_mod);

// IRQ
int cr_request_irq (unsigned int irq, void *handler, unsigned long flags,
                    const char *dev_name, void *dev_id);
void cr_free_irq (unsigned int irq, void *dev_id);

// Module initialization
// Just to make sure it's there. (CIL preprocessor issue):
extern struct module __this_module;
#define pci_register_driver(driver)                         cr_pci_register_driver(driver, &__this_module)
#define __pci_register_driver(driver, module)               cr_pci_register_driver(driver, module)

// IRQ
#define request_irq(irq, handler, flags, dev_name, dev_id)  cr_request_irq(irq, handler, flags, dev_name, dev_id)
#define free_irq(irq, dev_id)                               cr_free_irq(irq, dev_id)

#else // ENABLE_CARB_RUNTIME

// This function is specifically intended to be called manually
// in cases where pci_register_driver doesn't cut it.
// If we disable the runtime, we also need to remove this
// function call if it's present.
#define cr_force_register(this_mod) /* None */

#endif // ENABLE_CARB_RUNTIME

#endif // FAULT_INJECTION_H
