#include <stdio.h>
#define HERMES_AUX_ENABLED 0
#define HERMES_CONTROL 0
#define hermes_read_reg ioread16


int ioread16()
{
    static int c = 0;
    return c++;
}

int main ()
{
   int * a;
   int *c;
   int *d;
   int b;
   a = ioread16();
	
   while (a==0) {;}
   c = a; 
   d = c + 2; 
   if (a != NULL)
     printf (*a);

   d = (int *) "safe";
   printf (*c);

   printf (*d);
   return 0;
}
