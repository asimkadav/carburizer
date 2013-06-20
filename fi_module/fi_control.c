#include "fi_mod_control.h"

#include <stdio.h>
#include <stdlib.h>
#include <errno.h>
#include <sys/ioctl.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>

static int fimod_fd = -1;
static int crmod_fd = -1;

#define FI_UNDEFINED 0
#define FI_FAULT 1
#define FI_COMMAND 2

static struct FAULT {
    int type;
    char fault_str[32];
} g_faults[FI_MAX_PARAMS];
static int NUM_FAULTS = 0;

#define INITIALIZE() {                      \
    int i;                                  \
    for (i = 0; i < FI_MAX_PARAMS; i++) {   \
        g_faults[i].type = FI_UNDEFINED;    \
    }                                       \
}

#define FAULT(str, num) {                             \
        g_faults[num].type = FI_FAULT;                \
        strcpy (g_faults[num].fault_str, str);        \
        NUM_FAULTS++;                                 \
    }

#define COMMAND(str, num) {                           \
        g_faults[num].type = FI_COMMAND;              \
        strcpy (g_faults[num].fault_str, str);        \
        NUM_FAULTS++;                                 \
    }

static int  cr_handle_param       (int current, int argc, char **argv);
static int  fi_handle_param       (int current, int argc, char **argv);
static void fi_enable_fault       (int current, int argc, char **argv, unsigned int bit);
static void fi_disable_fault      (int current, int argc, char **argv, unsigned int bit);
static void fi_dump_diagnostics   (int current, int argc, char **argv);
static void fi_specify_line       (int current, int argc, char **argv);
static void fi_specify_line_mode  (int current, int argc, char **argv);
static void fi_specify_line_force (int current, int argc, char **argv);
static void fi_specify_dma_timer  (int current, int argc, char **argv);
static void fi_command            (int index);

// Helper
static unsigned int fi_convert_probability (double probability);

int main (int argc, char **argv) {
    int ret;
    int i;
    int new_i;

    int fimod_err = 0, crmod_err = 0;

    // Enabling individual faults
    INITIALIZE();
    FAULT ("bitflips", FI_BITFLIPS);
    FAULT ("stuckbits", FI_STUCKBITS);
    FAULT ("dombits", FI_DOMBITS);
    FAULT ("extrairqs", FI_EXTRAIRQS);
    FAULT ("ignoredirqs", FI_IGNOREDIRQS);
    FAULT ("randomgarbage", FI_RANDOMGARBAGE);
    FAULT ("corrupt_iomemports", FI_CORRUPT_IOMEMPORTS);
    FAULT ("corrupt_dma", FI_CORRUPT_DMA);
    FAULT ("corrupt_usb", FI_CORRUPT_USB);

    // clearlines:  erase all tracked lines
    COMMAND("clearlines", FI_COMMAND_CLEAR_LINES);

    // verbose:  toggle verbose mode, i.e. print lots of messages
    COMMAND("verbose", FI_COMMAND_VERBOSE);

    // inonly:  toggle whether we do fault injection when writing
    //  data to the hardware, or only on the return path.
    //  e.g. if this is true, inb, inl, readl etc may be corrupted
    COMMAND("inonly", FI_COMMAND_IN_ONLY);

    // diag:  print diagnostic information/configuration
    COMMAND("diag", FI_COMMAND_DIAG);
    
    if (argc < 2) {
        printf ("======================================\n");
        printf ("fimod:\n");
        for (i = 0; i < FI_MAX_PARAMS; i++) {
            if (g_faults[i].type == FI_FAULT) {
                printf ("-enable_%s\n", g_faults[i].fault_str);
                printf ("-disable_%s\n", g_faults[i].fault_str);
            } else if (g_faults[i].type == FI_COMMAND) {
                printf ("-%s\n", g_faults[i].fault_str);
            } else if (g_faults[i].type == FI_UNDEFINED) {
                continue;
            } else {
                printf ("FAILURE");
                return 1;
            }
        }
        printf ("-line: Specify line\n");
        printf ("-line_mode: Specify include, exclude, ignore\n");
        printf ("-line_force: Specify line, value, and/or/set, probability, and total number\n");
        printf ("-dma_timer: Specify DMA timer rate\n");
        printf ("======================================\n");
        printf ("crmod:\n");
        printf ("-enable_irq <number>\n");
        printf ("-disable_irq <number>\n");
        printf ("-diag\n");
        printf ("======================================\n");
        
        printf ("Try that again.\n");
        return 1;
    }
    
    // Open the misc device
    fimod_fd = open ("/dev/fimod", O_RDONLY);
    crmod_fd = open ("/dev/crmod", O_RDONLY);
    
    if (fimod_fd != -1) {
        i = 1;
        for (;;) {
            if (i >= argc) {
                break;
            }
            
            new_i = fi_handle_param (i, argc, argv);
            
            if (new_i == i) {
                printf ("Bad parameter: %s\n", argv[i]);
                new_i++;
            }
            i = new_i;
        }
        
        ret = close (fimod_fd);
        if (ret != 0) {
            printf ("Failed to close fimod?  Error %d\n", ret);
        }
    } else {
        fimod_err = errno;
    }

    if (crmod_fd != -1) {
        i = 1;
        for (;;) {
            if (i >= argc) {
                break;
            }
            
            new_i = cr_handle_param (i, argc, argv);
            
            if (new_i == i) {
                printf ("Bad parameter: %s\n", argv[i]);
                new_i++;
            }
            i = new_i;
        }
        
        ret = close (crmod_fd);
        if (ret != 0) {
            printf ("Failed to close crmod?  Error %d\n", ret);
        }
    } else {
        crmod_err = errno;
    }

    if (crmod_fd == -1 && fimod_fd == -1) {
        printf ("Open crmod failed.  errno: %d\n", crmod_err);
        printf ("Open fimod failed.  errno: %d\n", fimod_err);
    }
    
    return 0;
}

static int cr_handle_param (int current, int argc, char **argv) {
    int i;
    int ret;

    ret = strcmp (argv[current], "-disable_irq");
    if (ret == 0) {
        int irq = atoi (argv[current + 1]);
        ioctl (crmod_fd, CR_DISABLE_IRQ, irq);
        current += 2;
        return current;
    }
    
    ret = strcmp (argv[current], "-enable_irq");
    if (ret == 0) {
        int irq = atoi (argv[current + 1]);
        ioctl (crmod_fd, CR_ENABLE_IRQ, irq);
        current += 2;
        return current;
    }

    ret = strcmp (argv[current], "-diag");
    if (ret == 0) {
        ioctl (crmod_fd, CR_COMMAND_DIAG, 0);
        current += 1;
        return current;
    }

    return current;
}

static int fi_handle_param (int current, int argc, char **argv) {
    int i;
    int ret;
    char temp_fault[256];

    ret = strcmp (argv[current], "-line");
    if (ret == 0) {
        fi_specify_line (current, argc, argv);
        current += 2;
        return current;
    }

    ret = strcmp (argv[current], "-line_mode");
    if (ret == 0) {
        fi_specify_line_mode (current, argc, argv);
        current += 2;
        return current;
    }

    ret = strcmp (argv[current], "-line_force");
    if (ret == 0) {
        fi_specify_line_force (current, argc, argv);
        current += 6;
        return current;
    }

    ret = strcmp (argv[current], "-dma_timer");
    if (ret == 0) {
        fi_specify_dma_timer (current, argc, argv);
        current += 2;
        return current;
    }

    for (i = 0; i < FI_MAX_PARAMS; i++) {
        if (g_faults[i].type == FI_FAULT) {
            strcpy (temp_fault, "-enable_");
            strcat (temp_fault, g_faults[i].fault_str);
            ret = strcmp (argv[current], temp_fault);
            if (ret == 0) {
                fi_enable_fault (current, argc, argv, i);
                current += 2;
                return current;
            }
            
            strcpy (temp_fault, "-disable_");
            strcat (temp_fault, g_faults[i].fault_str);
            ret = strcmp (argv[current], temp_fault);
            if (ret == 0) {
                fi_disable_fault (current, argc, argv, i);
                current++;
                return current;
            }
        } else if (g_faults[i].type == FI_COMMAND) {
            strcpy (temp_fault, "-");
            strcat (temp_fault, g_faults[i].fault_str);
            ret = strcmp (argv[current], temp_fault);
            if (ret == 0) {
                fi_command (i);
                current++;
                return current;
            }
        } else if (g_faults[i].type == FI_UNDEFINED) {
            continue;
        } else {
            printf ("FAILURE\n");
            exit (1);
        }
    }

    return current;
}

// Enable a fault and give it a parameter
static void fi_enable_fault (int current, int argc, char **argv, unsigned int fault) {
    if (current + 1 >= argc) {
        printf ("Specify the odds\n");
    }
    else {
        // Enable fault injection
        double probability = atof (argv[current + 1]);
        unsigned int odds = fi_convert_probability (probability);
        ioctl(fimod_fd, fault, odds);
    }
}

// Same ioctl, except 0 is the parameter
static void fi_disable_fault (int current, int argc, char **argv, unsigned int fault) {
    ioctl (fimod_fd, fault, 0);
}

static void fi_specify_line (int current, int argc, char **argv) {
    if (current + 1 >= argc) {
        printf ("Specify the line\n");
    }
    else {
        int line = atoi (argv[current + 1]);
        ioctl (fimod_fd, FI_TOGGLE_LINE, line);
    }
}

static void fi_specify_line_mode (int current, int argc, char **argv) {
    if (current + 1 >= argc) {
        printf ("Specify the line mode: ignore, include, exclude\n");
    }
    else {
        if (strcmp (argv[current + 1], "ignore") == 0) {
            ioctl (fimod_fd, FI_SELECTIVE_LINES, LINE_SELECTION_EXCLUDE);
        } else if (strcmp (argv[current + 1], "include") == 0) {
            ioctl (fimod_fd, FI_SELECTIVE_LINES, LINE_SELECTION_INCLUDE);
        } else if (strcmp (argv[current + 1], "exclude") == 0) {
            ioctl (fimod_fd, FI_SELECTIVE_LINES, LINE_SELECTION_EXCLUDE);
        } else {
            printf ("Specify one of: ignore, include, exclude\n");
        }
    }
}

static void fi_specify_line_force (int current, int argc, char **argv) {
    if (current + 1 >= argc) {
        printf ("Specify the line, the value to force, and the probability of forcing it\n");
    }
    else {
        double probability;
        int operation;
        struct line_force parameter;

        // Parameter 1
        parameter.line = atoi (argv[current + 1]);

        // Parameter 2
        parameter.value = strtoul (argv[current + 2], NULL, 10);

        // Parameter 3
        if (strcmp (argv[current + 3], "set") == 0) {
            parameter.operation = LINE_FORCE_SET;
        } else if (strcmp (argv[current + 3], "and") == 0) {
            parameter.operation = LINE_FORCE_AND;
        } else if (strcmp (argv[current + 3], "or") == 0) {
            parameter.operation = LINE_FORCE_OR;
        } else {
            printf ("Specify one of: set, and, or.  Defaulting to \"set\"\n");
            parameter.operation = LINE_FORCE_SET;
        }

        // Parameter 4
        probability = atof (argv[current + 4]);
        parameter.odds = fi_convert_probability (probability);

        // Parameter 5
        parameter.total_faults = strtoul (argv[current + 5], NULL, 10);

        // Send the data to the driver
        ioctl (fimod_fd, FI_FORCE_LINE, &parameter);
    }
}

static void fi_specify_dma_timer (int current, int argc, char **argv) {
    if (current + 1 >= argc) {
        printf ("Specify the timer rate, e.g. 50\n");
    }
    else {
        int rate = atoi (argv[current + 1]);
        ioctl (fimod_fd, FI_DMA_TIMER, rate);
    }
}

static void fi_command (int index) {
    ioctl (fimod_fd, index, 0);
}

//
// Helper functions
//
static unsigned int fi_convert_probability (double probability) {
    unsigned int odds;
    if (probability < 1 && probability >= 0) {
        probability *= 4294967296.0;
        odds = (unsigned int) probability;
    } else {
        odds = 4294967295U;
    }
    
    return odds;
}
