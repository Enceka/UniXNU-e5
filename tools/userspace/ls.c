typedef long i64; typedef unsigned long u64; typedef unsigned short u16; typedef unsigned char u8;
static i64 sys6(i64 n,u64 a,u64 b,u64 c,u64 d,u64 e,u64 f){
    register i64 x0 asm("x0")=(i64)a, x1 asm("x1")=(i64)b, x2 asm("x2")=(i64)c,
                 x3 asm("x3")=(i64)d, x4 asm("x4")=(i64)e, x5 asm("x5")=(i64)f;
    asm volatile("mov x16,%0\n svc 0x80\n" ::"r"((i64)n),"r"(x0),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"x16","memory");
    register i64 r0 asm("x0"); return r0;
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
void _start(void){
    u64 sp; asm("mov %0, sp":"=r"(sp));
    up("LS|START sp="); uph(sp);
    i64 argc = *(i64*)sp; char **argv = (char**)(sp+8);
    up(" argc="); uph((u64)argc);
    up(" a0="); uph((u64)argv[0]);
    up(" a1="); uph((u64)argv[1]);
    up(" a0s="); up(argv[0] ? argv[0] : "(null)");
    up(" a1s="); up(argv[1] ? argv[1] : "(null)");
    up("\n");

    if(argc < 2) exit(64);
    i64 fd = open(argv[1], 0, 0);
    up("LS|open fd="); uph((u64)fd); up("\n");

    if(fd < 0){ up("LS|open-fail\n"); exit(65); }
    up("LS|"); up(argv[1]); up(":");
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
