/*
    protocol_misc.c -- handle the meta-protocol, miscellaneous functions
    Copyright (C) 1999-2005 Ivo Timmermans,
                  2000-2013 Guus Sliepen <guus@tinc-vpn.org>

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "system.h"

#include "conf.h"
#include "connection.h"
#include "logger.h"
#include "meta.h"
#include "net.h"
#include "netutl.h"
#include "protocol.h"
#include "utils.h"
#include "xalloc.h"

int maxoutbufsize = 0;

/* Status and error notification routines */

bool send_status(connection_t *c, int statusno, const char *statusstring) {
	if(!statusstring)
		statusstring = "Status";

	return send_request(c, "%d %d %s", STATUS, statusno, statusstring);
}

bool status_h(connection_t *c, const char *request) {
	int statusno;
	char statusstring[MAX_STRING_SIZE];

	if(sscanf(request, "%*d %d " MAX_STRING, &statusno, statusstring) != 2) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Got bad %s from %s (%s)", "STATUS",
			   c->name, c->hostname);
		return false;
	}

	logger(DEBUG_STATUS, LOG_NOTICE, "Status message from %s (%s): %d: %s",
			   c->name, c->hostname, statusno, statusstring);

	return true;
}

bool send_error(connection_t *c, int err, const char *errstring) {
	if(!errstring)
		errstring = "Error";

	return send_request(c, "%d %d %s", ERROR, err, errstring);
}

bool error_h(connection_t *c, const char *request) {
	int err;
	char errorstring[MAX_STRING_SIZE];

	if(sscanf(request, "%*d %d " MAX_STRING, &err, errorstring) != 2) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Got bad %s from %s (%s)", "ERROR",
			   c->name, c->hostname);
		return false;
	}

	logger(DEBUG_ERROR, LOG_NOTICE, "Error message from %s (%s): %d: %s",
			   c->name, c->hostname, err, errorstring);

	return false;
}

bool send_termreq(connection_t *c) {
	return send_request(c, "%d", TERMREQ);
}

bool termreq_h(connection_t *c, const char *request) {
	return false;
}

bool send_ping(connection_t *c) {
	c->status.pinged = true;
	c->last_ping_time = now.tv_sec;

	return send_request(c, "%d", PING);
}

bool ping_h(connection_t *c, const char *request) {
	return send_pong(c);
}

bool send_pong(connection_t *c) {
	return send_request(c, "%d", PONG);
}

bool pong_h(connection_t *c, const char *request) {
	c->status.pinged = false;

	/* Succesful connection, reset timeout if this is an outgoing connection. */

	if(c->outgoing) {
		c->outgoing->timeout = 0;
		c->outgoing->cfg = NULL;
		if(c->outgoing->ai)
			freeaddrinfo(c->outgoing->ai);
		c->outgoing->ai = NULL;
		c->outgoing->aip = NULL;
	}

	return true;
}

/* Sending and receiving packets via TCP */

bool send_tcppacket(connection_t *c, const vpn_packet_t *packet) {
	/* If there already is a lot of data in the outbuf buffer, discard this packet.
	   We use a very simple Random Early Drop algorithm. */

	if(2.0 * c->outbuf.len / (float)maxoutbufsize - 1 > (float)rand()/(float)RAND_MAX)
		return true;

	if(!send_request(c, "%d %hd", PACKET, packet->len))
		return false;

	return send_meta(c, (char *)DATA(packet), packet->len);
}

bool tcppacket_h(connection_t *c, const char *request) {
	short int len;

	if(sscanf(request, "%*d %hd", &len) != 1) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Got bad %s from %s (%s)", "PACKET", c->name,
			   c->hostname);
		return false;
	}

	/* Set reqlen to len, this will tell receive_meta() that a tcppacket is coming. */

	c->tcplen = len;

	return true;
}

/* Transmitting UDP information */

bool send_udp_info(node_t *from, node_t *to) {
	/* If there's a static relay in the path, there's no point in sending the message
	   farther than the static relay. */
	to = (to->via == myself) ? to->nexthop : to->via;

	/* Skip cases where sending UDP info messages doesn't make sense.
	   This is done here in order to avoid repeating the same logic in multiple callsites. */

	if(to == myself)
		return true;

	if(!to->status.reachable)
		return true;

	if(from == myself && to->connection)
		return true;

	if((myself->options | from->options | to->options) & OPTION_TCPONLY)
		return true;

	if((to->nexthop->options >> 24) < 5)
		return true;

	char *from_address, *from_port;
	/* If we're the originator, the address we use is irrelevant
	   because the first intermediate node will ignore it.
	   We use our local address as it somewhat makes sense
	   and it's simpler than introducing an encoding for "null" addresses anyway. */
	sockaddr2str((from != myself) ? &from->address : &to->nexthop->connection->edge->local_address, &from_address, &from_port);

	bool x = send_request(to->nexthop->connection, "%d %s %s %s %s", UDP_INFO, from->name, to->name, from_address, from_port);

	free(from_address);
	free(from_port);

	return x;
}

bool udp_info_h(connection_t *c, const char* request) {
	char from_name[MAX_STRING_SIZE];
	char to_name[MAX_STRING_SIZE];
	char from_address[MAX_STRING_SIZE];
	char from_port[MAX_STRING_SIZE];

	if(sscanf(request, "%*d "MAX_STRING" "MAX_STRING" "MAX_STRING" "MAX_STRING, from_name, to_name, from_address, from_port) != 4) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Got bad %s from %s (%s)", "UDP_INFO", c->name, c->hostname);
		return false;
	}

	if(!check_id(from_name) || !check_id(to_name)) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Got bad %s from %s (%s): %s", "UDP_INFO", c->name, c->hostname, "invalid name");
		return false;
	}

	node_t *from = lookup_node(from_name);
	if(!from) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Got %s from %s (%s) origin %s which does not exist in our connection list", "UDP_INFO", c->name, c->hostname, from_name);
		return true;
	}

	if(from != from->via) {
		/* Not supposed to happen, as it means the message wandered past a static relay */
		logger(DEBUG_PROTOCOL, LOG_WARNING, "Got UDP info message from %s (%s) which we can't reach directly", from->name, from->hostname);
		return true;
	}

	/* If we have a direct edge to "from", we are in a better position
	   to guess its address than it is itself. */
	if(!from->connection && !from->status.udp_confirmed) {
		sockaddr_t from_addr = str2sockaddr(from_address, from_port);
		if(sockaddrcmp(&from_addr, &from->address))
			update_node_udp(from, &from_addr);
	}

	node_t *to = lookup_node(to_name);
	if(!to) {
		logger(DEBUG_ALWAYS, LOG_ERR, "Got %s from %s (%s) destination %s which does not exist in our connection list", "UDP_INFO", c->name, c->hostname, to_name);
		return true;
	}

	/* Send our own data (which could be what we just received) up the chain. */

	return send_udp_info(from, to);
}
