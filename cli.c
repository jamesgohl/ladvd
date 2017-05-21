/*
 * $Id$
 *
 * Copyright (c) 2008, 2009, 2010
 *      Sten Spans <sten@blinkenlights.nl>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include "common.h"
#include "util.h"
#include "proto/protos.h"
#include "cli.h"
#include <sys/file.h>
#include <sys/un.h>
#include <netdb.h>
#include <termios.h> 

extern struct proto protos[];
int status = EXIT_SUCCESS;
static void usage() __noreturn;

static struct mode modes[] = {
  { &cli_header, &cli_write, NULL },
  { NULL, NULL, NULL },
  { NULL, &batch_write, NULL },
  { &debug_header, &debug_write, &debug_close },
#if HAVE_EVHTTP_H
  { &http_connect, &http_request, &http_dispatch },
#endif /* HAVE_EVHTTP_H */
};

#define MODE_CLI    0
#define MODE_PRINT  1
#define MODE_BATCH  2
#define MODE_DEBUG  3
#define MODE_HTTP   4

#define TERM_DEFAULT 80
static int host_width = 20;
static int port_width = 10;

#if HAVE_EVHTTP_H
char *http_host = NULL;
char *http_path = NULL;
char *hostname = NULL;
short http_port = 0;

struct evhttp_connection *evcon = NULL;
struct evhttp_request *lreq = NULL;
#endif /* HAVE_EVHTTP_H */

__noreturn
void cli_main(int argc, char *argv[]) {
    int ch, i;
    uint8_t proto = 0, mode = MODE_CLI;
    uint32_t *indexes = NULL;
    struct sockaddr_un usock = {};
    int fd = -1;
    time_t now;
    struct parent_msg *msg;
    uint16_t holdtime;

    options = 0;

    while ((ch = getopt(argc, argv, "LCEFNbdfp:ovh")) != -1) {
	switch(ch) {
	    case 'L':
		proto |= (1 << PROTO_LLDP);
		break;
	    case 'C':
		proto |= (1 << PROTO_CDP);
		break;
	    case 'E':
		proto |= (1 << PROTO_EDP);
		break;
	    case 'F':
		proto |= (1 << PROTO_FDP);
		break;
	    case 'N':
		proto |= (1 << PROTO_NDP);
		break;
	    case 'b':
		mode = MODE_BATCH;
		break;
	    case 'd':
		mode = MODE_DEBUG;
		break;
	    case 'f':
		mode = MODE_PRINT;
		break;
#if HAVE_EVHTTP_H
	    case 'p':
		if (http_host)
		    usage();
		mode = MODE_HTTP;
		if (strncmp(optarg, "http://", 7) == 0)
		    optarg += 7;
		char *host = my_strdup(optarg);
		char *path = strchr(host, '/');
		if (path) {
		    http_path = my_strdup(path);
		    *path = '\0';
		} else {
		    http_path = my_strdup("/");
		}
		http_host = my_strdup(host);
		free(host);
		break;
#endif /* HAVE_EVHTTP_H */
	    case 'o':
		options |= OPT_ONCE;
		break;
	    case 'v':
		loglevel++;
		break;
	    default:
		usage();
	}
    }

    argc -= optind;
    argv += optind;

    // default to all protocols
    if (!proto)
	proto = UINT8_MAX;

    if (argc) {
	indexes = my_calloc(argc, sizeof(msg->index));
	for (i = 0; i < argc; i++) {
	    indexes[i] = if_nametoindex(argv[i]);
	    if (!indexes[i])
		usage();
	}
    }

    // open socket connection
    fd = socket(AF_UNIX, SOCK_SEQPACKET, 0);
    // XXX: make do with a stream and hope for the best
    if ((fd == -1) && (errno == EPROTONOSUPPORT))
	fd = my_socket(AF_UNIX, SOCK_STREAM, 0);
    if (fd == -1)
	my_fatale("failed to create socket");

    usock.sun_family = AF_UNIX;
    strlcpy(usock.sun_path, PACKAGE_SOCKET, sizeof(usock.sun_path));

    if (connect(fd, (struct sockaddr *)&usock, sizeof(usock)) == -1) {
	if (errno == EACCES)
	    my_fatal("please add yourself to the " PACKAGE_USER " group");
	else if (errno == ECONNREFUSED)
	    my_fatal("please enable receive mode before using " PACKAGE_CLI);
	else
	    my_fatale("failed to open " PACKAGE_SOCKET);
    }
    if ((now = time(NULL)) == (time_t)-1)
	my_fatale("failed to fetch time");

    if (modes[mode].init)
	modes[mode].init();

    msg = my_malloc(PARENT_MSG_SIZ);

    while (read(fd, msg, PARENT_MSG_MAX) == PARENT_MSG_MAX) {

	if (msg->proto >= PROTO_MAX)
	    continue;
	if ((msg->len < (ETHER_MIN_LEN - ETHER_VLAN_ENCAP_LEN)) || 
	    (msg->len > ETHER_MAX_LEN))
	    continue;
	
	// skip unwanted interfaces
	if (indexes) {
	    for (i = 0; i < argc; i++) {
		if (indexes[i] == msg->index)
		    break;
	    }
	    if (i == argc)
		continue;
	}

	// skip unwanted protocols
	if (!(proto & (1 << msg->proto)))
	    continue;

	// decode packet
	msg->decode = DECODE_STR;
	if (mode == MODE_PRINT)
	    msg->decode = DECODE_PRINT;

	if (protos[msg->proto].decode(msg) == 0) {
	    peer_free(msg->peer);
	    continue;
	}
	// skip expired packets
	if (msg->ttl < (now - msg->received))
	    continue;

	holdtime = msg->ttl - (now - msg->received);
	
	if (modes[mode].write)
	    modes[mode].write(msg, holdtime);

	peer_free(msg->peer);
	memset(msg, 0, PARENT_MSG_SIZ);

	if (options & OPT_ONCE)
	    goto out;

	if (mode == MODE_PRINT)
	    printf("\n");
    }

out:
    if (modes[mode].dispatch)
	modes[mode].dispatch();

    free(msg);
    exit(status);
}

static inline void swapchr(char *str, const int c, const int d) {
    if (!str)
	return;

    while ((str = strchr(str, c)) != NULL) {
	*str = d;
	 str++;
    }
}

#define STR(x)	(x) ? x : ""

void batch_write(struct parent_msg *msg, const uint16_t holdtime) {
    char *peer_host = msg->peer[PEER_HOSTNAME];
    char *peer_platform = msg->peer[PEER_PLATFORM];				// added by James Gohl 18/02/2017
    char *peer_portname = msg->peer[PEER_PORTNAME];
    char *peer_portdescr = msg->peer[PEER_PORTDESCR];
    char *peer_duplex = msg->peer[PEER_DUPLEX];					// added by James Gohl 18/02/2017
    char *peer_vtp_md = msg->peer[PEER_VTP_MD];					// added by James Gohl 18/02/2017
    char *cap = msg->peer[PEER_CAP];
    static unsigned int count = 0;

    swapchr(peer_host, '\'', '\"');
    swapchr(peer_platform, '\'', '\"');						// added by James Gohl 18/02/2017
    swapchr(peer_portname, '\'', '\"');
    swapchr(peer_portdescr, '\'', '\"');
    
    // my limited knowledge of C doesn't extend to being able to write a conditional output, STR() has a macrs which tries to "free()" a pointer ??
    swapchr(peer_duplex, '0', 'h');						// added by James Gohl 18/02/2017
    swapchr(peer_duplex, '1', 'F');						// added by James Gohl 18/02/2017

    printf("INTERFACE_%u='%s'\n", count, STR(msg->name));
    printf("HOSTNAME_%u='%s'\n", count, STR(peer_host));
    printf("PLATFORM_%u='%s'\n", count, STR(peer_platform));			// added by James Gohl 18/02/2017
    printf("PORTNAME_%u='%s'\n", count, STR(peer_portname));
    printf("PORTDESCR_%u='%s'\n", count, STR(peer_portdescr));
    printf("DUPLEX_%u='%s'\n", count, STR(peer_duplex));			// added by James Gohl 18/02/2017
    printf("PROTOCOL_%u='%s'\n", count, protos[msg->proto].name);
    printf("ADDR_INET4_%u='%s'\n", count, STR(msg->peer[PEER_ADDR_INET4]));
    printf("ADDR_INET6_%u='%s'\n", count, STR(msg->peer[PEER_ADDR_INET6]));
    printf("ADDR_802_%u='%s'\n", count, STR(msg->peer[PEER_ADDR_802]));
    printf("VLAN_ID_%u='%s'\n", count, STR(msg->peer[PEER_VLAN_ID]));
    printf("VTP_MGMT_DOM_%u='%s'\n", count, STR(peer_vtp_md));	// added by James Gohl 18/02/2017
    printf("CAPABILITIES_%u='%s'\n", count, STR(cap));
    printf("TTL_%u='%" PRIu16 "'\n", count, msg->ttl);
    printf("HOLDTIME_%u='%" PRIu16 "'\n", count, holdtime);

    count++;
}

void cli_header() {
    int twidth = TERM_DEFAULT;
    struct winsize ws = {};

    // Try to fetch the terminal width
    if (ioctl(0, TIOCGWINSZ, &ws) == 0)
	twidth = ws.ws_col;

    twidth -= TERM_DEFAULT;
    if (twidth > 0) {
	if (twidth > 20) {
	    host_width += 10;
	    port_width = twidth;
	} else {
	    twidth /= 2;
	    host_width += twidth;
	    port_width += twidth;
	}
    }

    printf("Capability Codes:\n"
	"\tr - Repeater, B - Bridge, H - Host, R - Router, S - Switch,\n"
	"\tW - WLAN Access Point, C - DOCSIS Device, T - Telephone, "
	"O - Other\n\n");
    printf("%-*s Local Intf    Proto   "
	"Hold-time    Capability    Port ID\n", host_width, "Device ID");
}

void cli_write(struct parent_msg *msg, const uint16_t holdtime) {
    char *peer_host = msg->peer[PEER_HOSTNAME];
    char *peer_portname = msg->peer[PEER_PORTNAME];
    char *peer_portdescr = msg->peer[PEER_PORTDESCR];
    char *peer_suffix = NULL;
    char *cap = msg->peer[PEER_CAP];

    // shorten
    if (peer_host && (strlen(peer_host) > host_width))
	peer_host[strcspn(peer_host, ".")] = '\0';

    peer_suffix = peer_portname;
    if (!peer_suffix)
	peer_suffix = peer_portdescr;
    if (peer_suffix)
	portname_abbr(peer_suffix);

    printf("%-*.*s %-13.13s %-7.7s %-12" PRIu16 " %-13.13s %-*.*s\n",
	host_width, host_width, STR(peer_host), STR(msg->name), protos[msg->proto].name,
	holdtime, STR(cap), port_width, port_width, STR(peer_suffix));
}

void debug_header() {
    my_pcap_init(STDOUT_FILENO);
}

void debug_write(struct parent_msg *msg, const uint16_t holdtime) {
    my_pcap_write(msg);
}

void debug_close() {
    my_pcap_close();
}

#if HAVE_EVHTTP_H
void http_connect() {
    struct servent *sp;
    struct hostent *hp;

    if (!http_port) {
	if ((sp = getservbyname("http", "tcp")) == NULL)
	    my_fatal("HTTP port not found");
	http_port = ntohs(sp->s_port);
    }

    // initalize the event library
    event_init();

    evcon = evhttp_connection_new(http_host, http_port);
    if (evcon == NULL)
        my_fatal("HTTP connection failed");

    hostname = my_malloc(_POSIX_HOST_NAME_MAX);
    if (gethostname(hostname, _POSIX_HOST_NAME_MAX) == -1)
	my_fatale("gethostname failed");
    if (((hp = gethostbyname(hostname)) != NULL) &&
	(strcmp(hp->h_name, "localhost") != 0))
	strlcpy(hostname, hp->h_name, _POSIX_HOST_NAME_MAX);
}

void http_request(struct parent_msg *msg, const uint16_t holdtime) {
    int ret;
    char *peer_host, *peer_port, *data;
    char *cap = msg->peer[PEER_CAP];
    struct evhttp_request *req = NULL;

    // url-encode the received strings
    peer_host = evhttp_encode_uri(STR(msg->peer[PEER_HOSTNAME]));
    peer_port = evhttp_encode_uri(STR(msg->peer[PEER_PORTNAME]));

    ret = asprintf(&data,
	"hostname=%s&interface=%s&peer_hostname=%s&peer_portname=%s&"
	"protocol=%s&capabilities=%s&ttl=%" PRIu16 "&holdtime=%" PRIu16
	"\r\n\r\n",
	hostname, STR(msg->name), peer_host, peer_port,
	protos[msg->proto].name, STR(cap), msg->ttl, holdtime);
    if (ret == -1)
	my_fatal("asprintf failed");

    free(peer_host);
    free(peer_port);

    req = evhttp_request_new(http_reply, NULL);
    if (ret == -1)
	my_fatal("failed to allocate HTTP request");

    evhttp_add_header(req->output_headers, "Host", http_host);
    evhttp_add_header(req->output_headers, "User-Agent", 
		PACKAGE_CLI "/" PACKAGE_VERSION);
    evhttp_add_header(req->output_headers, "Content-Type",
		"application/x-www-form-urlencoded");
    evbuffer_add(req->output_buffer, data, strlen(data));

    if (evhttp_make_request(evcon, req, EVHTTP_REQ_POST, http_path) == -1)
	my_fatal("failed to create HTTP request");
    lreq = req;

    free(data);
}

void http_reply(struct evhttp_request *req, void *arg) {
    if ((req == NULL) || (req->response_code == 0))
	my_fatal("HTTP request failed");

    if (req->response_code < HTTP_BADREQUEST)
	return;

    my_log(CRIT, "HTTP error %d received", req->response_code);
    status = EXIT_FAILURE;
}

void http_dispatch() {
    // auto-close the connection after the last request
    if (lreq)
	evhttp_add_header(lreq->output_headers, "Connection", "close");

    event_dispatch();
    evhttp_connection_free(evcon);
}
#endif /* HAVE_EVHTTP_H */

__noreturn
static void usage() {
    extern char *__progname;

    fprintf(stderr, PACKAGE_NAME " version " PACKAGE_VERSION "\n" 
	"Usage: %s [-LCEFN] [INTERFACE] [INTERFACE]\n"
	    "\t-L = Print LLDP\n"
	    "\t-C = Print CDP\n"
	    "\t-E = Print EDP\n"
	    "\t-F = Print FDP\n"
	    "\t-N = Print NDP\n"
	    "\t-b = Print scriptable output\n"
	    "\t-d = Dump pcap-compatible packets to stdout\n"
	    "\t-f = Print full decode\n"
	    "\t-o = Decode only one packet\n"
#if HAVE_EVHTTP_H
	    "\t-p <url> = Post decode to url\n"
#endif /* HAVE_EVHTTP_H */
	    "\t-v = Increase logging verbosity\n"
	    "\t-h = Print this message\n",
	    __progname);

    exit(EXIT_FAILURE);
}

