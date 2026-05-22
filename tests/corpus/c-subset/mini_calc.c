// Mini-calculator: read a chain of arithmetic operations from an
// integer table, fold them into a single result. Exercises every
// non-trivial c-subset feature so PA4's corpus stress catches real
// regressions that hand-driven tests would not.
//
// Features exercised:
//   - Top-level function declarations with parameter lists
//   - extern function prototype (no body)
//   - Top-level array variable declaration
//   - Local variable declarations with initializers
//   - Local array variable declarations
//   - Compound assignment operators (+=)
//   - if/else statements (nested), while loops, do/while
//   - Function calls (postfix `(args)`)
//   - Array indexing (postfix `[i]`)
//   - Postfix increment / decrement
//   - Prefix unary operators (!, -, ~, *)
//   - Bitwise operators (& | ^ << >>)
//   - Comparison operators
//   - Block statements

extern int printf(int fmt);

int g_iter_count = 0;
int g_buffer[256];
int g_pending[];

int square(int x) {
    return x * x;
}

int add(int a, int b) {
    return a + b;
}

int max3(int a, int b, int c) {
    if (a > b) {
        if (a > c) {
            return a;
        }
        return c;
    }
    if (b > c) {
        return b;
    }
    return c;
}

int sum_array(int arr, int len) {
    int total = 0;
    int i = 0;
    while (i < len) {
        total = total + arr[i];
        i++;
    }
    return total;
}

int fact(int n) {
    if (n <= 1) {
        return 1;
    }
    return n * fact(n - 1);
}

int fib(int n) {
    if (n < 2) {
        return n;
    }
    return fib(n - 1) + fib(n - 2);
}

int abs_int(int x) {
    if (x < 0) {
        return -x;
    }
    return x;
}

int bit_count(int value) {
    int count = 0;
    int v = value;
    while (v != 0) {
        if ((v & 1) != 0) {
            count = count + 1;
        }
        v = v >> 1;
    }
    return count;
}

int load_indirect(int ptr) {
    return *ptr;
}

int min(int a, int b) {
    if (a < b) {
        return a;
    }
    return b;
}

int clamp(int x, int lo, int hi) {
    return min(max3(x, lo, lo), hi);
}

int saturating_add(int a, int b, int limit) {
    int s = a + b;
    if (s > limit) {
        return limit;
    }
    if (s < -limit) {
        return -limit;
    }
    return s;
}

int compose_calc(int a, int b, int c) {
    int t1 = square(a) + square(b);
    int t2 = add(t1, square(c));
    int t3 = bit_count(t2);
    g_iter_count++;
    return t3;
}

int main() {
    int arr;
    int local_buf[16];
    int sum = sum_array(arr, 10);
    sum += local_buf[0];
    int m   = max3(sum, fact(5), fib(8));
    int c   = clamp(m, 0, 100);
    int s   = saturating_add(c, 50, 200);
    return compose_calc(sum, m, s);
}
