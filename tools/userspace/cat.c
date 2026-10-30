typedef long i64; typedef unsigned long u64;
static i64 sys6(i64 n,u64 a,u64 b,u64 c,u64 d,u64 e,u64 f){
    register u64 x0 __asm__("x0")=a, x1 __asm__("x1")=b, x2 __asm__("x2")=c,
                 x3 __asm__("x3")=d, x4 __asm__("x4")=e, x5 __asm__("x5")=f,
                 x16 __asm__("x16")=(u64)n;
    u64 failed;
    __asm__ volatile("svc #0x80\n\tcset %w[failed], cs"
        : [failed] "=&r"(failed), "+r"(x0), "+r"(x1)
        : "r"(x2), "r"(x3), "r"(x4), "r"(x5), "r"(x16) : "memory", "cc");
    return failed ? -(i64)x0 : (i64)x0;
}
#define open(p,f,m) sys6(5,(u64)(p),(u64)(f),(u64)(m),0,0,0)
#define read(f,b,n) sys6(3,(u64)(f),(u64)(b),(u64)(n),0,0,0)
#define write(f,b,n) sys6(4,(u64)(f),(u64)(b),(u64)(n),0,0,0)
#define exit(c) sys6(1,(u64)(c),0,0,0,0,0)
void e5_main(u64 *initial_sp){
    // Seed PID1 does not establish stdout/stderr before spawning tools.
    i64 console = open("/dev/console", 2, 0);
    if(console >= 0){
        sys6(90, (u64)console, 1, 0, 0, 0, 0);
        sys6(90, (u64)console, 2, 0, 0, 0, 0);
        if(console > 2) sys6(6, (u64)console, 0, 0, 0, 0, 0);
    }
    u64 sp = (u64)initial_sp;
    i64 argc = *(i64*)sp; char **argv = (char**)(sp+8);
    const char *path = argc > 1 ? argv[1] : "/sbin/init.cmd";
    i64 fd = open(path, 0, 0);
    if(fd < 0) exit(65);
    static char b[2048];
    i64 n, out = 1;
    while((n = read(fd, b, sizeof b)) > 0){
        i64 off = 0;
        while(off < n){
            i64 w = write(out, b+off, n-off);
            if(w <= 0) exit(66);
            off += w;
        }
    }
    exit(0);
}
