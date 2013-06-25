# Automatically adds CIL analysis in Makefiles. Requires an additioanl manual pruning step. 
#

find . -name "Makefile" -maxdepth 2 -mindepth 2 | xargs sed -i '1i\CC=cilly --dodrivers \nEXTRA_CFLAGS+= --save-temps --dodrivers -D HAPPY_MOOD -DCILLY_DONT_COMPILE_AFTER_MERGE -DCILLY_DONT_LINK_AFTER_MERGE -I myincludes' -i $1
find . -name "Makefile" -mindepth 3 | xargs sed -i '1i\CC=cilly --dodrivers \nEXTRA_CFLAGS+= --save-temps --dodrivers -D HAPPY_MOOD -DCILLY_DONT_COMPILE_AFTER_MERGE -DCILLY_DONT_LINK_AFTER_MERGE -I myincludes\nLD=cilly --merge --dodrivers\nAR=cilly --merge --mode=AR' $1

