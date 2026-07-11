[[nodiscard]] int must_use(void) { return 1; }
int main(void) {
    must_use();
    (void)must_use();
    int kept = must_use();
    return kept - 1;
}
