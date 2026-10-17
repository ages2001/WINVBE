#include "drv.h"
WORD host_alloc(DWORD size, BYTE **out);
SHORT EXPORT RealizeObject(LPVOID lpDev, SHORT style, LPVOID lpIn, LPVOID lpOut, LPTEXTXFORM lpXform);
int main(void){
  PDEVICE pd; LOGBRUSH lb = {BS_SOLID, 0xDDDDDD, 0, 0}; PBRUSH pb; DWORD ph;
  memset(&g,0,sizeof g); g.pf=PF_565; g.Bpp=2; g.mi.rsize=5; g.mi.rpos=11; g.mi.gsize=6; g.mi.gpos=5; g.mi.bsize=5;
  memset(&pd,0,sizeof pd); pd.bm.bmType=PDEV_TYPE;
  for (int i=0;i<16;i++){ lb.lbColor = (i*17)*0x010101; RealizeObject(&pd, OBJ_BRUSH, &lb, &pb, NULL);
  printf("%06x flags=%x fg=%08x pat=%04x mono=%02x\n", lb.lbColor, pb.flags, pb.fg, *(WORD*)pb.pat, pb.mono[0]); }
}
