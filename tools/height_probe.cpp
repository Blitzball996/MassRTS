// Quick height histogram for sdfcraft::World::surface_height_f over a region.
// Verifies the multi-noise mountain generator without GL/camera/fog.
#include "../src/sdfcraft/world.h"
#include <cstdio>
#include <algorithm>
#include <string>
int main(){
    sdfcraft::World w(1337);
    int lo=999, hi=-999; double sum=0; long cnt=0;
    int hist[140]={0};
    int peakx=0,peakz=0;
    for(int z=-1500; z<=1500; z+=5)
    for(int x=-1500; x<=1500; x+=5){
        int h=(int)w.surface_height_f((float)x,(float)z);
        if(h<lo)lo=h; if(h>hi){hi=h;peakx=x;peakz=z;}
        sum+=h; cnt++;
        if(h>=0&&h<140) hist[h]++;
    }
    printf("samples=%ld  min=%d  max=%d  mean=%.1f  sea=%d\n",cnt,lo,hi,sum/cnt,w.sea_level);
    printf("highest peak at (%d,%d)=%d\n",peakx,peakz,hi);
    printf("height histogram (10-block bins, %% of columns):\n");
    for(int b=0;b<140;b+=10){int s=0;for(int i=b;i<b+10&&i<140;i++)s+=hist[i];
        printf(" y%3d-%3d: %5.1f%% %s\n",b,b+9,100.0*s/cnt,std::string(s*200/cnt,'#').c_str());}
    // count columns well above sea level (mountains)
    long mtn=0; for(int i=w.sea_level+30;i<140;i++)mtn+=hist[i];
    printf("columns >30 above sea (mountainous): %.1f%%\n",100.0*mtn/cnt);
    return 0;
}
