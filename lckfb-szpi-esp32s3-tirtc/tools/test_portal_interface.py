#!/usr/bin/env python3
"""Execute the real portal gate with IPv4 and dual-stack socket addresses."""
from pathlib import Path
import subprocess
import tempfile
from test_captive_portal_contract import function_body

source=(Path(__file__).resolve().parents[1]/'components/wifi_manager/src/wifi_manager.c').read_text(encoding='utf-8')
body=function_body(source,'portal_socket_allowed')
harness=r'''
#include <assert.h>
#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <stdatomic.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#define CONFIG_LWIP_IPV6 1
#define ESP_OK 0
typedef struct {struct {uint32_t addr;} ip;} esp_netif_ip_info_t;
static atomic_bool s_provisioning=true;
static void *s_ap_netif=(void *)1;
static struct sockaddr_storage address;
static socklen_t address_size;
static int socket_error,netif_error;
static uint32_t ap;
static int mock_name(int fd,struct sockaddr *out,socklen_t *len) {
 (void)fd; socklen_t n=*len<address_size?*len:address_size;
 memcpy(out,&address,n); *len=address_size; return socket_error;
}
static int esp_netif_get_ip_info(void *netif,esp_netif_ip_info_t *out) {
 (void)netif; out->ip.addr=ap; return netif_error;
}
#define getsockname mock_name
static bool portal_socket_allowed(int socket_fd) {
BODY
}
static void addr(const char *text,int family) {
 memset(&address,0,sizeof(address));
 if(family==AF_INET) {
  struct sockaddr_in *v=(struct sockaddr_in *)&address;
  v->sin_family=family; inet_pton(family,text,&v->sin_addr); address_size=sizeof(*v);
 } else {
  struct sockaddr_in6 *v=(struct sockaddr_in6 *)&address;
  v->sin6_family=family; inet_pton(family,text,&v->sin6_addr); address_size=sizeof(*v);
 }
}
int main(void) {
 inet_pton(AF_INET,"192.168.6.1",&ap);
 addr("192.168.6.1",AF_INET); assert(portal_socket_allowed(1));
 addr("::ffff:192.168.6.1",AF_INET6); assert(portal_socket_allowed(1));
 addr("192.168.31.50",AF_INET); assert(!portal_socket_allowed(1));
 addr("::ffff:192.168.31.50",AF_INET6); assert(!portal_socket_allowed(1));
 addr("fe80::1",AF_INET6); assert(!portal_socket_allowed(1));
 addr("::1",AF_INET6); assert(!portal_socket_allowed(1));
 addr("::ffff:192.168.6.1",AF_INET6);
 s_provisioning=false; assert(!portal_socket_allowed(1)); s_provisioning=true;
 s_ap_netif=0; assert(!portal_socket_allowed(1)); s_ap_netif=(void *)1;
 socket_error=-1; assert(!portal_socket_allowed(1)); socket_error=0;
 netif_error=-1; assert(!portal_socket_allowed(1)); netif_error=0;
 address_size=sizeof(struct sockaddr_in); assert(!portal_socket_allowed(1));
 ap=0; addr("0.0.0.0",AF_INET); assert(!portal_socket_allowed(1));
 return 0;
}
'''.replace('BODY',body)
with tempfile.TemporaryDirectory() as tmp:
    c=Path(tmp)/'gate.c'; binary=Path(tmp)/'gate'
    c.write_text(harness)
    subprocess.run(['cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',str(c),'-o',str(binary)],check=True)
    subprocess.run([str(binary)],check=True)
print('PASS: AP IPv4/mapped-IPv6 allowed; STA/native-IPv6/closed/error/truncated sockets rejected')
