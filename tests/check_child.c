/*
 * $Id$
 *
 * Copyright (c) 2008, 2009
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

#include <check.h>

#include "common.h"
#include "util.h"
#include "proto/protos.h"
#include "main.h"
#include "child.h"
#include "check_wrap.h"

uint32_t options = OPT_DAEMON | OPT_CHECK;
extern struct nhead netifs;
extern struct mhead mqueue;
extern struct sysinfo sysinfo;
extern int msock;

void read_packet(struct master_msg *msg, const char *suffix) {
    int fd;
    char *prefix, *path = NULL;

    memset(msg->msg, 0, ETHER_MAX_LEN);
    msg->len = 0;
    msg->ttl = 0;
    memset(msg->peer.name, 0, IFDESCRSIZE);
    memset(msg->peer.port, 0, IFDESCRSIZE);

    if ((prefix = getenv("srcdir")) == NULL)
	prefix = ".";

    fail_if(asprintf(&path, "%s/%s", prefix, suffix) == -1,
	    "asprintf failed");

    mark_point();
    fail_if((fd = open(path, O_RDONLY)) == -1, "failed to open %s", path);
    msg->len = read(fd, msg->msg, ETHER_MAX_LEN);

    free(path);
}

START_TEST(test_child_init) {
    struct master_msg *mreq;
    struct netif *netif, *nnetif;
    const char *errstr = NULL;
    int spair[2];
    pid_t pid;

    options |= OPT_ONCE;
    loglevel = CRIT;
    my_socketpair(spair);

    // start a dummy replier
    pid = fork();
    if (pid == 0) {
	close(spair[0]);
	mreq = my_malloc(MASTER_MSG_SIZE);
	while (read(spair[1], mreq, MASTER_MSG_SIZE) > 0) {
	    mreq->completed = 1;
	    if (mreq->cmd == MASTER_DEVICE)
		mreq->len = 1;
	    if (write(spair[1], mreq, MASTER_MSG_SIZE) != MASTER_MSG_SIZE)
		exit(1);
	}
	exit (0);
    }
    close(spair[1]);

    child_init(spair[0], -1, 0, NULL);

    errstr = PACKAGE_STRING " running";
    fail_unless (strncmp(check_wrap_errstr, errstr, strlen(errstr)) == 0,
	"incorrect message logged: %s", check_wrap_errstr);

    // reset
    kill(pid, SIGTERM);
    loglevel = INFO;
    options = OPT_DAEMON | OPT_CHECK;
    TAILQ_FOREACH_SAFE(netif, &netifs, entries, nnetif) {
	TAILQ_REMOVE(&netifs, netif, entries);
    }
}
END_TEST

START_TEST(test_child_send) {
    struct master_msg *mreq;
    struct netif *netif, *nnetif;
    int spair[2];
    struct event evs;
    pid_t pid;

    loglevel = INFO;
    my_socketpair(spair);
    msock = spair[0];

    // initalize the event library
    event_init();
    evtimer_set(&evs, (void *)child_send, &evs);

    // start a dummy replier
    pid = fork();
    if (pid == 0) {
	close(spair[0]);
	mreq = my_malloc(MASTER_MSG_SIZE);
	while (read(spair[1], mreq, MASTER_MSG_SIZE) > 0) {
	    mreq->completed = 1;
	    if (mreq->cmd == MASTER_DEVICE)
		mreq->len = 1;
	    if (write(spair[1], mreq, MASTER_MSG_SIZE) != MASTER_MSG_SIZE)
		exit(1);
	}
	exit (0);
    }
    close(spair[1]);

    // no protocols enabled
    mark_point();
    protos[PROTO_LLDP].enabled = 0;
    child_send(-1, EV_TIMEOUT, &evs);

    // LLDP enabled
    mark_point();
    protos[PROTO_LLDP].enabled = 1;
    child_send(-1, EV_TIMEOUT, &evs);

    // CDP enabled
    mark_point();
    protos[PROTO_CDP].enabled = 1;
    child_send(-1, EV_TIMEOUT, &evs);

    // reset
    kill(pid, SIGTERM);
    TAILQ_FOREACH_SAFE(netif, &netifs, entries, nnetif) {
	TAILQ_REMOVE(&netifs, netif, entries);
    }
}
END_TEST

START_TEST(test_child_queue) {
    struct master_msg msg, *dmsg, *nmsg;
    struct netif netif;
    struct ether_hdr ether;
    static uint8_t lldp_dst[] = LLDP_MULTICAST_ADDR;
    int spair[2];
    short event = 0;
    const char *errstr = NULL;

    loglevel = INFO;
    my_socketpair(spair);
    memset(&msg, 0, sizeof(struct master_msg));
    msg.cmd = MASTER_RECV;
    msg.len = ETHER_MIN_LEN;
    msg.proto = PROTO_LLDP;

    // unknown interface
    mark_point();
    errstr = "receiving message from master";
    WRAP_WRITE(spair[0], &msg, MASTER_MSG_SIZE);
    child_queue(spair[1], event);
    fail_unless (strncmp(check_wrap_errstr, errstr, strlen(errstr)) == 0,
	"incorrect message logged: %s", check_wrap_errstr);
    
    // locally generated packet
    mark_point();
    memset(&netif, 0, sizeof(struct netif));
    netif.index = 1;
    strlcpy(netif.name, "lo0", IFNAMSIZ);
    TAILQ_INSERT_TAIL(&netifs, &netif, entries);
    msg.index = 1;
    WRAP_WRITE(spair[0], &msg, MASTER_MSG_SIZE);
    child_queue(spair[1], event);
    fail_unless (strncmp(check_wrap_errstr, errstr, strlen(errstr)) == 0,
	"incorrect message logged: %s", check_wrap_errstr);

    // invalid message contents
    mark_point();
    errstr = "Invalid LLDP packet";
    memset(&ether.src, 'A', ETHER_ADDR_LEN);
    memcpy(&ether.dst, lldp_dst, ETHER_ADDR_LEN);
    ether.type = htons(ETHERTYPE_LLDP);
    memcpy(msg.msg, &ether, sizeof(ether));
    WRAP_WRITE(spair[0], &msg, MASTER_MSG_SIZE);
    child_queue(spair[1], event);
    fail_unless (strncmp(check_wrap_errstr, errstr, strlen(errstr)) == 0,
	"incorrect message logged: %s", check_wrap_errstr);

    // valid message contents
    mark_point();
    read_packet(&msg, "proto/lldp/42.good.big");
    WRAP_WRITE(spair[0], &msg, MASTER_MSG_SIZE);
    child_queue(spair[1], event);

    // and the same peer again
    mark_point();
    read_packet(&msg, "proto/lldp/42.good.big");
    WRAP_WRITE(spair[0], &msg, MASTER_MSG_SIZE);
    child_queue(spair[1], event);

    // test with OPT_AUTO
    mark_point();
    options |= OPT_AUTO;
    read_packet(&msg, "proto/lldp/43.good.lldpmed");
    WRAP_WRITE(spair[0], &msg, MASTER_MSG_SIZE);
    child_queue(spair[1], event);

    // test with OPT_ARGV
    mark_point();
    options |= OPT_ARGV;
    msg.proto = PROTO_CDP;
    read_packet(&msg, "proto/cdp/45.good.6504");
    WRAP_WRITE(spair[0], &msg, MASTER_MSG_SIZE);
    child_queue(spair[1], event);

    // reset
    options = OPT_DAEMON | OPT_CHECK;
    TAILQ_REMOVE(&netifs, &netif, entries);
    TAILQ_FOREACH_SAFE(dmsg, &mqueue, entries, nmsg) {
	TAILQ_REMOVE(&mqueue, dmsg, entries);
    }
}
END_TEST

START_TEST(test_child_expire) {
    struct master_msg msg, *dmsg, *nmsg;
    struct netif netif;
    int spair[2], count;
    short event = 0;

    loglevel = INFO;
    my_socketpair(spair);

    memset(&netif, 0, sizeof(struct netif));
    netif.index = 1;
    strlcpy(netif.name, "lo0", IFNAMSIZ);
    TAILQ_INSERT_TAIL(&netifs, &netif, entries);

    memset(&msg, 0, sizeof(struct master_msg));
    msg.cmd = MASTER_RECV;
    msg.index = 1;
    msg.len = ETHER_MIN_LEN;
    msg.proto = PROTO_LLDP;

    fail_unless (TAILQ_EMPTY(&mqueue), "the queue should be empty");

    // add an lldp message
    mark_point();
    read_packet(&msg, "proto/lldp/42.good.big");
    WRAP_WRITE(spair[0], &msg, MASTER_MSG_SIZE);
    child_queue(spair[1], event);
    child_expire();

    // check the message count
    count = 0;
    TAILQ_FOREACH(dmsg, &mqueue, entries) {
	count++;
    }
    fail_unless (count == 1, "invalid message count: %d != 1", count);

    // add an cdp message
    mark_point();
    msg.proto = PROTO_CDP;
    read_packet(&msg, "proto/cdp/45.good.6504");
    WRAP_WRITE(spair[0], &msg, MASTER_MSG_SIZE);
    child_queue(spair[1], event);
    child_expire();

    // check the message count
    count = 0;
    TAILQ_FOREACH(dmsg, &mqueue, entries) {
	count++;
    }
    fail_unless (count == 2, "invalid message count: %d != 2", count);

    // expire a message
    options |= OPT_AUTO;
    dmsg = TAILQ_FIRST(&mqueue);
    dmsg->ttl = 0;
    child_expire();

    // check the message count
    count = 0;
    TAILQ_FOREACH(dmsg, &mqueue, entries) {
	count++;
    }
    fail_unless (count == 1, "invalid message count: %d != 1", count);

    // expire a message
    dmsg = TAILQ_FIRST(&mqueue);
    dmsg->ttl = 0;
    child_expire();

    // check the message count
    fail_unless (TAILQ_EMPTY(&mqueue), "the queue should be empty");

    // reset
    options = OPT_DAEMON | OPT_CHECK;
    TAILQ_REMOVE(&netifs, &netif, entries);
    TAILQ_FOREACH_SAFE(dmsg, &mqueue, entries, nmsg) {
	TAILQ_REMOVE(&mqueue, dmsg, entries);
    }
}
END_TEST

Suite * child_suite (void) {
    Suite *s = suite_create("child.c");

    TAILQ_INIT(&netifs);
    TAILQ_INIT(&mqueue);
    sysinfo_fetch(&sysinfo);

    // child test case
    TCase *tc_child = tcase_create("child");
    tcase_add_test(tc_child, test_child_init);
    tcase_add_test(tc_child, test_child_send);
    tcase_add_test(tc_child, test_child_queue);
    tcase_add_test(tc_child, test_child_expire);
    suite_add_tcase(s, tc_child);

    return s;
}

int main (void) {
    int number_failed;
    Suite *s = child_suite ();
    SRunner *sr = srunner_create (s);
    srunner_run_all (sr, CK_NORMAL);
    number_failed = srunner_ntests_failed (sr);
    srunner_free (sr);
    return (number_failed == 0) ? EXIT_SUCCESS : EXIT_FAILURE;
}

