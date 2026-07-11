[[deprecated("use replacement")]] int old_api(void) { return 1; }
int main(void) {
    int a = old_api();
    return a + old_api();
}
