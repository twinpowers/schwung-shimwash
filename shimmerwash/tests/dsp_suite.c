#include "dsp_base.h"
static float g_noise(int n){ (void)n; return 0.25f*rnd(); }
static float g_loud(int n){ return 0.9f*sinf(6.2831853f*440*n/44100.0f); }
static float g_dyad(int n){ float t=n/44100.0f; return 0.25f*sinf(6.2831853f*220*t)+0.25f*sinf(6.2831853f*329.6f*t); }
static float g_pluck(int n){ float t=n/44100.0f; float e=expf(-8*fmodf(t,3.0f)); return 0.5f*e*(sinf(6.2831853f*220*t)+0.5f*sinf(6.2831853f*440*t)); }
static float g_burst(int n){ return n<4000? 0.9f*sinf(6.2831853f*330*n/44100.0f):0; }
static float g_tone220(int n){ return n<22050? 0.5f*sinf(6.2831853f*220*n/44100.0f):0; }
static float g_tone220s(int n){ return n<4410? 0.5f*sinf(6.2831853f*220*n/44100.0f):0; }

static sw_t* mk(void){ sw_t*p=v2_create_instance(".",NULL); setp(p,"mix",100); return p; }

int main(void){
  float*L; sw_t*p;
  puts("== 1. defaults: wet level on noise, RT60 =="); 
  p=mk(); setp(p,"bloom",0); setp(p,"predelay",0); setp(p,"a_amt",0); setp(p,"b_amt",0);
  L=render(p,10,g_noise); printf("noise in rms 0.144 -> wet rms %.3f (ratio %.2f)\n", rmsw(L,5*44100,10*44100), rmsw(L,5*44100,10*44100)/0.144); free(L); free(p);
  p=mk(); setp(p,"bloom",0); setp(p,"predelay",0); setp(p,"a_amt",0); setp(p,"b_amt",0); setp(p,"modamt",0);
  { int total=44100*30; float*I=malloc(4*total); int16_t buf[1024]; double e[30]={0};
    for(int n=0;n<total;n+=512){ for(int i=0;i<512;i++){ float x=(n+i<50)?0.8f:0; buf[i*2]=buf[i*2+1]=(int16_t)(x*32767);} v2_process_block(p,buf,512); for(int i=0;i<512&&n+i<total;i++){ double l=buf[i*2]/32768.0; e[(n+i)/44100]+=l*l; } }
    printf("energy/sec dB:"); for(int k=0;k<14;k++) printf(" %.0f",10*log10((e[k]+1e-20)/(e[0]+1e-20))); printf("\n"); free(I);} free(p);

  puts("== 2. stability: both shifters FEEDBACK at 100, long decay, each pitch combo ==");
  int combos[][2]={{12,7},{12,12},{24,12},{-12,12},{-24,24},{7,-5}};
  for(int c=0;c<6;c++){ p=mk(); setp(p,"decay",100); setp(p,"color",100); setp(p,"a_amt",100); setp(p,"b_amt",100); setp(p,"a_pitch",combos[c][0]); setp(p,"b_pitch",combos[c][1]); setp(p,"spread",100); setp(p,"modamt",100);
    L=render(p,50,g_burst); printf("A%+3d B%+3d burst: rms[30-40s]=%.4f rms[45-50s]=%.4f nan=%d\n",combos[c][0],combos[c][1],rmsw(L,30*44100,40*44100),rmsw(L,45*44100,50*44100),nanck(L,50*44100)); free(L); free(p);}
  puts("-- steady tones with shifters maxed (last 15 s of 50 s) --");
  { const char*nm[]={"loud sine","dyad"}; gen_t gs[]={g_loud,g_dyad};
    for(int k=0;k<2;k++){ p=mk(); setp(p,"decay",100); setp(p,"a_amt",100); setp(p,"b_amt",100); L=render(p,50,gs[k]); printf("%s d100 A/B fb100: rms=%.3f peak=%.3f\n",nm[k],rmsw(L,35*44100,50*44100),peakw(L,35*44100,50*44100)); free(L); free(p);} }
  puts("-- with 1000 ms shifter delays (feedback) --");
  p=mk(); setp(p,"decay",100); setp(p,"a_amt",100); setp(p,"b_amt",100); setp(p,"a_delay",1000); setp(p,"b_delay",650);
  L=render(p,50,g_burst); printf("burst: rms[30-40s]=%.4f rms[45-50s]=%.4f nan=%d\n",rmsw(L,30*44100,40*44100),rmsw(L,45*44100,50*44100),nanck(L,50*44100)); free(L); free(p);
  puts("-- PRE and POST at 100 --");
  { int modes[][2]={{0,0},{2,2},{0,2}}; for(int m=0;m<3;m++){ p=mk(); setp(p,"decay",100); setp(p,"a_mode",modes[m][0]); setp(p,"b_mode",modes[m][1]); setp(p,"a_amt",100); setp(p,"b_amt",100);
    L=render(p,50,g_dyad); printf("modes A%d B%d dyad: rms=%.3f peak=%.3f; burst-decay below\n",modes[m][0],modes[m][1],rmsw(L,35*44100,50*44100),peakw(L,35*44100,50*44100)); free(L); free(p);} }
  puts("-- normal playing: defaults on plucks --");
  p=mk(); L=render(p,40,g_pluck); printf("defaults pluck: rms=%.3f peak=%.3f\n",rmsw(L,20*44100,40*44100),peakw(L,20*44100,40*44100)); free(L); free(p);

  puts("== 3. spectrum (220 Hz tone, 1-4 s window, wet only), dB re fundamental band ==");
  struct { const char*nm; int am,bm,aa,ba,ap,bp; } cs[]={
    {"shifters off",1,1,0,0,12,7},{"defaults (A+12 B+7, fb 40)",1,1,40,40,12,7},{"A+12 fb100 only",1,1,100,0,12,7},
    {"A-12 fb100 only",1,1,100,0,-12,7},{"A+12 POST 100",2,1,100,0,12,7},{"A+12 PRE 100",0,1,100,0,12,7},{"B+19 fb100 only",1,1,0,100,12,19} };
  for(int c=0;c<7;c++){ p=mk(); setp(p,"decay",55); setp(p,"bloom",0); setp(p,"a_mode",cs[c].am); setp(p,"b_mode",cs[c].bm); setp(p,"a_amt",cs[c].aa); setp(p,"b_amt",cs[c].ba); setp(p,"a_pitch",cs[c].ap); setp(p,"b_pitch",cs[c].bp);
    L=render(p,10,g_tone220); int a=44100, len=3*44100;
    double f0=band(L+a,len,210,230);
    printf("%-30s 110Hz(-12)=%6.1f 330(+7)=%6.1f 440(+12)=%6.1f 880(+24)=%6.1f 660(+19)=%6.1f\n",cs[c].nm,10*log10(band(L+a,len,100,120)/f0+1e-12),10*log10(band(L+a,len,315,345)/f0+1e-12),10*log10(band(L+a,len,415,465)/f0+1e-12),10*log10(band(L+a,len,860,930)/f0+1e-12),10*log10(band(L+a,len,640,690)/f0+1e-12)); free(L); free(p);}

  puts("== 4. shifter delay (POST, +12, reverb short): 440 Hz band energy in 100ms windows after a 100 ms burst ==");
  for(int dly=0; dly<=500; dly+=500){ p=mk(); setp(p,"decay",0); setp(p,"size",0); setp(p,"bloom",0); setp(p,"predelay",0); setp(p,"mix",100); setp(p,"a_mode",2); setp(p,"a_amt",100); setp(p,"a_pitch",12); setp(p,"a_fine",0); setp(p,"a_delay",dly); setp(p,"b_amt",0);
    L=render(p,3,g_tone220s);
    printf("A delay %3d ms: 440Hz energy dB per 100ms window t=0..1.0s:",dly);
    for(int w=0;w<10;w++) printf(" %5.0f",10*log10(band(L+w*4410,4410,430,450)+1e-14)); printf("\n"); free(L); free(p);}

  puts("== 5. ducker: wet rms during a 1 s loud tone vs after, amount 0 vs 100 ==");
  for(int amt=0; amt<=100; amt+=100){ p=mk(); setp(p,"bloom",0); setp(p,"duck_amt",amt); setp(p,"duck_thr",-30); setp(p,"decay",60);
    int total=44100*6; float*X=malloc(4*total); int16_t buf[1024]; 
    for(int n=0;n<total;n+=512){ for(int i=0;i<512;i++){ int k=n+i; float x= (k>=44100&&k<88200)? 0.5f*sinf(6.2831853f*196*k/44100.0f):0; buf[i*2]=buf[i*2+1]=(int16_t)(x*32767);} v2_process_block(p,buf,512); for(int i=0;i<512&&n+i<total;i++) X[n+i]=buf[i*2]/32768.0f; }
    printf("duck amt %3d: wet rms during tone(1.5-2s)=%.3f  1s after (2.2-3.2s)=%.3f  3s after (5-6s)=%.3f\n",amt,rmsw(X,66150,88200),rmsw(X,97020,141120),rmsw(X,220500,264600)); free(X); free(p);}

  puts("== 6. input filter on white noise (wet rms; fewer = more filtered) ==");
  { struct{const char*nm;int type,freq;} fs[]={{"off (HP 20Hz)",0,0},{"HP ~630Hz",0,50},{"HP ~2.5kHz",0,70},{"LP ~630Hz",1,50},{"LP ~2.5kHz",1,70}};
    for(int c=0;c<5;c++){ p=mk(); setp(p,"bloom",0); setp(p,"predelay",0); setp(p,"a_amt",0); setp(p,"b_amt",0); setp(p,"filt_type",fs[c].type); setp(p,"filt_freq",fs[c].freq); setp(p,"decay",20);
      L=render(p,5,g_noise); printf("%-16s wet rms=%.3f\n",fs[c].nm,rmsw(L,2*44100,5*44100)); free(L); free(p);} }

  puts("== 7. live parameter sweeps (size/delay/pitch/mode) stay finite and bounded ==");
  p=mk(); { int16_t buf[1024]; float pk=0; int bad=0;
    for(int n=0;n<44100*30;n+=512){ if((n/512)%40==0){ setp(p,"size",rnd()>0?100:0); setp(p,"a_delay",(int)(500+500*rnd())); setp(p,"b_delay",(int)(500+500*rnd())); setp(p,"a_pitch",(int)(24*rnd())); setp(p,"b_pitch",(int)(24*rnd())); setp(p,"a_mode",(int)(1.5+1.5*rnd())); setp(p,"b_mode",(int)(1.5+1.5*rnd())); setp(p,"decay",(int)(50+50*rnd())); setp(p,"a_amt",(int)(50+50*rnd())); setp(p,"b_amt",(int)(50+50*rnd())); setp(p,"freeze",rnd()>0.5?1:0);}
      for(int i=0;i<512;i++){ float x=0.4f*sinf(6.2831853f*196*(n+i)/44100.0f); buf[i*2]=buf[i*2+1]=(int16_t)(x*32767);} v2_process_block(p,buf,512);
      for(int i=0;i<1024;i++){ float l=buf[i]/32768.0f; if(fabsf(l)>pk)pk=fabsf(l); } }
    printf("random sweeps 30 s: peak=%.3f\n",pk);} free(p);

  puts("== 8. state round-trip + CPU ==");
  p=v2_create_instance(".",NULL); setp(p,"a_pitch",-7); setp(p,"b_mode",2); setp(p,"duck_thr",-40); setp(p,"a_fine",-33);
  char st[2048]; v2_get_param(p,"state",st,sizeof st); sw_t*q=v2_create_instance(".",NULL); v2_set_param(q,"state",st);
  int same=1; for(int i=0;i<NPARAMS;i++) if(p->v[i]!=q->v[i]) same=0; printf("state round-trip identical: %s (%d bytes)\n",same?"yes":"NO",(int)strlen(st));
  { int16_t buf[256]; clock_t t0=clock(); setp(p,"a_delay",800); setp(p,"duck_amt",50); int blocks=44100*20/128; for(int i=0;i<blocks;i++){ for(int k=0;k<128;k++) buf[k*2]=buf[k*2+1]=(int16_t)(8000*rnd()); v2_process_block(p,buf,128);} double dt=(double)(clock()-t0)/CLOCKS_PER_SEC; printf("CPU: 20 s audio in %.3f s (%.1f%% of one x86 core)\n",dt,100*dt/20); }
  free(p); free(q);
  return 0; }
