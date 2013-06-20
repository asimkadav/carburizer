DEBUG = y
EXTRA_CFLAGS += -fno-inline -O0 -g -Wall
OUTPUT_DIRECTORY=`pwd` "/bin"

# Fault injection module
obj-m := fimod.o

# Runtime module: used for calling the interrupt handler if necessary
obj-m += crmod.o

all: fi_control all_modules
	@echo "BUILD: fi_module built"

all_modules:
	make -C /scratch/fi/kernels/compiled_images/$(KERNEL_VERSION) M=`pwd` modules

fi_control: fi_control.o
	gcc fi_control.o -o fi_control

fi_control.o: fi_control.i
	gcc -c fi_control.i

fi_control.i: fi_control.c fi_mod_control.h
	gcc -E fi_control.c -o fi_control.i

clean:
	rm -f *.o *.ko *.mod.c Module.symvers
	rm -rf ./.tmp_versions
	rm -f \.*.cmd
	rm -f fi_control
