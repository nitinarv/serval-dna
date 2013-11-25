/*
Serval Distributed Numbering Architecture (DNA)
Copyright (C) 2010 Paul Gardner-Stephen
 
This program is free software; you can redistribute it and/or
modify it under the terms of the GNU General Public License
as published by the Free Software Foundation; either version 2
of the License, or (at your option) any later version.
 
This program is distributed in the hope that it will be useful,
but WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
GNU General Public License for more details.
 
You should have received a copy of the GNU General Public License
along with this program; if not, write to the Free Software
Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA  02110-1301, USA.
*/

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <assert.h>
#include <time.h>
#include <fnmatch.h>
#include "serval.h"
#include "conf.h"
#include "strbuf.h"
#include "strbuf_helpers.h"
#include "overlay_buffer.h"
#include "overlay_packet.h"
#include "str.h"
#include "radio_link.h"

#ifdef HAVE_IFADDRS_H
#include <ifaddrs.h>
#endif

int overlay_ready=0;
int overlay_interface_count=0;
overlay_interface overlay_interfaces[OVERLAY_MAX_INTERFACES];
int overlay_last_interface_number=-1;

struct profile_total interface_poll_stats;

struct sched_ent sock_any;
struct sockaddr_in sock_any_addr;
struct profile_total sock_any_stats;

static void overlay_interface_poll(struct sched_ent *alarm);
static int re_init_socket(int interface_index);

static void
overlay_interface_close(overlay_interface *interface){
  link_interface_down(interface);
  INFOF("Interface %s addr %s is down", 
	interface->name, inet_ntoa(interface->address.sin_addr));
  unschedule(&interface->alarm);
  unwatch(&interface->alarm);
  close(interface->alarm.poll.fd);
  if (interface->radio_link_state)
    radio_link_free(interface);
  interface->alarm.poll.fd=-1;
  interface->state=INTERFACE_STATE_DOWN;
}

void interface_state_html(struct strbuf *b, struct overlay_interface *interface)
{
  switch(interface->state){
    case INTERFACE_STATE_UP:
      strbuf_sprintf(b, "Interface %s is Up<br>", interface->name);
      break;
    default:
      strbuf_puts(b, "Interface Down");
      return;
  }
  switch(interface->type){
    case OVERLAY_INTERFACE_PACKETRADIO:
      strbuf_puts(b, "Type: Packet Radio<br>");
      radio_link_state_html(b, interface);
      break;
    case OVERLAY_INTERFACE_ETHERNET:
      strbuf_puts(b, "Type: Ethernet<br>");
      break;
    case OVERLAY_INTERFACE_WIFI:
      strbuf_puts(b, "Type: Wifi<br>");
      break;
    default:
    case OVERLAY_INTERFACE_UNKNOWN:
      strbuf_puts(b, "Type: Unknown<br>");
  }
  switch(interface->socket_type){
    case SOCK_STREAM:
      strbuf_puts(b, "Socket: Stream<br>");
      break;
    case SOCK_DGRAM:
      {
	char addrtxt[INET_ADDRSTRLEN];
	strbuf_puts(b, "Socket: DGram<br>");
	if (inet_ntop(AF_INET, (const void *)&interface->address.sin_addr, addrtxt, INET_ADDRSTRLEN))
	  strbuf_sprintf(b, "Address: %s:%d<br>", addrtxt, ntohs(interface->address.sin_port));
	if (inet_ntop(AF_INET, (const void *)&interface->destination->address.sin_addr, addrtxt, INET_ADDRSTRLEN))
	  strbuf_sprintf(b, "Broadcast Address: %s:%d<br>", addrtxt, ntohs(interface->destination->address.sin_port));
      }
      break;
    case SOCK_FILE:
      strbuf_puts(b, "Socket: File<br>");
      break;
  }
  strbuf_sprintf(b, "TX: %d<br>", interface->tx_count);
  strbuf_sprintf(b, "RX: %d<br>", interface->recv_count);
}

// create a socket with options common to all our UDP sockets
static int
overlay_bind_socket(const struct sockaddr *addr, size_t addr_size, char *interface_name){
  int fd;
  int reuseP = 1;
  int broadcastP = 1;
  
  fd = socket(PF_INET,SOCK_DGRAM,0);
  if (fd < 0) {
    WHY_perror("Error creating socket");
    return -1;
  } 
  
  if (setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuseP, sizeof(reuseP)) < 0) {
    WHY_perror("setsockopt(SO_REUSEADR)");
    goto error;
  }
  
  #ifdef SO_REUSEPORT
    if (setsockopt(fd, SOL_SOCKET, SO_REUSEPORT, &reuseP, sizeof(reuseP)) < 0) {
      WHY_perror("setsockopt(SO_REUSEPORT)");
      goto error;
    }
  #endif
  
  if (setsockopt(fd, SOL_SOCKET, SO_BROADCAST, &broadcastP, sizeof(broadcastP)) < 0) {
    WHY_perror("setsockopt(SO_BROADCAST)");
    goto error;
  }
  
  /* Automatically close socket on calls to exec().
   This makes life easier when we restart with an exec after receiving
   a bad signal. */
  fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, NULL) | 
#ifdef FD_CLOEXEC
						FD_CLOEXEC
#else
						O_CLOEXEC
#endif
	);
  
#ifdef SO_BINDTODEVICE
  /*
   Limit incoming and outgoing packets to this interface, no matter what the routing table says.
   This should allow for a device with multiple interfaces on the same subnet.
   Don't abort if this fails, I believe it requires root, just log it.
   */
  if (interface_name && setsockopt(fd, SOL_SOCKET, SO_BINDTODEVICE, interface_name, strlen(interface_name)+1) < 0) {
    WHY_perror("setsockopt(SO_BINDTODEVICE)");
  }
#endif

  if (bind(fd, addr, addr_size)) {
    WHY_perror("Bind failed");
    goto error;
  }
  
  return fd;
  
error:
  close(fd);
  return -1;
}

// find an interface marked for use as a default internet route
overlay_interface * overlay_interface_get_default(){
  int i;
  for (i=0;i<OVERLAY_MAX_INTERFACES;i++){
    if (overlay_interfaces[i].state==INTERFACE_STATE_UP && overlay_interfaces[i].default_route)
      return &overlay_interfaces[i];
  }
  return NULL;
}

// find an interface that can send a packet to this address
overlay_interface * overlay_interface_find(struct in_addr addr, int return_default){
  int i;
  overlay_interface *ret = NULL;
  for (i=0;i<OVERLAY_MAX_INTERFACES;i++){
    if (overlay_interfaces[i].state!=INTERFACE_STATE_UP)
      continue;
    
    if ((overlay_interfaces[i].netmask.s_addr & addr.s_addr) == (overlay_interfaces[i].netmask.s_addr & overlay_interfaces[i].address.sin_addr.s_addr)){
      return &overlay_interfaces[i];
    }
    
    // check if this is a default interface
    if (return_default && overlay_interfaces[i].default_route)
      ret=&overlay_interfaces[i];
  }
  
  return ret;
}

// find an interface by name
overlay_interface * overlay_interface_find_name(const char *name){
  int i;
  for (i=0;i<OVERLAY_MAX_INTERFACES;i++){
    if (overlay_interfaces[i].state!=INTERFACE_STATE_UP)
      continue;
    if (strcasecmp(name, overlay_interfaces[i].name) == 0)
      return &overlay_interfaces[i];
  }
  return NULL;
}

static int interface_type_priority(int type)
{
  switch(type){
    case OVERLAY_INTERFACE_ETHERNET:
      return 1;
    case OVERLAY_INTERFACE_WIFI:
      return 2;
    case OVERLAY_INTERFACE_PACKETRADIO:
      return 4;
  }
  return 3;
}

// Which interface is better for routing packets?
// returns -1 to indicate the first is better, 0 for equal, 1 for the second
int overlay_interface_compare(overlay_interface *one, overlay_interface *two)
{
  if (one==two)
    return 0;
  int p1 = interface_type_priority(one->type);
  int p2 = interface_type_priority(two->type);
  if (p1<p2)
    return -1;
  if (p2<p1)
    return 1;
  return 0;
}

// OSX doesn't recieve broadcast packets on sockets bound to an interface's address
// So we have to bind a socket to INADDR_ANY to receive these packets.
static void
overlay_interface_read_any(struct sched_ent *alarm){
  if (alarm->poll.revents & POLLIN) {
    int plen=0;
    int recvttl=1;
    unsigned char packet[16384];
    overlay_interface *interface=NULL;
    struct sockaddr src_addr;
    socklen_t addrlen = sizeof(src_addr);
    
    /* Read only one UDP packet per call to share resources more fairly, and also
     enable stats to accurately count packets received */
    plen = recvwithttl(alarm->poll.fd, packet, sizeof(packet), &recvttl, &src_addr, &addrlen);
    if (plen == -1) {
      WHY_perror("recvwithttl(c)");
      unwatch(alarm);
      close(alarm->poll.fd);
      return;
    }
    
    struct in_addr src = ((struct sockaddr_in *)&src_addr)->sin_addr;
    
    /* Try to identify the real interface that the packet arrived on */
    interface = overlay_interface_find(src, 0);
    
    /* Drop the packet if we don't find a match */
    if (!interface){
      if (config.debug.overlayinterfaces)
	DEBUGF("Could not find matching interface for packet received from %s", inet_ntoa(src));
      return;
    }
    packetOkOverlay(interface, packet, plen, recvttl, &src_addr, addrlen);
  }
  if (alarm->poll.revents & (POLLHUP | POLLERR)) {
    INFO("Closing broadcast socket due to error");
    unwatch(alarm);
    close(alarm->poll.fd);
    alarm->poll.fd=-1;
  }  
}

// bind a socket to INADDR_ANY:port
// for now, we don't have a graceful close for this interface but it should go away when the process dies
static int overlay_interface_init_any(int port)
{
  struct sockaddr_in addr;
  
  if (sock_any.poll.fd>0){
    // Check the port number matches
    if (sock_any_addr.sin_port != htons(port))
      return WHYF("Unable to listen to broadcast packets for ports %d & %d", port, ntohs(sock_any_addr.sin_port));
    
    return 0;
  }
  addr.sin_family = AF_INET;
  addr.sin_port = htons(port);
  addr.sin_addr.s_addr = INADDR_ANY;

  sock_any.poll.fd = overlay_bind_socket((const struct sockaddr *)&addr, sizeof(addr), NULL);
  if (sock_any.poll.fd<0)
    return -1;
  
  sock_any_addr = addr;
  
  sock_any.poll.events=POLLIN;
  sock_any.function = overlay_interface_read_any;
  
  sock_any_stats.name="overlay_interface_read_any";
  sock_any.stats=&sock_any_stats;
  watch(&sock_any);
  return 0;
}

static int
overlay_interface_init_socket(int interface_index)
{
  overlay_interface *const interface = &overlay_interfaces[interface_index];

  /*
   On linux you can bind to the broadcast address to receive broadcast packets per interface [or subnet],
   but then you can't receive unicast packets on the same socket.
   
   On osx, you can only receive broadcast packets if you bind to INADDR_ANY.
   
   So the most portable way to do this is to bind to each interface's IP address for sending broadcasts 
   and receiving unicasts, and bind a separate socket to INADDR_ANY just for receiving broadcast packets.
   
   Sending packets from INADDR_ANY would probably work, but gives us less control over which interfaces are sending packets.
   But there may be some platforms that need some other combination for everything to work.
   */
  
  overlay_interface_init_any(interface->port);
  
  interface->alarm.poll.fd = overlay_bind_socket(
      (const struct sockaddr *)&interface->address, 
      sizeof(interface->address), interface->name);
      
  if (interface->alarm.poll.fd<0){
    interface->state=INTERFACE_STATE_DOWN;
    return WHYF("Failed to bind interface %s", interface->name);
  }
  
  if (config.debug.packetrx || config.debug.io) {
    char srctxt[INET_ADDRSTRLEN];
    if (inet_ntop(AF_INET, (const void *)&interface->address.sin_addr, srctxt, INET_ADDRSTRLEN))
      DEBUGF("Bound to %s:%d", srctxt, ntohs(interface->address.sin_port));
  }

  interface->alarm.poll.events=POLLIN;
  watch(&interface->alarm);
  
  return 0;
}

static int re_init_socket(int interface_index){
  if (overlay_interface_init_socket(interface_index))
    return -1;
  overlay_interface *interface = &overlay_interfaces[interface_index];
  // schedule the first tick asap
  interface->alarm.alarm=gettime_ms();
  interface->alarm.deadline=interface->alarm.alarm;
  schedule(&interface->alarm);
  interface->state=INTERFACE_STATE_UP;
  INFOF("Interface %s addr %s:%d, is up",interface->name,
	inet_ntoa(interface->address.sin_addr), ntohs(interface->address.sin_port));
  
  directory_registration();
  
  return 0;
}

/* Returns 0 if interface is successfully added.
 * Returns 1 if interface is not added (eg, dummy file does not exist).
 * Returns -1 in case of error (misconfiguration or system error).
 */
static int
overlay_interface_init(const char *name, struct in_addr src_addr, struct in_addr netmask, struct in_addr broadcast,
		       const struct config_network_interface *ifconfig)
{
  int cleanup_ret = -1;

  /* Too many interfaces */
  if (overlay_interface_count >= OVERLAY_MAX_INTERFACES)
    return WHY("Too many interfaces -- Increase OVERLAY_MAX_INTERFACES");

  overlay_interface *const interface = &overlay_interfaces[overlay_interface_count];

  strncpy(interface->name, name, sizeof interface->name);
  
  // copy ifconfig values
  interface->drop_broadcasts = ifconfig->drop_broadcasts;
  interface->drop_unicasts = ifconfig->drop_unicasts;
  interface->drop_packets = ifconfig->drop_packets;
  interface->port = ifconfig->port;
  interface->type = ifconfig->type;
  interface->send_broadcasts = ifconfig->send_broadcasts;
  interface->prefer_unicast = ifconfig->prefer_unicast;
  interface->default_route = ifconfig->default_route;
  interface->socket_type = ifconfig->socket_type;
  interface->uartbps = ifconfig->uartbps;
  interface->ctsrts = ifconfig->ctsrts;
  set_destination_ref(&interface->destination, NULL);
  interface->destination = new_destination(interface, ifconfig->encapsulation);
  
  /* Pick a reasonable default MTU.
     This will ultimately get tuned by the bandwidth and other properties of the interface */
  interface->mtu = 1200;
  interface->point_to_point = ifconfig->point_to_point;
  
  interface->state=INTERFACE_STATE_DOWN;
  interface->alarm.poll.fd=0;
  interface->debug = ifconfig->debug;
  interface->tx_count=0;
  interface->recv_count=0;

  // How often do we announce ourselves on this interface?
  int tick_ms=-1;
  int packet_interval=-1;
  int reachable_timeout_ms = -1;

  // hard coded defaults:
  switch (ifconfig->type) {
    case OVERLAY_INTERFACE_PACKETRADIO:
      tick_ms = 15000;
      packet_interval = 1000;
      break;
    case OVERLAY_INTERFACE_ETHERNET:
      tick_ms = 500;
      packet_interval = 100;
      break;
    case OVERLAY_INTERFACE_WIFI:
      tick_ms = 500;
      packet_interval = 400;
      break;
    case OVERLAY_INTERFACE_UNKNOWN:
      tick_ms = 500;
      packet_interval = 100;
      break;
  }
  // configurable defaults per interface
  {
    int i = config_mdp_iftypelist__get(&config.mdp.iftype, &ifconfig->type);
    if (i != -1){
      if (config.mdp.iftype.av[i].value.tick_ms>=0)
	tick_ms = config.mdp.iftype.av[i].value.tick_ms;
      if (config.mdp.iftype.av[i].value.packet_interval>=0)
	packet_interval=config.mdp.iftype.av[i].value.packet_interval;
      if (config.mdp.iftype.av[i].value.reachable_timeout_ms >= 0)
	reachable_timeout_ms = config.mdp.iftype.av[i].value.reachable_timeout_ms;
    }
  }
  // specific value for this interface
  if (ifconfig->mdp.tick_ms>=0)
    tick_ms = ifconfig->mdp.tick_ms;
  if (ifconfig->mdp.packet_interval>=0)
    packet_interval=ifconfig->mdp.packet_interval;
  if (ifconfig->mdp.reachable_timeout_ms >= 0)
    reachable_timeout_ms = ifconfig->mdp.reachable_timeout_ms;
  
  if (packet_interval<0)
    return WHYF("Invalid packet interval %d specified for interface %s", packet_interval, name);
  if (packet_interval==0){
    INFOF("Interface %s is not sending any traffic!", name);
    tick_ms=0;
  }else if (!interface->send_broadcasts){
    INFOF("Interface %s is not sending any broadcast traffic!", name);
  }else if (tick_ms==0)
    INFOF("Interface %s is running tickless", name);
  
  if (tick_ms<0)
    return WHYF("No tick interval specified for interface %s", name);

  interface->destination->tick_ms = tick_ms;
  interface->destination->reachable_timeout_ms = reachable_timeout_ms >= 0 ? reachable_timeout_ms : tick_ms > 0 ? tick_ms * 5 : 2500;
  
  limit_init(&interface->destination->transfer_limit, packet_interval);

  interface->address.sin_family=AF_INET;
  interface->address.sin_port = htons(ifconfig->port);
  
  interface->destination->address.sin_family=AF_INET;
  interface->destination->address.sin_port = htons(ifconfig->port);
  
  interface->alarm.function = overlay_interface_poll;
  interface_poll_stats.name="overlay_interface_poll";
  interface->alarm.stats=&interface_poll_stats;
  
  if (ifconfig->socket_type==SOCK_DGRAM){
    interface->address.sin_addr = src_addr;
    interface->destination->address.sin_addr = broadcast;
    interface->netmask = netmask;
    interface->local_echo = 1;
    
    if (overlay_interface_init_socket(overlay_interface_count))
      return WHY("overlay_interface_init_socket() failed");
  }else{
    char read_file[1024];
    
    interface->address.sin_addr = ifconfig->dummy_address;
    interface->netmask = ifconfig->dummy_netmask;
    interface->destination->address.sin_addr.s_addr = interface->address.sin_addr.s_addr | ~interface->netmask.s_addr;
    interface->local_echo = interface->point_to_point?0:1;

    strbuf d = strbuf_local(read_file, sizeof read_file);
    strbuf_path_join(d, serval_instancepath(), config.server.interface_path, ifconfig->file, NULL);
    if (strbuf_overrun(d))
      return WHYF("interface file name overrun: %s", alloca_str_toprint(strbuf_str(d)));
    
    if ((interface->alarm.poll.fd = open(read_file, O_APPEND|O_RDWR)) == -1) {
      if (errno == ENOENT && ifconfig->socket_type == SOCK_FILE) {
	cleanup_ret = 1;
	WARNF("dummy interface not enabled: %s does not exist", alloca_str_toprint(read_file));
      } else {
	cleanup_ret = WHYF_perror("file interface not enabled: open(%s, O_APPEND|O_RDWR)", alloca_str_toprint(read_file));
      }
      goto cleanup;
    }
    
    if (ifconfig->type==OVERLAY_INTERFACE_PACKETRADIO)
      overlay_packetradio_setup_port(interface);
    
    switch (ifconfig->socket_type) {
    case SOCK_STREAM:
      radio_link_init(interface);
      interface->alarm.poll.events=POLLIN|POLLOUT;
      watch(&interface->alarm);

      break;
    case SOCK_FILE:
      /* Seek to end of file as initial reading point */
      interface->recv_offset = lseek(interface->alarm.poll.fd,0,SEEK_END);
      break;
    }
  }
  
  // schedule the first tick asap
  interface->alarm.alarm=gettime_ms();
  interface->alarm.deadline=interface->alarm.alarm;
  schedule(&interface->alarm);
  interface->state=INTERFACE_STATE_UP;
  INFOF("Interface %s addr %s:%d, is up",interface->name,
	inet_ntoa(interface->address.sin_addr), ntohs(interface->address.sin_port));
  
  directory_registration();
  
  INFOF("Allowing a maximum of %d packets every %"PRId64"ms",
        interface->destination->transfer_limit.burst_size,
        interface->destination->transfer_limit.burst_length);

  overlay_interface_count++;
  return 0;
  
cleanup:
  if (interface->alarm.poll.fd>=0){
    unwatch(&interface->alarm);
    close(interface->alarm.poll.fd);
    interface->alarm.poll.fd=-1;
  }
  interface->state=INTERFACE_STATE_DOWN;
  return cleanup_ret;
}

static void interface_read_dgram(struct overlay_interface *interface){
  int plen=0;
  unsigned char packet[8096];
  
  struct sockaddr src_addr;
  socklen_t addrlen = sizeof(src_addr);
  
  
  /* Read only one UDP packet per call to share resources more fairly, and also
   enable stats to accurately count packets received */
  int recvttl=1;
  plen = recvwithttl(interface->alarm.poll.fd,packet, sizeof(packet), &recvttl, &src_addr, &addrlen);
  if (plen == -1) {
    WHY_perror("recvwithttl(c)");
    overlay_interface_close(interface);
    return;
  }
  
  packetOkOverlay(interface, packet, plen, recvttl, &src_addr, addrlen);
}

struct file_packet{
  struct sockaddr_in src_addr;
  struct sockaddr_in dst_addr;
  int pid;
  int payload_length;
  
  /* TODO ? ;
   half-power beam height (uint16)
   half-power beam width (uint16)
   range in metres, centre beam (uint32)
   latitude (uint32)
   longitude (uint32)
   X/Z direction (uint16)
   Y direction (uint16)
   speed in metres per second (uint16)
   TX frequency in Hz, uncorrected for doppler (which must be done at the receiving end to take into account
   relative motion)
   coding method (use for doppler response etc) null terminated string
   */
  
  unsigned char payload[1400];
};

static int should_drop(struct overlay_interface *interface, struct sockaddr_in addr){
  if (interface->drop_packets>=100)
    return 1;
  
  if (memcmp(&addr, &interface->address, sizeof(addr))==0){
    if (interface->drop_unicasts)
      return 1;
  }else if (memcmp(&addr, &interface->destination->address, sizeof(addr))==0){
    if (interface->drop_broadcasts)
      return 1;
  }else
    return 1;
  
  if (interface->drop_packets <= 0)
    return 0;
  if (rand()%100 >= interface->drop_packets)
    return 0;
  return 1;
}

static void interface_read_file(struct overlay_interface *interface)
{
  IN();
  /* Grab packets, unpackage and dispatch frames to consumers */
  struct file_packet packet;
  
  /* Read from interface file */
  off_t length = lseek(interface->alarm.poll.fd, (off_t)0, SEEK_END);
  
  int new_packets = (length - interface->recv_offset) / sizeof packet;
  if (new_packets > 20)
    WARNF("Getting behind, there are %d unread packets", new_packets);
  
  if (interface->recv_offset<length){
    if (lseek(interface->alarm.poll.fd,interface->recv_offset,SEEK_SET) == -1){
      WHY_perror("lseek");
      OUT();
      return;
    }
    
    if (config.debug.overlayinterfaces)
      DEBUGF("Read interface %s (size=%"PRId64") at offset=%d",interface->name, (int64_t)length, interface->recv_offset);
    
    ssize_t nread = read(interface->alarm.poll.fd, &packet, sizeof packet);
    if (nread == -1){
      WHY_perror("read");
      OUT();
      return;
    }
    
    if (nread == sizeof packet) {
      interface->recv_offset += nread;
      if (should_drop(interface, packet.dst_addr) || (packet.pid == getpid() && !interface->local_echo)){
	if (config.debug.packetrx)
	  DEBUGF("Ignoring packet from %d, addressed to %s:%d", packet.pid,
	      inet_ntoa(packet.dst_addr.sin_addr), ntohs(packet.dst_addr.sin_port));
      }else{
	packetOkOverlay(interface, packet.payload, packet.payload_length, -1, 
			    (struct sockaddr*)&packet.src_addr, (socklen_t) sizeof(packet.src_addr));
      }
    }
  }
  
  /* if there's no input, while we want to check for more soon,
   we need to allow all other low priority alarms to fire first,
   otherwise we'll dominate the scheduler without accomplishing anything */
  time_ms_t now = gettime_ms();
  if (interface->recv_offset>=length){
    if (interface->alarm.alarm == -1 || now + 5 < interface->alarm.alarm){
      interface->alarm.alarm = now + 5;
      interface->alarm.deadline = interface->alarm.alarm + 500;
    }
  }else{
    /* keep reading new packets as fast as possible, 
     but don't completely prevent other high priority alarms */
    if (interface->alarm.alarm == -1 || now < interface->alarm.alarm){
      interface->alarm.alarm = now;
      interface->alarm.deadline = interface->alarm.alarm + 100;
    }
  }
  OUT();
}

static void interface_read_stream(struct overlay_interface *interface){
  IN();
  unsigned char buffer[OVERLAY_INTERFACE_RX_BUFFER_SIZE];
  ssize_t nread = read(interface->alarm.poll.fd, buffer, OVERLAY_INTERFACE_RX_BUFFER_SIZE);
  if (nread == -1){
    WHY_perror("read");
    OUT();
    return;
  }
  
  
  int i;
  for (i=0;i<nread;i++)
    radio_link_decode(interface, buffer[i]);
    
  OUT();
}

static void overlay_interface_poll(struct sched_ent *alarm)
{
  struct overlay_interface *interface = (overlay_interface *)alarm;
  time_ms_t now = gettime_ms();
    
  if (alarm->poll.revents==0){
    alarm->alarm=-1;
    
    if (interface->state==INTERFACE_STATE_UP 
      && interface->destination->tick_ms>0
      && interface->send_broadcasts
      && !radio_link_is_busy(interface)){
      
      if (now >= interface->destination->last_tx+interface->destination->tick_ms)
        overlay_send_tick_packet(interface->destination);
	
      alarm->alarm=interface->destination->last_tx+interface->destination->tick_ms;
      alarm->deadline=alarm->alarm+interface->destination->tick_ms/2;
    }
    
    switch(interface->socket_type){
      case SOCK_STREAM:
	radio_link_tx(interface);
	return;
      case SOCK_DGRAM:
	break;
      case SOCK_FILE:
	interface_read_file(interface);
        now = gettime_ms();
	break;
    }
    
    unschedule(alarm);
    if (alarm->alarm!=-1 && interface->state==INTERFACE_STATE_UP) {
      if (alarm->alarm < now)
        alarm->alarm = now;
      schedule(alarm);
    }
  }
  
  if (alarm->poll.revents & POLLOUT){
    switch(interface->socket_type){
      case SOCK_STREAM:
	radio_link_tx(interface);
	return;
      case SOCK_DGRAM:
      case SOCK_FILE:
	//XXX error? fatal?
	break;
    }
  }
  
  if (alarm->poll.revents & POLLIN) {
    switch(interface->socket_type){
      case SOCK_DGRAM:
	interface_read_dgram(interface);
	break;
      case SOCK_STREAM:
	interface_read_stream(interface);
	// if we read a valid heartbeat packet, we may be able to write more bytes now.
	if (interface->state==INTERFACE_STATE_UP){
	  radio_link_tx(interface);
	  return;
	}
	break;
      case SOCK_FILE:
	interface_read_file(interface);
	break;
    }
  }
  
  if (alarm->poll.revents & (POLLHUP | POLLERR)) {
    overlay_interface_close(interface);
  }  
}

int overlay_broadcast_ensemble(struct network_destination *destination, struct overlay_buffer *buffer)
{
  assert(destination && destination->interface);
  const unsigned char *bytes = ob_ptr(buffer);
  int len = ob_position(buffer);
  
  struct overlay_interface *interface = destination->interface;
  destination->last_tx = gettime_ms();
  
  if (config.debug.packettx){
    DEBUGF("Sending this packet via interface %s (len=%d)",interface->name,len);
    DEBUG_packet_visualise(NULL, bytes, len);
  }

  if (interface->state!=INTERFACE_STATE_UP){
    ob_free(buffer);
    return WHYF("Cannot send to interface %s as it is down", interface->name);
  }

  if (interface->debug)
    DEBUGF("Sending on %s, len %d: %s", interface->name, len, alloca_tohex(bytes, len>64?64:len));

  interface->tx_count++;
  
  switch(interface->socket_type){
    case SOCK_STREAM:
      return radio_link_queue_packet(interface, buffer);
      
    case SOCK_FILE:
    {
      struct file_packet packet={
	.src_addr = interface->address,
	.dst_addr = destination->address,
	.pid = getpid(),
      };
      
      if (len > sizeof(packet.payload)){
	WARN("Truncating long packet to fit within MTU byte limit for dummy interface");
	len = sizeof(packet.payload);
      }
      packet.payload_length=len;
      bcopy(bytes, packet.payload, len);
      ob_free(buffer);
      /* This lseek() is unneccessary because the dummy file is opened in O_APPEND mode.  It's
       only purpose is to find out the offset to print in the DEBUG statement.  It is vulnerable
       to a race condition with other processes appending to the same file. */
      if (config.debug.overlayinterfaces) {
	off_t fsize = lseek(interface->alarm.poll.fd, (off_t) 0, SEEK_END);
	if (fsize == -1) {
	  /* Don't complain if the seek fails because we are writing to a pipe or device that does
	    not support seeking. */
	  if (errno != ESPIPE)
	    return WHY_perror("lseek");
	  DEBUGF("Write to interface %s at unknown offset", interface->name);
	} else
	  DEBUGF("Write to interface %s at offset=%"PRId64, interface->name, (int64_t)fsize);
      }
      ssize_t nwrite = write(interface->alarm.poll.fd, &packet, sizeof(packet));
      if (nwrite == -1)
	return WHY_perror("write");
      if (nwrite != sizeof(packet))
	return WHYF("only wrote %d of %d bytes", (int)nwrite, (int)sizeof(packet));
      return 0;
    }
      
    case SOCK_DGRAM:
    {
      if (config.debug.overlayinterfaces) 
	DEBUGF("Sending %zu byte overlay frame on %s to %s", (size_t)len, interface->name, inet_ntoa(destination->address.sin_addr));
      ssize_t sent = sendto(interface->alarm.poll.fd, 
		bytes, (size_t)len, 0, 
		(struct sockaddr *)&destination->address, sizeof(destination->address));
      ob_free(buffer);
      if (sent == -1 || (size_t)sent != (size_t)len) {
	if (sent == -1)
	  WHYF_perror("sendto(fd=%d,len=%zu,addr=%s) on interface %s",
	      interface->alarm.poll.fd,
	      (size_t)len,
	      alloca_sockaddr((struct sockaddr *)&destination->address, sizeof destination->address),
	      interface->name
	    );
	else
	  WHYF("sendto() sent %zu bytes of overlay frame (%zu) to interface %s (socket=%d)",
	      (size_t)sent, (size_t)len, interface->name, interface->alarm.poll.fd);
	// close the interface if we had any error while sending broadcast packets,
	// unicast packets should not bring the interface down
	if (destination == interface->destination)
	  overlay_interface_close(interface);
	// TODO mark unicast destination as failed
	return -1;
      }
      return 0;
    }
      
    default:
      ob_free(buffer);
      return WHY("Unsupported socket type");
  }
}

/* Register the real interface, or update the existing interface registration. */
int
overlay_interface_register(char *name,
			   struct in_addr addr,
			   struct in_addr mask)
{
  struct in_addr broadcast = {.s_addr = addr.s_addr | ~mask.s_addr};

  if (config.debug.overlayinterfaces) {
    // note, inet_ntop doesn't seem to behave on android
    DEBUGF("%s address: %s", name, inet_ntoa(addr));
    DEBUGF("%s broadcast address: %s", name, inet_ntoa(broadcast));
  }

  // Find the matching non-dummy interface rule.
  const struct config_network_interface *ifconfig = NULL;
  int i;
  for (i = 0; i < config.interfaces.ac; ++i, ifconfig = NULL) {
    ifconfig = &config.interfaces.av[i].value;
    if (ifconfig->socket_type==SOCK_DGRAM) {
      int j;
      for (j = 0; j < ifconfig->match.patc; ++j){
	if (fnmatch(ifconfig->match.patv[j], name, 0) == 0)
	  break;
      }
      
      if (j < ifconfig->match.patc)
	break;
    }
  }
  if (ifconfig == NULL) {
    if (config.debug.overlayinterfaces)
      DEBUGF("Interface %s does not match any rule", name);
    return 0;
  }
  if (ifconfig->exclude) {
    if (config.debug.overlayinterfaces)
      DEBUGF("Interface %s is explicitly excluded", name);
    return 0;
  }

  /* Search in the exist list of interfaces */
  int found_interface= -1;
  for(i = 0; i < overlay_interface_count; i++){
    int broadcast_match = 0;
    int name_match =0;
    
    if (overlay_interfaces[i].destination->address.sin_addr.s_addr == broadcast.s_addr)
      broadcast_match = 1;
    
    name_match = !strcasecmp(overlay_interfaces[i].name, name);
    
    // if we find an exact match we can stop searching
    if (name_match && broadcast_match){
      // mark this interface as still alive
      if (overlay_interfaces[i].state==INTERFACE_STATE_DETECTING)
	overlay_interfaces[i].state=INTERFACE_STATE_UP;
      
      // try to bring the interface back up again even if the address has changed
      if (overlay_interfaces[i].state==INTERFACE_STATE_DOWN){
	overlay_interfaces[i].address.sin_addr = addr;
	re_init_socket(i);
      }
      
      // we already know about this interface, and it's up so stop looking immediately
      return 0;
    }
    
    // remember this slot to bring the interface back up again, even if the address has changed
    if (name_match && overlay_interfaces[i].state==INTERFACE_STATE_DOWN)
      found_interface=i;
  }
  
  if (found_interface>=0){
    // try to reactivate the existing interface
    overlay_interfaces[found_interface].address.sin_addr = addr;
    overlay_interfaces[found_interface].destination->address.sin_addr = broadcast;
    overlay_interfaces[found_interface].netmask = mask;
    return re_init_socket(found_interface);
  }
  
  /* New interface, so register it */
  if (overlay_interface_init(name, addr, mask, broadcast, ifconfig))
    return WHYF("Could not initialise newly seen interface %s", name);
  else
    if (config.debug.overlayinterfaces) DEBUGF("Registered interface %s", name);

  return 0;
}
  
void overlay_interface_discover(struct sched_ent *alarm)
{
  /* Mark all UP interfaces as DETECTING, so we can tell which interfaces are new, and which are dead */
  int i;
  for (i = 0; i < overlay_interface_count; i++)
    if (overlay_interfaces[i].state==INTERFACE_STATE_UP)
      overlay_interfaces[i].state=INTERFACE_STATE_DETECTING;   

  /* Register new dummy interfaces */
  int detect_real_interfaces = 0;
  const struct config_network_interface *ifconfig = NULL;
  for (i = 0; i < config.interfaces.ac; ++i, ifconfig = NULL) {
    ifconfig = &config.interfaces.av[i].value;
    if (ifconfig->exclude)
      continue;
    if (ifconfig->socket_type==SOCK_DGRAM) {
      detect_real_interfaces = 1;
      continue;
    }
    int j;
    for (j = 0; j < overlay_interface_count; j++){
      if (overlay_interfaces[j].socket_type == ifconfig->socket_type && 
	  strcasecmp(overlay_interfaces[j].name, ifconfig->file) == 0 && 
	  overlay_interfaces[j].state==INTERFACE_STATE_DETECTING){
	overlay_interfaces[j].state=INTERFACE_STATE_UP;
	break;
      }
    }
    
    if (j >= overlay_interface_count) {
      // New dummy interface, so register it.
      struct in_addr dummyaddr = hton_in_addr(INADDR_NONE);
      overlay_interface_init(ifconfig->file, dummyaddr, dummyaddr, dummyaddr, ifconfig);
    }
  }

  // Register new real interfaces
  if (detect_real_interfaces) {
    int no_route = 1;
#ifdef HAVE_IFADDRS_H
    if (no_route != 0)
      no_route = doifaddrs();
#endif
#ifdef SIOCGIFCONF
    if (no_route != 0)
      no_route = lsif();
#endif
#ifdef linux
    if (no_route != 0)
      no_route = scrapeProcNetRoute();
#endif
    if (no_route != 0) {
      FATAL("Unable to get any interface information");
    }
  }

  // Close any interfaces that have gone away.
  for(i = 0; i < overlay_interface_count; i++)
    if (overlay_interfaces[i].state==INTERFACE_STATE_DETECTING) {
      DEBUGF("Closing interface stuck in DETECTING state.");
      overlay_interface_close(&overlay_interfaces[i]);
    }

  alarm->alarm = gettime_ms()+5000;
  alarm->deadline = alarm->alarm + 10000;
  schedule(alarm);
  return;
}

void logServalPacket(int level, struct __sourceloc __whence, const char *message, const unsigned char *packet, size_t len) {
  struct mallocbuf mb = STRUCT_MALLOCBUF_NULL;
  if (!message) message="<no message>";
  if (serval_packetvisualise_xpf(XPRINTF_MALLOCBUF(&mb), message, packet, len) == -1)
    WHY("serval_packetvisualise() failed");
  else if (mb.buffer == NULL)
    WHYF("serval_packetvisualise() output buffer missing, message=%s packet=%p len=%lu", alloca_toprint(-1, message, strlen(message)), packet, (long unsigned int)len);
  else
    logString(level, __whence, mb.buffer);
  if (mb.buffer)
    free(mb.buffer);
}
