#include <time.h>
#define SHIMMERWASH_TEST
#include "../src/shimmerwash.c"
static unsigned rng=12345; static float rnd(void){ rng=rng*1664525u+1013904223u; return ((rng>>8)&0xFFFF)/32768.0f-1.0f; }
static void setp(sw_t*p,const char*k,int v){ char b[16]; snprintf(b,16,"%d",v); v2_set_param(p,k,b); }
typedef float (*gen_t)(int);
/* run: returns via out arrays; L channel */
static float* render(sw_t*p,int secs,gen_t gen){ int total=44100*secs; float*L=malloc(sizeof(float)*total); int16_t buf[512*2];
  for(int n=0;n<total;n+=512){ for(int i=0;i<512;i++){int16_t q=(int16_t)(gen(n+i)*32767); buf[i*2]=buf[i*2+1]=q;} v2_process_block(p,buf,512);
    for(int i=0;i<512&&n+i<total;i++) L[n+i]=buf[i*2]/32768.0f; } return L; }
static double rmsw(const float*x,int a,int b){ double s=0; for(int i=a;i<b;i++) s+=x[i]*x[i]; return sqrt(s/(b-a)); }
static float peakw(const float*x,int a,int b){ float m=0; for(int i=a;i<b;i++) if(fabsf(x[i])>m)m=fabsf(x[i]); return m; }
static double gz(const float*x,int n,double f){ double w=2*M_PI*f/44100.0,c=2*cos(w),s0,s1=0,s2=0; for(int i=0;i<n;i++){s0=x[i]+c*s1-s2;s2=s1;s1=s0;} return (s1*s1+s2*s2-c*s1*s2)/((double)n*n)*4; }
static double band(const float*x,int n,double lo,double hi){ double e=0; for(double f=lo;f<=hi;f+=1.0) e+=gz(x,n,f); return e; }
static int nanck(const float*x,int n){ for(int i=0;i<n;i++) if(!(x[i]==x[i])) return 1; return 0; }
