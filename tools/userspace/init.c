// E5 XNU init v2 — batch harness: read /sbin/init.cmd, spawn each line, uplink report
typedef long long i64; typedef unsigned long long u64; typedef unsigned char u8;

static i64 sys6(u64 n,u64 a,u64 b,u64 c,u64 d,u64 e,u64 f){
    register u64 x0 __asm__("x0")=a, x1 __asm__("x1")=b, x2 __asm__("x2")=c,
                  x3 __asm__("x3")=d, x4 __asm__("x4")=e, x5 __asm__("x5")=f,
                  x16 __asm__("x16")=n;
    __asm__ volatile("svc #0x80":"+r"(x0):"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5),"r"(x16):"memory","cc");
    return (i64)x0;
}
#define exit(v)     sys6(1,(u64)(v),0,0,0,0,0)
#define read(f,b,n) sys6(3,(u64)(f),(u64)(b),(u64)(n),0,0,0)
#define write(f,b,n) sys6(4,(u64)(f),(u64)(b),(u64)(n),0,0,0)
#define open(p,f,m) sys6(5,(u64)(p),(u64)(f),(u64)(m),0,0,0)
#define openat(d,p,f,m) sys6(463,(u64)(d),(u64)(p),(u64)(f),(u64)(m),0,0)
#define close(f) sys6(6,(u64)(f),0,0,0,0,0)
#define wait4(p,st,o,r) sys6(7,(u64)(p),(u64)(st),(u64)(o),(u64)(r),0,0)
#define posix_spawn(pidp,path,fa,attr,av,ev) sys6(244,(u64)(pidp),(u64)(path),(u64)(fa),(u64)(attr),(u64)(av),(u64)(ev))

static u64 mystrlen(const char *s){ u64 n=0; while(s[n]) n++; return n; }
void *memcpy(void *d,const void *s,u64 n){u8*dd=d;const u8*ss=s;while(n--)*dd++=*ss++;return d;}
void *memset(void *d,int c,u64 n){u8*dd=d;while(n--)*dd++=(u8)c;return d;}

static void uplink(const char *s){
    sys6(4,229,(u64)s,mystrlen(s),0,0,0);
}
static char buf[256];
static void uplink_dec(const char *tag, i64 v){
    char *e=buf+254; *e=0; u64 u = v<0 ? -(u64)v : (u64)v;
    char b[24]; int i=0;
    do{ b[i++]='0'+(u%10); u/=10; }while(u);
    while(i) *--e=b[--i];
    if(v<0) *--e='-';
    char *t=buf;
    // append: tag then number
    const char *p=tag; while(*p) *t++=*p++;
    char *q=e; while(*q) *t++=*q++;
    *t++='\n'; *t=0;
    uplink(buf);
}

static void uplink_hex(const char *tag, u64 v){
    static const char *hxd="0123456789abcdef";
    char *t=buf; while(*t=*tag++) t++;
    int started=0;
    for(int s=60;s>=0;s-=4){ int d=(v>>s)&15; if(d||started||s==0){ *t++=hxd[d]; started=1; } }
    *t++=0x0a; *t++=0;
    uplink(buf);
}
static char cmdbuf[4096];
static char *lines[64];

int main(void){
    uplink("INIT-START\n");
    {   // probe: read /sbin/launchd magic + init.cmd twice
        static const char *hx="0123456789abcdef";
        i64 fdx = open("/sbin/launchd", 0, 0);
        uplink_dec("LD-FD ", fdx);
        if(fdx >= 0){
            i64 rn = read(fdx, cmdbuf, 32);
            uplink_dec("LD-N ", rn);
            char hb[40]; int hn=0;
            for(int i=0;i<8 && i<rn;i++){ hb[hn++]=' '; hb[hn++]=hx[(cmdbuf[i]>>4)&15]; hb[hn++]=hx[cmdbuf[i]&15]; }
            hb[hn]='\n'; hb[hn+1]=0; uplink(hb);
            close(fdx);
        }
        i64 fdc = open("/sbin/init.cmd", 0, 0);
        uplink_dec("CMD-FD ", fdc);
        if(fdc >= 0){
            i64 r1 = read(fdc, cmdbuf, 64);
            uplink_dec("C1-N ", r1);
            char hb[40]; int hn=0;
            for(int i=0;i<8 && i<r1;i++){ hb[hn++]=' '; hb[hn++]=hx[(cmdbuf[i]>>4)&15]; hb[hn++]=hx[cmdbuf[i]&15]; }
            hb[hn]='\n'; hb[hn+1]=0; uplink(hb);
            i64 r2 = read(fdc, cmdbuf, 64);
            uplink_dec("C2-N ", r2);
            close(fdc);
        }
    }
    i64 fd = open("/sbin/init.cmd", 0, 0);
    if(fd < 0){ uplink_dec("NO-CMDFILE ", fd); exit(6); }
    i64 n = read(fd, cmdbuf, sizeof(cmdbuf)-1);
    close(fd);
    uplink_dec("READ-N ", n);
    if(n <= 0){ uplink_dec("CMD-EMPTY ", n); exit(6); }
    cmdbuf[n] = 0;
    {   // hex dump first 16 bytes
        static const char *hx="0123456789abcdef";
        char hb[40]; int hn=0;
        for(int i=0;i<16 && i<n;i++){ hb[hn++]=' '; hb[hn++]=hx[(cmdbuf[i]>>4)&15]; hb[hn++]=hx[cmdbuf[i]&15]; }
        hb[hn]='\n'; hb[hn+1]=0; uplink(hb);
    }
    int nl = 0;
    lines[nl++] = cmdbuf;
    for(char *q = cmdbuf; *q; q++){
        if(*q == '\n'){ *q = 0; if(nl < 64 && q[1]) lines[nl++] = q+1; }
    }
    uplink_dec("CMD-LINES ", nl);
    for(int i = 0; i < nl; i++){
        if(!lines[i][0] || lines[i][0] == '#') continue;
        uplink("RUN|"); uplink(lines[i]); uplink("\n");
        // split on spaces into argv slots
        char *argv[9]; int ac = 0;
        char *s = lines[i];
        while(*s && ac < 8){
            while(*s == ' ') *s++ = 0;
            if(*s) argv[ac++] = s;
            while(*s && *s != ' ') s++;
        }
        argv[ac] = 0;
        i64 pid = 0;
        i64 r = posix_spawn(&pid, lines[i], 0, 0, argv, 0);
        if(r != 0){ uplink_dec("SPAWN-FAIL ", r); continue; }
        i64 st = 0;
        i64 w = wait4(-1, &st, 0, 0);
        uplink_dec("EXIT|", (w>=0 && st>=0) ? (st>>8) : w);
        uplink_hex("EXITH|", (w>=0 && st>=0) ? (u64)st : (u64)w);
    }
    uplink("INIT-DONE\n");
    // ---- persistent shell loop: on-screen terminal via e5-keys/e5-fb ----
    // fd 230 write renders on the panel (kernel e5-fb hook mirrors fd 1/2
    // too, so spawned tools print on the screen); fd 230 read polls the
    // matrix keypad (kernel e5-keys hook).  fd 229 stays the serial uplink
    // for remote logs.  Line editing: back erases, select submits.
    uplink("SH|READY screen fd230\n");
    write(230, "E5 XNU seed shell\n", 18);
    static char line[256];
    int alive = 1;
    while(alive){
        write(230, "E5# ", 4);
        int len = 0;
        for(;;){
            char c = 0;
            i64 n = sys6(3, 230, (u64)&c, 1, 0, 0, 0);
            if(n <= 0){ uplink("SH|READ-END\n"); alive = 0; break; }
            if(c == '\n' || c == '\r'){ break; }
            if(c == '\b' || c == 0x7f){
                if(len > 0){ len--; write(230, "\b \b", 3); }
                continue;
            }
            if(c < 0x20){ continue; }
            if(len < 255){ line[len++] = c; write(230, &c, 1); }
        }
        if(alive == 0){ break; }
        write(230, "\n", 1);
        line[len] = 0;
        uplink("SH|GOT "); uplink(line); uplink("\n");
        char *argv[9]; int ac = 0;
        char *s = line;
        while(*s && ac < 8){
            while(*s == ' ') *s++ = 0;
            if(*s) argv[ac++] = s;
            while(*s && *s != ' ') s++;
        }
        argv[ac] = 0;
        if(ac == 0) continue;
        i64 pid = 0;
        i64 r = posix_spawn(&pid, argv[0], 0, 0, argv, 0);
        if(r){
            uplink_dec("SH|SPAWN-FAIL ", r);
            write(230, "spawn failed\n", 13);
            continue;
        }
        i64 st = 0;
        i64 w = wait4(-1, &st, 0, 0);
        uplink_dec("SH|EXIT|", (w >= 0) ? (st >> 8) : w);
    }
    // never exit: keep PID1 alive forever
    static u64 m[4];
    for(;;) sys6(111,(u64)&m[0],0,0,0,0,0);
}
