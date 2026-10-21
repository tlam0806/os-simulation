// Adds 1..10 and exits with the result (55) as its exit status.
//
// There is no C library yet, so there is no main(): execution starts at
// _start, and the program ends itself with the exit system call (spec B3).
static void exit(int status) {
    register int a0 asm("a0") = status;   // argument: the exit status
    register int a7 asm("a7") = 1;        // system call number: exit
    asm volatile("ecall" :: "r"(a0), "r"(a7));
    for (;;) {}                           // exit never returns
}

void _start(void) {
    int sum = 0;
    for (int i = 1; i <= 10; i++) sum += i;
    exit(sum);
}
