// SPDX-License-Identifier: BSD-3-Clause

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/sendfile.h>
#include <sys/eventfd.h>
#include <libaio.h>
#include <errno.h>

#include "aws.h"
#include "utils/util.h"
#include "utils/debug.h"
#include "utils/sock_util.h"
#include "utils/w_epoll.h"

#define EVENT_SIZE 20

/* server socket file descriptor */
static int listenfd;

/* epoll file descriptor */
static int epollfd;

static io_context_t ctx;

static int aws_on_path_cb(http_parser *p, const char *buf, size_t len)
{
	struct connection *conn = (struct connection *)p->data;

	memcpy(conn->request_path, buf, len);
	conn->request_path[len] = '\0';
	conn->have_path = 1;

	return 0;
}

static void connection_prepare_send_reply_header(struct connection *conn)
{
	conn->send_len = snprintf(conn->send_buffer, sizeof(conn->send_buffer),
							  "HTTP/1.1 200 OK\r\nServer: Apache/2.2.9\r\n"
							  "Accept-Ranges: bytes\r\n"
							  "Content-Length: %ld\r\n"
							  "Vary: Accept-Encoding\r\n"
							  "Connection: close\r\n"
							  "Content-Type: text/html\r\n\r\n",
							  conn->file_size);
}

static void connection_prepare_send_404(struct connection *conn)
{
	/* Prepare the connection buffer to send the 404 header. */
	const char *sent = "HTTP/1.1 404 Not Found\r\nContent-Type: text/html\r\nConnection: close\r\n\r\n<html><body><h1>404 Not Found</h1></body></html>\n";
	
	while (send(conn->sockfd, sent, strlen(sent), 0) > 0)
		;
}

static enum resource_type connection_get_resource_type(struct connection *conn)
{
	/* Get resource type depending on request path/filename. Filename
	 * should point to the static or dynamic folder.
	 */
	if (strstr(conn->request_path, "/static/") == conn->request_path)
		conn->res_type = RESOURCE_TYPE_STATIC;
	else if (strstr(conn->request_path, "/dynamic/") == conn->request_path)
		conn->res_type = RESOURCE_TYPE_DYNAMIC;
	else
		conn->res_type = RESOURCE_TYPE_NONE;
	return conn->res_type;
}

struct connection *connection_create(int sockfd)
{
	/* Initialize connection structure on given socket. */
	struct connection *conn = malloc(sizeof(struct connection));

	DIE(conn == NULL, "malloc");

	*conn = (struct connection){
		.sockfd = sockfd,
		.fd = -1,
		.state = STATE_INITIAL,
		.recv_len = 0,
		.send_pos = 0,
		.send_len = 0,
		.file_pos = 0,
		.file_size = 0,
		.recv_buffer[0] = '\0',
		.send_buffer[0] = '\0',
		.eventfd = eventfd(0, EFD_NONBLOCK),
	};

	return conn;
}

void connection_start_async_io(struct connection *conn)
{
	io_prep_pread(&conn->iocb, conn->fd, conn->send_buffer,
				  sizeof(conn->send_buffer), conn->file_pos);
	conn->piocb[0] = &conn->iocb;
	w_epoll_add_ptr_in(epollfd, conn->eventfd, conn);
	io_set_eventfd(&conn->iocb, conn->eventfd);
	w_epoll_update_ptr_in(epollfd, conn->sockfd, conn);
	DIE(io_submit(ctx, 1, conn->piocb) != 1, "io_submit");
	conn->ctx = ctx;
}

void connection_remove(struct connection *conn)
{
	DIE(conn == NULL, "connection_remove");
	close(conn->fd);
	close(conn->sockfd);
	free(conn);
}

void handle_new_connection(void)
{
	int client_socket = accept(listenfd, NULL, NULL);

	DIE(client_socket == -1, "accept");

	int flags, rc;

	flags = fcntl(client_socket, F_GETFL, 0);
	DIE(flags == -1, "fcntl");

	rc = fcntl(client_socket, F_SETFL, flags | O_NONBLOCK);
	DIE(rc == -1, "fcntl");
	struct connection *conn = connection_create(client_socket);

	w_epoll_add_ptr_in(epollfd, conn->sockfd, conn);
}

void receive_data(struct connection *conn)
{
	/* Receive message on socket.
	 * Store message in recv_buffer in struct connection.
	 */
	char buffer[BUFSIZ];
	ssize_t bytes = recv(conn->sockfd, buffer, BUFSIZ, 0);

	while (bytes > 0) {
		strcat(conn->recv_buffer, buffer);
		conn->recv_len += bytes;
		bytes = recv(conn->sockfd, buffer, BUFSIZ, 0);
	}

	if (parse_header(conn) == -1 || connection_open_file(conn) == -1)
		conn->state = STATE_SENDING_404;
	else
		conn->state = STATE_REQUEST_RECEIVED;
}

int connection_open_file(struct connection *conn)
{
	int fd = open(conn->filename, O_RDONLY);
	struct stat st;

	if (fd == -1) {
		ERR("open");
		return -1;
	}
	if (fstat(fd, &st) == -1) {
		ERR("fstat");
		close(fd);
		return -1;
	}

	conn->file_size = st.st_size;
	conn->file_pos = 0;
	conn->fd = fd;

	return 0;
}

void connection_complete_async_io(struct connection *conn)
{
	struct io_event events[EVENT_SIZE];

	DIE(io_getevents(conn->ctx, 1, 1, events, NULL) == -1, "io_getevents");
	DIE(events[0].res < 0, "io_getevents");

	conn->send_pos = 0;
	conn->send_len = events[0].res;
	conn->file_pos += events[0].res;
	conn->state = STATE_SENDING_DYNAMIC;
	w_epoll_remove_ptr(epollfd, conn->eventfd, conn);
	w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);

}

int parse_header(struct connection *conn)
{
	http_parser *parser = malloc(sizeof(http_parser));

	http_parser_init(parser, HTTP_REQUEST);
	parser->data = conn;
	http_parser_settings settings_on_path;

	memset(&settings_on_path, 0, sizeof(settings_on_path));
	settings_on_path.on_path = aws_on_path_cb;
	settings_on_path.on_body = 0;
	settings_on_path.on_url = 0;
	settings_on_path.on_message_begin = 0;
	settings_on_path.on_message_complete = 0;
	settings_on_path.on_header_field = 0;
	settings_on_path.on_header_value = 0;
	settings_on_path.on_headers_complete = 0;
	settings_on_path.on_query_string = 0;
	settings_on_path.on_fragment = 0;

	http_parser_execute(parser, &settings_on_path, conn->recv_buffer, conn->recv_len);

	if (!conn->have_path)
		return 1;

	switch (connection_get_resource_type(conn)) {
	case RESOURCE_TYPE_STATIC:
		snprintf(conn->filename, BUFSIZ, "%s%s", AWS_DOCUMENT_ROOT, conn->request_path + 1);
		break;
	case RESOURCE_TYPE_DYNAMIC:
		snprintf(conn->filename, BUFSIZ, "%s%s", AWS_DOCUMENT_ROOT, conn->request_path + 1);
		break;
	default:
		connection_prepare_send_404(conn);
		return -1;
	}
	return 0;
}

enum connection_state connection_send_static(struct connection *conn)
{
	/* Send static data using sendfile(2). */
	off_t offset = conn->file_pos;
	size_t size = conn->file_size - conn->file_pos;
	int package_size = sendfile(conn->sockfd, conn->fd, &offset, size);

	if (package_size > 0) {
		conn->file_pos += package_size;
		if (conn->file_pos == conn->file_size) {
			conn->state = STATE_DATA_SENT;
			return STATE_DATA_SENT;
		}
		conn->state = STATE_SENDING_DATA;
		return STATE_SENDING_DATA;

	} else if (errno != EWOULDBLOCK && errno != EAGAIN) {
		ERR("sendfile");
		conn->state = STATE_SENDING_404;
		return conn->state;
	}
	return STATE_SENDING_DATA;
}

int connection_send_data(struct connection *conn)
{
	int total_sent = 0;

	do {
		size_t bytes_left = conn->send_len - conn->send_pos;
		int bytes_sent = send(conn->sockfd, conn->send_buffer + conn->send_pos, bytes_left, 0);

		total_sent += bytes_sent;
		conn->send_pos += bytes_sent;
	} while (conn->send_len > conn->send_pos);

	return total_sent;
}

void handle_input(struct connection *conn)
{
	switch (conn->state) {
	case STATE_ASYNC_ONGOING:
		connection_complete_async_io(conn);
		break;

	case STATE_INITIAL:
		conn->state = STATE_RECEIVING_DATA;
		receive_data(conn);
		w_epoll_update_ptr_out(epollfd, conn->sockfd, conn);
		break;

	default:
		printf("shouldn't get here %d\n", conn->state);
		break;
	}
}

void handle_output(struct connection *conn)
{
	switch (conn->state) {
	case STATE_REQUEST_RECEIVED:
		conn->state = STATE_SENDING_HEADER;
		connection_prepare_send_reply_header(conn);
		break;

	case STATE_SENDING_HEADER:
		conn->state = STATE_HEADER_SENT;
		connection_send_data(conn);
		break;

	case STATE_HEADER_SENT:
		switch (conn->res_type) {
		case RESOURCE_TYPE_STATIC:
			conn->state = STATE_SENDING_DATA;
			break;
		case RESOURCE_TYPE_DYNAMIC:
			conn->state = STATE_SENDING_DYNAMIC;
			break;
		default:
			conn->state = STATE_SENDING_404;
			break;
		}
		break;

	case STATE_SENDING_404:
		conn->state = STATE_404_SENT;
		connection_prepare_send_404(conn);
		break;

	case STATE_404_SENT:
		connection_remove(conn);
		break;

	case STATE_SENDING_DATA:
		switch (conn->res_type) {
		case RESOURCE_TYPE_STATIC:
			conn->state = connection_send_static(conn);
			break;
		case RESOURCE_TYPE_DYNAMIC:
			conn->state = STATE_ASYNC_ONGOING;
			connection_start_async_io(conn);
			break;
		default:
			conn->state = STATE_SENDING_404;
			break;
		}
		break;

	case STATE_DATA_SENT:
		connection_remove(conn);
		break;

	case STATE_SENDING_DYNAMIC:
		connection_send_data(conn);
		if (conn->file_pos != conn->file_size)
			conn->state = STATE_SENDING_DATA;
		else
			conn->state = STATE_DATA_SENT;
		break;

	case STATE_ASYNC_ONGOING:
		connection_start_async_io(conn);
		break;

	default:
		ERR("Unexpected state\n");
		exit(1);
	}
}

void handle_client(uint32_t events, struct connection *conn)
{
	if (events & (EPOLLHUP | EPOLLERR))
		connection_remove(conn);

	if (events & EPOLLIN)
		handle_input(conn);

	if (events & EPOLLOUT)
		handle_output(conn);
}

int main(void)
{
	int rc;

	ctx = 0;
	rc = io_setup(1, &ctx);
	DIE(rc != 0, "io_setup");

	epollfd = w_epoll_create();
	DIE(epollfd < 0, "w_epoll_create");

	listenfd = tcp_create_listener(AWS_LISTEN_PORT, DEFAULT_LISTEN_BACKLOG);
	DIE(listenfd < 0, "tcp_create_listener");

	rc = w_epoll_add_fd_in(epollfd, listenfd);
	DIE(rc < 0, "w_epoll_add_fd_in");

	/* server main loop */
	while (1) {
		struct epoll_event rev;
		int nfds = epoll_wait(epollfd, &rev, 1, -1);

		DIE(nfds < 0, "w_epoll_wait");

		if (rev.data.fd == listenfd) {
			handle_new_connection();
		} else {
			struct connection *conn = (struct connection *)rev.data.ptr;

			handle_client(rev.events, conn);
		}
	}
	return 0;
}
