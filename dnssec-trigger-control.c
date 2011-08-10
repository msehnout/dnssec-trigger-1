/*
 * dnssec-trigger-control.c - remote control utility for dnssec-trigger.
 *
 * Copyright (c) 2008, NLnet Labs. All rights reserved.
 *
 * This software is open source.
 * 
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 
 * Redistributions of source code must retain the above copyright notice,
 * this list of conditions and the following disclaimer.
 * 
 * Redistributions in binary form must reproduce the above copyright notice,
 * this list of conditions and the following disclaimer in the documentation
 * and/or other materials provided with the distribution.
 * 
 * Neither the name of the NLNET LABS nor the names of its contributors may
 * be used to endorse or promote products derived from this software without
 * specific prior written permission.
 * 
 * THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS
 * "AS IS" AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED
 * TO, THE IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR
 * PURPOSE ARE DISCLAIMED. IN NO EVENT SHALL THE REGENTS OR CONTRIBUTORS BE
 * LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
 * CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
 * SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
 * INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
 * CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
 * ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
 * POSSIBILITY OF SUCH DAMAGE.
 */

/**
 * \file
 *
 * The remote control utility contacts the dnssec-trigger server over ssl 
 */

#include "config.h"
#ifdef HAVE_GETOPT_H
#include <getopt.h>
#endif
#ifdef HAVE_OPENSSL_SSL_H
#include <openssl/ssl.h>
#endif
#ifdef HAVE_OPENSSL_ERR_H
#include <openssl/err.h>
#endif
#ifdef HAVE_OPENSSL_RAND_H
#include <openssl/rand.h>
#endif
#include "riggerd/log.h"
#include "riggerd/cfg.h"
#include "riggerd/net_help.h"

/** Give dnssec-trigger-control usage, and exit (1). */
static void
usage()
{
	printf("Usage:	dnssec-trigger-control [options] command\n");
	printf("	Remote control utility for dnssec-trigger server.\n");
	printf("Options:\n");
	printf("  -c file	config file, default is %s\n", CONFIGFILE);
	printf("  -s ip[@port]	server address, if omitted config is used.\n");
	printf("  -h		show this usage help.\n");
	printf("Commands:\n");
	printf("submit <ips>	submit a list of DHCP provided DNS servers\n");
	printf("Version %s\n", PACKAGE_VERSION);
	printf("BSD licensed, see LICENSE in source package for details.\n");
	printf("Report bugs to %s\n", PACKAGE_BUGREPORT);
	exit(1);
}

/** exit with ssl error */
static void ssl_err(const char* s)
{
	fprintf(stderr, "error: %s\n", s);
	ERR_print_errors_fp(stderr);
	exit(1);
}

/** setup SSL context */
static SSL_CTX*
setup_ctx(struct cfg* cfg)
{
	char* s_cert, *c_key, *c_cert;
	SSL_CTX* ctx;

	s_cert = cfg->server_cert_file;
	c_key = cfg->control_key_file;
	c_cert = cfg->control_cert_file;
        ctx = SSL_CTX_new(SSLv23_client_method());
	if(!ctx)
		ssl_err("could not allocate SSL_CTX pointer");
        if(!(SSL_CTX_set_options(ctx, SSL_OP_NO_SSLv2) & SSL_OP_NO_SSLv2))
		ssl_err("could not set SSL_OP_NO_SSLv2");
	if(!SSL_CTX_use_certificate_file(ctx,c_cert,SSL_FILETYPE_PEM) ||
		!SSL_CTX_use_PrivateKey_file(ctx,c_key,SSL_FILETYPE_PEM)
		|| !SSL_CTX_check_private_key(ctx))
		ssl_err("Error setting up SSL_CTX client key and cert");
	if (SSL_CTX_load_verify_locations(ctx, s_cert, NULL) != 1)
		ssl_err("Error setting up SSL_CTX verify, server cert");
	SSL_CTX_set_verify(ctx, SSL_VERIFY_PEER, NULL);

	return ctx;
}

/** contact the server with TCP connect */
static int
contact_server(const char* svr, struct cfg* cfg, int statuscmd)
{
	struct sockaddr_storage addr;
	socklen_t addrlen;
	int fd;
	/* use svr or the first config entry */
	if(!svr) {
		svr = "127.0.0.1";
		/* config 0 addr (everything), means ask localhost */
		if(strcmp(svr, "0.0.0.0") == 0)
			svr = "127.0.0.1";
		else if(strcmp(svr, "::0") == 0 ||
			strcmp(svr, "0::0") == 0 ||
			strcmp(svr, "0::") == 0 ||
			strcmp(svr, "::") == 0)
			svr = "::1";
	}
	if(strchr(svr, '@')) {
		if(!extstrtoaddr(svr, &addr, &addrlen))
			fatal_exit("could not parse IP@port: %s", svr);
	} else {
		if(!ipstrtoaddr(svr, cfg->control_port, &addr, &addrlen))
			fatal_exit("could not parse IP: %s", svr);
	}
	fd = socket(addr_is_ip6(&addr, addrlen)?AF_INET6:AF_INET, 
		SOCK_STREAM, 0);
	if(fd == -1) {
#ifndef USE_WINSOCK
		fatal_exit("socket: %s", strerror(errno));
#else
		fatal_exit("socket: %s", wsa_strerror(WSAGetLastError()));
#endif
	}
	if(connect(fd, (struct sockaddr*)&addr, addrlen) < 0) {
		log_addr(0, "address", &addr, addrlen);
#ifndef USE_WINSOCK
		log_err("connect: %s", strerror(errno));
		if(errno == ECONNREFUSED && statuscmd) {
			printf("the daemon is stopped\n");
			exit(3);
		}
#else
		log_err("connect: %s", wsa_strerror(WSAGetLastError()));
		if(WSAGetLastError() == WSAECONNREFUSED && statuscmd) {
			printf("the daemon is stopped\n");
			exit(3);
		}
#endif
		exit(1);
	}
	return fd;
}

/** setup SSL on the connection */
static SSL*
setup_ssl(SSL_CTX* ctx, int fd)
{
	SSL* ssl;
	X509* x;
	int r;

	ssl = SSL_new(ctx);
	if(!ssl)
		ssl_err("could not SSL_new");
	SSL_set_connect_state(ssl);
	(void)SSL_set_mode(ssl, SSL_MODE_AUTO_RETRY);
	if(!SSL_set_fd(ssl, fd))
		ssl_err("could not SSL_set_fd");
	while(1) {
		ERR_clear_error();
		if( (r=SSL_do_handshake(ssl)) == 1)
			break;
		r = SSL_get_error(ssl, r);
		if(r != SSL_ERROR_WANT_READ && r != SSL_ERROR_WANT_WRITE)
			ssl_err("SSL handshake failed");
		/* wants to be called again */
	}

	/* check authenticity of server */
	if(SSL_get_verify_result(ssl) != X509_V_OK)
		ssl_err("SSL verification failed");
	x = SSL_get_peer_certificate(ssl);
	if(!x)
		ssl_err("Server presented no peer certificate");
	X509_free(x);
	return ssl;
}

/** send stdin to server */
static void
send_file(SSL* ssl, FILE* in, char* buf, size_t sz)
{
	while(fgets(buf, (int)sz, in)) {
		if(SSL_write(ssl, buf, (int)strlen(buf)) <= 0)
			ssl_err("could not SSL_write contents");
	}
}

/** send command and display result */
static int
go_cmd(SSL* ssl, int argc, char* argv[])
{
	char pre[10];
	const char* space=" ";
	const char* newline="\n";
	int was_error = 0, first_line = 1;
	int r, i;
	char buf[1024];
	snprintf(pre, sizeof(pre), "DNSTRIG%d ", CONTROL_VERSION);
	if(SSL_write(ssl, pre, (int)strlen(pre)) <= 0)
		ssl_err("could not SSL_write");
	for(i=0; i<argc; i++) {
		if(SSL_write(ssl, space, (int)strlen(space)) <= 0)
			ssl_err("could not SSL_write");
		if(SSL_write(ssl, argv[i], (int)strlen(argv[i])) <= 0)
			ssl_err("could not SSL_write");
	}
	if(SSL_write(ssl, newline, (int)strlen(newline)) <= 0)
		ssl_err("could not SSL_write");

	if(argc == 1 && strcmp(argv[0], "load_cache") == 0) {
		send_file(ssl, stdin, buf, sizeof(buf));
	}

	while(1) {
		ERR_clear_error();
		if((r = SSL_read(ssl, buf, (int)sizeof(buf)-1)) <= 0) {
			if(SSL_get_error(ssl, r) == SSL_ERROR_ZERO_RETURN) {
				/* EOF */
				break;
			}
			ssl_err("could not SSL_read");
		}
		buf[r] = 0;
		printf("%s", buf);
		if(first_line && strncmp(buf, "error", 5) == 0)
			was_error = 1;
		first_line = 0;
	}
	return was_error;
}

/** go ahead and read config, contact server and perform command and display */
static int
go(const char* cfgfile, char* svr, int argc, char* argv[])
{
	struct cfg* cfg;
	int fd, ret;
	SSL_CTX* ctx;
	SSL* ssl;

	/* read config */
	if(!(cfg = cfg_create(cfgfile)))
		fatal_exit("could not get config file");
	ctx = setup_ctx(cfg);
	
	/* contact server */
	fd = contact_server(svr, cfg, argc>0&&strcmp(argv[0],"status")==0);
	ssl = setup_ssl(ctx, fd);
	
	/* send command */
	ret = go_cmd(ssl, argc, argv);

	SSL_free(ssl);
#ifndef USE_WINSOCK
	close(fd);
#else
	closesocket(fd);
#endif
	SSL_CTX_free(ctx);
	cfg_delete(cfg);
	return ret;
}

/** getopt global, in case header files fail to declare it. */
extern int optind;
/** getopt global, in case header files fail to declare it. */
extern char* optarg;

/** Main routine for dnssec-trigger-control */
int main(int argc, char* argv[])
{
	int c, ret;
	const char* cfgfile = CONFIGFILE;
	char* svr = NULL;
#ifdef USE_WINSOCK
	int r;
	WSADATA wsa_data;
#endif
	log_ident_set("dnssec-trigger-control");
	log_init(NULL, 0, NULL);
#ifdef USE_WINSOCK
	if((r = WSAStartup(MAKEWORD(2,2), &wsa_data)) != 0)
		fatal_exit("WSAStartup failed: %s", wsa_strerror(r));
#endif

	ERR_load_crypto_strings();
	ERR_load_SSL_strings();
	OpenSSL_add_all_algorithms();
	(void)SSL_library_init();

	if(!RAND_status()) {
                /* try to seed it */
                unsigned char buf[256];
                unsigned int v, seed=(unsigned)time(NULL) ^ (unsigned)getpid();
                size_t i;
                for(i=0; i<256/sizeof(v); i++) {
                        memmove(buf+i*sizeof(v), &v, sizeof(v));
                        v = v*seed + (unsigned int)i;
                }
                RAND_seed(buf, 256);
		log_warn("no entropy, seeding openssl PRNG with time\n");
	}

	/* parse the options */
	while( (c=getopt(argc, argv, "c:s:h")) != -1) {
		switch(c) {
		case 'c':
			cfgfile = optarg;
			break;
		case 's':
			svr = optarg;
			break;
		case '?':
		case 'h':
		default:
			usage();
		}
	}
	argc -= optind;
	argv += optind;
	if(argc == 0)
		usage();
	ret = go(cfgfile, svr, argc, argv);

#ifdef USE_WINSOCK
        WSACleanup();
#endif
	return ret;
}