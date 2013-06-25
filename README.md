carburizer
==========

Carburizer is a tool that removes/finds hardware dependence bugs.

Carburizer is a tool that removes hardware dependence bugs. More details here. This page demonstrates how to get carburizer (the static analysis component) working on your local machine.

Software Required
=================

The following software is required to run Carburizer:
 - Any Linux OS
 - Ocaml (3.08 or higher). Download from here or install it from within your distribution using yum or apt-get. 
 - CIL (in /cil in this git) 

Steps to install CIL
====================

- Install OCaml from your linux distribution or download from here. Make sure ocaml is in your path.
- Run "ocaml -version". It should return 3.08 or higher.
- Install CIL from here. Download, untar the above file and run: 

  ./configure 
  make        
  make install 

This will generate the executable cilly in cil/bin/cilly. Ensure this executable is in path. Now we are ready to test drivers for hardware dependence bugs using Carburizer. Carburizer modifies cilly to introduce a new -dodrivers flag to test drivers for hardware dependence bugs.

To test any driver, locate the corresponding Makefile for the driver and add the following lines:

CC=cilly --dodrivers
EXTRA_CFLAGS+=  --save-temps --dodrivers -I myincludes
LD=cilly --dodrivers
AR=cilly --mode=AR

Now build the driver, using the command make. This should show list of hardware dependence bugs and generate a hardened binary. 

Drivers with multiple object files
==================================

If you want to run carburizer over drivers that consist of multiple files, like e1000. Add the following lines to your top-level Makefile. For example, drivers/net/e1000/Makfile.

CC=cilly --merge --dodrivers
EXTRA_CFLAGS+= --save-temps --dodrivers -D HAPPY_MOOD -DCILLY_DONT_COMPILE_AFTER_MERGE -DCILLY_DONT_LINK_AFTER_MERGE -I myincludes
LD=cilly --merge --dodrivers
AR=cilly --merge --mode=AR

These lines run carburizer analysis on the combined file. This enables taint propogation across different files in a driver module.

Contact

Please email me(kadav in the domain of  cs.wisc.edu)  for any questions about Carburizer.
