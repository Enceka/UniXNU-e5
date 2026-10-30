typedef long i64; typedef unsigned long u64; typedef unsigned short u16; typedef unsigned char u8;
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
#define close(f) sys6(6,(u64)(f),0,0,0,0,0)
#define getdirentries64(f,b,n,p) sys6(344,(u64)(f),(u64)(b),(u64)(n),(u64)(p),0,0)
#define write(f,b,n) sys6(4,(u64)(f),(u64)(b),(u64)(n),0,0,0)
#define exit(c) sys6(1,(u64)(c),0,0,0,0,0)
static void up(const char *s){ i64 n=0; while(s[n]) n++; write(229,s,n); write(1,s,n); }
static void uph(u64 v){ static const char *hx="0123456789abcdef"; char b[20]; int i=0;
    for(int s=60;s>=0;s-=4) b[i++]=hx[(v>>s)&15]; b[i]=0; up(b); }
static void upd(u64 v){ char b[24]; int i=23; b[i--]=0; do{ b[i--]='0'+v%10; v/=10; }while(v); up(" "); up(b+i+1); }
void e5_main(u64 *initial_sp){
    // Seed PID1 does not establish stdout/stderr before spawning tools.
    i64 console = open("/dev/console", 2, 0);
    if(console >= 0){
        sys6(90, (u64)console, 1, 0, 0, 0, 0);
        sys6(90, (u64)console, 2, 0, 0, 0, 0);
        if(console > 2) sys6(6, (u64)console, 0, 0, 0, 0, 0);
    }
    u64 sp = (u64)initial_sp;
    up("LS|START sp="); uph(sp);
    i64 argc = *(i64*)sp; char **argv = (char**)(sp+8);
    up(" argc="); uph((u64)argc);
    up(" a0="); uph((u64)argv[0]);
    up(" a1="); uph((u64)argv[1]);
    up(" a0s="); up(argv[0] ? argv[0] : "(null)");
    up(" a1s="); up(argv[1] ? argv[1] : "(null)");
    up("\n");

    const char *path = argc > 1 ? argv[1] : "/bin";
    i64 fd = open(path, 0, 0);
    up("LS|open fd="); uph((u64)fd); up("\n");

    if(fd < 0){ up("LS|open-fail\n"); exit(65); }
    up("LS|"); up(path); up(":");
    static char buf[4096];
    static i64 pos = 0;
    for(;;){
        i64 n = getdirentries64(fd, buf, sizeof buf, &pos);
        up("LS|gde n="); uph((u64)n); up("\n");
        if(n <= 0) break;
        i64 off = 0;
        while(off < n){
            char *e = buf + off;
            u16 reclen = *(u16*)(e+16);
            u16 namlen = *(u16*)(e+18);
            u8  type  = *(u8*)(e+20);
            up("\nLS| ");
            if(type == 4) up("DIR  "); else up("FILE ");
            for(int i=0;i<namlen;i++){ char c[2]={e[21+i],0}; up(c); }
            upd(*(u64*)(e)); // inode
            if(!reclen) break;
            off += reclen;
        }
    }
    up("\n");
    close(fd);
    exit(0);
}
