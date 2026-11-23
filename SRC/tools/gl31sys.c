/*
 * gl31sys.c - enable OpenGL for every program on Windows NT 3.1, system wide.
 *
 * Windows NT 3.1's GDI32.DLL has none of the OpenGL pixel-format or buffer
 * functions (ChoosePixelFormat, SetPixelFormat, DescribePixelFormat,
 * GetPixelFormat, SwapBuffers) - they arrived in NT 3.5 - so an OpenGL
 * program fails to load. The gl31pat tool fixes one program at a time. This
 * tool instead adds the five functions to GDI32.DLL itself, as forwarders to
 * WINVBE's OPENGL32.DLL, so that afterwards every OpenGL program loads and
 * runs with no per-program patching.
 *
 * It appends a rebuilt export table to GDI32.DLL (the code is untouched; only
 * five exports are added) and fixes the PE checksum that NT 3.1 verifies on
 * this system DLL. It backs the original up as GDI32.WVB first. Run it once,
 * then reboot; OPENGL32.DLL and the ICD (WVBGL32.DLL, WVBGLX.DLL) must be in
 * SYSTEM32. To undo, run  GL31SYS /U  (restores GDI32.WVB), or copy GDI32.WVB
 * back over GDI32.DLL from a boot that does not use it.
 *
 *     GL31SYS               patch %SystemRoot%\SYSTEM32\GDI32.DLL
 *     GL31SYS path\GDI32.DLL   patch a named copy
 *     GL31SYS /U            undo (restore from GDI32.WVB)
 *
 * Only for NT 3.1. On NT 3.5 and later GDI32 already has these functions.
 * A GUI program (reports in a message box), KERNEL32 + USER32 only, no CRT.
 *
 * Build: see gl/build.sh (output GL31SYS.EXE, an 8.3 name for FAT volumes).
 */

#include <windows.h>

/* ---- helpers (no CRT) ----------------------------------------------------- */
static unsigned slen(const char *s){unsigned n=0;while(s[n])n++;return n;}
static void mcpy(void *d,const void *s,unsigned n){unsigned char*p=d;const unsigned char*q=s;while(n--)*p++=*q++;}
static void mset(void *d,int c,unsigned n){unsigned char*p=d;while(n--)*p++=(unsigned char)c;}
static int scmp(const char*a,const char*b){while(*a&&*a==*b){a++;b++;}return (int)(unsigned char)*a-(int)(unsigned char)*b;}
static int seqi(const char*a,const char*b){for(;;a++,b++){char x=*a,y=*b;if(x>='a'&&x<='z')x-=32;if(y>='a'&&y<='z')y-=32;if(x!=y)return 0;if(!x)return 1;}}

static char msg[1024]; static unsigned msglen;
static void out(const char *s){while(*s&&msglen<sizeof(msg)-1)msg[msglen++]=*s++;msg[msglen]=0;}
static void outx(DWORD v){char b[9];int i;const char*h="0123456789abcdef";for(i=7;i>=0;i--){b[i]=h[v&15];v>>=4;}b[8]=0;out(b);}

static unsigned char *img; static DWORD imglen, imgcap;
static DWORD rd32(DWORD o){return img[o]|(img[o+1]<<8)|(img[o+2]<<16)|((DWORD)img[o+3]<<24);}
static WORD  rd16(DWORD o){return (WORD)(img[o]|(img[o+1]<<8));}
static void  wr32(DWORD o,DWORD v){img[o]=(BYTE)v;img[o+1]=(BYTE)(v>>8);img[o+2]=(BYTE)(v>>16);img[o+3]=(BYTE)(v>>24);}
static void  wr16(DWORD o,WORD v){img[o]=(BYTE)v;img[o+1]=(BYTE)(v>>8);}

#define MAXSEC 96
static WORD nsec; static DWORD sec_va[MAXSEC],sec_vs[MAXSEC],sec_raw[MAXSEC],sec_rs[MAXSEC];
static DWORD rva2off(DWORD r){int i;for(i=0;i<nsec;i++){DWORD e=sec_va[i]+(sec_vs[i]>sec_rs[i]?sec_vs[i]:sec_rs[i]);if(r>=sec_va[i]&&r<e)return r-sec_va[i]+sec_raw[i];}return r<sec_va[0]?r:0;}

static const char *const GL[5]={
    "ChoosePixelFormat","DescribePixelFormat","GetPixelFormat","SetPixelFormat","SwapBuffers"};
#define NG 5

/* standard PE image checksum (CheckSumMappedFile) */
static DWORD pe_checksum(DWORD csum_off)
{
    DWORD s=0,i=0,n=imglen;
    while(i+1<n){
        if(i==csum_off||i==csum_off+2){i+=2;continue;}
        s+=img[i]|(img[i+1]<<8); s=(s&0xffff)+(s>>16); i+=2;
    }
    if(n&1){s+=img[n-1];s=(s&0xffff)+(s>>16);}
    s=(s&0xffff)+(s>>16); s=(s+(s>>16))&0xffff;
    return s+n;
}

static char path[600];
static char bak[600];
static char tmp[600];

int patch(void);
static int save_file(const char *p);
static int load_file(const char *p, DWORD extra);

/* GDI32.DLL is loaded now and cannot be overwritten, but Windows NT lets a
   loaded image file be RENAMED (even on FAT): move the live GDI32.DLL aside to
   GDI32.WVB, then write the patched copy as a new GDI32.DLL. The running system
   keeps using the old file (now GDI32.WVB) until the reboot, which then loads
   the patched GDI32.DLL. Returns 0 on failure. */
static int swap_in(const char *live, const char *patched_is_saved_here_live, const char *backup)
{
    (void) patched_is_saved_here_live;
    DeleteFileA(backup);                        /* clear any earlier backup */
    if (!MoveFileA(live, backup))               /* live GDI32.DLL -> GDI32.WVB */
        return 0;
    if (!save_file(live)) {                     /* write patched image as GDI32.DLL */
        MoveFileA(backup, live);                /* undo the rename on failure */
        return 0;
    }
    return 1;
}

static int quiet;      /* /Q : no message box (for the installer) */
static void report_exit(UINT code)
{
    if(!quiet)
        MessageBoxA(NULL,msg,"gl31sys",MB_OK|(code?MB_ICONWARNING:MB_ICONINFORMATION));
    ExitProcess(code);
}

static int load_file(const char *p, DWORD extra)
{
    HANDLE f=CreateFileA(p,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
    DWORD got;
    if(f==INVALID_HANDLE_VALUE)return 0;
    imglen=GetFileSize(f,NULL);
    imgcap=imglen+extra;
    img=(unsigned char*)VirtualAlloc(NULL,imgcap,MEM_COMMIT,PAGE_READWRITE);
    if(!img){CloseHandle(f);return 0;}
    if(!ReadFile(f,img,imglen,&got,NULL)||got!=imglen){CloseHandle(f);return 0;}
    CloseHandle(f);
    return 1;
}
static int save_file(const char *p)
{
    HANDLE f=CreateFileA(p,GENERIC_WRITE,0,NULL,CREATE_ALWAYS,0,NULL);
    DWORD wr;
    if(f==INVALID_HANDLE_VALUE)return 0;
    WriteFile(f,img,imglen,&wr,NULL);
    CloseHandle(f);
    return wr==imglen;
}

void __stdcall start(void)
{
    char *cmd; DWORD n; int undo=0, explicit_path=0;

    cmd=GetCommandLineA();
    if(*cmd=='"'){cmd++;while(*cmd&&*cmd!='"')cmd++;if(*cmd)cmd++;}
    else while(*cmd&&*cmd!=' ')cmd++;
    while(*cmd==' ')cmd++;
    while((cmd[0]=='/'||cmd[0]=='-')&&cmd[1]){
        char o=cmd[1]; if(o>='a'&&o<='z')o-=32;
        if(o=='U')undo=1; else if(o=='Q')quiet=1; else break;
        cmd+=2; while(*cmd==' ')cmd++;
    }

    /* the file to work on */
    if(*cmd){
        if(*cmd=='"'){cmd++;n=0;while(*cmd&&*cmd!='"'&&n<sizeof(path)-1)path[n++]=*cmd++;}
        else {n=0;while(*cmd&&*cmd!=' '&&n<sizeof(path)-1)path[n++]=*cmd++;}
        path[n]=0; explicit_path=1;
    } else {
        n=GetWindowsDirectoryA(path,sizeof(path)-32);
        if(!n){out("cannot find the Windows directory\r\n");report_exit(1);}
        if(path[n-1]!='\\')path[n++]='\\';
        mcpy(path+n,"SYSTEM32\\GDI32.DLL",19);
    }
    /* backup (GDI32.WVB) and temp (GDI32.WVN), both next to the target */
    {
        int i,c=-1; for(i=0;path[i];i++){bak[i]=tmp[i]=path[i];if(path[i]=='\\'||path[i]=='/')c=i;}
        bak[i]=tmp[i]=0;
        mcpy(bak+c+1,"GDI32.WVB",10);
        mcpy(tmp+c+1,"GDI32.WVN",10);
    }

    if(undo){
        HANDLE t=CreateFileA(bak,GENERIC_READ,FILE_SHARE_READ,NULL,OPEN_EXISTING,0,NULL);
        if(t==INVALID_HANDLE_VALUE){out("no backup GDI32.WVB found next to\r\n  ");out(path);out("\r\n");report_exit(1);}
        CloseHandle(t);
        if(explicit_path){
            if(!load_file(bak,0)||!save_file(path)){out("cannot restore ");out(path);out("\r\n");report_exit(1);}
            out("restored ");out(path);out(" from GDI32.WVB.\r\n");report_exit(0);
        }
        /* live GDI32 is patched: move it aside and rename the backup back */
        DeleteFileA(tmp);
        if(!MoveFileA(path,tmp)||!MoveFileA(bak,path)){out("could not restore GDI32.DLL.\r\n");report_exit(1);}
        DeleteFileA(tmp);
        out("GDI32.DLL restored from GDI32.WVB. Reboot to use it.\r\n");
        report_exit(0);
    }

    if(!load_file(path,0x8000)){out("cannot open ");out(path);out("\r\n");report_exit(1);}
    {
        int r=patch();
        if(r==2){out(path);out("\r\n already has the OpenGL exports - nothing to do.\r\n");report_exit(0);}
        if(!r)report_exit(1);
    }
    if(explicit_path){
        /* a named copy (not the live GDI32): back it up and overwrite directly */
        CopyFileA(path,bak,TRUE);
        if(!save_file(path)){out("cannot write ");out(path);out("\r\n");report_exit(1);}
        out("patched ");out(path);out("\r\n -> OpenGL exports added. Put it in SYSTEM32\r\n");
        out("as GDI32.DLL (with OPENGL32.DLL + WVBGL32.DLL + WVBGLX.DLL) and reboot.\r\n");
        report_exit(0);
    }
    /* the live GDI32.DLL is in use: rename it aside, write the patched one */
    if(!swap_in(path,tmp,bak)){out("cannot replace ");out(path);out("\r\n(is it read-only?)\r\n");report_exit(1);}
    out("OpenGL enabled system-wide for Windows NT 3.1.\r\n\r\n");
    out("Reboot now: the patched GDI32.DLL takes effect (original saved as GDI32.WVB).\r\n");
    out("OPENGL32.DLL, WVBGL32.DLL and WVBGLX.DLL must be in SYSTEM32.\r\n");
    out("To undo: GL31SYS /U  (then reboot).\r\n");
    report_exit(0);
}

int patch(void)
{
    DWORD pe,opt,exprva,e,base,nf,nn,eat,npt,ot,dllname,salign,falign,sizeimg;
    DWORD newva,newoff,nnf,nnn,off_eat,off_npt,off_ord,off_str,cur,i,k,total,rawsz,csum_off;
    DWORD name_rva[NG],fwd_rva[NG],blob;
    /* merged table built in place: parallel arrays of (name file-ptr, eat index) */
    static DWORD m_nrva[512]; static const char* m_str[512]; static DWORD m_eidx[512]; DWORD mn;

    if(rd16(0)!=0x5A4D){out("not an executable\r\n");return 0;}
    pe=rd32(0x3C); if(rd32(pe)!=0x00004550){out("not a PE file\r\n");return 0;}
    if(rd16(pe+24)!=0x10B){out("not a 32-bit PE\r\n");return 0;}
    opt=pe+24; nsec=rd16(pe+6);
    if(nsec>=MAXSEC){out("too many sections\r\n");return 0;}
    { DWORD s=opt+rd16(pe+20); for(i=0;i<nsec;i++,s+=40){sec_vs[i]=rd32(s+8);sec_va[i]=rd32(s+12);sec_rs[i]=rd32(s+16);sec_raw[i]=rd32(s+20);} }
    salign=rd32(opt+32); falign=rd32(opt+36); sizeimg=rd32(opt+56);
    exprva=rd32(opt+96); if(!exprva){out("no export table\r\n");return 0;}
    e=rva2off(exprva);
    dllname=rd32(e+12); base=rd32(e+16); nf=rd32(e+20); nn=rd32(e+24);
    eat=rva2off(rd32(e+28)); npt=rva2off(rd32(e+32)); ot=rva2off(rd32(e+36));
    (void)dllname;
    if(nn+NG>512||nf+NG>512){out("export table too large\r\n");return 0;}

    /* build merged (name, eat-index) list = existing names + 5 new */
    mn=0;
    for(i=0;i<nn;i++){
        DWORD nr=rd32(npt+4*i); DWORD no=rva2off(nr);
        /* already patched? */
        for(k=0;k<NG;k++) if(seqi((char*)img+no,GL[k])){ return 2; }
        m_nrva[mn]=nr; m_str[mn]=(const char*)img+no; m_eidx[mn]=rd16(ot+2*i); mn++;
    }
    newva=(sizeimg+salign-1)/salign*salign;
    newoff=(imglen+falign-1)/falign*falign;
    nnf=nf+NG; nnn=nn+NG;
    /* layout within the new blob */
    off_eat=40;
    off_npt=off_eat+nnf*4;
    off_ord=off_npt+nnn*4;
    off_str=(off_ord+nnn*2+3)&~3U;
    cur=off_str;
    /* reserve blob in image (after newoff); we build it there directly */
    /* first compute string RVAs */
    for(k=0;k<NG;k++){ name_rva[k]=newva+cur; cur+=slen(GL[k])+1; }
    for(k=0;k<NG;k++){ fwd_rva[k]=newva+cur; cur+=8/*OPENGL32*/+1+slen(GL[k])+1; }
    total=cur;
    blob=newoff;
    rawsz=(total+falign-1)/falign*falign;
    if(newoff+rawsz>imgcap){out("internal: buffer too small\r\n");return 0;}
    /* zero the gap + blob area */
    mset(img+imglen,0,(newoff-imglen)+rawsz);
    /* write strings into the new section (its RVAs are not yet in the section
       table, so map them by hand: file off = blob + (rva - newva)) */
    for(k=0;k<NG;k++){ DWORD o=blob+(name_rva[k]-newva); mcpy(img+o,GL[k],slen(GL[k])+1); }
    for(k=0;k<NG;k++){ DWORD o=blob+(fwd_rva[k]-newva); mcpy(img+o,"OPENGL32.",9); mcpy(img+o+9,GL[k],slen(GL[k])+1); }
    /* add the 5 new names to the merge list (strings now in image) */
    for(k=0;k<NG;k++){ m_nrva[mn]=name_rva[k]; m_str[mn]=(const char*)img+(blob+(name_rva[k]-newva)); m_eidx[mn]=nf+k; mn++; }
    /* insertion sort by string (ASCII, as the loader's binary search needs) */
    for(i=1;i<mn;i++){
        DWORD a=m_nrva[i],c2=m_eidx[i]; const char*b=m_str[i]; DWORD j=i;
        while(j>0&&scmp(m_str[j-1],b)>0){m_nrva[j]=m_nrva[j-1];m_str[j]=m_str[j-1];m_eidx[j]=m_eidx[j-1];j--;}
        m_nrva[j]=a;m_str[j]=b;m_eidx[j]=c2;
    }
    /* write EAT: old nf entries copied, then 5 forwarders */
    for(i=0;i<nf;i++) wr32(blob+off_eat+4*i, rd32(eat+4*i));
    for(k=0;k<NG;k++) wr32(blob+off_eat+4*(nf+k), fwd_rva[k]);
    /* write NPT + ORD from merged */
    for(i=0;i<mn;i++){ wr32(blob+off_npt+4*i, m_nrva[i]); wr16(blob+off_ord+2*i, (WORD)m_eidx[i]); }
    /* export directory header: copy old 40 bytes, patch counts + pointers */
    mcpy(img+blob, img+e, 40);
    wr32(blob+20, nnf);
    wr32(blob+24, nnn);
    wr32(blob+28, newva+off_eat);
    wr32(blob+32, newva+off_npt);
    wr32(blob+36, newva+off_ord);
    /* DataDirectory[EXPORT] -> whole blob so forwarder RVAs fall inside */
    wr32(opt+96, newva);
    wr32(opt+96+4, total);
    /* new section header */
    { DWORD s=opt+rd16(pe+20)+nsec*40; DWORD first=0xFFFFFFFF;
      for(i=0;i<nsec;i++) if(sec_raw[i]&&sec_raw[i]<first) first=sec_raw[i];
      if(s+40>first) { out("no room for a section header\r\n"); return 0; }
      mset(img+s,0,40);
      mcpy(img+s,".wvbexp",7);
      wr32(s+8,total); wr32(s+12,newva); wr32(s+16,rawsz); wr32(s+20,newoff);
      wr32(s+36,0x40000040UL);               /* initialized data, read */
    }
    wr16(pe+6,(WORD)(nsec+1));
    wr32(opt+56, newva+(total+salign-1)/salign*salign);
    imglen=newoff+rawsz;
    /* PE checksum (NT 3.1 verifies it for this system DLL) */
    csum_off=opt+64;
    wr32(csum_off,0);
    wr32(csum_off, pe_checksum(csum_off));
    return 1;
}
