/* Copyright © 2020 Björn Victor (bjorn@victor.se) */
/* Simple demonstration program for 
   NCP (Network Control Program) implementing Chaosnet transport layer
   for cbridge, the bridge program for various Chaosnet implementations. */
/*
   Licensed under the Apache License, Version 2.0 (the "License");
   you may not use this file except in compliance with the License.
   You may obtain a copy of the License at

       http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
   See the License for the specific language governing permissions and
   limitations under the License.
*/

// TODO: byte order for ANS data wrong here and in NCP.

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <ctype.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <time.h>

#include "cbridge-chaos.h"

void usage(char *s) 
{
  fprintf(stderr,"usage: %s host [options] [contact]\n"
	  " Handles \"simple\" connectionless Chaosnet protocols.\n"
	  " Contact defaults to STATUS. Try also TIME, UPTIME, DUMP-ROUTING-TABLE, LASTCN, FINGER, LOAD.\n"
	  "  (Contact name is not case sensitive.)\n"
	  " Options: -q quiet, -v verbose, -r raw output, -a ascii output, -t sec to set RFC timeout (default 30).\n",
	  s);
  exit(1);
}

char chaos_socket_directory[] = "/tmp";

static int
connect_to_named_socket(int socktype, char *path, int quiet)
{
  int sock, slen;
  struct sockaddr_un local, server;
  
  local.sun_family = AF_UNIX;
  sprintf(local.sun_path, "%s/%s_%d", chaos_socket_directory, path, getpid());
  if (unlink(local.sun_path) < 0) {
    //perror("unlink(chaos_sockfile)");
  } 
  
  if ((sock = socket(AF_UNIX, socktype, 0)) < 0) {
    if (!quiet) perror("socket(AF_UNIX)");
    exit(1);
  }
  slen = strlen(local.sun_path)+ 1 + sizeof(local.sun_family);
  if (bind(sock, (struct sockaddr *)&local, slen) < 0) {
    if (!quiet) perror("bind(local)");
    exit(1);
  }
  if (chmod(local.sun_path, 0777) < 0) {
    if (!quiet) perror("chmod(local, 0777)");
  }
  
  server.sun_family = AF_UNIX;
  sprintf(server.sun_path, "%s/%s", chaos_socket_directory, path);
  slen = strlen(server.sun_path)+ 1 + sizeof(server.sun_family);

  if (connect(sock, (struct sockaddr *)&server, slen) < 0) {
    if (!quiet) perror("connect(server)");
    exit(1);
  }
  return sock;
}


static char *
ch_char(unsigned char x, char *buf) {
  if (x < 32)
    sprintf(buf,"^%c", x+64);
  else if (x == 127)
    sprintf(buf,"^?");
  else if (x < 127)
    sprintf(buf,"%2c",x);
  else
    sprintf(buf,"%2x",x);
  return buf;
}

int
ch_11_gets(unsigned char *in, unsigned char *out, int nbytes)
{
  int i;
  // round up because the last byte might be in the lsb of the last word
  if (nbytes % 2) nbytes++;
  for (i = 0; i < nbytes; i++) {
    if (i % 2 == 1)
      out[i] = in[i-1];
    else
      out[i] = in[i+1];
  }
  out[i] = '\0';
  return i-1;
}

void print_buf(u_char *ucp, int len) 
{
  int row, i;
  char b1[3],b2[3];

  printf("Read %d bytes:\n", len);
  for (row = 0; row*8 < len; row++) {
    for (i = 0; (i < 8) && (i+row*8 < len); i++) {
      fprintf(stderr, "  %02x", ucp[i+row*8]);
      fprintf(stderr, "%02x", ucp[(++i)+row*8]);
    }
    fprintf(stderr, " (hex)\r\n");
#if 1
    for (i = 0; (i < 8) && (i+row*8 < len); i++) {
      fprintf(stderr, "  %2s", ch_char(ucp[i+row*8],(char *)&b1));
      fprintf(stderr, "%2s", ch_char(ucp[(++i)+row*8],(char *)&b2));
    }
    fprintf(stderr, " (chars)\r\n");
    for (i = 0; (i < 8) && (i+row*8 < len); i++) {
      fprintf(stderr, "  %2s", ch_char(ucp[i+1+row*8],(char *)&b1));
      fprintf(stderr, "%2s", ch_char(ucp[(i++)+row*8],(char *)&b2));
    }
    fprintf(stderr, " (11-chars)\r\n");
#endif
  }
}

void print_ascii_buf(u_char *bp, int len)
{
  char *buf = calloc(1, len+1);
  if (buf == NULL) {
    perror("calloc failed");
    exit(1);
  }
  // @@@@ should do standard conversion, at least \215 to \n
  strncpy(buf, (char *)bp, len);
  printf("%s\n", buf);
}

// ;;; Routing table format: for N subnets, N*4 bytes of data, holding N*2 words
// ;;; For subnet n, pkt[2n] has the method; if this is less than 400 (octal), it's
// ;;; an interface number; otherwise, it's a host which will forward packets to that
// ;;; subnet.  pkt[2n+1] has the host's idea of the cost.
void print_routing_table(u_char *bp, int len, u_short src)
{
  u_short *ep, *dp = (u_short *)bp;
  int i, sub, maxroutes = len/4;

  printf("Routing table received from host %#o\n", src);
  printf("%-8s %-8s %s\n", "Subnet", "Method", "Cost");
  for (sub = 0; sub < maxroutes; sub++) {
    if (dp[sub*2] != 0) {
      printf("%#-8o %#-8o %-8d\n",
	     sub, dp[sub*2], dp[sub*2+1]);
    }
  }
}

char *seconds_as_interval(u_int t)
{
  char tbuf[64], *tp = tbuf;
  
  if (t == 0)
    return strdup("now");

  if (t > 365*60*60*24) {
    int y = t/(365*60*60*24);
    sprintf(tp, "%d year%s ", y, y==1 ? "" : "s");
    t %= 365*60*60*24;
    tp += strlen(tp);
  }
  if (t > 60*60*24*7) {
    int w = t/(60*60*24*7);
    sprintf(tp, "%d week%s ", w, w == 1 ? "" : "s");
    t %= 60*60*24*7;
    tp += strlen(tp);
  }
  if (t > 60*60*24) {
    int d = t/(60*60*24);
    sprintf(tp, "%d day%s ", d, d == 1 ? "" : "s");
    t %= (60*60*24);
    tp = &tbuf[strlen(tbuf)];
  }
  if (t > 60*60) {
    int h = t/(60*60);
    sprintf(tp, "%d hour%s ", h, h == 1 ? "" : "s");
    t %= 60*60;
    tp = &tbuf[strlen(tbuf)];
  }
  if (t > 60)
    sprintf(tp, "%dm %ds", (t/60), t % 60);
  else
    sprintf(tp, "%d s", t);
  return strdup(tbuf);
}

void print_time(u_char *bp, int len, u_short src, int verbose)
{
  u_short *dp = (u_short *)bp;
  char tbuf[64];

  if (len != 4) { printf("Bad time length %d (expected 4)\n", len); exit(1); }

  time_t t = (u_short)(*dp++); t |= (u_long) ((u_short)(*dp)<<16);
  time_t here = time(NULL);
#if __APPLE__
  // imagine this
  t &= 0xffffffff;
#endif
  if (t > 2208988800UL) {  /* see RFC 868 */
    t -= 2208988800UL;
    strftime(tbuf, sizeof(tbuf), "%F %T", localtime(&t));
    if (verbose)
      printf("%s (diff %s%s)\n", tbuf, (t-here)==0?"": (t-here) > 0 ? "+":"-", 
	     (t-here)==0 ?"none":seconds_as_interval(labs(t-here)));
    else
      printf("%s\n", tbuf);
  } else 
    printf("Unexpected time value %ld <= %ld\n", t, 2208988800UL);
}

void print_uptime(u_char *bp, int len, u_short src)
{
  u_short *dp = (u_short *)bp;

  if (len != 4) { printf("Bad time length %d (expected 4)\n", len); exit(1); }

  u_int t = (u_short)(*dp++); t |= (u_long) ((u_short)(*dp)<<16);

  t /= 60;
  printf("Host %#o uptime: %s\n", src, seconds_as_interval(t));
}

void print_lastcn(u_char *bp, int len, u_short src)
{
  u_short *dp = (u_short *)bp;
  int i;
  
  // @@@@ prettyprint age, host?
  printf("Last seen at host %#o:\n", src);
  printf("%-8s %8s %-8s %-4s %s\n", "Host","#in","Via","FC","Age(s)");
  for (i = 0; i < len/2; ) {
    u_short wpe = (*dp++);
    if (wpe < 7) { printf("Unexpected WPE of LASTCN: %d should be >= 7\n", wpe); exit(1); }
    u_short addr = (*dp++);
    u_int in = (*dp++); in |= ((*dp++)<<16);
    u_short last = (*dp++);
    u_int age = (*dp++); age |= ((*dp++)<<16);
    if (wpe > 7) {
      u_short fc = (*dp++);
      printf("%#-8o %8d %#-8o %-4d %s\n", addr, in, last, fc, seconds_as_interval(age));
    } else
      printf("%#-8o %8d %#-8o %-4s %s\n", addr, in, last, "", seconds_as_interval(age));
    i += wpe;
  }
}

void print_load(u_char *bp, int len, u_short src)
{
  print_ascii_buf(bp, len);
}

void print_status(u_char *bp, int len, u_short src)
{
  u_char hname[32+1];
  u_short *dp;
  int i;

  // First 32 bytes contain the name of the node, padded on the right with zero bytes.
  memset(hname, 0, sizeof(hname));
  strncpy((char *)hname, (char *)bp, sizeof(hname)-1);
  printf("Hostat for host %s (%#o)\n", hname, src);
  bp += 32;

  dp = (u_short *)bp;
  u_short *ep = (u_short *)(bp+(len - 32));

  printf("%s \t%-8s %-8s %-8s %-8s %-8s %-8s %-8s %-8s\n",
	 "Net", "In", "Out", "Abort", "Lost", "crcerr", "ram", "Badlen", "Rejected");
  for (i = 0; dp < ep; i++) {
    u_short subnet = (*dp++);
    if ((subnet - 0400) < 0) { printf("Unexpected format of subnet: %#o (%#x)\n", subnet, subnet); exit(1); }
    subnet -= 0400;
    u_short elen = (*dp++);
    u_int in = (*dp++); in |= ((*dp++)<<16);
    u_int out = (*dp++); out |= ((*dp++)<<16);
    if (elen == 4) {		/* TOPS-20 */
      printf("%#o \t%-8d %-8d\n",
	     subnet, in, out);
    } else {      
      u_int aborted = (*dp++); aborted |= ((*dp++)<<16);
      u_int lost = (*dp++); lost |= ((*dp++)<<16);
      u_int crcerr = (*dp++); crcerr |= ((*dp++)<<16);
      u_int crcerr_post = (*dp++); crcerr_post |= ((*dp++)<<16);
      u_int badlen = (*dp++); badlen |= ((*dp++)<<16);
      u_int rejected = 0;
      if (elen == 16) {
	rejected = (*dp++); rejected |= ((*dp++)<<16);
      }
      printf("%#o \t%-8d %-8d %-8d %-8d %-8d %-8d %-8d %-8d\n",
	     subnet, in, out, aborted, lost, crcerr, crcerr_post, badlen, rejected);
    }
  }
}

void
print_finger_info(u_char *bp, int len, char *host, u_short src)
{
  u_char *nl;
  u_char *uid = NULL, *loc = NULL, *idle = NULL, *pname = NULL, *aff = NULL;
  uid = bp;
  if ((nl = (u_char *)index((char *)bp, 0215)) != NULL) {
    *nl = '\0';
    loc = ++nl;
    if ((nl = (u_char *)index((char *)nl, 0215)) != NULL) {
      *nl = '\0';
      idle = ++nl;
      if ((nl = (u_char *)index((char *)nl, 0215)) != NULL) {
	*nl = '\0';
	pname = ++nl;
	if ((nl = (u_char *)index((char *)nl, 0215)) != NULL) {
	  *nl = '\0';
	  aff = ++nl;
	}
      }
    }
  }
  char nmbuf[8+5+3];
  sprintf(nmbuf,"User at %#o", src);
  printf("%-15s %.1s %-22s %-10s %5s    %s\n"
	 "%-15.15s %.1s %-22.22s %-10.10s %5.5s    %s\n",
	 nmbuf," ","Name","Host","Idle","Location",
	 uid,aff,pname,host,idle,loc);
}

int
main(int argc, char *argv[])
{
  signed char c;
  char opts[] = "qvrat:";		// quiet, verbose, raw, timeout X
  char *host, *contact = "STATUS", *pname, *space;
  char buf[CH_PK_MAXLEN+2];
  char *nl, *bp;
  int i, cnt, sock, anslen, ncnt, raw = 0, ascii = 0, timeout = 0, quiet = 0, verbose = 0;
  u_short src;

  pname = argv[0];

  while ((c = getopt(argc, argv, opts)) != -1) {
    switch (c) {
    case 'r': raw = 1; break;
    case 'a': ascii = 1; break;
    case 'q': quiet = 1; break;
    case 'v': verbose = 1; break;
    case 't': timeout = atoi(optarg); break;
    default:
      fprintf(stderr,"unknown option '%c'\n", c);
      usage(pname);
    }
  }
  argc -= optind;
  argv += optind;

  if (argc < 1) 
    usage(pname);

  host = argv[0];
  if (argc > 1)
    contact = argv[1];

  sock = connect_to_named_socket(SOCK_STREAM, "chaos_stream", quiet);
  
  // printf("Trying %s %s...\n", host, contact);
  if (timeout > 0)
    dprintf(sock,"RFC [timeout=%d] %s %s\r\n", timeout, host, contact);
  else
    dprintf(sock,"RFC %s %s\r\n", host, contact);

  if ((cnt = recv(sock, buf, sizeof(buf), 0)) < 0) {
    if (!quiet) perror("recv"); 
    exit(1);
  }
  buf[cnt] = '\0';
  nl = index((char *)buf, '\n');
  if (nl != NULL) {
    *nl = '\0';
    nl++;
  }

  if (strncmp(buf, "ANS ", 4) != 0) {
    if (nl != NULL) *nl = '\0';
    if (!quiet) fprintf(stderr,"Unexpected reply from %s: %s\n", host, buf);
    exit(1);
  }
  if (sscanf(&buf[4],"%ho", &src) != 1) {
    if (!quiet) fprintf(stderr,"Cannot parse ANS source address: %s\n", buf);
    exit(1);
  }
  space = index(&buf[4], ' ');
  if ((space == NULL) || sscanf(space+1, "%d", &anslen) != 1) {
    if (!quiet) fprintf(stderr, "Cannot parse ANS length: %s\n", buf);
    exit(1);
  }
#if 0
  fprintf(stderr,"Got ANS from %#o, %d bytes\n", src, anslen);
#endif
  for (bp = nl+cnt; bp-nl < anslen; ncnt = recv(sock, bp, sizeof(buf)-(bp-buf), 0)) {
    cnt += ncnt;
    bp += ncnt;
  }
  if (quiet)
    ;
  else if (raw)
    print_buf((u_char *)nl, anslen);
  else if (strcasecmp(contact, "STATUS") == 0)
    print_status((u_char *)nl, anslen, src);
  else if (strcasecmp(contact, "TIME") == 0)
    print_time((u_char *)nl, anslen, src, verbose);
  else if (strcasecmp(contact, "UPTIME") == 0)
    print_uptime((u_char *)nl, anslen, src);
  else if (strcasecmp(contact, "DUMP-ROUTING-TABLE") == 0)
    print_routing_table((u_char *)nl, anslen, src);
  else if (strcasecmp(contact, "FINGER") == 0)
    print_finger_info((u_char *)nl, anslen, host, src);
  else if (strcasecmp(contact, "LASTCN") == 0)
    print_lastcn((u_char *)nl, anslen, src);
  else if (strcasecmp(contact, "LOAD") == 0)
    print_load((u_char *)nl, anslen, src);
  else if (ascii) 
    print_ascii_buf((u_char *)nl, anslen);
  else
    print_buf((u_char *)nl, anslen);
  exit(0);
}
