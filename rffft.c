#include <stdio.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <fftw3.h>
#include <getopt.h>
#include <time.h>
#include <sys/time.h>
#include "rftime.h"

void usage(void)
{
  printf("rffft: FFT RF observations\n\n");
  printf("-i <file>       Input file (can be fifo)\n");
  printf("-p <prefix>     Output prefix\n");
  printf("-f <frequency>  Center frequency (Hz)\n");
  printf("-s <samprate>   Sample rate (Hz)\n");
  printf("-c <chansize>   Channel size [100Hz]\n");
  printf("-t <tint>       Integration time [1s]\n");
  printf("-n <nsub>       Number of integrations per file [60]\n");
  printf("-m <use>        Use every mth integration [1]\n");
  printf("-F <format>     Input format char, int, float [int]\n");
  printf("-T <start time> YYYY-MM-DDTHH:MM:SSS.sss\n");
  printf("-b              Digitize output to bytes [off]\n");
  printf("-q              Quiet mode, no output [off]\n");
  printf("-h              This help\n");

  return;
}

int main(int argc,char *argv[])
{
  int i,j,k,l,m,nchan,nint=1,arg=0,nbytes,nsub=60,flag,nuse=1,realtime=1,quiet=0;
  fftwf_complex *c,*d;
  fftwf_plan fft;
  FILE *infile,*outfile;
  char infname[128],outfname[128],path[64]=".",prefix[32]="";
  char informat='i',outformat='f';
  int16_t *ibuf;
  char *cbuf;
  float *fbuf;
  float *z,length,fchan=100.0,tint=1.0,zavg,zstd,*zw;
  char *cz;
  double freq,samp_rate,mjd;
  struct timeval start,end;
  char tbuf[30],nfd[32],header[256]="";

  // Read arguments
  if (argc>1) {
    while ((arg=getopt(argc,argv,"i:f:s:c:t:p:n:hm:F:T:bq"))!=-1) {
      switch(arg) {
	
      case 'i':
	strcpy(infname,optarg);
	break;
	
      case 'p':
	strcpy(path,optarg);
	break;
	
      case 'f':
	freq=(double) atof(optarg);
	break;
	
      case 's':
	samp_rate=(double) atof(optarg);
	break;
	
      case 'c':
	fchan=atof(optarg);
	break;
	
      case 'F':
	if (strcmp(optarg,"char")==0)
	  informat='c';
	else if (strcmp(optarg,"int")==0)
	  informat='i';
	else if (strcmp(optarg,"float")==0)
	  informat='f';
	break;

      case 'b':
	outformat='c';
	break;

      case 'n':
	nsub=atoi(optarg);
	break;

      case 'q':
	quiet=1;
	break;

      case 'm':
	nuse=atoi(optarg);
	break;
	
      case 't':
	tint=atof(optarg);
	break;
	
      case 'T':
	strcpy(nfd,optarg);
	realtime=0;
	break;

      case 'h':
	usage();
	return 0;

      default:
	usage();
	return 0;
      }
    }
  } else {
    usage();
    return 0;
  }

  // Number of channels
  nchan=(int) (samp_rate/fchan);

  // Number of integrations
  nint=(int) (tint*(float) samp_rate/(float) nchan);

  // Dump statistics
  printf("Filename: %s\n",infname);
  printf("Frequency: %f MHz\n",freq*1e-6);
  printf("Bandwidth: %f MHz\n",samp_rate*1e-6);
  printf("Sampling time: %f us\n",1e6/samp_rate);
  printf("Number of channels: %d\n",nchan);
  printf("Channel size: %f Hz\n",samp_rate/(float) nchan);
  printf("Integration time: %f s\n",tint);
  printf("Number of averaged spectra: %d\n",nint);
  printf("Number of subints per file: %d\n",nsub);

  // Allocate
  c=fftwf_malloc(sizeof(fftwf_complex)*nchan);
  d=fftwf_malloc(sizeof(fftwf_complex)*nchan);
  ibuf=(int16_t *) malloc(sizeof(int16_t)*2*nchan);
  cbuf=(char *) malloc(sizeof(char)*2*nchan);
  fbuf=(float *) malloc(sizeof(float)*2*nchan);
  z=(float *) malloc(sizeof(float)*nchan);
  cz=(char *) malloc(sizeof(char)*nchan);
  zw=(float *) malloc(sizeof(float)*nchan);

  // Compute window
  for (i=0;i<nchan;i++)
    zw[i]=0.54-0.46*cos(2.0*M_PI*i/(nchan-1));
  

  // Plan
  fft=fftwf_plan_dft_1d(nchan,c,d,FFTW_FORWARD,FFTW_ESTIMATE);

  // Create prefix
  if (realtime==1) {
    gettimeofday(&start,0);
    strftime(prefix,30,"%Y-%m-%dT%T",gmtime(&start.tv_sec));
  } else {
    sprintf(prefix,"%.19s",nfd);
    mjd=nfd2mjd(nfd);
  }
  
  // Open file
  infile=fopen(infname,"r");

  // Forever loop
  for (m=0;;m++) {
    // File name
    sprintf(outfname,"%s/%s_%06d.bin",path,prefix,m);
    outfile=fopen(outfname,"w");

    // Loop over subints to dump
    for (k=0;k<nsub;k++) {
      // Initialize
      for (i=0;i<nchan;i++) 
	z[i]=0.0;
      
      // Log start time
      gettimeofday(&start,0);
      
      // Integrate
      for (j=0;j<nint;j++) {
	// Read buffer
	if (informat=='i')
	  nbytes=fread(ibuf,sizeof(int16_t),2*nchan,infile);
	else if (informat=='c')
	  nbytes=fread(cbuf,sizeof(char),2*nchan,infile);
	else if (informat=='f')
	  nbytes=fread(fbuf,sizeof(float),2*nchan,infile);

	// End on empty buffer
	if (nbytes==0)
	  break;

	// Skip buffer
	if (j%nuse!=0)
	  continue;

	// Unpack 
	if (informat=='i') {
	  for (i=0;i<nchan;i++) {
	    c[i][0]=(float) ibuf[2*i]/32768.0*zw[i];
	    c[i][1]=(float) ibuf[2*i+1]/32768.0*zw[i];
	  } 
	} else if (informat=='c') {
	  for (i=0;i<nchan;i++) {
	    c[i][0]=(float) cbuf[2*i]/256.0*zw[i];
	    c[i][1]=(float) cbuf[2*i+1]/256.0*zw[i];
	  } 
	} else if (informat=='f') {
	  for (i=0;i<nchan;i++) {
	    c[i][0]=(float) fbuf[2*i]*zw[i];
	    c[i][1]=(float) fbuf[2*i+1]*zw[i];
	  } 
	}
	  
	// Execute
	fftwf_execute(fft);
	
	// Add
	for (i=0;i<nchan;i++) {
	  if (i<nchan/2)
	    l=i+nchan/2;
	  else
	    l=i-nchan/2;
	  
	  //z[l]+=sqrt(d[i][0]*d[i][0]+d[i][1]*d[i][1]);
	  z[l]+=d[i][0]*d[i][0]+d[i][1]*d[i][1];
	}
      }

      // Log end time
      gettimeofday(&end,0);

      // Time stats
      length=(end.tv_sec-start.tv_sec)+(end.tv_usec-start.tv_usec)*1e-6;
      
      // Scale
      for (i=0;i<nchan;i++) 
	z[i]*=(float) nuse/(float) nchan;
      
      // Scale to bytes
      if (outformat=='c') {
	// Compute average
	for (i=0,zavg=0.0;i<nchan;i++)
	  zavg+=z[i];
	zavg/=(float) nchan;
	
      // Compute standard deviation
	for (i=0,zstd=0.0;i<nchan;i++)
	  zstd+=pow(z[i]-zavg,2);
	zstd=sqrt(zstd/(float) nchan);

	// Convert
	for (i=0;i<nchan;i++) {
	  z[i]=256.0/6.0*(z[i]-zavg)/zstd;
	  if (z[i]<-128.0)
	    z[i]=-128.0;
	  if (z[i]>127.0)
	    z[i]=127.0;
	  cz[i]=(char) z[i];
	}
      }

      // Format start time
      if (realtime==1) {
	strftime(tbuf,30,"%Y-%m-%dT%T",gmtime(&start.tv_sec));
	sprintf(nfd,"%s.%03ld",tbuf,start.tv_usec/1000);
      } else {
	mjd2nfd(mjd+(m*nsub+k)*tint/86400.0,nfd); 
	length=tint;
      }

      // Header
      if (outformat=='f') 
	sprintf(header,"HEADER\nUTC_START    %s\nFREQ         %lf Hz\nBW           %lf Hz\nLENGTH       %f s\nNCHAN        %d\nNSUB         %d\nEND\n",nfd,freq,samp_rate,length,nchan,nsub);
      else if (outformat=='c')
	sprintf(header,"HEADER\nUTC_START    %s\nFREQ         %lf Hz\nBW           %lf Hz\nLENGTH       %f s\nNCHAN        %d\nNSUB         %d\nNBITS         8\nMEAN         %e\nRMS          %e\nEND\n",nfd,freq,samp_rate,length,nchan,nsub,zavg,zstd);

      // Limit output
      if (!quiet)
	printf("%s %s %f %d\n",outfname,nfd,length,j);
      
      // Dump file
      fwrite(header,sizeof(char),256,outfile);
      if (outformat=='f')
	fwrite(z,sizeof(float),nchan,outfile);
      else if (outformat=='c')
	fwrite(cz,sizeof(char),nchan,outfile);
      
      // Break;
      if (nbytes==0)
	break;
    }

    // Break;
    if (nbytes==0)
      break;
    
    // Close file
    fclose(outfile);
  }
  fclose(infile);

  // Destroy plan
  fftwf_destroy_plan(fft);

  // Deallocate
  free(ibuf);
  fftwf_free(c);
  fftwf_free(d);
  free(z);
  free(cz);
  free(zw);
  
  return 0;
}

