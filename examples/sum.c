// Adds 1..10 and reports the result (55) in a0.
//
// There is no OS yet (milestone 1), so there is no main() and no return:
// execution starts at _start, and the program stops itself with ecall.
void _start(void) {
    int sum = 0;
    for (int i = 1; i <= 10; i++) sum += i;

    register int a0 asm("a0") = sum;   // put the result in register a0
    asm volatile("ecall" :: "r"(a0));  // trap: the simulator stops here
    for (;;) {}                        // never reached
}
