// mini_os.cpp — mô phỏng CPU + RAM + MMU + kernel nhỏ trong ~150 dòng.
// Build: g++ -std=c++17 -O2 mini_os.cpp -o mini_os && ./mini_os
//
// Ý tưởng cốt lõi: "phần cứng" chỉ là biến C++.
//   - RAM vật lý   = một vector<uint8_t>
//   - CPU          = vài thanh ghi + vòng lặp fetch/decode/execute
//   - MMU          = hàm translate(): địa chỉ ảo -> địa chỉ vật lý qua page table
//   - Kernel       = các hàm C++ mà CPU "nhảy vào" khi có trap (syscall, page fault)
//   - Chương trình = các byte nằm trong RAM, CPU đọc và chạy từng lệnh
#include <cstdint>
#include <cstdio>
#include <unordered_map>
#include <vector>

constexpr int PAGE = 256;   // page nhỏ để dễ thấy
constexpr int FRAMES = 8;   // RAM vật lý = 8 * 256 = 2 KB

// ---------------- "Phần cứng" ----------------
std::vector<uint8_t> phys(FRAMES * PAGE);   // RAM vật lý
int next_free_frame = 0;

enum Op : uint8_t { HALT, LOADI, LOAD, STORE, ADDI, JNZ, SYSCALL };
// Mỗi lệnh 4 byte: [op][reg][imm lo][imm hi]

struct Process {
    int pid;
    uint32_t reg[4] = {0};
    uint16_t pc = 0;
    std::unordered_map<uint16_t, int> page_table;   // virtual page -> frame
    bool alive = true;
};

struct Fault { uint16_t vaddr; };   // CPU "trap" vào kernel khi page fault

// MMU: mọi truy cập memory của chương trình đều đi qua đây.
uint32_t translate(Process& p, uint16_t vaddr) {
    uint16_t vpage = vaddr / PAGE, offset = vaddr % PAGE;
    auto it = p.page_table.find(vpage);
    if (it == p.page_table.end()) throw Fault{vaddr};   // không có mapping -> page fault
    return it->second * PAGE + offset;
}

// ---------------- Kernel ----------------
constexpr uint16_t HEAP_LIMIT = 0x0800;   // vùng hợp lệ của process: [0, 0x800)
int page_faults = 0;

// Page fault handler: hợp lệ thì cấp frame mới (demand paging), không thì segfault.
void kernel_page_fault(Process& p, uint16_t vaddr) {
    page_faults++;
    if (vaddr >= HEAP_LIMIT || next_free_frame >= FRAMES) {
        printf("  [kernel] pid %d: SEGFAULT at 0x%04x -> kill process\n", p.pid, vaddr);
        p.alive = false;
        return;
    }
    int frame = next_free_frame++;
    p.page_table[vaddr / PAGE] = frame;
    printf("  [kernel] page fault at 0x%04x -> map vpage %d to frame %d\n",
           vaddr, vaddr / PAGE, frame);
}

// Syscall handler: reg[0] = số syscall, reg[1] = tham số.
void kernel_syscall(Process& p) {
    switch (p.reg[0]) {
        case 1: printf("  [syscall write] pid %d prints %u\n", p.pid, p.reg[1]); break;
        case 2: printf("  [syscall exit]  pid %d exits\n", p.pid); p.alive = false; break;
    }
}

// ---------------- CPU ----------------
uint8_t  read8 (Process& p, uint16_t a) { return phys[translate(p, a)]; }
uint32_t read32(Process& p, uint16_t a) { uint32_t v = 0; for (int i = 0; i < 4; i++) v |= read8(p, a + i) << (8 * i); return v; }
void     write32(Process& p, uint16_t a, uint32_t v) { for (int i = 0; i < 4; i++) phys[translate(p, a + i)] = v >> (8 * i); }

// Chạy một lệnh. Nếu có fault, kernel xử lý rồi CHẠY LẠI đúng lệnh đó (pc không tăng).
void step(Process& p) {
    try {
        uint8_t op = read8(p, p.pc), r = read8(p, p.pc + 1);
        uint16_t imm = read8(p, p.pc + 2) | (read8(p, p.pc + 3) << 8);
        uint16_t next = p.pc + 4;
        switch (op) {
            case HALT:    p.alive = false; break;
            case LOADI:   p.reg[r] = imm; break;
            case LOAD:    p.reg[r] = read32(p, imm); break;
            case STORE:   write32(p, imm, p.reg[r]); break;
            case ADDI:    p.reg[r] += (int16_t)imm; break;
            case JNZ:     if (p.reg[r]) next = imm; break;
            case SYSCALL: kernel_syscall(p); break;   // user mode -> kernel mode
        }
        p.pc = next;
    } catch (Fault f) {
        kernel_page_fault(p, f.vaddr);
    }
}

// ---------------- Loader ----------------
// Kernel nạp chương trình vào page 0 của process (giống exec()).
void load_program(Process& p, const std::vector<uint8_t>& code) {
    int frame = next_free_frame++;
    p.page_table[0] = frame;
    for (size_t i = 0; i < code.size(); i++) phys[frame * PAGE + i] = code[i];
}

#define I(op, r, imm) op, r, (uint8_t)((imm) & 0xff), (uint8_t)((imm) >> 8)

int main() {
    // Chương trình: đếm ngược 3 -> 1, lưu biến đếm ở 0x0400 (chưa map -> page fault),
    // in ra bằng syscall, rồi cố đọc 0x1000 (ngoài vùng hợp lệ -> segfault).
    std::vector<uint8_t> code = {
        I(LOADI, 2, 3),          // 0x00: r2 = 3
        I(STORE, 2, 0x0400),     // 0x04: mem[0x400] = r2      <- page fault lần đầu
        I(LOAD, 1, 0x0400),      // 0x08: r1 = mem[0x400]
        I(LOADI, 0, 1),          // 0x0c: r0 = 1 (write)
        I(SYSCALL, 0, 0),        // 0x10: write(r1)
        I(ADDI, 2, 0xffff),      // 0x14: r2 -= 1
        I(JNZ, 2, 0x04),         // 0x18: nếu r2 != 0 quay lại 0x04
        I(LOAD, 3, 0x1000),      // 0x1c: đọc địa chỉ không hợp lệ <- segfault
        I(LOADI, 0, 2),          // 0x20: exit (không bao giờ tới)
        I(SYSCALL, 0, 0),
    };

    Process p{1};
    load_program(p, code);
    int cycles = 0;
    while (p.alive) { step(p); cycles++; }
    printf("done: %d cycles, %d page faults, %zu pages mapped\n",
           cycles, page_faults, p.page_table.size());
}
