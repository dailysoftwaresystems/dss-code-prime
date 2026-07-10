constexpr thread_local int c = 5;
int main(void) {
    register thread_local int r = 1;
    return c + r;
}
