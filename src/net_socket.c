/*
    net_socket.c -- Handle various kinds of sockets.
    Copyright (C) 1998-2002 Ivo Timmermans <ivo@o2w.nl>,
                  2000-2002 Guus Sliepen <guus@sliepen.eu.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program; if not, write to the Free Software
    Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.

    $Id: net_socket.c,v 1.1.2.22 2002/09/15 14:55:53 guus Exp $
*/

#include "config.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include <sys/time.h>
#include <sys/types.h>
#include <syslog.h>
#include <unistd.h>
#include <sys/ioctl.h>
/* SunOS really wants sys/socket.h BEFORE net/if.h,
   and FreeBSD wants these lines below the rest. */
#include <arpa/inet.h>
#include <sys/socket.h>
#include <net/if.h>
#ifdef HAVE_NETINET_IN_SYSTM_H
#include <netinet/in_systm.h>
#endif
#ifdef HAVE_NETINET_IP_H
#include <netinet/ip.h>
#endif
#ifdef HAVE_NETINET_TCP_H
#include <netinet/tcp.h>
#endif

#include <utils.h>
#include <xalloc.h>
#include <avl_tree.h>
#include <list.h>

#include "conf.h"
#include "connection.h"
#include "meta.h"
#include "net.h"
#include "netutl.h"
#include "process.h"
#include "protocol.h"
#include "subnet.h"
#include "graph.h"
#include "process.h"
#include "route.h"
#include "device.h"
#include "event.h"

#include "system.h"

#ifndef HAVE_RAND_PSEUDO_BYTES
#define RAND_pseudo_bytes RAND_bytes
#endif

int addressfamily = AF_INET;
int maxtimeout = 900;
int seconds_till_retry = 5;

listen_socket_t listen_socket[MAXSOCKETS];
int listen_sockets;

/* Setup sockets */

int setup_listen_socket(sockaddr_t *sa)
{
	int nfd, flags;
	char *addrstr;
	int option;
#if defined(SOL_SOCKET) && defined(SO_BINDTODEVICE)
	char *interface;
	struct ifreq ifr;
#endif

	cp();

	nfd = socket(sa->sa.sa_family, SOCK_STREAM, IPPROTO_TCP);

	if(nfd < 0) {
		syslog(LOG_ERR, _("Creating metasocket failed: %s"), strerror(errno));
		return -1;
	}

	flags = fcntl(nfd, F_GETFL);

	if(fcntl(nfd, F_SETFL, flags | O_NONBLOCK) < 0) {
		close(nfd);
		syslog(LOG_ERR, _("System call `%s' failed: %s"), "fcntl",
			   strerror(errno));
		return -1;
	}

	/* Optimize TCP settings */

	option = 1;
	setsockopt(nfd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

#if defined(SOL_TCP) && defined(TCP_NODELAY)
	setsockopt(nfd, SOL_TCP, TCP_NODELAY, &option, sizeof(option));
#endif

#if defined(SOL_IP) && defined(IP_TOS) && defined(IPTOS_LOWDELAY)
	option = IPTOS_LOWDELAY;
	setsockopt(nfd, SOL_IP, IP_TOS, &option, sizeof(option));
#endif

	if(get_config_string
	   (lookup_config(config_tree, "BindToInterface"), &interface)) {
#if defined(SOL_SOCKET) && defined(SO_BINDTODEVICE)
		memset(&ifr, 0, sizeof(ifr));
		strncpy(ifr.ifr_ifrn.ifrn_name, interface, IFNAMSIZ);

		if(setsockopt(nfd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr))) {
			close(nfd);
			syslog(LOG_ERR, _("Can't bind to interface %s: %s"), interface,
				   strerror(errno));
			return -1;
		}
#else
		syslog(LOG_WARNING, _("BindToDevice not supported on this platform"));
#endif
	}

	if(bind(nfd, &sa->sa, SALEN(sa->sa))) {
		close(nfd);
		addrstr = sockaddr2hostname(sa);
		syslog(LOG_ERR, _("Can't bind to %s/tcp: %s"), addrstr,
			   strerror(errno));
		free(addrstr);
		return -1;
	}

	if(listen(nfd, 3)) {
		close(nfd);
		syslog(LOG_ERR, _("System call `%s' failed: %s"), "listen",
			   strerror(errno));
		return -1;
	}

	return nfd;
}

int setup_vpn_in_socket(sockaddr_t *sa)
{
	int nfd, flags;
	char *addrstr;
	int option;
#if defined(SOL_SOCKET) && defined(SO_BINDTODEVICE)
	char *interface;
	struct ifreq ifr;
#endif

	cp();

	nfd = socket(sa->sa.sa_family, SOCK_DGRAM, IPPROTO_UDP);

	if(nfd < 0) {
		syslog(LOG_ERR, _("Creating UDP socket failed: %s"), strerror(errno));
		return -1;
	}

	flags = fcntl(nfd, F_GETFL);
	if(fcntl(nfd, F_SETFL, flags | O_NONBLOCK) < 0) {
		close(nfd);
		syslog(LOG_ERR, _("System call `%s' failed: %s"), "fcntl",
			   strerror(errno));
		return -1;
	}

	option = 1;
	setsockopt(nfd, SOL_SOCKET, SO_REUSEADDR, &option, sizeof(option));

#if defined(SOL_SOCKET) && defined(SO_BINDTODEVICE)
	if(get_config_string
	   (lookup_config(config_tree, "BindToInterface"), &interface)) {
		memset(&ifr, 0, sizeof(ifr));
		strncpy(ifr.ifr_ifrn.ifrn_name, interface, IFNAMSIZ);

		if(setsockopt(nfd, SOL_SOCKET, SO_BINDTODEVICE, &ifr, sizeof(ifr))) {
			close(nfd);
			syslog(LOG_ERR, _("Can't bind to interface %s: %s"), interface,
				   strerror(errno));
			return -1;
		}
	}
#endif

	if(bind(nfd, &sa->sa, SALEN(sa->sa))) {
		close(nfd);
		addrstr = sockaddr2hostname(sa);
		syslog(LOG_ERR, _("Can't bind to %s/udp: %s"), addrstr,
			   strerror(errno));
		free(addrstr);
		return -1;
	}

	return nfd;
}

void retry_outgoing(outgoing_t *outgoing)
{
	event_t *event;

	cp();

	outgoing->timeout += 5;

	if(outgoing->timeout > maxtimeout)
		outgoing->timeout = maxtimeout;

	event = new_event();
	event->handler = (event_handler_t) setup_outgoing_connection;
	event->time = now + outgoing->timeout;
	event->data = outgoing;
	event_add(event);

	if(debug_lvl >= DEBUG_CONNECTIONS)
		syslog(LOG_NOTICE,
			   _("Trying to re-establish outgoing connection in %d seconds"),
			   outgoing->timeout);
}

int setup_outgoing_socket(connection_t *c)
{
	int option;

	cp();

	if(debug_lvl >= DEBUG_CONNECTIONS)
		syslog(LOG_INFO, _("Trying to connect to %s (%s)"), c->name,
			   c->hostname);

	c->socket = socket(c->address.sa.sa_family, SOCK_STREAM, IPPROTO_TCP);

	if(c->socket == -1) {
		syslog(LOG_ERR, _("Creating socket for %s failed: %s"), c->hostname,
			   strerror(errno));
		return -1;
	}

	/* Optimize TCP settings */

#if defined(SOL_TCP) && defined(TCP_NODELAY)
	option = 1;
	setsockopt(c->socket, SOL_TCP, TCP_NODELAY, &option, sizeof(option));
#endif

#if defined(SOL_IP) && defined(IP_TOS)
	option = IPTOS_LOWDELAY;
	setsockopt(c->socket, SOL_IP, IP_TOS, &option, sizeof(option));
#endif

	/* Connect */

	if(connect(c->socket, &c->address.sa, SALEN(c->address.sa)) == -1) {
		close(c->socket);
		syslog(LOG_ERR, _("Error while connecting to %s (%s): %s"), c->name,
			   c->hostname, strerror(errno));
		return -1;
	}

	if(debug_lvl >= DEBUG_CONNECTIONS)
		syslog(LOG_INFO, _("Connected to %s (%s)"), c->name, c->hostname);

	return 0;
}


void finish_connecting(connection_t *c)
{
	cp();

	if(debug_lvl >= DEBUG_CONNECTIONS)
		syslog(LOG_INFO, _("Connected to %s (%s)"), c->name, c->hostname);

	c->last_ping_time = now;

	send_id(c);
}

void do_outgoing_connection(connection_t *c)
{
	char *address, *port;
	int option, result, flags;

	cp();

begin:
	if(!c->outgoing->ai) {
		if(!c->outgoing->cfg) {
			if(debug_lvl >= DEBUG_CONNECTIONS)
				syslog(LOG_ERR, _("Could not set up a meta connection to %s"),
					   c->name);
			c->status.remove = 1;
			retry_outgoing(c->outgoing);
			return;
		}

		get_config_string(c->outgoing->cfg, &address);

		if(!get_config_string(lookup_config(c->config_tree, "Port"), &port))
			asprintf(&port, "655");

		c->outgoing->ai = str2addrinfo(address, port, SOCK_STREAM);
		free(address);
		free(port);

		c->outgoing->aip = c->outgoing->ai;
		c->outgoing->cfg = lookup_config_next(c->config_tree, c->outgoing->cfg);
	}

	if(!c->outgoing->aip) {
		freeaddrinfo(c->outgoing->ai);
		c->outgoing->ai = NULL;
		goto begin;
	}

	memcpy(&c->address, c->outgoing->aip->ai_addr,
		   c->outgoing->aip->ai_addrlen);
	c->outgoing->aip = c->outgoing->aip->ai_next;

	if(c->hostname)
		free(c->hostname);

	c->hostname = sockaddr2hostname(&c->address);

	if(debug_lvl >= DEBUG_CONNECTIONS)
		syslog(LOG_INFO, _("Trying to connect to %s (%s)"), c->name,
			   c->hostname);

	c->socket = socket(c->address.sa.sa_family, SOCK_STREAM, IPPROTO_TCP);

	if(c->socket == -1) {
		if(debug_lvl >= DEBUG_CONNECTIONS)
			syslog(LOG_ERR, _("Creating socket for %s failed: %s"), c->hostname,
				   strerror(errno));

		goto begin;
	}

	/* Optimize TCP settings */

#if defined(SOL_TCP) && defined(TCP_NODELAY)
	option = 1;
	setsockopt(c->socket, SOL_TCP, TCP_NODELAY, &option, sizeof(option));
#endif

#if defined(SOL_IP) && defined(IP_TOS)
	option = IPTOS_LOWDELAY;
	setsockopt(c->socket, SOL_IP, IP_TOS, &option, sizeof(option));
#endif

	/* Non-blocking */

	flags = fcntl(c->socket, F_GETFL);

	if(fcntl(c->socket, F_SETFL, flags | O_NONBLOCK) < 0) {
		syslog(LOG_ERR, _("fcntl for %s: %s"), c->hostname, strerror(errno));
	}

	/* Connect */

	result = connect(c->socket, &c->address.sa, SALEN(c->address.sa));

	if(result == -1) {
		if(errno == EINPROGRESS) {
			c->status.connecting = 1;
			return;
		}

		close(c->socket);

		if(debug_lvl >= DEBUG_CONNECTIONS)
			syslog(LOG_ERR, _("%s: %s"), c->hostname, strerror(errno));

		goto begin;
	}

	finish_connecting(c);

	return;
}

void setup_outgoing_connection(outgoing_t *outgoing)
{
	connection_t *c;
	node_t *n;

	cp();

	n = lookup_node(outgoing->name);

	if(n)
		if(n->connection) {
			if(debug_lvl >= DEBUG_CONNECTIONS)
				syslog(LOG_INFO, _("Already connected to %s"), outgoing->name);

			n->connection->outgoing = outgoing;
			return;
		}

	c = new_connection();
	c->name = xstrdup(outgoing->name);
	c->outcipher = myself->connection->outcipher;
	c->outdigest = myself->connection->outdigest;
	c->outmaclength = myself->connection->outmaclength;
	c->outcompression = myself->connection->outcompression;

	init_configuration(&c->config_tree);
	read_connection_config(c);

	outgoing->cfg = lookup_config(c->config_tree, "Address");

	if(!outgoing->cfg) {
		syslog(LOG_ERR, _("No address specified for %s"), c->name);
		free_connection(c);
		free(outgoing->name);
		free(outgoing);
		return;
	}

	c->outgoing = outgoing;
	c->last_ping_time = now;

	connection_add(c);

	do_outgoing_connection(c);
}

/*
  accept a new tcp connect and create a
  new connection
*/
int handle_new_meta_connection(int sock)
{
	connection_t *c;
	sockaddr_t sa;
	int fd, len = sizeof(sa);

	cp();

	fd = accept(sock, &sa.sa, &len);

	if(fd < 0) {
		syslog(LOG_ERR, _("Accepting a new connection failed: %s"),
			   strerror(errno));
		return -1;
	}

	sockaddrunmap(&sa);

	c = new_connection();
	c->outcipher = myself->connection->outcipher;
	c->outdigest = myself->connection->outdigest;
	c->outmaclength = myself->connection->outmaclength;
	c->outcompression = myself->connection->outcompression;

	c->address = sa;
	c->hostname = sockaddr2hostname(&sa);
	c->socket = fd;
	c->last_ping_time = now;

	if(debug_lvl >= DEBUG_CONNECTIONS)
		syslog(LOG_NOTICE, _("Connection from %s"), c->hostname);

	connection_add(c);

	c->allow_request = ID;
	send_id(c);

	return 0;
}

void try_outgoing_connections(void)
{
	static config_t *cfg = NULL;
	char *name;
	outgoing_t *outgoing;

	cp();

	for(cfg = lookup_config(config_tree, "ConnectTo"); cfg;
		cfg = lookup_config_next(config_tree, cfg)) {
		get_config_string(cfg, &name);

		if(check_id(name)) {
			syslog(LOG_ERR,
				   _("Invalid name for outgoing connection in %s line %d"),
				   cfg->file, cfg->line);
			free(name);
			continue;
		}

		outgoing = xmalloc_and_zero(sizeof(*outgoing));
		outgoing->name = name;
		setup_outgoing_connection(outgoing);
	}
}
