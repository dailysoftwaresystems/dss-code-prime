int sub_add(int a, int b, int c) {
    return a - b + c;
}

int div_div(int a, int b, int c) {
    return a / b / c;
}

int sub_sub(int a, int b, int c) {
    return a - b - c;
}

int main() {
    int s = sub_add (50, 10, 2);
    int d = div_div(100, 5, 2);
    int t = sub_sub(20, 6, 4);
    return s - d - t + 20;
}
