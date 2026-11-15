/* SPDX-License-Identifier: GPL-2.0-only */
#define _GNU_SOURCE
#include "nbdriver.h"
#include <arpa/inet.h>
#include <errno.h>
#include <poll.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <time.h>
#include <unistd.h>

static int checks;
static void check(int ok,const char *what){if(!ok){fprintf(stderr,"FAIL %s errno=%d\n",what,errno);exit(1);}++checks;}
static uint64_t now_ms(void){struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (uint64_t)t.tv_sec*1000+t.tv_nsec/1000000;}
static void verify_packet(int fd,uint64_t generation,uint16_t port,unsigned length){
    struct nbdriver_udp_packet packets[NBDRIVER_UDP_READ_MAX];struct nbdriver_udp_status status;
    uint64_t deadline=now_ms()+2000;int found=0;
    while(now_ms()<deadline&&!found){
        struct pollfd wait={fd,POLLIN,0};int ready=poll(&wait,1,200);
        check(ready>=0,"poll capture");if(!ready)continue;
        check(!(wait.revents&(POLLHUP|POLLERR)),"capture remains active");
        int n=nbdriver_udp_capture_read(fd,packets,NBDRIVER_UDP_READ_MAX,generation,&status);check(n>=0,"read capture batch");
        for(int i=0;i<n;++i){
            const struct nbdriver_udp_packet *p=&packets[i];
            if(p->dst_port!=port||p->original_len!=length||p->payload_len<16||memcmp(p->payload,"NBD-UDP-FIXTURE!",16))continue;
            check(p->timestamp_ns&&p->sequence,"kernel timestamp and sequence");
            check(p->payload_len==(length>2048?2048:length),"captured length");
            check(!!(p->flags&NBDRIVER_UDP_F_TRUNCATED)==(length>2048),"truncation flag");
            for(unsigned j=16;j<p->payload_len;++j)check(p->payload[j]==(unsigned char)j,"raw payload integrity");
            for(unsigned j=p->payload_len;j<2048;++j)check(!p->payload[j],"unused tail is zero");
            found=1;
        }
    }
    check(found,"self-owned loopback datagram captured");
}
int main(void){
    int fd=nbdriver_open();check(fd>=0,"connect driver");struct nbdriver_abi_info abi;
    check(!nbdriver_get_abi(fd,&abi),"query ABI");
    if(!(abi.features&NBDRIVER_FEATURE_UDP_CAPTURE)){printf("SKIP UDP feature absent: ABI=%u features=0x%x; flash the new module before this test\n",abi.version,abi.features);close(fd);return 77;}
    int receiver=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0),sender=socket(AF_INET,SOCK_DGRAM|SOCK_CLOEXEC,0);check(receiver>=0&&sender>=0,"create own fixture sockets");
    struct sockaddr_in address={.sin_family=AF_INET,.sin_addr.s_addr=htonl(INADDR_LOOPBACK)};
    check(!bind(receiver,(struct sockaddr*)&address,sizeof(address)),"bind own receiver");socklen_t size=sizeof(address);check(!getsockname(receiver,(struct sockaddr*)&address,&size),"fixture port");
    struct timeval timeout={2,0};check(!setsockopt(receiver,SOL_SOCKET,SO_RCVTIMEO,&timeout,sizeof(timeout)),"receiver timeout");
    struct nbdriver_udp_start_request start;
    check(!nbdriver_udp_capture_start(fd,getpid(),16,2048,0,&start),"start capture of own UID");uint64_t first_generation=start.generation;
    int other=nbdriver_open();check(other>=0,"open unrelated client");struct nbdriver_udp_start_request busy;
    check(nbdriver_udp_capture_start(other,getpid(),16,2048,0,&busy)==-1&&errno==EBUSY,"second owner cannot replace capture");close(other);
    int duplicate=dup(fd);check(duplicate>=0,"duplicate owning file");close(fd);fd=duplicate;
    unsigned char payload[3072],received[3072];memcpy(payload,"NBD-UDP-FIXTURE!",16);for(unsigned i=16;i<sizeof(payload);++i)payload[i]=(unsigned char)i;
    for(unsigned length=64;length<=3072;length=3072){
        check(sendto(sender,payload,length,0,(struct sockaddr*)&address,sizeof(address))==(ssize_t)length,"send own fixture");
        verify_packet(fd,start.generation,ntohs(address.sin_port),length);
        check(recv(receiver,received,sizeof(received),0)==(ssize_t)length&&!memcmp(received,payload,length),"original socket receives unmodified packet");
        if(length==3072)break;
    }
    check(!nbdriver_udp_capture_stop(fd),"stop capture");
    struct nbdriver_udp_status status;check(!nbdriver_udp_capture_status(fd,&status)&&!status.active&&!status.queued,"stopped queue cleared");
    check(!nbdriver_udp_capture_start(fd,getpid(),16,2048,0,&start)&&start.generation!=first_generation,"fresh generation on restart");
    close(fd);fd=nbdriver_open();check(fd>=0,"reopen after owner final close");
    check(!nbdriver_udp_capture_start(fd,getpid(),16,2048,0,&start),"final close released capture ownership");
    check(!nbdriver_udp_capture_stop(fd),"final stop");close(fd);close(receiver);close(sender);
    printf("PASS UDP kernel fixture: %d checks; self-owned loopback capture, payload/truncation, poll, unrelated close, dup, restart, final-close cleanup\n",checks);return 0;
}
