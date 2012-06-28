#include <stdlib.h>
#include <stdio.h>
#include <string.h>	/* strerror(3) */
#include <errno.h>	/* errno(3) */
#include <getopt.h>	/* getopt_long(3) */
#include <string.h>	/* strtok(3) */
#include <ctype.h>	/* isprint(3) */
#include <unistd.h>	/* close(2), alarm(2), usleep(3) */
#include <error.h>	/* error(3) */
#include <signal.h>	/* signal(2) */
#include <netdb.h>	/* getaddrinfo(3) */
#include <sys/socket.h>	/* socket(7) */
#include <fcntl.h>	/* fcntl(2) */
#include <sys/select.h>	/* select(2) */
#include <sys/time.h>	/* gettimeofday(2) */
#include <time.h>	/* strftime(3), localtime(3) */

#ifndef DEBUG
#define DEBUG 0
#endif
#define dp(fmt, ...) \
	do { if (DEBUG || opt_verbose) fprintf(stderr, "%s:%d:%s(): " fmt, __FILE__, \
			__LINE__, __func__, ## __VA_ARGS__); } while (0)

#define MAXHOSTBUFSIZE 256

char *opt_dstaddr = NULL;
char *opt_dstport = NULL;
unsigned long long int opt_delay = 500000;
char *opt_srcaddr = NULL;
unsigned long long int opt_timeout = 500000;
unsigned long long int opt_runtime = 0;
unsigned long long int opt_count = 0;
int opt_verbose = 0;

unsigned long long int stats_tries;

void usage(char *prog)
{
	fprintf(stderr, "%s: IPv4/6 connection tester\n", prog);
	fprintf(stderr, "Usage: %s [OPTION]... destination\n", prog);
	fprintf(stderr, "Options:\n");
	fprintf(stderr, "  -p, --port=PORT     Port number or name to connect to. Default: 80\n");
	fprintf(stderr, "  -d, --delay=NUM     Delay between connections in us. Default: 500000\n");
	fprintf(stderr, "  -t, --timeout=SEC   Connection timeout in us. Default: 500000\n");
	fprintf(stderr, "  -T, --runtime=SEC   Running time in seconds. Default: 0=inf\n");
	fprintf(stderr, "  -c, --count=NUM     Connection count. Default: 0=inf\n");
	fprintf(stderr, "  -v, --verbose       Verbose operation. Default: not enabled\n");
	fprintf(stderr, "\n");
	return;
}

int parse_options(int argc, char **argv)
{
	int c;
	static struct option long_options[] = {
		{"port",	1, 0, 'p'},
		{"delay",	1, 0, 'd'},
		{"timeout",	1, 0, 't'},
		{"runtime",	1, 0, 'T'},
		{"count",	1, 0, 'c'},
		{"verbose",	0, 0, 'v'},
		{"help",	0, 0, 'h'},
		{NULL,		0, NULL, 0}
	};
	int option_index = 0;
	while ((c = getopt_long(argc, argv, "p:d:t:T:c:vh",
					long_options, &option_index)) != -1) {
		switch (c) {
			case 'p':
				opt_dstport = optarg;
				break;
			case 'd':
				opt_delay = strtoull(optarg, NULL, 10);
				break;
			case 't':
				opt_timeout = atoi(optarg);
				break;
			case 'T':
				opt_runtime = atoi(optarg);
				break;
			case 'c':
				opt_count = atoi(optarg);
				break;
			case 'v':
				opt_verbose++;
				break;
			case 'h':
				usage(argv[0]);
				exit(EXIT_SUCCESS);
				break;
			case '?':
			default:
				/* FIXME: How does '?' work? */
				/*
				if(isprint(c))
					fprintf(stderr, "Fatal: Unknown option `-%c'.\n", c);
				else
					fprintf(stderr, "Fatal: Unknown option character `\\x%x'.\n", c);
				*/
				usage(argv[0]);
				return -1;
				break;
		}
	}

	if (optind < argc)
		opt_dstaddr = argv[optind++];
	else
	{
		fprintf(stderr, "FATAL: Missing mandatory destination argument.\n");
		usage(argv[0]);
		exit(EXIT_FAILURE);
	}
	return 0;
}

void sig_handler(int sig)
{
	dp("Caught signal %d\n", sig);
	switch(sig)
	{
		case SIGALRM:
			fprintf(stderr, "%llu second timeout reached\n", opt_runtime);
			break;
		case SIGINT:
		case SIGTERM:
			fprintf(stderr, "Caught term/interrupt\n");
			break;
	}
	printf("Tried %llu connections\n", stats_tries);
	exit(EXIT_SUCCESS);
}

int main(int argc, char** argv)
{
	int rc, sock = -1;
	unsigned long long int i;
	struct addrinfo *ai, *r;
	struct addrinfo hints;
	char bufhost[MAXHOSTBUFSIZE];
	char bufport[MAXHOSTBUFSIZE];
	fd_set wfds;
	struct timeval tv, tvs;
	int err = 0;
	socklen_t errsize = sizeof(err);
	char buf[32];
	time_t local;

	/* Option defaults */
	opt_dstaddr = "127.0.0.1";
	opt_dstport = "80";
	if((rc = parse_options(argc, argv)) != 0)
	{
		fprintf(stderr, "Option parsing failed (%d). See stderr.\n", rc);
		exit(EXIT_FAILURE);
	}

	signal(SIGALRM, sig_handler);
	signal(SIGINT, sig_handler);
	signal(SIGTERM, sig_handler);
	if(opt_runtime)
		alarm(opt_runtime);

	memset(&hints, '\0', sizeof(hints));
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = IPPROTO_TCP;
	hints.ai_flags = AI_ADDRCONFIG;
	dp("Destination address set to %s\n", opt_dstaddr);
	int e = getaddrinfo(opt_dstaddr, opt_dstport, &hints, &ai);
	if (e != 0)
	{
		fprintf(stderr, "getaddrinfo: %s\n", gai_strerror(e));
		exit(EXIT_FAILURE);
	}

	/* Find a suitable socket, and connect */
	for(r = ai; r != NULL; r = r->ai_next) {
		dp("-> Trying family/socktype/proto %d/%d/%d\n",
				r->ai_family, r->ai_socktype, r->ai_protocol);
		sock = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
		if(sock != -1)
		{
			/* Try connect() with timeout */
			fcntl(sock, F_SETFL, O_NONBLOCK);
			tv.tv_sec = 0; /* FIXME */
			tv.tv_usec = opt_timeout * 2;
			connect(sock, r->ai_addr, r->ai_addrlen);
			FD_ZERO(&wfds);
			FD_SET(sock, &wfds);
			rc = select(sock+1, NULL, &wfds, NULL, &tv);
			if(rc == -1)
				dp("-> select() error: %s\n", strerror(errno));
			else if(rc)
			{
				getsockopt(sock, SOL_SOCKET, SO_ERROR, (void *)&err, &errsize);
				if(!err)
				{
					dp("-> connect() successful\n");
					break;
				}
				else
					dp("-> connect() failed: %s\n", strerror(err));
			}
			else
				dp("-> connect() timeout\n");
			close(sock);
			sock = -1;
		}
		else
			dp("-> socket() failed: %s\n", strerror(errno));
	}
	if (sock == -1)
	{
		freeaddrinfo(ai);
		perror("socket()"); /* FIXME: broken errno? */
		exit(EXIT_FAILURE);
	}

	(void) getnameinfo(r->ai_addr, r->ai_addrlen,
			bufhost, sizeof (bufhost), bufport, sizeof(bufport),
			NI_NUMERICHOST|NI_NUMERICSERV);
	dp("Successfully opened socket to %s:%s\n", bufhost, bufport);
	close(sock);
	stats_tries = 1;

	/* Connection loop */
	dp("Starting connection loop.\n");
	for(i = 0; opt_count==0 ? 1 : i < opt_count-1; i++)
	{
		sock = socket(r->ai_family, r->ai_socktype, r->ai_protocol);
		fcntl(sock, F_SETFL, O_NONBLOCK);
		FD_ZERO(&wfds);
		FD_SET(sock, &wfds);
		tv.tv_sec = 0; /* FIXME */
		tv.tv_usec = opt_timeout;
		connect(sock, r->ai_addr, r->ai_addrlen);
		gettimeofday(&tvs, NULL); /* Approx SYN timestamp */
		rc = select(sock+1, NULL, &wfds, NULL, &tv);
		getsockopt(sock, SOL_SOCKET, SO_ERROR, (void *)&err, &errsize);
		if(rc == -1)
			fprintf(stderr, "WARNING: select() error: %s",
					strerror(errno));
		else if(rc)
		{
			/* fd available */
			if(err)
			{
				/* connect() error */
				local = tvs.tv_sec;
				strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S",
						localtime(&local));
				fprintf(stderr, "Connection failed: %s "
						"[%s.%06ld (%ld.%06ld)]\n",
						strerror(err),
						buf, tvs.tv_usec,
						tvs.tv_sec, tvs.tv_usec);
			}
			else if(opt_verbose > 2)
			{
				/* connect() success */
				local = tvs.tv_sec;
				strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S",
						localtime(&local));
				dp("Connection successful "
						"[%s.%06ld (%ld.%06ld)]\n",
						buf, tvs.tv_usec,
						tvs.tv_sec, tvs.tv_usec);
			}
		}
		else
		{
			/* fd not available */
			local = tvs.tv_sec;
			strftime(buf, sizeof(buf), "%Y-%m-%dT%H:%M:%S",
					localtime(&local));
			fprintf(stderr, "Connection timeout "
					"[%s.%06ld (%ld.%06ld)]\n",
					buf, tvs.tv_usec,
					tvs.tv_sec, tvs.tv_usec);
		}
		close(sock);

		stats_tries++;
		if(opt_delay)
			usleep(opt_delay);
	}

	close(sock);
	freeaddrinfo(ai);

	alarm(0);

	printf("Tried %llu connections\n", stats_tries);
	return 0;
}
