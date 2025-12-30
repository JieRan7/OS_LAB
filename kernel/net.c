#include "types.h"
#include "param.h"
#include "memlayout.h"
#include "riscv.h"
#include "spinlock.h"
#include "proc.h"
#include "defs.h"
#include "fs.h"
#include "sleeplock.h"
#include "file.h"
#include "net.h"

// xv6's ethernet and IP addresses
static uint8 local_mac[ETHADDR_LEN] = { 0x52, 0x54, 0x00, 0x12, 0x34, 0x56 };
static uint32 local_ip = MAKE_IP_ADDR(10, 0, 2, 15);

// qemu host's ethernet address.
static uint8 host_mac[ETHADDR_LEN] = { 0x52, 0x55, 0x0a, 0x00, 0x02, 0x02 };

static struct spinlock netlock;

// add

int port_bindings[MAX_PORTS];
static struct spinlock netlock;
static struct spinlock portlock;
struct udp_queue udp_queues[MAX_PORTS];   //UDP包队列，每个端口都有一个队列

void
netinit(void)
{
  initlock(&netlock, "netlock");
  initlock(&portlock, "portlock");
  memset(port_bindings, 0, sizeof(port_bindings));
  for(int i = 0; i < MAX_PORTS; i++) {//初始化做相应的修改
    udp_queues[i].head = 0;
    udp_queues[i].tail = 0;
    udp_queues[i].count = 0;
  }
}


//
// bind(int port)
// prepare to receive UDP packets address to the port,
// i.e. allocate any queues &c needed.
//
uint64
sys_bind(void)
{
  //
  // Your code here.
  //

  int port;
  argint(0, &port);
  if(port < 0 || port > MAX_PORTS-1)
    return -1;
  acquire(&portlock);
  if(port_bindings[port] != 0){
    release(&portlock);
    return -1;
  }
  port_bindings[port] = myproc()->pid;
  release(&portlock);
  return 0;
}

//
// unbind(int port)
// release any resources previously created by bind(port);
// from now on UDP packets addressed to port should be dropped.
//
uint64
sys_unbind(void)
{
  //
  // Optional: Your code here.
  //
  
  int port;
  argint(0, &port);
  if(port < 0 || port > MAX_PORTS-1)
    return -1;
  acquire(&portlock);
  if(port_bindings[port] == 0){
    release(&portlock);
    return -1;
  }
  port_bindings[port] = 0;
  release(&portlock);
  return 0;
}

//
// recv(int dport, int *src, short *sport, char *buf, int maxlen)
// if there's a received UDP packet already queued that was
// addressed to dport, then return it.
// otherwise wait for such a packet.
//
// sets *src to the IP source address.
// sets *sport to the UDP source port.
// copies up to maxlen bytes of UDP payload to buf.
// returns the number of bytes copied,
// and -1 if there was an error.
//
// dport, *src, and *sport are host byte order.
// bind(dport) must previously have been called.
//

uint64
sys_recv(void)
{
  //
  // Your code here.
  //
  
  int dport;
  uint64 srcaddr,sportaddr,bufaddr;
  int maxlen;

  argint(0, &dport);
  argaddr(1, &srcaddr);
  argaddr(2, &sportaddr);
  argaddr(3, &bufaddr);
  argint(4, &maxlen);
  acquire(&portlock);
  if (port_bindings[dport] == 0) {
    release(&portlock);
    return -1;  // 端口未绑定，返回错误
  }
  struct udp_queue *queue = &udp_queues[dport];
  while (queue->count == 0)
    sleep(queue, &portlock);
  struct udp_packet *packet = queue->packets[queue->head];
  if (copyout(myproc()->pagetable, srcaddr, (char *)&packet->sip, sizeof(packet->sip)) < 0 ||
      copyout(myproc()->pagetable, sportaddr, (char *)&packet->sport, sizeof(packet->sport)) < 0) {
    release(&portlock);
    return -1;  // 复制数据失败
  }

  int copylen = packet->len < maxlen ? packet->len : maxlen;
  if (copyout(myproc()->pagetable, bufaddr, packet->payload, copylen) < 0) {
    release(&portlock);
    return -1;  // 复制数据失败
  }
  queue->head = (queue->head + 1) % UDP_QUEUE_LIMIT;
  queue->count--;
  kfree(packet);
  release(&portlock);

  return copylen;
}

// This code is lifted from FreeBSD's ping.c, and is copyright by the Regents
// of the University of California.
static unsigned short
in_cksum(const unsigned char *addr, int len)
{
  int nleft = len;
  const unsigned short *w = (const unsigned short *)addr;
  unsigned int sum = 0;
  unsigned short answer = 0;

  /*
   * Our algorithm is simple, using a 32 bit accumulator (sum), we add
   * sequential 16 bit words to it, and at the end, fold back all the
   * carry bits from the top 16 bits into the lower 16 bits.
   */
  while (nleft > 1)  {
    sum += *w++;
    nleft -= 2;
  }

  /* mop up an odd byte, if necessary */
  if (nleft == 1) {
    *(unsigned char *)(&answer) = *(const unsigned char *)w;
    sum += answer;
  }

  /* add back carry outs from top 16 bits to low 16 bits */
  sum = (sum & 0xffff) + (sum >> 16);
  sum += (sum >> 16);
  /* guaranteed now that the lower 16 bits of sum are correct */

  answer = ~sum; /* truncate to 16 bits */
  return answer;
}

//
// send(int sport, int dst, int dport, char *buf, int len)
//
uint64
sys_send(void)
{
  struct proc *p = myproc();
  int sport;
  int dst;
  int dport;
  uint64 bufaddr;
  int len;

  argint(0, &sport);
  argint(1, &dst);
  argint(2, &dport);
  argaddr(3, &bufaddr);
  argint(4, &len);

  int total = len + sizeof(struct eth) + sizeof(struct ip) + sizeof(struct udp);
  if(total > PGSIZE)
    return -1;

  char *buf = kalloc();
  if(buf == 0){
    printf("sys_send: kalloc failed\n");
    return -1;
  }
  memset(buf, 0, PGSIZE);

  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, host_mac, ETHADDR_LEN);
  memmove(eth->shost, local_mac, ETHADDR_LEN);
  eth->type = htons(ETHTYPE_IP);

  struct ip *ip = (struct ip *)(eth + 1);
  ip->ip_vhl = 0x45; // version 4, header length 4*5
  ip->ip_tos = 0;
  ip->ip_len = htons(sizeof(struct ip) + sizeof(struct udp) + len);
  ip->ip_id = 0;
  ip->ip_off = 0;
  ip->ip_ttl = 100;
  ip->ip_p = IPPROTO_UDP;
  ip->ip_src = htonl(local_ip);
  ip->ip_dst = htonl(dst);
  ip->ip_sum = in_cksum((unsigned char *)ip, sizeof(*ip));

  struct udp *udp = (struct udp *)(ip + 1);
  udp->sport = htons(sport);
  udp->dport = htons(dport);
  udp->ulen = htons(len + sizeof(struct udp));

  char *payload = (char *)(udp + 1);
  if(copyin(p->pagetable, payload, bufaddr, len) < 0){
    kfree(buf);
    printf("send: copyin failed\n");
    return -1;
  }

  e1000_transmit(buf, total);

  return 0;
}

void
ip_rx(char *buf, int len)
{
  // don't delete this printf; make grade depends on it.
  static int seen_ip = 0;
  if(seen_ip == 0)
    printf("ip_rx: received an IP packet\n");
  seen_ip = 1;

  //
  // Your code here.
  //
  
  struct eth *eth = (struct eth *)buf;
  // 解析 IP 头
  struct ip *ip = (struct ip *)(eth + 1);
  if(ip->ip_p != IPPROTO_UDP){//不是UDP包
    kfree(buf);
    return;
  }
  struct udp *udp = (struct udp *)(ip + 1);
  int dport = ntohs(udp->dport);
  int sport = ntohs(udp->sport);
  int ulen = ntohs(udp->ulen)-sizeof(struct udp);//UDP有效载荷长度
  acquire(&portlock);
  if(port_bindings[dport] == 0){//没有绑定端口
    release(&portlock);
    // printf("ip_rx: port %d not bound\n", dport);
    kfree(buf);
    return;
  }

  struct udp_queue *queue = &udp_queues[dport];
  if(queue->count>=UDP_QUEUE_LIMIT){//队列满
    release(&portlock);
    kfree(buf);
    return;
  }
  struct udp_packet *packet;
  if((packet = kalloc()) == 0){
    release(&portlock);
    kfree(buf);
    return;
  }
  packet->sip = ntohl(ip->ip_src);
  packet->sport = sport;
  packet->len = ulen;
  memmove(packet->payload, (char *)udp+sizeof(struct udp) , ulen);
  queue->packets[queue->tail] = packet;
  queue->tail = (queue->tail + 1) % UDP_QUEUE_LIMIT;
  queue->count++;
  release(&portlock);
  kfree(buf);
  wakeup(queue);
}

//
// send an ARP reply packet to tell qemu to map
// xv6's ip address to its ethernet address.
// this is the bare minimum needed to persuade
// qemu to send IP packets to xv6; the real ARP
// protocol is more complex.
//
void
arp_rx(char *inbuf)
{
  static int seen_arp = 0;

  if(seen_arp){
    kfree(inbuf);
    return;
  }
  printf("arp_rx: received an ARP packet\n");
  seen_arp = 1;

  struct eth *ineth = (struct eth *) inbuf;
  struct arp *inarp = (struct arp *) (ineth + 1);

  char *buf = kalloc();
  if(buf == 0)
    panic("send_arp_reply");
  
  struct eth *eth = (struct eth *) buf;
  memmove(eth->dhost, ineth->shost, ETHADDR_LEN); // ethernet destination = query source
  memmove(eth->shost, local_mac, ETHADDR_LEN); // ethernet source = xv6's ethernet address
  eth->type = htons(ETHTYPE_ARP);

  struct arp *arp = (struct arp *)(eth + 1);
  arp->hrd = htons(ARP_HRD_ETHER);
  arp->pro = htons(ETHTYPE_IP);
  arp->hln = ETHADDR_LEN;
  arp->pln = sizeof(uint32);
  arp->op = htons(ARP_OP_REPLY);

  memmove(arp->sha, local_mac, ETHADDR_LEN);
  arp->sip = htonl(local_ip);
  memmove(arp->tha, ineth->shost, ETHADDR_LEN);
  arp->tip = inarp->sip;

  e1000_transmit(buf, sizeof(*eth) + sizeof(*arp));

  kfree(inbuf);
}

void
net_rx(char *buf, int len)
{
  struct eth *eth = (struct eth *) buf;

  if(len >= sizeof(struct eth) + sizeof(struct arp) &&
     ntohs(eth->type) == ETHTYPE_ARP){
    arp_rx(buf);
  } else if(len >= sizeof(struct eth) + sizeof(struct ip) &&
     ntohs(eth->type) == ETHTYPE_IP){
    ip_rx(buf, len);
  } else {
    kfree(buf);
  }
}
