int main(void) {
    thread_local int x = 1;
    for (thread_local int i = 0; i < 2; i = i + 1) {}
    return x;
}
