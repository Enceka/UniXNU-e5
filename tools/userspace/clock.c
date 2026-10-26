typedef long i64; typedef unsigned long u64;
static i64 sys6(i64 n,u64 a,u64 b,u64 c,u64 d,u64 e,u64 f){
    register i64 x0 asm("x0")=(i64)a, x1 asm("x1")=(i64)b, x2 asm("x2")=(i64)c,
                 x3 asm("x3")=(i64)d, x4 asm("x4")=(i64)e, x5 asm("x5")=(i64)f;
    asm volatile("mov x16,%0\n svc 0x80\n" ::"r"((i64)n),"r"(x0),"r"(x1),"r"(x2),"r"(x3),"r"(x4),"r"(x5):"x16","memory");
    register i64 r0 asm("x0"); return r0;
}
#define gettimeofday(tp,tz,mt) sys6(116,(u64)(tp),(u64)(tz),(u64)(mt),0,0,0)
#define setitimer(w,i,o) sys6(83,(u64)(w),(u64)(i),(u64)(o),0,0,0)
#define sigsuspend(m) sys6(111,(u64)(m),0,0,0,0,0)
#define write(f,b,n) sys6(4,(u64)(f),(u64)(b),(u64)(n),0,0,0)
#define exit(c) sys6(1,(u64)(c),0,0,0,0,0)
static void up(const char *s){ i64 n=0; while(s[n]) n++; write(229,s,n); write(1,s,n); }
static void upd(u64 v){ char b[24]; int i=23; b[i--]=0; do{ b[i--]='0'+v%10; v/=10; }while(v); up(" "); up(b+i+1); }
struct tv { i64 sec; i64 usec; };
void _start(void){
    static struct tv t1, t2;
    i64 r1 = gettimeofday(&t1, 0, 0);
    up("CLK|GTOD-RET"); upd(r1); up(" sec"); upd(t1.sec); up(" usec"); upd(t1.usec); up("\n");
    // ITIMER_REAL 100ms: it_interval=0 it_value=0.1s
    static struct { struct tv iv, val; } it = {{0,0},{0,100000}};
    i64 r2 = setitimer(0, &it, 0);
    up("CLK|ITIMER-RET"); upd(r2); up("\n");
    up("CLK|WAITING-ALRM...\n");
    static u64 emptymask[4];
    i64 r3 = sigsuspend(&emptymask[0]);
    up("CLK|ALRM-RCVD sigsuspend="); upd(r3); up("\n");
    gettimeofday(&t2, 0, 0);
    i64 dt = (t2.sec - t1.sec)*1000000 + (t2.usec - t1.usec);
    up("CLK|ELAPSED-US"); upd(dt); up("\n");
    exit(42);
}
