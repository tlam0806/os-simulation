// Prints "ping" three times, with a busy loop between prints that is longer
// than one scheduler quantum. Run it together with another program to watch
// the two take turns:  simulation run build/ping.elf build/pong.elf
static int write(int fd, const char* buf, int len) {
    register int a0 asm("a0") = fd;
    register const char* a1 asm("a1") = buf;
    register int a2 asm("a2") = len;
    register int a7 asm("a7") = 2;   // write
    asm volatile("ecall" : "+r"(a0) : "r"(a1), "r"(a2), "r"(a7) : "memory");
    return a0;
}

static void exit(int status) {
    register int a0 asm("a0") = status;
    register int a7 asm("a7") = 1;   // exit
    asm volatile("ecall" :: "r"(a0), "r"(a7));
    for (;;) {}
}

void _start(void) {
    for (int i = 0; i < 3; i++) {
        write(1, "ping\n", 5);
        for (volatile int j = 0; j < 500; j++) {}   // longer than one quantum
    }
    exit(0);
}
