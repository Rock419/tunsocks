#include <stdlib.h>
#include <lwip/tcp.h>
#include <lwip/udp.h>
#include <lwip/dns.h>
#include <event2/event.h>
#include <event2/bufferevent.h>
#include <event2/buffer.h>

#include "container_of.h"
#include "socks.h"
#include "socks5.h"
#include "pipe.h"

#define SOCKS5_ATYP_IPV4	0x01
#define SOCKS5_ATYP_FQDN	0x03
#define SOCKS5_ATYP_IPV6	0x04

#define SOCKS5_CMD_CONNECT	0x01
#define SOCKS5_CMD_BIND		0x02
#define SOCKS5_CMD_UDP		0x03
#define SOCKS5_CMD_RESOLVE	0xf0	/* TOR extension */

#define SOCKS5_RESP_GRANTED		0x00
#define SOCKS5_RESP_FAILURE		0x01
#define SOCKS5_RESP_PERM		0x02
#define SOCKS5_RESP_NET_UNREACH		0x03
#define SOCKS5_RESP_HOST_UNREACH	0x04
#define SOCKS5_RESP_REFUSED		0x05
#define SOCKS5_RESP_TTL			0x06
#define SOCKS5_RESP_CMD_UNSUP		0x07
#define SOCKS5_RESP_ADDR_UNSUP		0x08

struct socks5_req {
	u_char	version;
	u_char	cmd;
	u_char	reserved;
	u_char	atyp;
} __attribute__((__packed__));

struct socks5_rep {
	u_char version;
	u_char auth;
} __attribute__((__packed__));

struct socks5_data {
	struct socks_data socks;
	u_char cmd;
};

struct socks5_udp_hdr {
	u_short rsv;
	u_char frag;
	u_char atyp;
} __attribute__((__packed__));

struct socks5_udp_ipv4 {
	u_int addr;
	u_short port;
} __attribute__((__packed__));

static void
socks5_kill(struct socks_data *sdata)
{
	struct socks5_data *data;
	data = container_of(sdata, struct socks5_data, socks);
	free(data);
}

static void
socks5_response(struct socks_data *sdata, int code, int connected, int die)
{
	struct socks5_data *data;
	struct socks5_req req = {
		.version = 5,
		.cmd = code,
	};
	u_int16_t port;
	void *addr;
	u_char addr_len;

	data = container_of(sdata, struct socks5_data, socks);

	LWIP_DEBUGF(SOCKS_DEBUG, ("%s: code %d, die %d\n", __func__, code, die));
	if (!die) {
		if (data->cmd == SOCKS5_CMD_UDP) {
			struct sockaddr *sa = &sdata->server->addr;
			if (sa->sa_family == AF_INET) {
				struct sockaddr_in *sin;
				sin = (struct sockaddr_in *) sa;
				req.atyp = SOCKS5_ATYP_IPV4;
				addr = &sin->sin_addr.s_addr;
				addr_len = 4;
			} else if (sa->sa_family == AF_INET6) {
				struct sockaddr_in6 *sin;
				sin = (struct sockaddr_in6 *) sa;
				req.atyp = SOCKS5_ATYP_IPV6;
				addr = &sin->sin6_addr.s6_addr;
				addr_len = 16;
			} else {
				req.atyp = 0;
				addr = NULL;
				addr_len = 0;
				die = 1;
			}
			port = htons(sdata->udp_port);
		} else if (connected && data->cmd == SOCKS5_CMD_BIND) {
			req.atyp = SOCKS5_ATYP_IPV4;
			addr = &sdata->pcb->remote_ip.addr;
			addr_len = 4;
			port = htons(sdata->pcb->remote_port);
		} else {
			req.atyp = SOCKS5_ATYP_IPV4;
			addr = &sdata->pcb->local_ip.addr;
			addr_len = 4;
			port = htons(sdata->pcb->local_port);
		}
	} else {
		req.atyp = SOCKS5_ATYP_IPV4;
		addr = &sdata->ipaddr.addr;
		addr_len = 4;
		port = sdata->port;
	}
	bufferevent_write(sdata->bev, &req, sizeof(req));
	bufferevent_write(sdata->bev, addr, addr_len);
	bufferevent_write(sdata->bev, &port, 2);
	if (die)
		socks_flush(sdata);
}

static void
socks5_connect_ok(struct socks_data *sdata)
{
	struct socks5_data *data;
 	data = container_of(sdata, struct socks5_data, socks);

	socks5_response(sdata, SOCKS5_RESP_GRANTED, 1, 0);

	pipe_join(sdata->pcb, sdata->bev);
	free(data);
}

static void
socks5_connect_failed(struct socks_data *sdata)
{
	socks5_response(sdata, SOCKS5_RESP_HOST_UNREACH, 0, 1);
}

static void
socks5_udp_send(struct socks_data *data, struct pbuf *p)
{
	struct socks5_udp_hdr *hdr;
	ip_addr_t ipaddr;
	u_short port;
	err_t err;

	hdr = p->payload;
	if (pbuf_header(p, -(short) sizeof(*hdr))) {
		LWIP_DEBUGF(SOCKS_DEBUG, ("%s: runt\n", __func__));
		pbuf_free(p);
		return;
	}

	if (hdr->frag) {
		LWIP_DEBUGF(SOCKS_DEBUG, ("%s: frag not supported\n", __func__));
		pbuf_free(p);
		return;
	}

	if (hdr->atyp == SOCKS5_ATYP_IPV4) {
		struct socks5_udp_ipv4 *ipv4;
		ipv4 = p->payload;
		if (pbuf_header(p, -(short) sizeof(*ipv4))) {
			LWIP_DEBUGF(SOCKS_DEBUG, ("%s: runt\n", __func__));
			pbuf_free(p);
			return;
		}
		ipaddr.addr = ipv4->addr;
		port = ntohs(ipv4->port);
	} else if (hdr->atyp == SOCKS5_ATYP_FQDN) {
		u_char *len;
		char *fqdn;
		len = p->payload;
		fqdn = p->payload + 1;
		if (pbuf_header(p, -1) || pbuf_header(p, -(short) *len)) {
			LWIP_DEBUGF(SOCKS_DEBUG, ("%s: runt\n", __func__));
			pbuf_free(p);
			return;
		}
		port = *((typeof(&port)) p->payload);
		if (pbuf_header(p, -(short) sizeof(port))) {
			LWIP_DEBUGF(SOCKS_DEBUG, ("%s: runt\n", __func__));
			pbuf_free(p);
			return;
		}

		if (!strncmp(data->host.fqdn, fqdn, *len) &&
						!data->host.fqdn[*len]) {
			/* Matches current lookup */
			if (data->udp_queue) {
				/* Still ongoing, enqueue */
				struct pbuf *curr = data->udp_queue;
				for (; curr->next; curr = curr->next);
				curr->next = p;
				return;
			} else
				ipaddr.addr = data->host.ipaddr.addr;
		} else if (!data->udp_queue) {
			/* New lookup */
			memcpy(data->host.fqdn, fqdn, *len);
			data->host.fqdn[*len] = '\0';
			data->udp_queue = p;
			host_lookup(&data->host);
			return;
		} else {
			/* Concurrent DNS lookups not allowed */
			LWIP_DEBUGF(SOCKS_DEBUG, ("%s: concurrent lookup\n", __func__));
			pbuf_free(p);
			return;
		}
	} else {
		/* Unsupported */
		LWIP_DEBUGF(SOCKS_DEBUG, ("%s: Unsupported address type\n", __func__));
		return;
	}

	err = udp_sendto(data->upcb, p, &ipaddr, port);
	if (err < 0)
		LWIP_DEBUGF(SOCKS_DEBUG, ("%s: udp_sendto failed: %d\n", __func__, err));
}

static void
socks5_udp_recv(struct socks_data *data, struct pbuf *p,
			const ip_addr_t *addr, u16_t port)
{
	struct socks5_udp_hdr *hdr;

	if (pbuf_header(p, sizeof(port))) {
		/* Not enough header space */
		return;
	}
	*((typeof(&port)) p->payload) = port;

	if (pbuf_header(p, sizeof(addr->addr))) {
		/* Not enough header space */
		return;
	}
	*((u32_t *) p->payload) = addr->addr;

	if (pbuf_header(p, sizeof(*hdr))) {
		/* Not enough header space */
		return;
	}
	hdr = p->payload;

	hdr->rsv = 0;
	hdr->frag = 0;
	hdr->atyp = SOCKS5_ATYP_IPV4;

	if (send(data->udp_fd, p->payload, p->len, 0) < 0) {
		/* Error sending packet */
	}
}

static void
socks5_read_port(struct socks_data *sdata)
{
	struct socks5_data *data;
	struct event_base *base;
 	data = container_of(sdata, struct socks5_data, socks);

	bufferevent_read(sdata->bev, &sdata->port, 2);
	sdata->port = ntohs(sdata->port);

	LWIP_DEBUGF(SOCKS_DEBUG, ("%s: port %d\n", __func__, sdata->port));

	switch (data->cmd) {
	case SOCKS5_CMD_CONNECT:
		socks_tcp_connect(sdata);
		break;
	case SOCKS5_CMD_BIND:
		if (socks_tcp_bind(sdata) < 0) {
			socks5_response(sdata, SOCKS5_RESP_FAILURE, 0, 1);
		} else {
			/*
			 * If the user sends any input data at this point, it is
			 * an error
			 */
			socks_request(sdata, 1, socks_kill);
			socks5_response(sdata, SOCKS5_RESP_GRANTED, 0, 0);
		}
		break;
	case SOCKS5_CMD_UDP:
		base = bufferevent_get_base(sdata->bev);
		if (socks_udp_bind(base, sdata) < 0) {
			socks5_response(sdata, SOCKS5_RESP_FAILURE, 0, 1);
		} else {
			socks_request(sdata, 1, socks_kill);
			socks5_response(sdata, SOCKS5_RESP_GRANTED, 0, 0);
		}
		break;
	case SOCKS5_CMD_RESOLVE:
		socks5_response(sdata, SOCKS5_RESP_GRANTED, 0, 1);
		break;
	default:
		socks5_response(sdata, SOCKS5_RESP_CMD_UNSUP, 0, 1);
	}
}

static void
socks5_read_ipv4(struct socks_data *sdata)
{
	bufferevent_read(sdata->bev, &sdata->ipaddr.addr, 4);
	socks_request(sdata, 2, socks5_read_port);
}

static void
socks5_host_found(struct host_data *hdata)
{
	struct socks_data *sdata;
	struct socks5_data *data;

	sdata = container_of(hdata, struct socks_data, host);
	data = container_of(sdata, struct socks5_data, socks);

	LWIP_DEBUGF(SOCKS_DEBUG, ("%s\n", __func__));

	if (data->cmd == SOCKS5_CMD_UDP) {
		while (sdata->udp_queue) {
			/* Empty the send queue */
			struct pbuf *p = sdata->udp_queue;
			u16_t port;
			sdata->udp_queue = p->next;
			p->next = NULL;
			port = *((typeof(&port)) (p->payload - 2));
			udp_sendto(sdata->upcb, p, &hdata->ipaddr, port);
			pbuf_free(p);
		}
	} else {
		sdata->ipaddr = hdata->ipaddr;
		socks_request(sdata, 2, socks5_read_port);
	}
}

static void
socks5_host_failed(struct host_data *hdata)
{
	struct socks_data *sdata;
	struct socks5_data *data;

	sdata = container_of(hdata, struct socks_data, host);
	data = container_of(sdata, struct socks5_data, socks);

	LWIP_DEBUGF(SOCKS_DEBUG, ("%s\n", __func__));
	if (data->cmd == SOCKS5_CMD_UDP) {
		while (sdata->udp_queue) {
			/* Free the send queue */
			struct pbuf *p = sdata->udp_queue;
			sdata->udp_queue = p->next;
			pbuf_free(p);
		}
	} else
		socks5_response(sdata, SOCKS5_RESP_FAILURE, 0, 1);
}

static void
socks5_read_fqdn(struct socks_data *sdata)
{
	bufferevent_read(sdata->bev, sdata->host.fqdn, sdata->req_len);
	sdata->host.fqdn[sdata->req_len] = '\0';
	LWIP_DEBUGF(SOCKS_DEBUG, ("%s: fqdn %s\n", __func__, sdata->host.fqdn));
	bufferevent_disable(sdata->bev, EV_READ);
	host_lookup(&sdata->host);
}

static void
socks5_read_n_fqdn(struct socks_data *sdata)
{
	unsigned char nfqdn;

	bufferevent_read(sdata->bev, &nfqdn, 1);
	LWIP_DEBUGF(SOCKS_DEBUG, ("%s: nfqdn %d\n", __func__, nfqdn));
	if (!nfqdn)
		socks5_response(sdata, SOCKS5_RESP_CMD_UNSUP, 0, 1);
	else
		socks_request(sdata, nfqdn, socks5_read_fqdn);
}

static void
socks5_read_hdr(struct socks_data *sdata)
{
	struct socks5_data *data;
	struct socks5_req req;

	data = container_of(sdata, struct socks5_data, socks);

	bufferevent_read(sdata->bev, &req, sizeof(req));
	if (req.version != 5) {
		socks_kill(sdata);
		return;
	}

	data->cmd = req.cmd;

	LWIP_DEBUGF(SOCKS_DEBUG, ("%s: cmd %d, atyp %d\n", __func__, req.cmd, req.atyp));

	if (req.atyp == SOCKS5_ATYP_IPV4)
		socks_request(sdata, 4, socks5_read_ipv4);
	else if (req.atyp == SOCKS5_ATYP_FQDN)
		socks_request(sdata, 1, socks5_read_n_fqdn);
	else
		socks5_response(sdata, SOCKS5_RESP_ADDR_UNSUP, 0, 1);
}

static void
socks5_read_auth(struct socks_data *sdata)
{
	u_char auth[255];
	struct socks5_rep rep = {5, 0};

	if (sdata->req_len)
		bufferevent_read(sdata->bev, auth, sdata->req_len);

	bufferevent_write(sdata->bev, &rep, sizeof(rep));

	socks_request(sdata, sizeof(struct socks5_req), socks5_read_hdr);
}

static void
socks5_read_n_auth(struct socks_data *sdata)
{
	unsigned char nauth;

	bufferevent_read(sdata->bev, &nauth, 1);

	LWIP_DEBUGF(SOCKS_DEBUG, ("%s: nauth %d\n", __func__, nauth));

	socks_request(sdata, nauth, socks5_read_auth);
}

void
socks5_start(struct socks_server *s, struct bufferevent *bev)
{
	struct socks5_data *data;
	struct socks_data *sdata;

	data = calloc(1, sizeof(struct socks5_data));
	sdata = &data->socks;
	sdata->server = s;
	sdata->host.found = socks5_host_found;
	sdata->host.failed = socks5_host_failed;
	sdata->connect_ok = socks5_connect_ok;
	sdata->connect_failed = socks5_connect_failed;
	sdata->kill = socks5_kill;
	sdata->udp_send = socks5_udp_send;
	sdata->udp_recv = socks5_udp_recv;
	sdata->bev = bev;
	socks_request(sdata, 1, socks5_read_n_auth);
}
