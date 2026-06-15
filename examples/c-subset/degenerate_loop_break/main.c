int once(int x) {
    while (1) {
        x = x + 5;
        break;
    }
    return x;
}

int main() {
    int acc = once(30);
    while (1) {
        acc = acc + 7;
        break;
    }
    return acc;
}
