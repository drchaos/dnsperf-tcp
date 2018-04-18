/*
 * Copyright (C) 2000, 2001  Nominum, Inc.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND INTERNET SOFTWARE CONSORTIUM
 * DISCLAIMS ALL WARRANTIES WITH REGARD TO THIS SOFTWARE INCLUDING ALL
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL
 * INTERNET SOFTWARE CONSORTIUM BE LIABLE FOR ANY SPECIAL, DIRECT,
 * INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING
 * FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN ACTION OF CONTRACT,
 * NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF OR IN CONNECTION
 * WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Copyright (C) 2004 - 2015 Nominum, Inc.
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose with or without fee is hereby granted,
 * provided that the above copyright notice and this permission notice
 * appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND NOMINUM DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL NOMINUM BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/*
 * Copyright (C) 2016,2018 Sinodun IT Ltd.
 *
 * Permission to use, copy, modify, and distribute this software and its
 * documentation for any purpose with or without fee is hereby granted,
 * provided that the above copyright notice and this permission notice
 * appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND SINODUN DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL SINODUN BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT
 * OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

/***
 ***	DNS Performance Testing Tool
 ***
 ***	Version $Id: dnsperf.c 263303 2015-12-15 01:09:36Z bwelling $
 ***/

#include <assert.h>
#include <errno.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/ioctl.h>

#include <sys/time.h>

#define ISC_BUFFER_USEINLINE

#include <isc/buffer.h>
#include <isc/file.h>
#include <isc/list.h>
#include <isc/mem.h>
#include <isc/netaddr.h>
#include <isc/print.h>
#include <isc/region.h>
#include <isc/result.h>
#include <isc/sockaddr.h>
#include <isc/types.h>

#include <openssl/ssl.h>
#include <openssl/err.h>

#include <dns/rcode.h>
#include <dns/result.h>

#include "net.h"
#include "datafile.h"
#include "dns.h"
#include "log.h"
#include "opt.h"
#include "os.h"
#include "util.h"
#include "config.h"

#define DEFAULT_SERVER_NAME		"127.0.0.1"
#define DEFAULT_SERVER_PORT		53
#define DEFAULT_TLS_SERVER_PORT		853
#define DEFAULT_LOCAL_PORT		0
#define DEFAULT_MAX_OUTSTANDING		100
#define DEFAULT_TIMEOUT			5

#define TIMEOUT_CHECK_TIME		100000

#define MAX_INPUT_DATA			(64 * 1024)

#define MAX_SOCKETS			256

#define RECV_BATCH_SIZE			16

typedef struct {
	int argc;
	char **argv;
	int family;
	isc_uint32_t clients;
	isc_uint32_t threads;
	isc_uint32_t maxruns;
	isc_uint64_t timelimit;
	isc_sockaddr_t server_addr;
	isc_sockaddr_t local_addr;
	isc_uint64_t timeout;
	isc_uint32_t bufsize;
	isc_boolean_t edns;
	isc_boolean_t dnssec;
	perf_dnstsigkey_t *tsigkey;
	isc_uint32_t max_outstanding;
	isc_uint32_t max_qps;
	isc_uint64_t stats_interval;
	isc_boolean_t updates;
	isc_boolean_t verbose;
	isc_boolean_t debug;
	isc_boolean_t usetcp;
	isc_boolean_t usetcptls;
	isc_uint32_t max_tcp_q;
} config_t;

typedef struct {
	isc_uint64_t start_time;
	isc_uint64_t end_time;
	isc_uint64_t stop_time;
	struct timespec stop_time_ns;
	isc_uint64_t tcp_hs_time;
	isc_uint64_t tls_hs_time;
} times_t;

typedef struct {
	isc_uint64_t rcodecounts[16];

	isc_uint64_t num_sent;
	isc_uint64_t num_interrupted;
	isc_uint64_t num_timedout;
	isc_uint64_t num_completed;
	isc_uint64_t num_tcp_conns;

	isc_uint64_t total_request_size;
	isc_uint64_t total_response_size;

	isc_uint64_t latency_sum;
	isc_uint64_t latency_sum_squares;
	isc_uint64_t latency_min;
	isc_uint64_t latency_max;
} stats_t;

typedef enum {
	SOCKET_SEND_CLOSED,
	SOCKET_SEND_TCP_HANDSHAKE,
	SOCKET_SEND_TLS_HANDSHAKE,
	SOCKET_SEND_READY,
	SOCKET_SEND_SENDING,
	SOCKET_SEND_TCP_SENT_MAX,
} socket_send_state_t;

typedef enum {
	SOCKET_RECV_CLOSED,
	SOCKET_RECV_HANDSHAKE,
	SOCKET_RECV_READY,
	SOCKET_RECV_READING,
} socket_recv_state_t;

typedef struct 
{
	int fd;
	unsigned int socket_number;
	unsigned int port_offset;
	isc_uint64_t num_recv;
	isc_uint64_t num_sent;
	isc_uint64_t num_in_flight;
	socket_send_state_t send_state;
	socket_recv_state_t recv_state;
	isc_buffer_t sending;
	unsigned char sending_buffer[MAX_EDNS_PACKET];
	unsigned int tcp_to_read;
	SSL *ssl;
	isc_uint64_t con_start_time;
	isc_uint64_t tcp_hs_done_time;
	isc_uint64_t tls_hs_done_time;
	isc_uint64_t cumulative_tcp_hs_time;
	isc_uint64_t cumulative_tls_hs_time;
} sockinfo_t;

typedef ISC_LIST(struct query_info) query_list;

typedef struct query_info {
	isc_uint64_t timestamp;
	query_list *list;
	char *desc;
	sockinfo_t *sock;
	/*
	 * This link links the query into the list of outstanding
	 * queries or the list of available query IDs.
	 */
	ISC_LINK(struct query_info) link;
} query_info;

#define NQIDS 65536

typedef struct {
	query_info queries[NQIDS];
	query_list outstanding_queries;
	query_list unused_queries;

	pthread_t sender;
	pthread_t receiver;

	pthread_mutex_t lock;
	pthread_cond_t cond;

	unsigned int nsocks;
	int current_sock;
	sockinfo_t *socks;

	perf_dnsctx_t *dnsctx;

	isc_boolean_t done_sending;
	isc_uint64_t done_send_time;

	const config_t *config;
	const times_t *times;
	stats_t stats;

	isc_uint32_t max_outstanding;
	isc_uint32_t max_qps;

	isc_uint64_t last_recv;
} threadinfo_t;

static threadinfo_t *threads;

static pthread_mutex_t start_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t start_cond = PTHREAD_COND_INITIALIZER;
static isc_boolean_t started;

static isc_boolean_t interrupted = ISC_FALSE;

static int threadpipe[2];
static int mainpipe[2];
static int intrpipe[2];

static isc_mem_t *mctx;

static perf_datafile_t *input;

static SSL_CTX *ctx;

static void
handle_sigint(int sig)
{
	(void)sig;
	int unused = write(intrpipe[1], "", 1);
	(void)unused;
}

static void
print_initial_status(const config_t *config)
{
	time_t now;
	isc_netaddr_t addr;
	char buf[ISC_NETADDR_FORMATSIZE];
	int i;

	printf("[Status] Command line: %s",
	       isc_file_basename(config->argv[0]));
	for (i = 1; i < config->argc; i++)
		printf(" %s", config->argv[i]);
	printf("\n");

	isc_netaddr_fromsockaddr(&addr, &config->server_addr);
	isc_netaddr_format(&addr, buf, sizeof(buf));
	printf("[Status] Sending %s (to %s) over %s %s\n",
	       config->updates ? "updates" : "queries", buf, config->usetcp ? "TCP" : "UDP",
	       config->usetcptls ? "TLS" : "");

	now = time(NULL);
	printf("[Status] Started at: %s", ctime(&now));

	printf("[Status] Stopping after ");
	if (config->timelimit)
		printf("%u.%06u seconds",
		       (unsigned int)(config->timelimit / MILLION),
		       (unsigned int)(config->timelimit % MILLION));
	if (config->timelimit && config->maxruns)
		printf(" or ");
	if (config->maxruns)
		printf("%u run%s through file", config->maxruns,
		       config->maxruns == 1 ? "" : "s");
	printf("\n");
}

static void
print_final_status(const config_t *config)
{
	const char *reason;

	if (interrupted)
		reason = "interruption";
	else if (config->maxruns > 0 &&
		 perf_datafile_nruns(input) == config->maxruns)
		reason = "end of file";
	else
		reason = "time limit";

	printf("[Status] Testing complete (%s)\n", reason);
	printf("\n");
}

static double
stddev(isc_uint64_t sum_of_squares, isc_uint64_t sum, isc_uint64_t total)
{
	double squared;

	squared = (double)sum * (double)sum;
	return sqrt((sum_of_squares - (squared / total)) / (total - 1));
}

static void
print_statistics(const config_t *config, const times_t *times, stats_t *stats)
{
	const char *units;
	isc_uint64_t run_time;
	isc_boolean_t first_rcode;
	isc_uint64_t latency_avg;
	isc_uint64_t connection_time;
	unsigned int i;

	units = config->updates ? "Updates" : "Queries";

	run_time = times->end_time - times->start_time;

	printf("Statistics:\n\n");

	printf("  %s sent:         %" ISC_PRINT_QUADFORMAT "u\n",
               units, stats->num_sent);
	printf("  %s completed:    %" ISC_PRINT_QUADFORMAT "u (%.2lf%%)\n",
	       units, stats->num_completed,
	       SAFE_DIV(100.0 * stats->num_completed, stats->num_sent));
	printf("  %s lost:         %" ISC_PRINT_QUADFORMAT "u (%.2lf%%)\n",
	       units, stats->num_timedout,
	       SAFE_DIV(100.0 * stats->num_timedout, stats->num_sent));
	if (stats->num_interrupted > 0)
		printf("  %s interrupted:  %" ISC_PRINT_QUADFORMAT "u "
                       "(%.2lf%%)\n",
		       units, stats->num_interrupted,
		       SAFE_DIV(100.0 * stats->num_interrupted,
				stats->num_sent));
	printf("\n");

	printf("  Response codes:       ");
	first_rcode = ISC_TRUE;
	for (i = 0; i < 16; i++) {
		if (stats->rcodecounts[i] == 0)
			continue;
		if (first_rcode)
			first_rcode = ISC_FALSE;
		else
			printf(", ");
		printf("%s %" ISC_PRINT_QUADFORMAT "u (%.2lf%%)",
		       perf_dns_rcode_strings[i], stats->rcodecounts[i],
		       (stats->rcodecounts[i] * 100.0) / stats->num_completed);
	}
	printf("\n");

	printf("  Average packet size:  request %u, response %u\n",
	       (unsigned int)SAFE_DIV(stats->total_request_size,
				      stats->num_sent),
	       (unsigned int)SAFE_DIV(stats->total_response_size,
				      stats->num_completed));
	printf("  Run time (s):         %u.%06u\n",
	       (unsigned int)(run_time / MILLION),
	       (unsigned int)(run_time % MILLION));
	printf("  %s per second:   %.6lf\n\n", units,
	       SAFE_DIV(stats->num_completed, (((double)run_time) / MILLION)));
	if (stats->num_tcp_conns != 0) {
		printf("  TCP connections:      %u\n",
		        (unsigned int)stats->num_tcp_conns);
		printf("  Ave %s per conn: %i\n", units,
		       (int)round(SAFE_DIV((double)stats->num_completed,
		                (double)stats->num_tcp_conns)));
		printf("  TCP HS time (s) :     %u.%06u  (%.2lf%%)\n",
		          (unsigned int)(times->tcp_hs_time / MILLION),
		          (unsigned int)(times->tcp_hs_time % MILLION),
		          (100.0 * times->tcp_hs_time / run_time));
		printf("  TLS HS time (s) :     %u.%06u  (%.2lf%%)\n",
		          (unsigned int)(times->tls_hs_time / MILLION),
		          (unsigned int)(times->tls_hs_time % MILLION),
		          (100.0 * times->tls_hs_time / run_time));
		printf("  Total HS time (s) :   %u.%06u  (%.2lf%%)\n",
		          (unsigned int)((times->tcp_hs_time+times->tls_hs_time) / MILLION),
		          (unsigned int)((times->tcp_hs_time+times->tls_hs_time) % MILLION),
		          (100.0 * (times->tcp_hs_time+times->tls_hs_time) / run_time));
		connection_time = run_time - times->tcp_hs_time - times->tls_hs_time;
		printf("  Adjusted %s/s:   %.6lf\n\n", units,
		       SAFE_DIV(stats->num_completed, (((double)connection_time) / MILLION)));
	}
	printf("\n");

	latency_avg = SAFE_DIV(stats->latency_sum, stats->num_completed);
	printf("  Average RTT (s):      %u.%06u (min %u.%06u, max %u.%06u)\n",
	       (unsigned int)(latency_avg / MILLION),
	       (unsigned int)(latency_avg % MILLION),
	       (unsigned int)(stats->latency_min / MILLION),
	       (unsigned int)(stats->latency_min % MILLION),
	       (unsigned int)(stats->latency_max / MILLION),
	       (unsigned int)(stats->latency_max % MILLION));
	if (stats->num_completed > 1) {
		printf("  RTT StdDev (s):       %f\n",
		       stddev(stats->latency_sum_squares, stats->latency_sum,
			      stats->num_completed) / MILLION);
	}

	printf("\n");
}

static void
sum_stats(const config_t *config, stats_t *total)
{
	unsigned int i, j;

	memset(total, 0, sizeof(*total));

	for (i = 0; i < config->threads; i++) {
		stats_t *stats = &threads[i].stats;

		for (j = 0; j < 16; j++)
			total->rcodecounts[j] += stats->rcodecounts[j];

		total->num_sent += stats->num_sent;
		total->num_interrupted += stats->num_interrupted;
		total->num_timedout += stats->num_timedout;
		total->num_completed += stats->num_completed;
		total->num_tcp_conns += stats->num_tcp_conns;

		total->total_request_size += stats->total_request_size;
		total->total_response_size += stats->total_response_size;

		total->latency_sum += stats->latency_sum;
		total->latency_sum_squares += stats->latency_sum_squares;
		total->latency_min += stats->latency_min;
		total->latency_max += stats->latency_max;
	}
}

#define str(x) #x
#define stringify(x) str(x)

static void
setup(int argc, char **argv, config_t *config)
{
	const char *family = NULL;
	const char *server_name = DEFAULT_SERVER_NAME;
	in_port_t server_port = 0;
	const char *local_name = NULL;
	in_port_t local_port = DEFAULT_LOCAL_PORT;
	const char *filename = NULL;
	const char *tsigkey = NULL;
	isc_result_t result;

	result = isc_mem_create(0, 0, &mctx);
	if (result != ISC_R_SUCCESS)
		perf_log_fatal("creating memory context: %s",
			       isc_result_totext(result));

	dns_result_register();

	memset(config, 0, sizeof(*config));
	config->argc = argc;
	config->argv = argv;

	config->family = AF_UNSPEC;
	config->clients = 1;
	config->threads = 1;
	config->timeout = DEFAULT_TIMEOUT * MILLION;
	config->max_outstanding = DEFAULT_MAX_OUTSTANDING;

	perf_opt_add('f', perf_opt_string, "family",
		     "address family of DNS transport, inet or inet6", "any",
		     &family);
	perf_opt_add('s', perf_opt_string, "server_addr",
		     "the server to query", DEFAULT_SERVER_NAME, &server_name);
	perf_opt_add('p', perf_opt_port, "port",
		     "the port on which to query the server",
		     stringify(DEFAULT_SERVER_PORT) " or " stringify(DEFAULT_TLS_SERVER_PORT) " for TCP/TLS",
		     &server_port);
	perf_opt_add('a', perf_opt_string, "local_addr",
		     "the local address from which to send queries", NULL,
		     &local_name);
	perf_opt_add('x', perf_opt_port, "local_port",
		     "the local port from which to send queries",
		     stringify(DEFAULT_LOCAL_PORT), &local_port);
	perf_opt_add('d', perf_opt_string, "datafile",
		     "the input data file", "stdin", &filename);
	perf_opt_add('c', perf_opt_uint, "clients",
		     "the number of clients to act as", NULL,
		     &config->clients);
	perf_opt_add('T', perf_opt_uint, "threads",
		     "the number of threads to run", NULL,
		     &config->threads);
	perf_opt_add('n', perf_opt_uint, "maxruns",
		     "run through input at most N times", NULL,
		     &config->maxruns);
	perf_opt_add('l', perf_opt_timeval, "timelimit",
		     "run for at most this many seconds", NULL,
		     &config->timelimit);
	perf_opt_add('b', perf_opt_uint, "buffer_size",
		     "socket send/receive buffer size in kilobytes", NULL,
		     &config->bufsize);
	perf_opt_add('t', perf_opt_timeval, "timeout",
		     "the timeout for query completion in seconds",
		     stringify(DEFAULT_TIMEOUT), &config->timeout);
	perf_opt_add('e', perf_opt_boolean, NULL,
		     "enable EDNS 0", NULL, &config->edns);
	perf_opt_add('D', perf_opt_boolean, NULL,
		     "set the DNSSEC OK bit (implies EDNS)", NULL,
		     &config->dnssec);
	perf_opt_add('y', perf_opt_string, "[alg:]name:secret",
		     "the TSIG algorithm, name and secret", NULL,
		     &tsigkey);
	perf_opt_add('q', perf_opt_uint, "num_queries",
		     "the maximum number of queries outstanding",
		     stringify(DEFAULT_MAX_OUTSTANDING),
		     &config->max_outstanding);
	perf_opt_add('Q', perf_opt_uint, "max_qps",
		     "limit the number of queries per second", NULL,
		     &config->max_qps);
	perf_opt_add('S', perf_opt_timeval, "stats_interval",
		     "print qps statistics every N seconds",
		     NULL, &config->stats_interval);
	perf_opt_add('u', perf_opt_boolean, NULL,
		     "send dynamic updates instead of queries",
		     NULL, &config->updates);
	perf_opt_add('v', perf_opt_boolean, NULL,
		     "verbose: report each query to stdout",
		     NULL, &config->verbose);
	perf_opt_add('z', perf_opt_boolean, NULL,
		     "use TCP",
		     NULL, &config->usetcp);
	perf_opt_add('L', perf_opt_boolean, NULL,
		     "use TCP/TLS",
		     NULL, &config->usetcptls);
	perf_opt_add('g', perf_opt_boolean, NULL,
		     "debug: report debug level info to stdout",
		     NULL, &config->debug);
	perf_opt_add('Z', perf_opt_uint, "num_queries on tcp connection",
		     "max no. of queries to be sent on a single TCP connection",
		     stringify(0),
		     &config->max_tcp_q);	
	perf_opt_parse(argc, argv);

        if (server_port == 0)
            server_port = (config->usetcptls) ? DEFAULT_TLS_SERVER_PORT : DEFAULT_SERVER_PORT;
        
	if (family != NULL)
		config->family = perf_net_parsefamily(family);
	perf_net_parseserver(config->family, server_name, server_port,
			     &config->server_addr);
	perf_net_parselocal(isc_sockaddr_pf(&config->server_addr),
			    local_name, local_port, &config->local_addr);

	input = perf_datafile_open(mctx, filename);

	if (config->maxruns == 0 && config->timelimit == 0)
		config->maxruns = 1;
	perf_datafile_setmaxruns(input, config->maxruns);

	if (config->dnssec)
		config->edns = ISC_TRUE;

	if (config->usetcptls)
		config->usetcp = ISC_TRUE;

	if (tsigkey != NULL)
		config->tsigkey = perf_dns_parsetsigkey(tsigkey, mctx);

	/*
	 * If we run more threads than max-qps, some threads will have
	 * ->max_qps set to 0, and be unlimited.
	 */
	if (config->max_qps > 0 && config->threads > config->max_qps)
		config->threads = config->max_qps;

	/*
	 * We also can't run more threads than clients.
	 */
	if (config->threads > config->clients)
		config->threads = config->clients;
}

static void
cleanup(config_t *config)
{
	unsigned int i;

	perf_datafile_close(&input);
	for (i = 0; i < 2; i++) {
		close(threadpipe[i]);
		close(mainpipe[i]);
		close(intrpipe[i]);
	}
	if (config->tsigkey != NULL)
		perf_dns_destroytsigkey(&config->tsigkey);
	isc_mem_destroy(&mctx);
}

typedef enum {
	prepend_unused,
	append_unused,
	prepend_outstanding,
} query_move_op;

static inline void
query_move(threadinfo_t *tinfo, query_info *q, query_move_op op)
{
	ISC_LIST_UNLINK(*q->list, q, link);
	switch (op) {
	case prepend_unused:
		q->list = &tinfo->unused_queries;
		ISC_LIST_PREPEND(tinfo->unused_queries, q, link);
		break;
	case append_unused:
		q->list = &tinfo->unused_queries;
		ISC_LIST_APPEND(tinfo->unused_queries, q, link);
		break;
	case prepend_outstanding:
		q->list = &tinfo->outstanding_queries;
		ISC_LIST_PREPEND(tinfo->outstanding_queries, q, link);
		break;
	}
}

static inline isc_uint64_t
num_outstanding(const stats_t *stats)
{
	return stats->num_sent - stats->num_completed - stats->num_timedout;
}

static void
wait_for_start(void)
{
	LOCK(&start_lock);
	while (!started)
		WAIT(&start_cond, &start_lock);
	UNLOCK(&start_lock);
}

static int send_msg(threadinfo_t *tinfo, sockinfo_t *s, isc_buffer_t *msg)
{
	assert(s->send_state == SOCKET_SEND_SENDING ||
	       s->send_state == SOCKET_SEND_READY);
	
	int error = 0, res, ssl_err;
	unsigned int length = isc_buffer_usedlength(msg);
	if (tinfo->config->usetcptls) {
		LOCK(&tinfo->lock);
		res = SSL_write(s->ssl, isc_buffer_base(msg), length);
		ssl_err = SSL_get_error(s->ssl, res);
		UNLOCK(&tinfo->lock);
		if (res <= 0) {
			if (ssl_err == SSL_ERROR_WANT_READ || ssl_err == SSL_ERROR_WANT_WRITE)
				error = EAGAIN;
			else
				error = ssl_err;
		}
	} else {
		res = sendto(s->fd,
			     isc_buffer_base(msg), length,
			     0,
			     &tinfo->config->server_addr.type.sa,
			     tinfo->config->server_addr.length);
		if (res < 0) {
			if (errno == EWOULDBLOCK || errno == EINPROGRESS || errno == EAGAIN)
				error = EAGAIN;
			else
				error = errno;
			if (error != EAGAIN)
				perf_log_warning("failed to send packet: %d (%s)",
						 errno, strerror(errno));
		} else if ((unsigned int) res != length)
			perf_log_fatal("unexpected short write: %d", errno);
	}
	
	if (tinfo->config->usetcp && error == 0) {
		LOCK(&tinfo->lock);
		s->num_sent++;
		s->num_in_flight++;
		if (tinfo->config->max_tcp_q != 0 &&  /* A limit is set */
		    s->num_sent == tinfo->config->max_tcp_q) {
			s->send_state = SOCKET_SEND_TCP_SENT_MAX;
		}
		if (s->send_state == SOCKET_SEND_SENDING) {
			s->send_state = SOCKET_SEND_READY;
		}
		UNLOCK(&tinfo->lock);
	}
	return error;
}

static isc_boolean_t
find_sending_tcp_connection(int *socknum, threadinfo_t *tinfo) 
{
	int i, error;
	for (i = 0; i < tinfo->nsocks; i++, (*socknum)++) {
		if (*socknum == tinfo->nsocks)
			(*socknum) = 0;
		sockinfo_t *sock = &tinfo->socks[*socknum];
		
		if (sock->send_state == SOCKET_SEND_CLOSED ||
		    sock->send_state == SOCKET_SEND_TCP_SENT_MAX)
			continue;
		if (sock->send_state == SOCKET_SEND_SENDING) {
			error = send_msg(tinfo, sock, &sock->sending);
			if (error != 0) {
				if (error == EAGAIN)
					continue;
				else {
					/* TODO: Need to reset the connection again */
					perf_log_warning("Error: cannot use connection %i, fd %d: %s", 
							 *socknum, sock->fd, strerror(error));
					continue;
				}
			} else {
				LOCK(&tinfo->lock);
				sock->send_state = SOCKET_SEND_READY;
				UNLOCK(&tinfo->lock);
			}
		}
		if (sock->send_state == SOCKET_SEND_TCP_HANDSHAKE) {
			if (perf_os_waituntilwriteable(sock->fd, 0)
			                                  != ISC_R_SUCCESS)
				continue;
			LOCK(&tinfo->lock);
			if (tinfo->config->usetcptls)
				sock->send_state = SOCKET_SEND_TLS_HANDSHAKE;
			else {
				sock->send_state = SOCKET_SEND_READY;
				sock->recv_state = SOCKET_RECV_READY;
				if (tinfo->config->debug) {
					isc_uint64_t now = get_time();
					perf_log_printf("[DEBUG] Ready on sock %d in thread %p at %" PRIu64 "(usec)", sock->socket_number, tinfo, now);
				}
			}
			sock->tcp_hs_done_time = get_time();
			UNLOCK(&tinfo->lock);
			if (tinfo->config->debug) {
				isc_uint64_t now = get_time();
				perf_log_printf("[DEBUG] TCP HS done on sock %d in thread %p at %" PRIu64 "(usec)", sock->socket_number, tinfo, now);
			}
		}
		if (sock->send_state == SOCKET_SEND_TLS_HANDSHAKE) {
			LOCK(&tinfo->lock);
			int res = SSL_connect(sock->ssl);
			error = SSL_get_error(sock->ssl, res);
			UNLOCK(&tinfo->lock);
			switch(res) {
			case 0:
				/* TODO: Need to reset the connection again */
				perf_log_fatal("SSL_connect failed (controlled fail): %d", error);
				break;

			case 1:
				LOCK(&tinfo->lock);
				sock->send_state = SOCKET_SEND_READY;
				sock->recv_state = SOCKET_RECV_READY;
				sock->tls_hs_done_time = get_time();
				UNLOCK(&tinfo->lock);
				if (tinfo->config->debug) {
					isc_uint64_t now = get_time();
					perf_log_printf("[DEBUG] TLS HS done on sock %d in thread %p at %" PRIu64 "(usec)", sock->socket_number, tinfo, now);
				}
				break;

			default:
				if (error == SSL_ERROR_WANT_READ ||
				    error == SSL_ERROR_WANT_WRITE)
					continue;
				perf_log_fatal("SSL_connect fail (fatal, not clean): %d", error);
				break;
			}
		}
		return ISC_TRUE;
	}
	return ISC_FALSE;
}

static void *
do_send(void *arg)
{
	threadinfo_t *tinfo;
	const config_t *config;
	const times_t *times;
	stats_t *stats;
	isc_buffer_t msg;
	isc_uint64_t now, run_time, req_time;
	char input_data[MAX_INPUT_DATA];
	isc_buffer_t lines;
	isc_region_t used;
	query_info *q;
	int qid;
	unsigned char packet_buffer[MAX_EDNS_PACKET];
	unsigned int length;
	int error;
	isc_result_t result;
	int socknum;
	
	tinfo = (threadinfo_t *) arg;
	config = tinfo->config;
	times = tinfo->times;
	stats = &tinfo->stats;
	isc_buffer_init(&msg, packet_buffer, sizeof(packet_buffer));
	isc_buffer_init(&lines, input_data, sizeof(input_data));

	wait_for_start();
	now = get_time();
	while (!interrupted && now < times->stop_time) {
		/* Avoid flooding the network too quickly. */
		if (stats->num_sent < tinfo->max_outstanding &&
		    stats->num_sent % 2 == 1)
		{
			if (stats->num_completed == 0)
				usleep(1000);
			else
				sleep(0);
			now = get_time();
		}

		/* Rate limiting */
		if (tinfo->max_qps > 0) {
			run_time = now - times->start_time;
			req_time = (MILLION * stats->num_sent) /
				   tinfo->max_qps;
			if (req_time > run_time) {
				usleep(req_time - run_time);
				now = get_time();
				continue;
			}
		}

		LOCK(&tinfo->lock);

		/* Limit in-flight queries */
		if (num_outstanding(stats) >= tinfo->max_outstanding) {
			TIMEDWAIT(&tinfo->cond, &tinfo->lock,
				  &times->stop_time_ns, NULL);
			UNLOCK(&tinfo->lock);
			now = get_time();
			continue;
		}

		UNLOCK(&tinfo->lock);

		socknum = tinfo->current_sock++ % tinfo->nsocks;
		// if (tinfo->config->debug)
		// 	perf_log_printf("[DEBUG] do_send: socknum = %d, tinfo->current_sock++ = %d",
		// 	                socknum, tinfo->current_sock++);
		if (tinfo->config->usetcp && 
		    !find_sending_tcp_connection(&socknum, tinfo)) {
			// if (tinfo->config->debug)
			// 	perf_log_printf("[DEBUG] No working TCP connection");
			now = get_time();
			continue;
		}

		LOCK(&tinfo->lock);

		q = ISC_LIST_HEAD(tinfo->unused_queries);
		query_move(tinfo, q, prepend_outstanding);
		q->timestamp = ISC_UINT64_MAX;
		q->sock = &tinfo->socks[socknum];

		UNLOCK(&tinfo->lock);

		isc_buffer_clear(&lines);
		result = perf_datafile_next(input, &lines, config->updates);
		if (result != ISC_R_SUCCESS) {
			if (result == ISC_R_INVALIDFILE)
				perf_log_fatal("input file contains no data");
			break;
		}

		qid = q - tinfo->queries;
		isc_buffer_usedregion(&lines, &used);
		isc_buffer_clear(&msg);
		result = perf_dns_buildrequest(tinfo->dnsctx,
					       (isc_textregion_t *) &used,
					       qid, config->edns,
					       config->dnssec, config->tsigkey,
					       &msg);
		if (result != ISC_R_SUCCESS) {
			LOCK(&tinfo->lock);
			query_move(tinfo, q, prepend_unused);
			UNLOCK(&tinfo->lock);
			now = get_time();
			continue;
		}

		if (tinfo->config->usetcp){
			/* TODO: Better to use writev for TCP instead
			   tcp needs two bytes for dns payload length */
			memmove(msg.base + 2, msg.base, msg.used);
			uint8_t * my_pointer = (uint8_t *) msg.base;
			my_pointer[0] = 0x00;
			my_pointer[1] = msg.used;
			msg.used += 2;
		}

		length = isc_buffer_usedlength(&msg);

		now = get_time();
		if (config->verbose) {
			q->desc = strdup(lines.base);
			if (q->desc == NULL)
				perf_log_fatal("out of memory");
		}
		q->timestamp = now;
		error = send_msg(tinfo, q->sock, &msg);

		if (error != 0) {
			if (error == EAGAIN) {
				isc_region_t region;
				LOCK(&tinfo->lock);
				isc_buffer_usedregion(&msg, &region);
				isc_buffer_clear(&q->sock->sending);
				isc_buffer_copyregion(&q->sock->sending, &region);
				q->sock->send_state = SOCKET_SEND_SENDING;
				UNLOCK(&tinfo->lock);
			} else {
				LOCK(&tinfo->lock);
				query_move(tinfo, q, prepend_unused);
				UNLOCK(&tinfo->lock);
				continue;
			}
		}
		stats->num_sent++;
		stats->total_request_size += length;
	}
	tinfo->done_send_time = get_time();
	tinfo->done_sending = ISC_TRUE;
	int unused = write(mainpipe[1], "", 1);
	(void)unused;
	return NULL;
}

static void
process_timeouts(threadinfo_t *tinfo, isc_uint64_t now)
{
	struct query_info *q;
	const config_t *config;

	config = tinfo->config;

	/* Avoid locking unless we need to. */
	q = ISC_LIST_TAIL(tinfo->outstanding_queries);
	if (q == NULL || q->timestamp > now ||
	    now - q->timestamp < config->timeout)
		return;

	LOCK(&tinfo->lock);

	do {
		query_move(tinfo, q, append_unused);
		q->sock->num_in_flight--;

		tinfo->stats.num_timedout++;

		if (q->desc != NULL) {
			perf_log_printf("> T %s", q->desc);
		} else {
			perf_log_printf("[Timeout] %s timed out: msg id %u",
					config->updates ? "Update" : "Query",
					(unsigned int)(q - tinfo->queries));
		}
		q = ISC_LIST_TAIL(tinfo->outstanding_queries);
	} while (q != NULL && q->timestamp < now &&
		 now - q->timestamp >= config->timeout);

	UNLOCK(&tinfo->lock);
}

int count_pending(threadinfo_t *tinfo, sockinfo_t *s)
{
	if (tinfo->config->usetcptls) {
		int res;
		LOCK(&tinfo->lock);
		SSL_read(s->ssl, NULL, 0);
		res = SSL_pending(s->ssl);
		UNLOCK(&tinfo->lock);
		return res;
	} else {
		int avbytes = 0;
		if (ioctl(s->fd, FIONREAD, &avbytes) == -1)
			return errno;
		return avbytes;
	}
}

int recv_buf(threadinfo_t *tinfo, sockinfo_t *s, unsigned char *buf, unsigned int buflen)
{
	int bytes_read, error;
	if (tinfo->config->usetcptls) {
		LOCK(&tinfo->lock);
		bytes_read = SSL_read(s->ssl, buf, buflen);
		error = SSL_get_error(s->ssl, bytes_read);
		UNLOCK(&tinfo->lock);
		if (bytes_read <= 0)
			return error;
		else
			return bytes_read;
	} else {
		bytes_read = recv(s->fd, buf, buflen, 0);
		if (bytes_read == -1)
			return errno;
		else
			return bytes_read;
	}
}

typedef struct {
	sockinfo_t *sock;
	isc_uint16_t qid;
	isc_uint16_t rcode;
	unsigned int size;
	isc_uint64_t when;
	isc_uint64_t sent;
	isc_boolean_t unexpected;
	isc_boolean_t short_response;
	char *desc;
} received_query_t;

static isc_boolean_t
recv_one(threadinfo_t *tinfo, int which_sock,
	 unsigned char *packet_buffer, unsigned int packet_size,
	 received_query_t *recvd, int *saved_errnop)
{
	isc_uint16_t *packet_header;
	sockinfo_t *s;
	uint8_t tcplength[2];
	int bytes_read = 0;
	int pending = 0;

	packet_header = (isc_uint16_t *) packet_buffer;

	s = &tinfo->socks[which_sock];

	assert(s->recv_state == SOCKET_RECV_READY ||
	       s->recv_state == SOCKET_RECV_READING);

	if (!tinfo->config->usetcp) {
		bytes_read = recv(s->fd, packet_buffer, packet_size, 0);
	} else {
		if (s->recv_state != SOCKET_RECV_READING) {
			pending = count_pending(tinfo, s);
			if (pending < 0) {
				*saved_errnop = errno;
				return ISC_FALSE;
			}
			if (pending >= 2) {
				bytes_read = recv_buf(tinfo, s, tcplength, 2);
				if (bytes_read != 2) {
					perf_log_warning("bad read length");
					*saved_errnop = EBADMSG; /* return bad message */
					return ISC_FALSE;
				}
				LOCK(&tinfo->lock);
				s->tcp_to_read = ((uint16_t)tcplength[0] << 8) | tcplength[1];
				s->recv_state = SOCKET_RECV_READING;
				UNLOCK(&tinfo->lock);
			} else {
				*saved_errnop = EAGAIN;
				return ISC_FALSE;
			}
		}
		/* Now in SOCKET_RECV_READING */
		pending = count_pending(tinfo, s);
		if (pending < 0) {
			*saved_errnop = errno;
			return ISC_FALSE;
		}
		if (pending >= s->tcp_to_read) {
			bytes_read = recv_buf(tinfo, s, packet_buffer, s->tcp_to_read);
			if (bytes_read != s->tcp_to_read) {
				perf_log_warning("bad read length");
				*saved_errnop = EBADMSG; /* return bad message */
				return ISC_FALSE;
			}
			LOCK(&tinfo->lock);
			s->recv_state = SOCKET_RECV_READY;
			UNLOCK(&tinfo->lock);
		} else {
			*saved_errnop = EAGAIN;
			return ISC_FALSE;
		}
	}
	
	if (bytes_read < 0) {
		*saved_errnop = errno;
		return ISC_FALSE;
	}
	s->num_recv++;
	
	recvd->sock = s;
	recvd->qid = ntohs(packet_header[0]);
	recvd->rcode = ntohs(packet_header[1]) & 0xF;
	recvd->size = bytes_read;
	recvd->when = get_time();
	recvd->sent = 0;
	recvd->unexpected = ISC_FALSE;
	recvd->short_response = ISC_TF(bytes_read < 4);
	recvd->desc = NULL;
	return ISC_TRUE;
}

static inline void
bit_set(unsigned char *bits, unsigned int bit)
{
	unsigned int shift, mask;

	shift = 7 - (bit % 8);
	mask = 1 << shift;

	bits[bit / 8] |= mask;
}

static inline isc_boolean_t
bit_check(unsigned char *bits, unsigned int bit)
{
	unsigned int shift;

	shift = 7 - (bit % 8);

	if ((bits[bit / 8] >> shift) & 0x01)
		return ISC_TRUE;
	return ISC_FALSE;
}

static void
close_socket(threadinfo_t *tinfo, sockinfo_t *sock)
{
	if (tinfo->config->debug) {
		isc_uint64_t now = get_time();
		perf_log_printf("[DEBUG] Shutting down sock %d in thread %p at %" PRIu64 "(usec)", sock->socket_number, tinfo, now);
	}
	LOCK(&tinfo->lock);
	if (sock->ssl != NULL) {
		SSL_shutdown(sock->ssl);
		SSL_free(sock->ssl);
		sock->ssl = NULL;
	}
	close(sock->fd);
	sock->fd = -1;
	sock->send_state = SOCKET_SEND_CLOSED;
	sock->recv_state = SOCKET_RECV_CLOSED;

	if (tinfo->config->usetcp) {
		sock->cumulative_tcp_hs_time += sock->tcp_hs_done_time - sock->con_start_time;
		if (tinfo->config->usetcptls)
			sock->cumulative_tls_hs_time += sock->tls_hs_done_time - sock->tcp_hs_done_time;
	}
	UNLOCK(&tinfo->lock);
}

static isc_boolean_t
open_socket(threadinfo_t *tinfo, sockinfo_t *sock, isc_boolean_t reopen)
{
	int sock_type = (tinfo->config->usetcp) ? SOCK_STREAM : SOCK_DGRAM;
	/*
	 * If re-opening, we can't easily re-open the same port because
	 * it's probably in TIME_WAIT, so for now disregard the -x option.
	 */
	int fd = perf_net_opensocket(&tinfo->config->server_addr,
				     &tinfo->config->local_addr,
				     (reopen) ? UINT_MAX : sock->port_offset,
				     tinfo->config->bufsize,
				     sock_type,
				     tinfo->config->debug);
	
	LOCK(&tinfo->lock);
	sock->fd = fd;
	sock->num_in_flight = 0;

	if (tinfo->config->usetcp) {
		tinfo->stats.num_tcp_conns++;
		sock->num_sent = 0;
		sock->num_recv = 0;
		isc_buffer_init(&sock->sending, sock->sending_buffer, sizeof(sock->sending_buffer));
		sock->tcp_hs_done_time = 0;
		sock->tls_hs_done_time = 0;
		sock->con_start_time = get_time();
	}
	
	if (fd != -1 ) {
		if (tinfo->config->usetcp) {
			if (tinfo->config->usetcptls) {
				sock->ssl = SSL_new(ctx);
				if (sock->ssl == NULL)
					perf_log_fatal("creating SSL object: %s",
						       ERR_reason_error_string(ERR_get_error()));
				SSL_set_fd(sock->ssl, fd);
			}
			sock->send_state = SOCKET_SEND_TCP_HANDSHAKE;
			sock->recv_state = SOCKET_RECV_HANDSHAKE;
		} else {
			sock->send_state = SOCKET_SEND_READY;
			sock->recv_state = SOCKET_RECV_READY;
		}
	} else {
		sock->send_state = SOCKET_SEND_CLOSED;
		sock->recv_state = SOCKET_RECV_CLOSED;
	}
	UNLOCK(&tinfo->lock);
	
	if (tinfo->config->debug) 
		perf_log_printf("[DEBUG] %sInitializing: Opened socket %d in thread %p at %" PRIu64 "(usec) at socket offset %d with fd %d",
				(reopen) ? "Re-" : "",
				sock->socket_number,
				tinfo,
				get_time(),
				sock->port_offset,
				sock->fd);

	return (fd != -1) ? ISC_TRUE : ISC_FALSE;
}

static isc_boolean_t
check_tcp_connection(threadinfo_t *tinfo, stats_t *stats, unsigned int socket)
{
	sockinfo_t *sock;
	sock = &tinfo->socks[socket];
	if (sock->recv_state == SOCKET_RECV_CLOSED ||
	    sock->recv_state == SOCKET_RECV_HANDSHAKE)
		return ISC_FALSE;
	if (sock->send_state == SOCKET_SEND_TCP_SENT_MAX &&
	    sock->num_in_flight == 0) {
		close_socket(tinfo, sock);
		open_socket(tinfo, sock, ISC_TRUE);
		return ISC_FALSE; /* Handshake needs to happen. */
	}
	return ISC_TRUE;
}

static void *
do_recv(void *arg)
{
	threadinfo_t *tinfo;
	stats_t *stats;
	unsigned char packet_buffer[MAX_EDNS_PACKET];
	received_query_t recvd[RECV_BATCH_SIZE];
	unsigned int nrecvd;
	int saved_errno;
	unsigned char socketbits[MAX_SOCKETS / 8];
	isc_uint64_t now, latency;
	query_info *q;
	unsigned int current_socket, last_socket;
	unsigned int i, j;

	tinfo = (threadinfo_t *) arg;
	stats = &tinfo->stats;

	wait_for_start();
	now = get_time();
	last_socket = 0;
	while (!interrupted) {
		process_timeouts(tinfo, now);

		/*
		 * If we're done sending and either all responses have been
		 * received, stop.
		 */
		if (tinfo->done_sending && num_outstanding(stats) == 0)
			break;

		/*
		 * Try to receive a few packets, so that we can process them
		 * atomically.
		 */
		saved_errno = 0;
		memset(socketbits, 0, sizeof(socketbits));
		for (i = 0; i < RECV_BATCH_SIZE; i++) {
			for (j = 0; j < tinfo->nsocks; j++) {
				current_socket = (j + last_socket) %
					tinfo->nsocks;
				if (tinfo->config->usetcp &&
					!check_tcp_connection(tinfo, stats, current_socket))
					continue;
				if (bit_check(socketbits, current_socket))
					continue;
				if (recv_one(tinfo, current_socket,
					     packet_buffer,
					     sizeof(packet_buffer),
					      &recvd[i], &saved_errno))
				{
					last_socket = (current_socket + 1);
					break;
				}
				bit_set(socketbits, current_socket);
				if (saved_errno != EAGAIN)
					break;
			}
			if (j == tinfo->nsocks)
				break;
		}
		nrecvd = i;

		/* Do all of the processing that requires the lock */
		LOCK(&tinfo->lock);
		for (i = 0; i < nrecvd; i++) {
			if (recvd[i].short_response)
				continue;

			q = &tinfo->queries[recvd[i].qid];
			if (q->list != &tinfo->outstanding_queries ||
			    q->timestamp == ISC_UINT64_MAX ||
			    q->sock != recvd[i].sock)
			{
				recvd[i].unexpected = ISC_TRUE;
				continue;
			}
			query_move(tinfo, q, append_unused);
			recvd[i].sent = q->timestamp;
			recvd[i].desc = q->desc;
			q->desc = NULL;
			q->sock->num_in_flight--;
		}
		SIGNAL(&tinfo->cond);
		UNLOCK(&tinfo->lock);

		/* Now do the rest of the processing unlocked */
		for (i = 0; i < nrecvd; i++) {
			if (recvd[i].short_response) {
				perf_log_warning("received short response");
				continue;
			}
			if (recvd[i].unexpected) {
				perf_log_warning("received a response with an "
						 "unexpected (maybe timed out) "
						 "id: %u", recvd[i].qid);
				continue;
			}
			latency = recvd[i].when - recvd[i].sent;
			if (recvd[i].desc != NULL) {
				perf_log_printf(
					"> %s %s %u.%06u",
					perf_dns_rcode_strings[recvd[i].rcode],
					recvd[i].desc,
					(unsigned int)(latency / MILLION),
					(unsigned int)(latency % MILLION));
				free(recvd[i].desc);
			}

			stats->num_completed++;
			stats->total_response_size += recvd[i].size;
			stats->rcodecounts[recvd[i].rcode]++;
			stats->latency_sum += latency;
			stats->latency_sum_squares += (latency * latency);
			if (latency < stats->latency_min ||
			    stats->num_completed == 1)
				stats->latency_min = latency;
			if (latency > stats->latency_max)
				stats->latency_max = latency;
		}

		if (nrecvd > 0)
			tinfo->last_recv = recvd[nrecvd - 1].when;

		/*
		 * If there was an error, handle it (by either ignoring it,
		 * blocking, or exiting).
		 */
		if (nrecvd < RECV_BATCH_SIZE) {
			if (saved_errno == EINTR) {
				continue;
			} else if (saved_errno == 0 || saved_errno == EAGAIN) {
				int fds[MAX_SOCKETS];
				for (int i = 0; i < tinfo->nsocks; ++i)
					fds[i] = tinfo->socks[i].fd;
				perf_os_waituntilanyreadable(
					fds,
					tinfo->nsocks,
					threadpipe[0],
					TIMEOUT_CHECK_TIME);
				now = get_time();
				continue;
			} else {
				perf_log_fatal("failed to receive packet: %d (%s)",
					       saved_errno, strerror(saved_errno));
			}
		}
	}

	return NULL;
}

static void *
do_interval_stats(void *arg)
{
	threadinfo_t *tinfo;
	stats_t total;
	isc_uint64_t now;
	isc_uint64_t last_interval_time;
	isc_uint64_t last_completed;
	isc_uint64_t interval_time;
	isc_uint64_t num_completed;
	double qps;

	tinfo = arg;
	last_interval_time = tinfo->times->start_time;
	last_completed = 0;

	wait_for_start();
	while (perf_os_waituntilreadable(threadpipe[0], threadpipe[0],
					 tinfo->config->stats_interval) ==
	       ISC_R_TIMEDOUT)
	{
		now = get_time();
		sum_stats(tinfo->config, &total);
		interval_time = now - last_interval_time;
		num_completed = total.num_completed - last_completed;
		qps = num_completed / (((double)interval_time) / MILLION);
		perf_log_printf("%u.%06u: %.6lf",
				(unsigned int)(now / MILLION),
				(unsigned int)(now % MILLION), qps);
		last_interval_time = now;
		last_completed = total.num_completed;
	}

	return NULL;
}

static void
cancel_queries(threadinfo_t *tinfo)
{
	struct query_info *q;

	while (ISC_TRUE) {
		q = ISC_LIST_TAIL(tinfo->outstanding_queries);
		if (q == NULL)
			break;
		query_move(tinfo, q, append_unused);

		if (q->timestamp == ISC_UINT64_MAX)
			continue;

		tinfo->stats.num_interrupted++;
		if (q->desc != NULL) {
			perf_log_printf("> I %s", q->desc);
			free(q->desc);
			q->desc = NULL;
		}
	}
}

static isc_uint32_t
per_thread(isc_uint32_t total, isc_uint32_t nthreads, unsigned int offset)
{
	isc_uint32_t value;

	value = total / nthreads;
	if (value % nthreads > offset)
		value++;
	return value;
}

static void
threadinfo_init(threadinfo_t *tinfo, const config_t *config,
		const times_t *times)
{
	unsigned int offset, i;

	memset(tinfo, 0, sizeof(*tinfo));
	MUTEX_INIT(&tinfo->lock);
	COND_INIT(&tinfo->cond);

	ISC_LIST_INIT(tinfo->outstanding_queries);
	ISC_LIST_INIT(tinfo->unused_queries);
	for (i = 0; i < NQIDS; i++) {
		ISC_LINK_INIT(&tinfo->queries[i], link);
		ISC_LIST_APPEND(tinfo->unused_queries,
				&tinfo->queries[i], link);
		tinfo->queries[i].list = &tinfo->unused_queries;
	}

	offset = tinfo - threads;

	tinfo->dnsctx = perf_dns_createctx(config->updates);

	tinfo->config = config;
	tinfo->times = times;

	/*
	 * Compute per-thread limits based on global values.
	 */
	tinfo->max_outstanding = per_thread(config->max_outstanding,
					    config->threads, offset);
	tinfo->max_qps = per_thread(config->max_qps, config->threads, offset);
	tinfo->nsocks = per_thread(config->clients, config->threads, offset);

	/*
	 * We can't have more than 64k outstanding queries per thread.
	 */
	if (tinfo->max_outstanding > NQIDS)
		tinfo->max_outstanding = NQIDS;

	if (tinfo->nsocks > MAX_SOCKETS)
		tinfo->nsocks = MAX_SOCKETS;

	if (tinfo->config->debug)
		perf_log_printf("[DEBUG] Per-thread values: max_out = %d, max_qps = %d, max_nsocks = %d (Offset = %d)",
		                 tinfo->max_outstanding, tinfo->max_qps, tinfo->nsocks, offset);

	tinfo->socks = isc_mem_get(mctx, tinfo->nsocks * sizeof(sockinfo_t));
	if (tinfo->socks == NULL)
		perf_log_fatal("out of memory");

	int port_offset = 0;
	for (i = 0; i < offset; i++)
		port_offset += threads[i].nsocks;
	for (i = 0; i < tinfo->nsocks; i++) {
		sockinfo_t *sock = &tinfo->socks[i];
		memset(sock, 0, sizeof(sockinfo_t));
		
		sock->socket_number = i;
		sock->port_offset = port_offset++;

		open_socket(tinfo, sock, ISC_FALSE);
	}
	tinfo->current_sock = 0;

	THREAD(&tinfo->receiver, do_recv, tinfo);
	THREAD(&tinfo->sender, do_send, tinfo);
}

static void
threadinfo_stop(threadinfo_t *tinfo)
{
	SIGNAL(&tinfo->cond);
	JOIN(tinfo->sender, NULL);
	JOIN(tinfo->receiver, NULL);
}

static void
threadinfo_cleanup(threadinfo_t *tinfo, times_t *times)
{
	unsigned int i;

	if (interrupted)
		cancel_queries(tinfo);
	for (i = 0; i < tinfo->nsocks; i++) {
		close(tinfo->socks[i].fd);
		times->tcp_hs_time += tinfo->socks[i].cumulative_tcp_hs_time;
		times->tls_hs_time += tinfo->socks[i].cumulative_tls_hs_time;
	}
	isc_mem_put(mctx, tinfo->socks, tinfo->nsocks * sizeof(sockinfo_t));
	perf_dns_destroyctx(&tinfo->dnsctx);
	if (tinfo->last_recv > times->end_time)
		times->end_time = tinfo->last_recv;
}

int
main(int argc, char **argv)
{
	config_t config;
	times_t times;
	stats_t total_stats;
	threadinfo_t stats_thread;
	unsigned int i;
	isc_result_t result;
	const SSL_METHOD *method;

	printf("DNS Performance Testing Tool\n"
	       "Nominum Version " VERSION "\n\n");

	SSL_library_init();
	OpenSSL_add_all_algorithms();
	SSL_load_error_strings();
#if OPENSSL_VERSION_NUMBER < 0x10100000L
	method = SSLv23_client_method();
#else
	method = TLS_client_method();
#endif
	ctx = SSL_CTX_new(method);
	if (ctx == NULL)
		perf_log_fatal("creating SSL context: %s",
			       ERR_reason_error_string(ERR_get_error()));

	setup(argc, argv, &config);

	if (pipe(threadpipe) < 0 || pipe(mainpipe) < 0 ||
	    pipe(intrpipe) < 0)
		perf_log_fatal("creating pipe");

	perf_datafile_setpipefd(input, threadpipe[0]);

	perf_os_blocksignal(SIGINT, ISC_TRUE);

	print_initial_status(&config);

	threads = isc_mem_get(mctx, config.threads * sizeof(threadinfo_t));
	if (threads == NULL)
		perf_log_fatal("out of memory");
	/* TCP Handshakes start in threadinfo_init*/
	times.start_time = get_time();
	times.tcp_hs_time = 0;
	times.tls_hs_time = 0;
	for (i = 0; i < config.threads; i++)
		threadinfo_init(&threads[i], &config, &times);
	if (config.stats_interval > 0) {
		stats_thread.config = &config;
		stats_thread.times = &times;
		THREAD(&stats_thread.sender, do_interval_stats, &stats_thread);
	}

	if (config.timelimit > 0)
		times.stop_time = times.start_time + config.timelimit;
	else
		times.stop_time = ISC_UINT64_MAX;
	times.stop_time_ns.tv_sec = times.stop_time / MILLION;
	times.stop_time_ns.tv_nsec = (times.stop_time % MILLION) * 1000;

	LOCK(&start_lock);
	started = ISC_TRUE;
	BROADCAST(&start_cond);
	UNLOCK(&start_lock);

	perf_os_handlesignal(SIGINT, handle_sigint);
	perf_os_blocksignal(SIGINT, ISC_FALSE);
	result = perf_os_waituntilreadable(mainpipe[0], intrpipe[0],
					   times.stop_time - times.start_time);
	if (result == ISC_R_CANCELED)
		interrupted = ISC_TRUE;

	times.end_time = get_time();

	int unused = write(threadpipe[1], "", 1);
	(void)unused;
	for (i = 0; i < config.threads; i++)
	    threadinfo_stop(&threads[i]);
	if (config.stats_interval > 0)
	    JOIN(stats_thread.sender, NULL);

	for (i = 0; i < config.threads; i++)
		threadinfo_cleanup(&threads[i], &times);

	print_final_status(&config);

	sum_stats(&config, &total_stats);
	print_statistics(&config, &times, &total_stats);

	isc_mem_put(mctx, threads, config.threads * sizeof(threadinfo_t));
	cleanup(&config);

	return (0);
}
