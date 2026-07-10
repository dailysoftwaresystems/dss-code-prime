int main(void) {
    const char *x;
    x = __func__;
    __func__ = x;
    return 0;
}
