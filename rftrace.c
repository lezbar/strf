#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdlib.h>
#include "sgdp4h.h"
#include "satutl.h"
#include "rftime.h"
#include "rftrace.h"
#include <sys/time.h>
#include <time.h>

#define LIM 80
#define D2R M_PI/180.0
#define R2D 180.0/M_PI
#define XKMPER 6378.135 // Earth radius in km
#define XKMPAU 149597879.691 // AU in km
#define FLAT (1.0/298.257)
#define C 299792.458 // Speed of light in km/s

struct point {
  xyz_t obspos,obsvel;
  xyz_t grpos,grvel;
};
struct site {
  int id;
  double lng,lat;
  float alt;
  char observer[64];
};

// Return x modulo y [0,y)
double modulo(double x,double y)
{
  x=fmod(x,y);
  if (x<0.0) x+=y;

  return x;
}

// Read a line of maximum length int lim from file FILE into string s
int fgetline(FILE *file,char *s,int lim)
{
  int c,i=0;
 
  while (--lim > 0 && (c=fgetc(file)) != EOF && c != '\n')
    s[i++] = c;
  //  if (c == '\n')
  //    s[i++] = c;
  s[i] = '\0';
  return i;
}

// Greenwich Mean Sidereal Time
double gmst(double mjd)
{
  double t,gmst;

  t=(mjd-51544.5)/36525.0;

  gmst=modulo(280.46061837+360.98564736629*(mjd-51544.5)+t*t*(0.000387933-t/38710000),360.0);

  return gmst;
}

// Greenwich Mean Sidereal Time
double dgmst(double mjd)
{
  double t,dgmst;

  t=(mjd-51544.5)/36525.0;

  dgmst=360.98564736629+t*(0.000387933-t/38710000);

  return dgmst;
}

// Observer position
void obspos_xyz(double mjd,double lng,double lat,float alt,xyz_t *pos,xyz_t *vel)
{
  double ff,gc,gs,theta,s,dtheta;

  s=sin(lat*D2R);
  ff=sqrt(1.0-FLAT*(2.0-FLAT)*s*s);
  gc=1.0/ff+alt/XKMPER;
  gs=(1.0-FLAT)*(1.0-FLAT)/ff+alt/XKMPER;

  theta=gmst(mjd)+lng;
  dtheta=dgmst(mjd)*D2R/86400;

  pos->x=gc*cos(lat*D2R)*cos(theta*D2R)*XKMPER;
  pos->y=gc*cos(lat*D2R)*sin(theta*D2R)*XKMPER; 
  pos->z=gs*sin(lat*D2R)*XKMPER;
  vel->x=-gc*cos(lat*D2R)*sin(theta*D2R)*XKMPER*dtheta;
  vel->y=gc*cos(lat*D2R)*cos(theta*D2R)*XKMPER*dtheta; 
  vel->z=0.0;

  return;
}

// Convert equatorial into horizontal coordinates
void equatorial2horizontal(double mjd,double ra,double de,double lng,double lat,double *azi,double *alt)
{
  double h;

  h=gmst(mjd)+lng-ra;
  
  *azi=modulo(atan2(sin(h*D2R),cos(h*D2R)*sin(lat*D2R)-tan(de*D2R)*cos(lat*D2R))*R2D,360.0);
  *alt=asin(sin(lat*D2R)*sin(de*D2R)+cos(lat*D2R)*cos(de*D2R)*cos(h*D2R))*R2D;

  return;
}

// Get observing site
struct site get_site(int site_id)
{
  int i=0,status;
  char line[LIM];
  FILE *file;
  int id;
  double lat,lng;
  float alt;
  char abbrev[3],observer[64];
  struct site s;
  char *env,filename[LIM];

  env=getenv("ST_DATADIR");
  sprintf(filename,"%s/data/sites.txt",env);

  file=fopen(filename,"r");
  if (file==NULL) {
    printf("File with site information not found!\n");
    return s;
  }
  while (fgets(line,LIM,file)!=NULL) {
    // Skip
    if (strstr(line,"#")!=NULL)
      continue;

    // Strip newline
    line[strlen(line)-1]='\0';

    // Read data
    status=sscanf(line,"%4d %2s %lf %lf %f",
	   &id,abbrev,&lat,&lng,&alt);
    strcpy(observer,line+38);

    // Change to km
    alt/=1000.0;
    
    // Copy site
    if (id==site_id) {
      s.lat=lat;
      s.lng=lng;
      s.alt=alt;
      s.id=id;
      strcpy(s.observer,observer);
    }

  }
  fclose(file);

  return s;
}

// Identify trace
void identify_trace_graves(char *tlefile,struct trace t,int satno)
{
  int i,imode,flag=0,status,imid;
  struct point *p;
  struct site s,sg;
  double *v,*vg;
  orbit_t orb;
  xyz_t satpos,satvel;
  FILE *file;
  double dx,dy,dz,dvx,dvy,dvz,r,za;
  double sum1,sum2,beta,freq0,rms,mjd0;
  char nfd[32],nfdmin[32],text[16];
  int satnomin;
  double rmsmin,freqmin,altmin,azimin;
  double ra,de,azi,alt;
  char *env,freqlist[LIM];

  env=getenv("ST_DATADIR");
  sprintf(freqlist,"%s/data/frequencies.txt",env);  

  // Reloop stderr
  if (freopen("/tmp/stderr.txt","w",stderr)==NULL)
    fprintf(stderr,"Failed to redirect stderr\n");

  // Get sites
  s=get_site(t.site);
  sg=get_site(9999);

  // Allocate
  p=(struct point *) malloc(sizeof(struct point)*t.n);
  v=(double *) malloc(sizeof(double)*t.n);
  vg=(double *) malloc(sizeof(double)*t.n);

  // Get observer position
  for (i=0;i<t.n;i++) {
    obspos_xyz(t.mjd[i],s.lng,s.lat,s.alt,&p[i].obspos,&p[i].obsvel);
    obspos_xyz(t.mjd[i],sg.lng,sg.lat,sg.alt,&p[i].grpos,&p[i].grvel);
  }
  printf("Fitting trace:\n");

  // Mid point
  imid=t.n/2;

  // Loop over TLEs
  file=fopen(tlefile,"r");
  if (file==NULL) {
    fprintf(stderr,"TLE file %s not found\n",tlefile);
    return;
  }
  while (read_twoline(file,satno,&orb)==0) {
    // Initialize
    imode=init_sgdp4(&orb);
    if (imode==SGDP4_ERROR)
      printf("SGDP4 Error\n");

    // Loop over points
    for (i=0,sum1=0.0,sum2=0.0;i<t.n;i++) {
      // Get satellite position
      satpos_xyz(t.mjd[i]+2400000.5,&satpos,&satvel);

      dx=satpos.x-p[i].obspos.x;  
      dy=satpos.y-p[i].obspos.y;
      dz=satpos.z-p[i].obspos.z;
      dvx=satvel.x-p[i].obsvel.x;
      dvy=satvel.y-p[i].obsvel.y;
      dvz=satvel.z-p[i].obsvel.z;
      r=sqrt(dx*dx+dy*dy+dz*dz);
      v[i]=(dvx*dx+dvy*dy+dvz*dz)/r;
      za=acos((p[i].obspos.x*dx+p[i].obspos.y*dy+p[i].obspos.z*dz)/(r*XKMPER))*R2D;
      if (i==imid) {
	ra=modulo(atan2(dy,dx)*R2D,360.0);
	de=asin(dz/r)*R2D;
	equatorial2horizontal(t.mjd[i],ra,de,s.lng,s.lat,&azi,&alt);
      }
      dx=satpos.x-p[i].grpos.x;  
      dy=satpos.y-p[i].grpos.y;
      dz=satpos.z-p[i].grpos.z;
      dvx=satvel.x-p[i].grvel.x;
      dvy=satvel.y-p[i].grvel.y;
      dvz=satvel.z-p[i].grvel.z;
      r=sqrt(dx*dx+dy*dy+dz*dz);
      vg[i]=(dvx*dx+dvy*dy+dvz*dz)/r;


      //      t[j].freq[i]=(1.0-v/C)*(1.0-vg/C)*freq0;
      //      if (!((azi<90.0 || azi>270.0) && alt>15.0 && alt<40.0))
      //	t[j].za[i]=100.0;
    }
    freq0=143050000.0;

    // Compute residuals
    for (i=0,rms=0.0;i<t.n;i++) 
      rms+=pow(t.freq[i]-(1.0-v[i]/C)*(1.0-vg[i]/C)*freq0,2);
    rms=sqrt(rms/(double) t.n);

    // Find TCA
    for (i=0,mjd0=0.0;i<t.n-1;i++) 
      if (v[i]*v[i-1]<0.0)
	mjd0=t.mjd[i];
    
    if (mjd0>0.0)
      mjd2nfd(mjd0,nfd);
    else
      strcpy(nfd,"0000-00-00T00:00:00");
    
    if (rms<1000) {
      if (rms<50.0)
	printf("%05d: %s %8.1f Hz (%.1f,%.1f)\n",orb.satno,nfd,rms,modulo(azi+180.0,360.0),alt);
      //      printf("%05d: %s  %8.3f MHz %8.3f kHz\n",orb.satno,nfd,1e-6*freq0,1e-3*rms);
      if (flag==0 || rms<rmsmin) {
	satnomin=orb.satno;
	strcpy(nfdmin,nfd);
	freqmin=freq0;
	rmsmin=rms;
	altmin=alt;
	azimin=azi;
	flag=1;
      }
    }
  }
  fclose(file);
  fclose(stderr);

  if (flag==1) {
    printf("\nBest fitting object:\n");
    printf("%05d: %s  %8.1f Hz (%.1f,%.1f)\n",satnomin,nfdmin,rmsmin,modulo(azimin+180.0,360.0),altmin);
    printf("Store frequency? [y/n]\n");
    status=scanf("%s",text);
    if (text[0]=='y') {
      file=fopen(freqlist,"a");
      fprintf(file,"%05d %8.3f\n",satnomin,1e-6*freqmin);
      fclose(file);
      file=fopen("log.txt","a");
      fprintf(file,"%05d %8.3f %.3f %.19s\n",satnomin,1e-6*freqmin,1e-3*rmsmin,nfdmin);
      fclose(file);
      printf("Frequency stored\n\n");
    }
  } else {
    printf("\nTrace not identified..\n");
  }

  // Free
  free(p);
  free(v);

  return;
}

// Identify trace
void identify_trace(char *tlefile,struct trace t,int satno)
{
  int i,imode,flag=0,status;
  struct point *p;
  struct site s;
  double *v;
  orbit_t orb;
  xyz_t satpos,satvel;
  FILE *file;
  double dx,dy,dz,dvx,dvy,dvz,r,za;
  double sum1,sum2,beta,freq0,rms,mjd0;
  char nfd[32],nfdmin[32],text[16];
  int satnomin;
  double rmsmin,freqmin;
  char *env,freqlist[LIM];
  struct timeval tv;
  char tbuf[30];
  
  env=getenv("ST_DATADIR");
  sprintf(freqlist,"%s/data/frequencies.txt",env);  

  // Reloop stderr
  if (freopen("/tmp/stderr.txt","w",stderr)==NULL)
    fprintf(stderr,"Failed to redirect stderr\n");

  // Get site
  s=get_site(t.site);

  // Allocate
  p=(struct point *) malloc(sizeof(struct point)*t.n);
  v=(double *) malloc(sizeof(double)*t.n);

  // Get observer position
  for (i=0;i<t.n;i++) 
    obspos_xyz(t.mjd[i],s.lng,s.lat,s.alt,&p[i].obspos,&p[i].obsvel);

  printf("Fitting trace:\n");

  // Loop over TLEs
  file=fopen(tlefile,"r");
  if (file==NULL) {
    fprintf(stderr,"TLE file %s not found\n",tlefile);
    return;
  }
  while (read_twoline(file,satno,&orb)==0) {
    // Initialize
    imode=init_sgdp4(&orb);
    if (imode==SGDP4_ERROR)
      printf("SGDP4 Error\n");

    // Loop over points
    for (i=0,sum1=0.0,sum2=0.0;i<t.n;i++) {
      // Get satellite position
      satpos_xyz(t.mjd[i]+2400000.5,&satpos,&satvel);

      dx=satpos.x-p[i].obspos.x;  
      dy=satpos.y-p[i].obspos.y;
      dz=satpos.z-p[i].obspos.z;
      dvx=satvel.x-p[i].obsvel.x;
      dvy=satvel.y-p[i].obsvel.y;
      dvz=satvel.z-p[i].obsvel.z;
      r=sqrt(dx*dx+dy*dy+dz*dz);
      v[i]=(dvx*dx+dvy*dy+dvz*dz)/r;
      za=acos((p[i].obspos.x*dx+p[i].obspos.y*dy+p[i].obspos.z*dz)/(r*XKMPER))*R2D;

      beta=(1.0-v[i]/C);
      sum1+=beta*t.freq[i];
      sum2+=beta*beta;
    }
    freq0=sum1/sum2;

    // Compute residuals
    for (i=0,rms=0.0;i<t.n;i++) 
      rms+=pow(t.freq[i]-(1.0-v[i]/C)*freq0,2);
    rms=sqrt(rms/(double) t.n);

    // Find TCA
    for (i=0,mjd0=0.0;i<t.n-1;i++) 
      if (v[i]*v[i-1]<0.0)
	mjd0=t.mjd[i];
    
    if (mjd0>0.0)
      mjd2nfd(mjd0,nfd);
    else
      strcpy(nfd,"0000-00-00T00:00:00");
    
    if (rms<1000) {
      printf("%05d: %s  %8.3f MHz %8.3f kHz\n",orb.satno,nfd,1e-6*freq0,1e-3*rms);
      if (flag==0 || rms<rmsmin) {
	satnomin=orb.satno;
	strcpy(nfdmin,nfd);
	freqmin=freq0;
	rmsmin=rms;
	flag=1;
      }
    }
  }
  fclose(file);
  fclose(stderr);

  if (flag==1) {
    printf("\nBest fitting object:\n");
    printf("%05d: %s  %8.3f MHz %8.3f kHz\n",satnomin,nfdmin,1e-6*freqmin,1e-3*rmsmin);
    printf("Store frequency? [y/n]\n");
    status=scanf("%s",text);
    if (text[0]=='y') {
      gettimeofday(&tv,0);
      strftime(tbuf,30,"%Y-%m-%dT%T",gmtime(&tv.tv_sec));
      file=fopen(freqlist,"a");
      fprintf(file,"%05d %8.3f %.19s %04d\n",satnomin,1e-6*freqmin,tbuf,s.id);
      fclose(file);
      file=fopen("log.txt","a");
      fprintf(file,"%05d %8.3f %.3f %.19s\n",satnomin,1e-6*freqmin,1e-3*rmsmin,nfdmin);
      fclose(file);
      printf("Frequency stored\n\n");
    }
  } else {
    printf("\nTrace not identified..\n");
  }

  // Free
  free(p);
  free(v);

  return;
}

// Compute trace
struct trace *compute_trace(char *tlefile,double *mjd,int n,int site_id,float freq,float bw,int *nsat,int graves)
{
  int i,j,imode,flag,satno,tflag,m,status;
  struct point *p;
  struct site s,sg;
  FILE *file,*infile;
  orbit_t orb;
  xyz_t satpos,satvel;
  double dx,dy,dz,dvx,dvy,dvz,r,v,za,vg;
  double freq0;
  char line[LIM],text[8];
  struct trace *t;
  float fmin,fmax;
  char *env,freqlist[LIM];
  double ra,de,azi,alt;

  env=getenv("ST_DATADIR");
  sprintf(freqlist,"%s/data/frequencies.txt",env);  

  // Frequency limits
  fmin=freq-0.5*bw;
  fmax=freq+0.5*bw;

  // Reloop stderr
  if (freopen("/tmp/stderr.txt","w",stderr)==NULL)
    fprintf(stderr,"Failed to redirect stderr\n");
  
  // Find number of satellites in frequency range
  infile=fopen(freqlist,"r");
  if (infile==NULL) {
    printf("%s not found\n",freqlist);
    return t;
  } else {
    for (i=0;;) {
      if (fgetline(infile,line,LIM)<=0)
	break;
      status=sscanf(line,"%d %lf",&satno,&freq0);
      
      if (freq0>=fmin && freq0<=fmax)
	i++;
    }
    fclose(infile);
    *nsat=i;
  }
  // Break out
  if (i==0)
    return t;

  // Valid MJDs
  for (i=0;i<n;i++)
    if (mjd[i]==0.0)
      break;
  m=i;

  // Allocate traces
  t=(struct trace *) malloc(sizeof(struct trace)* *nsat);

  // Get site
  s=get_site(site_id);

  // Allocate
  p=(struct point *) malloc(sizeof(struct point)*m);

  // Get observer position
  for (i=0;i<m;i++) 
    obspos_xyz(mjd[i],s.lng,s.lat,s.alt,&p[i].obspos,&p[i].obsvel);

  // Compute Graves positions
  if (graves==1) {
    sg=get_site(9999);
    for (i=0;i<m;i++) 
      obspos_xyz(mjd[i],sg.lng,sg.lat,sg.alt,&p[i].grpos,&p[i].grvel);
  }

  infile=fopen(freqlist,"r");
  for (j=0;;) {
    if (fgetline(infile,line,LIM)<=0)
      break;
    status=sscanf(line,"%d %lf",&satno,&freq0);
    if (freq0<fmin || freq0>fmax)
      continue;

    // Allocate
    t[j].satno=satno;
    t[j].site=site_id;
    t[j].n=m;
    t[j].freq0=freq0;
    t[j].mjd=(double *) malloc(sizeof(double)*m);
    t[j].freq=(double *) malloc(sizeof(double)*m);
    t[j].za=(float *) malloc(sizeof(float)*m);

    sprintf(text," %d",satno);
    // Loop over TLEs
    file=fopen(tlefile,"r");
    while (read_twoline(file,satno,&orb)==0) {
      // Initialize
      imode=init_sgdp4(&orb);
      if (imode==SGDP4_ERROR)
	printf("Error\n");
      
      // Loop over points
      for (i=0,flag=0,tflag=0;i<m;i++) {
	// Get satellite position
	satpos_xyz(mjd[i]+2400000.5,&satpos,&satvel);
	
	dx=satpos.x-p[i].obspos.x;  
	dy=satpos.y-p[i].obspos.y;
	dz=satpos.z-p[i].obspos.z;
	dvx=satvel.x-p[i].obsvel.x;
	dvy=satvel.y-p[i].obsvel.y;
	dvz=satvel.z-p[i].obsvel.z;
	r=sqrt(dx*dx+dy*dy+dz*dz);
	v=(dvx*dx+dvy*dy+dvz*dz)/r;
	za=acos((p[i].obspos.x*dx+p[i].obspos.y*dy+p[i].obspos.z*dz)/(r*XKMPER))*R2D;
	
	// Store
	t[j].mjd[i]=mjd[i];
	t[j].freq[i]=(1.0-v/C)*freq0;
	t[j].za[i]=za;

	// Compute Graves velocity/frequency
	if (graves==1) {
	  dx=satpos.x-p[i].grpos.x;  
	  dy=satpos.y-p[i].grpos.y;
	  dz=satpos.z-p[i].grpos.z;
	  dvx=satvel.x-p[i].grvel.x;
	  dvy=satvel.y-p[i].grvel.y;
	  dvz=satvel.z-p[i].grvel.z;
	  r=sqrt(dx*dx+dy*dy+dz*dz);
	  vg=(dvx*dx+dvy*dy+dvz*dz)/r;
	  ra=modulo(atan2(dy,dx)*R2D,360.0);
	  de=asin(dz/r)*R2D;
	  equatorial2horizontal(mjd[i],ra,de,sg.lng,sg.lat,&azi,&alt);

	  t[j].freq[i]=(1.0-v/C)*(1.0-vg/C)*freq0;
	  if (!((azi<90.0 || azi>270.0) && alt>15.0 && alt<40.0))
	    t[j].za[i]=100.0;

	}
      }
    }
    fclose(file);

    j++;
  }
  fclose(infile);
  fclose(stderr);

  // Free
  free(p);

  return t;
}
