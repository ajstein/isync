/*
 * mbsync - mailbox synchronizer
 * Copyright (C) 2000-2002 Michael R. Elkins <me@mutt.org>
 * Copyright (C) 2002-2006,2008,2010,2011 Oswald Buddenhagen <ossi@users.sf.net>
 * Copyright (C) 2004 Theodore Y. Ts'o <tytso@mit.edu>
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License as published by
 *  the Free Software Foundation; either version 2 of the License, or
 *  (at your option) any later version.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License
 *  along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * As a special exception, mbsync may be linked with the OpenSSL library,
 * despite that library's more restrictive license.
 */

/* This must come before isync.h to avoid our #define S messing up
 * blowfish.h on MacOS X. */
#include <config.h>
#ifdef HAVE_LIBSSL
# include <openssl/ssl.h>
# include <openssl/err.h>
# include <openssl/hmac.h>
# include <openssl/x509v3.h>
#endif

#include "isync.h"

#include <assert.h>
#include <unistd.h>
#include <stdlib.h>
#include <stddef.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>

enum {
	SCK_CONNECTING,
#ifdef HAVE_LIBSSL
	SCK_STARTTLS,
#endif
	SCK_READY
};

static void
socket_fail( conn_t *conn )
{
	conn->bad_callback( conn->callback_aux );
}

#ifdef HAVE_LIBSSL
static int ssl_data_idx;

static int
ssl_return( const char *func, conn_t *conn, int ret )
{
	int err;

	switch ((err = SSL_get_error( conn->ssl, ret ))) {
	case SSL_ERROR_NONE:
		return ret;
	case SSL_ERROR_WANT_WRITE:
		conf_fd( conn->fd, POLLIN, POLLOUT );
		/* fallthrough */
	case SSL_ERROR_WANT_READ:
		return 0;
	case SSL_ERROR_SYSCALL:
	case SSL_ERROR_SSL:
		if (!(err = ERR_get_error())) {
			if (ret == 0)
				error( "Socket error: secure %s %s: unexpected EOF\n", func, conn->name );
			else
				sys_error( "Socket error: secure %s %s", func, conn->name );
		} else {
			error( "Socket error: secure %s %s: %s\n", func, conn->name, ERR_error_string( err, 0 ) );
		}
		break;
	default:
		error( "Socket error: secure %s %s: unhandled SSL error %d\n", func, conn->name, err );
		break;
	}
	if (conn->state == SCK_STARTTLS)
		conn->callbacks.starttls( 0, conn->callback_aux );
	else
		socket_fail( conn );
	return -1;
}

/* Some of this code is inspired by / lifted from mutt. */

static int
host_matches( const char *host, const char *pattern )
{
	if (pattern[0] == '*' && pattern[1] == '.') {
		pattern += 2;
		if (!(host = strchr( host, '.' )))
			return 0;
		host++;
	}

	return *host && *pattern && !strcasecmp( host, pattern );
}

static int
verify_hostname( X509 *cert, const char *hostname )
{
	int i, len, found;
	X509_NAME *subj;
	STACK_OF(GENERAL_NAME) *subj_alt_names;
	char cname[1000];

	/* try the DNS subjectAltNames */
	found = 0;
	if ((subj_alt_names = X509_get_ext_d2i( cert, NID_subject_alt_name, NULL, NULL ))) {
		int num_subj_alt_names = sk_GENERAL_NAME_num( subj_alt_names );
		for (i = 0; i < num_subj_alt_names; i++) {
			GENERAL_NAME *subj_alt_name = sk_GENERAL_NAME_value( subj_alt_names, i );
			if (subj_alt_name->type == GEN_DNS &&
			    strlen( (const char *)subj_alt_name->d.ia5->data ) == (size_t)subj_alt_name->d.ia5->length &&
			    host_matches( hostname, (const char *)(subj_alt_name->d.ia5->data) ))
			{
				found = 1;
				break;
			}
		}
		sk_GENERAL_NAME_pop_free( subj_alt_names, GENERAL_NAME_free );
	}
	if (found)
		return 0;

	/* try the common name */
	if (!(subj = X509_get_subject_name( cert ))) {
		error( "Error, cannot get certificate subject\n" );
		return -1;
	}
	if ((len = X509_NAME_get_text_by_NID( subj, NID_commonName, cname, sizeof(cname) )) < 0) {
		error( "Error, cannot get certificate common name\n" );
		return -1;
	}
	if (strlen( cname ) == (size_t)len && host_matches( hostname, cname ))
		return 0;

	error( "Error, certificate owner does not match hostname %s\n", hostname );
	return -1;
}

static int
verify_cert_host( const server_conf_t *conf, conn_t *sock )
{
	X509 *cert;

	if (!conf->host || sock->force_trusted > 0)
		return 0;

	cert = SSL_get_peer_certificate( sock->ssl );
	if (!cert) {
		error( "Error, no server certificate\n" );
		return -1;
	}

	return verify_hostname( cert, conf->host );
}

static int
ssl_verify_callback( int ok, X509_STORE_CTX *ctx )
{
	SSL *ssl = X509_STORE_CTX_get_ex_data( ctx, SSL_get_ex_data_X509_STORE_CTX_idx() );
	conn_t *conn = SSL_get_ex_data( ssl, ssl_data_idx );

	if (!conn->force_trusted) {
		X509 *cert = sk_X509_value( ctx->chain, 0 );
		STACK_OF(X509_OBJECT) *trusted = ctx->ctx->objs;
		unsigned i;

		conn->force_trusted = -1;
		for (i = 0; i < conn->conf->num_trusted; i++) {
			if (!X509_cmp( cert, sk_X509_OBJECT_value( trusted, i )->data.x509 )) {
				conn->force_trusted = 1;
				break;
			}
		}
	}
	if (conn->force_trusted > 0)
		ok = 1;
	return ok;
}

static int
init_ssl_ctx( const server_conf_t *conf )
{
	server_conf_t *mconf = (server_conf_t *)conf;
	int options = 0;

	if (conf->SSLContext)
		return conf->ssl_ctx_valid;

	mconf->SSLContext = SSL_CTX_new( SSLv23_client_method() );

	if (!conf->use_sslv2)
		options |= SSL_OP_NO_SSLv2;
	if (!conf->use_sslv3)
		options |= SSL_OP_NO_SSLv3;
	if (!conf->use_tlsv1)
		options |= SSL_OP_NO_TLSv1;
#ifdef SSL_OP_NO_TLSv1_1
	if (!conf->use_tlsv11)
		options |= SSL_OP_NO_TLSv1_1;
#endif
#ifdef SSL_OP_NO_TLSv1_2
	if (!conf->use_tlsv12)
		options |= SSL_OP_NO_TLSv1_2;
#endif

	SSL_CTX_set_options( mconf->SSLContext, options );

	if (conf->cert_file && !SSL_CTX_load_verify_locations( mconf->SSLContext, conf->cert_file, 0 )) {
		error( "Error while loading certificate file '%s': %s\n",
		       conf->cert_file, ERR_error_string( ERR_get_error(), 0 ) );
		return 0;
	}
	mconf->num_trusted = sk_X509_OBJECT_num( SSL_CTX_get_cert_store( mconf->SSLContext )->objs );
	if (!SSL_CTX_set_default_verify_paths( mconf->SSLContext ))
		warn( "Warning: Unable to load default certificate files: %s\n",
		      ERR_error_string( ERR_get_error(), 0 ) );

	SSL_CTX_set_verify( mconf->SSLContext, SSL_VERIFY_PEER, ssl_verify_callback );

	mconf->ssl_ctx_valid = 1;
	return 1;
}

static void start_tls_p2( conn_t * );
static void start_tls_p3( conn_t *, int );

void
socket_start_tls( conn_t *conn, void (*cb)( int ok, void *aux ) )
{
	static int ssl_inited;

	conn->callbacks.starttls = cb;

	if (!ssl_inited) {
		SSL_library_init();
		SSL_load_error_strings();
		ssl_data_idx = SSL_get_ex_new_index( 0, NULL, NULL, NULL, NULL );
		ssl_inited = 1;
	}

	if (!init_ssl_ctx( conn->conf )) {
		start_tls_p3( conn, 0 );
		return;
	}

	conn->ssl = SSL_new( ((server_conf_t *)conn->conf)->SSLContext );
	SSL_set_fd( conn->ssl, conn->fd );
	SSL_set_mode( conn->ssl, SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER );
	SSL_set_ex_data( conn->ssl, ssl_data_idx, conn );
	conn->state = SCK_STARTTLS;
	start_tls_p2( conn );
}

static void
start_tls_p2( conn_t *conn )
{
	if (ssl_return( "connect to", conn, SSL_connect( conn->ssl ) ) > 0) {
		/* verify whether the server hostname matches the certificate */
		if (verify_cert_host( conn->conf, conn )) {
			start_tls_p3( conn, 0 );
		} else {
			info( "Connection is now encrypted\n" );
			start_tls_p3( conn, 1 );
		}
	}
}

static void start_tls_p3( conn_t *conn, int ok )
{
	conn->state = SCK_READY;
	conn->callbacks.starttls( ok, conn->callback_aux );
}

#endif /* HAVE_LIBSSL */

static void socket_fd_cb( int, void * );

static void socket_connect_failed( conn_t * );
static void socket_connected( conn_t * );
static void socket_connect_bail( conn_t * );

static void
socket_close_internal( conn_t *sock )
{
	del_fd( sock->fd );
	close( sock->fd );
	sock->fd = -1;
}

void
socket_connect( conn_t *sock, void (*cb)( int ok, void *aux ) )
{
	const server_conf_t *conf = sock->conf;
	struct hostent *he;
	struct sockaddr_in addr;
	int s, a[2];

	sock->callbacks.connect = cb;

	/* open connection to IMAP server */
	if (conf->tunnel) {
		nfasprintf( &sock->name, "tunnel '%s'", conf->tunnel );
		infon( "Starting %s... ", sock->name );

		if (socketpair( PF_UNIX, SOCK_STREAM, 0, a )) {
			perror( "socketpair" );
			exit( 1 );
		}

		if (fork() == 0) {
			if (dup2( a[0], 0 ) == -1 || dup2( a[0], 1 ) == -1)
				_exit( 127 );
			close( a[0] );
			close( a[1] );
			execl( "/bin/sh", "sh", "-c", conf->tunnel, (char *)0 );
			_exit( 127 );
		}

		close( a[0] );
		sock->fd = a[1];

		fcntl( a[1], F_SETFL, O_NONBLOCK );
		add_fd( a[1], socket_fd_cb, sock );

	} else {
		memset( &addr, 0, sizeof(addr) );
		addr.sin_port = conf->port ? htons( conf->port ) :
#ifdef HAVE_LIBSSL
		                conf->use_imaps ? htons( 993 ) :
#endif
		                htons( 143 );
		addr.sin_family = AF_INET;

		infon( "Resolving %s... ", conf->host );
		he = gethostbyname( conf->host );
		if (!he) {
			error( "IMAP error: Cannot resolve server '%s'\n", conf->host );
			socket_connect_bail( sock );
			return;
		}
		info( "\vok\n" );

		addr.sin_addr.s_addr = *((int *)he->h_addr_list[0]);

		s = socket( PF_INET, SOCK_STREAM, 0 );
		if (s < 0) {
			perror( "socket" );
			exit( 1 );
		}
		sock->fd = s;
		fcntl( s, F_SETFL, O_NONBLOCK );
		add_fd( s, socket_fd_cb, sock );

		nfasprintf( &sock->name, "%s (%s:%hu)",
		            conf->host, inet_ntoa( addr.sin_addr ), ntohs( addr.sin_port ) );
		infon( "Connecting to %s... ", sock->name );
		if (connect( s, (struct sockaddr *)&addr, sizeof(addr) )) {
			if (errno != EINPROGRESS) {
				socket_connect_failed( sock );
				return;
			}
			conf_fd( s, 0, POLLOUT );
			sock->state = SCK_CONNECTING;
			info( "\v\n" );
			return;
		}

	}
	info( "\vok\n" );
	socket_connected( sock );
	return;
}

static void
socket_connect_failed( conn_t *conn )
{
	sys_error( "Cannot connect to %s", conn->name );
	socket_close_internal( conn );
	socket_connect_bail( conn );
}

static void
socket_connected( conn_t *conn )
{
	conf_fd( conn->fd, 0, POLLIN );
	conn->state = SCK_READY;
	conn->callbacks.connect( 1, conn->callback_aux );
}

static void
socket_connect_bail( conn_t *conn )
{
	free( conn->name );
	conn->name = 0;
	conn->callbacks.connect( 0, conn->callback_aux );
}

static void dispose_chunk( conn_t *conn );

void
socket_close( conn_t *sock )
{
	if (sock->fd >= 0)
		socket_close_internal( sock );
	free( sock->name );
	sock->name = 0;
#ifdef HAVE_LIBSSL
	if (sock->ssl) {
		SSL_free( sock->ssl );
		sock->ssl = 0;
	}
#endif
	while (sock->write_buf)
		dispose_chunk( sock );
}

static void
socket_fill( conn_t *sock )
{
	char *buf;
	int n = sock->offset + sock->bytes;
	int len = sizeof(sock->buf) - n;
	if (!len) {
		error( "Socket error: receive buffer full. Probably protocol error.\n" );
		socket_fail( sock );
		return;
	}
	assert( sock->fd >= 0 );
	buf = sock->buf + n;
#ifdef HAVE_LIBSSL
	if (sock->ssl) {
		if ((n = ssl_return( "read from", sock, SSL_read( sock->ssl, buf, len ) )) <= 0)
			return;
		if (n == len && SSL_pending( sock->ssl ))
			fake_fd( sock->fd, POLLIN );
	} else
#endif
	{
		if ((n = read( sock->fd, buf, len )) < 0) {
			sys_error( "Socket error: read from %s", sock->name );
			socket_fail( sock );
			return;
		} else if (!n) {
			error( "Socket error: read from %s: unexpected EOF\n", sock->name );
			socket_fail( sock );
			return;
		}
	}
	sock->bytes += n;
	sock->read_callback( sock->callback_aux );
}

int
socket_read( conn_t *conn, char *buf, int len )
{
	int n = conn->bytes;
	if (n > len)
		n = len;
	memcpy( buf, conn->buf + conn->offset, n );
	if (!(conn->bytes -= n))
		conn->offset = 0;
	else
		conn->offset += n;
	return n;
}

char *
socket_read_line( conn_t *b )
{
	char *p, *s;
	int n;

	s = b->buf + b->offset;
	p = memchr( s + b->scanoff, '\n', b->bytes - b->scanoff );
	if (!p) {
		b->scanoff = b->bytes;
		if (b->offset + b->bytes == sizeof(b->buf)) {
			memmove( b->buf, b->buf + b->offset, b->bytes );
			b->offset = 0;
		}
		return 0;
	}
	n = p + 1 - s;
	b->offset += n;
	b->bytes -= n;
	b->scanoff = 0;
	if (p != s && p[-1] == '\r')
		p--;
	*p = 0;
	if (DFlags & VERBOSE) {
		puts( s );
		fflush( stdout );
	}
	return s;
}

static int
do_write( conn_t *sock, char *buf, int len )
{
	int n;

	assert( sock->fd >= 0 );
#ifdef HAVE_LIBSSL
	if (sock->ssl)
		return ssl_return( "write to", sock, SSL_write( sock->ssl, buf, len ) );
#endif
	n = write( sock->fd, buf, len );
	if (n < 0) {
		if (errno != EAGAIN && errno != EWOULDBLOCK) {
			sys_error( "Socket error: write to %s", sock->name );
			socket_fail( sock );
		} else {
			n = 0;
			conf_fd( sock->fd, POLLIN, POLLOUT );
		}
	} else if (n != len) {
		conf_fd( sock->fd, POLLIN, POLLOUT );
	}
	return n;
}

static void
dispose_chunk( conn_t *conn )
{
	buff_chunk_t *bc = conn->write_buf;
	if (!(conn->write_buf = bc->next))
		conn->write_buf_append = &conn->write_buf;
	if (bc->data != bc->buf)
		free( bc->data );
	free( bc );
}

static int
do_queued_write( conn_t *conn )
{
	buff_chunk_t *bc;

	if (!conn->write_buf)
		return 0;

	while ((bc = conn->write_buf)) {
		int n, len = bc->len - conn->write_offset;
		if ((n = do_write( conn, bc->data + conn->write_offset, len )) < 0)
			return -1;
		if (n != len) {
			conn->write_offset += n;
			return 0;
		}
		conn->write_offset = 0;
		dispose_chunk( conn );
	}
#ifdef HAVE_LIBSSL
	if (conn->ssl && SSL_pending( conn->ssl ))
		fake_fd( conn->fd, POLLIN );
#endif
	return conn->write_callback( conn->callback_aux );
}

static void
do_append( conn_t *conn, char *buf, int len, ownership_t takeOwn )
{
	buff_chunk_t *bc;

	if (takeOwn == GiveOwn) {
		bc = nfmalloc( offsetof(buff_chunk_t, buf) );
		bc->data = buf;
	} else {
		bc = nfmalloc( offsetof(buff_chunk_t, buf) + len );
		bc->data = bc->buf;
		memcpy( bc->data, buf, len );
	}
	bc->len = len;
	bc->next = 0;
	*conn->write_buf_append = bc;
	conn->write_buf_append = &bc->next;
}

int
socket_write( conn_t *conn, char *buf, int len, ownership_t takeOwn )
{
	if (conn->write_buf) {
		do_append( conn, buf, len, takeOwn );
		return len;
	} else {
		int n = do_write( conn, buf, len );
		if (n != len && n >= 0) {
			conn->write_offset = n;
			do_append( conn, buf, len, takeOwn );
		} else if (takeOwn) {
			free( buf );
		}
		return n;
	}
}

static void
socket_fd_cb( int events, void *aux )
{
	conn_t *conn = (conn_t *)aux;

	if ((events & POLLERR) || conn->state == SCK_CONNECTING) {
		int soerr;
		socklen_t selen = sizeof(soerr);
		if (getsockopt( conn->fd, SOL_SOCKET, SO_ERROR, &soerr, &selen )) {
			perror( "getsockopt" );
			exit( 1 );
		}
		errno = soerr;
		if (conn->state == SCK_CONNECTING) {
			if (errno)
				socket_connect_failed( conn );
			else
				socket_connected( conn );
			return;
		}
		sys_error( "Socket error from %s", conn->name );
		socket_fail( conn );
		return;
	}

	if (events & POLLOUT)
		conf_fd( conn->fd, POLLIN, 0 );

#ifdef HAVE_LIBSSL
	if (conn->state == SCK_STARTTLS) {
		start_tls_p2( conn );
		return;
	}
	if (conn->ssl) {
		if (do_queued_write( conn ) < 0)
			return;
		socket_fill( conn );
		return;
	}
#endif

	if ((events & POLLOUT) && do_queued_write( conn ) < 0)
		return;
	if (events & POLLIN)
		socket_fill( conn );
}

#ifdef HAVE_LIBSSL
/* this isn't strictly socket code, but let's have all OpenSSL use in one file. */

#define ENCODED_SIZE(n) (4*((n+2)/3))

static char
hexchar( unsigned int b )
{
	if (b < 10)
		return '0' + b;
	return 'a' + (b - 10);
}

void
cram( const char *challenge, const char *user, const char *pass, char **_final, int *_finallen )
{
	char *response, *final;
	unsigned hashlen;
	int i, clen, blen, flen, olen;
	unsigned char hash[16];
	char buf[256], hex[33];
	HMAC_CTX hmac;

	HMAC_Init( &hmac, (unsigned char *)pass, strlen( pass ), EVP_md5() );

	clen = strlen( challenge );
	/* response will always be smaller than challenge because we are decoding. */
	response = nfcalloc( 1 + clen );
	EVP_DecodeBlock( (unsigned char *)response, (unsigned char *)challenge, clen );
	HMAC_Update( &hmac, (unsigned char *)response, strlen( response ) );
	free( response );

	hashlen = sizeof(hash);
	HMAC_Final( &hmac, hash, &hashlen );
	assert( hashlen == sizeof(hash) );

	hex[32] = 0;
	for (i = 0; i < 16; i++) {
		hex[2 * i] = hexchar( (hash[i] >> 4) & 0xf );
		hex[2 * i + 1] = hexchar( hash[i] & 0xf );
	}

	blen = nfsnprintf( buf, sizeof(buf), "%s %s", user, hex );

	flen = ENCODED_SIZE( blen );
	final = nfmalloc( flen + 1 );
	final[flen] = 0;
	olen = EVP_EncodeBlock( (unsigned char *)final, (unsigned char *)buf, blen );
	assert( olen == flen );

	*_final = final;
	*_finallen = flen;
}
#endif
