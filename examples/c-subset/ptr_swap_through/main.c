void swap(int* a, int* b) {
    int t;
    t = *a;
    *a = *b;
    *b = t;
}

int main() {
    int x;
    int y;
    x = 2;
    y = 40;
    swap(&x, &y);
    return x - y + 4;
}
