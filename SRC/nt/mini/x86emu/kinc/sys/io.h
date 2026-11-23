/* WINVBE: no host port access from the default handler - the miniport sets
   its own memory/port handler (emu10.c), which reaches the hardware */
#define inb(addr) 0xff
#define inw(addr) 0xffff
#define inl(addr) 0xffffffff
#define outb(val, addr)
#define outw(val, addr)
#define outl(val, addr)
