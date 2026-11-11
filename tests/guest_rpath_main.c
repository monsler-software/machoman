#include <stdio.h>
extern int parent_a(void), parent_b(void), exact_value(void);
int main(void) {
    int a=parent_a(),b=parent_b(),e=exact_value();
    if(a!=44||b!=22||e!=7) {fprintf(stderr,"runpath resolution: got %d %d %d, expected 44 22 7\n",a,b,e);return 1;}
    puts("dyld inherited runpaths, declaring image, exact tokens and candidate order passed");return 0;
}
