/* Links lib.c's STATIC LIBRARY and runs both of its interior-addressing functions over every case. */
#include <stdio.h>

int lib_switch(int k);
int lib_computed_goto(int k);

int main(void) {
    int switched = 0;
    for (int k = -1; k <= 12; ++k) switched += lib_switch(k);
    int jumped = 0;
    for (int k = 0; k < 6; ++k) jumped += lib_computed_goto(k);
    printf("switch: %d goto: %d\n", switched, jumped);
    return switched == 1264 && jumped == 48 ? 42 : 1;
}
