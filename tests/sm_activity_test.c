#include "sm_activity.h"
#ifdef NDEBUG
#undef NDEBUG
#endif
#include <assert.h>
#include <stdio.h>
#include <string.h>
static unsigned char ram[0x20000], before[sizeof(ram)];
static void put(unsigned at, unsigned value) { ram[at]=value;ram[at+1]=value>>8; }
static unsigned get(unsigned at) { return ram[at] | ram[at+1]<<8; }
static void enemy(unsigned i, int x, unsigned properties) {
  unsigned at=0xf78+i*64;
  put(at,0xd4bf);put(at+2,1000+x);put(at+6,100);put(at+10,8);put(at+14,properties);
}
static void setup(void) {
  memset(ram,0,sizeof(ram));put(0x911,1000);put(0x915,0);
  enemy(7,128,0);put(0x17a4,2);put(0x17a6,2);
  put(0x17ac,7*64);put(0x17ae,0xffff);put(0x17ec,7*64);put(0x17ee,0xffff);
}
int main(void) {
  const unsigned widths[]={43,96,213,214};
  for(unsigned k=0;k<4;k++) {
    unsigned extra=widths[k];setup();
    enemy(1,-(int)extra,0);enemy(8,256+extra,0x400);
    enemy(10,-(int)extra-9,0);enemy(11,256+extra+9,0);
    enemy(12,275,0x200);enemy(13,275,0);put(0xf78+13*64,0xdaff);
    enemy(14,275,0);put(0xf78+14*64+6,249);
    memcpy(before,ram,sizeof(ram));
    assert(SmActivityAugmentQueues(ram,0));assert(!memcmp(before,ram,sizeof(ram)));
    assert(SmActivityAugmentQueues(ram,extra));
    assert(get(0x17a4)==6 && get(0x17a6)==4);
    assert(get(0x17ac)==64 && get(0x17ae)==448 && get(0x17b0)==512 && get(0x17b2)==0xffff);
    assert(get(0x17ec)==64 && get(0x17ee)==448 && get(0x17f0)==0xffff);
    assert(!memcmp(ram+0xf78,before+0xf78,2048)); /* properties never forged */
    memcpy(before,ram,sizeof(ram));assert(SmActivityAugmentQueues(ram,extra));
    assert(!memcmp(before,ram,sizeof(ram))); /* idempotent */
  }
  setup();put(0x183c,1);enemy(1,-20,0);memcpy(before,ram,sizeof(ram));
  assert(SmActivityAugmentQueues(ram,214));assert(!memcmp(before,ram,sizeof(ram)));
  setup();for(unsigned i=0;i<31;i++)enemy(i,275,0);
  assert(SmActivityAugmentQueues(ram,43));assert(get(0x17a4)==62 && get(0x17ea)==0xffff);
  setup();for(unsigned i=0;i<32;i++)enemy(i,275,0);memcpy(before,ram,sizeof(ram));
  assert(!SmActivityAugmentQueues(ram,43));assert(!memcmp(before,ram,sizeof(ram)));
  setup();for(unsigned i=0;i<32;i++)enemy(i,275,0x400);
  assert(SmActivityAugmentQueues(ram,43));assert(get(0x17a4)==64 && get(0x17a6)==0 && get(0x17ec)==0xffff);
  memcpy(before,ram,sizeof(ram));
  assert(!SmActivityAugmentQueues(ram,215));assert(!memcmp(before,ram,sizeof(ram)));
  puts("PASS: widened queue boundaries, slot order, exclusions, interaction, native identity and capacity");
}
