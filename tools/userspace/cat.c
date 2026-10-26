typedef long i64; typedef unsigned long u64;
static i64 sys6(i64 n,u64 a,u64 b,u64 c,u64 d,u64 e,u64 f){
    register i64 x0 asm("x0")=(i64)a, x1 asm("x1")=(i64)b, x2 asm("x2")=(i64)c,
                 x3 asm("x3")=(i64)d, x4 asm("x4")=(i64)e, x5 asm("x5")=(i64)f;
    asm volatile("mov x16,%0\n svc 0x80\n" ::"r"((i64)n),"r"(x0),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"x16","memory");
    register i64 r0 asm("x0"); return r0;
}
#define open(p,f,m) sys6(5,(u64)(p),(u64)(f),(u64)(m),0,0,0)
#define read(f,b,n) sys6(3,(u64)(f),(u64)(b),(u64)(n),0,0,0)
#define write(f,b,n) sys6(4,(u64)(f),(u64)(b),(u64)(n),0,0,0)
#define exit(c) sys6(1,(u64)(c),0,0,0,0,0)
void _start(void){
    u64 sp; asm("mov %0, sp":"=r"(sp));
    i64 argc = *(i64*)sp; char **argv = (char**)(sp+8);
    if(argc < 2) exit(64);
    i64 fd = open(argv[1], 0, 0);
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
