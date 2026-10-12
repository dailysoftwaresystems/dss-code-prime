/* The other unit's tentative definitions of the same two names (see main.c). */
int shared_counter;
int pair[10];

int bump(void) { return ++shared_counter; }
int last(void) {
    pair[9] = 20;
    return pair[9] + pair[0];
}
