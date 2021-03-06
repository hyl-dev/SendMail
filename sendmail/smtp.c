#include <arpa/inet.h>
#include <errno.h>
#include <netinet/in.h>
#include <netdb.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "smtp.h"
#include "lib/base64.h"

enum SMTP_ERROR {
	SMTP_ERROR_OK,

	SMTP_ERROR_SOCKET,

	SMTP_ERROR_CONNECT,

	SMTP_ERROR_DOMAIN,

	SMTP_ERROR_READ,

	SMTP_ERROR_WRITE,

	SMTP_ERROR_SERVER_STATUS
};

#define SMTP_BUFFER_SIZE 1024

/**
 * @struct smtp
 */
struct smtp {
	const char* domain;
	const char* user_name;
	const char* password;
	const char* subject;
	const char* content;
	const char** to;
	int to_len;
	int status;
	int socket;
	char buffer[SMTP_BUFFER_SIZE];
	char* cmd;
	char* data;
};

enum SMTP_STATUS {
	SMTP_STATUS_NULL,     //!< SMTP_STATUS_NULL
	SMTP_STATUS_EHLO,     //!< SMTP_STATUS_EHLO
	SMTP_STATUS_AUTH,     //!< SMTP_STATUS_AUTH
	SMTP_STATUS_SEND,     //!< SMTP_STATUS_SEND
	SMTP_STATUS_QUIT,     //!< SMTP_STATUS_QUIT
	SMTP_STATUS_MAX       //!< SMTP_STATUS_MAX
};

static int smtp_read(struct smtp* sm) {
	for (;;) {
		int size = recv(sm->socket, sm->buffer, SMTP_BUFFER_SIZE - 1, 0);
		if (size == -1) {
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				continue;
		}
		if (size <= 0)
			break;

		sm->buffer[size] = 0;
		printf("SERVER: %s\n", sm->buffer);
		sm->cmd = sm->buffer;
		char* p = strchr(sm->buffer, ' ');
		if (p) {
			*p = '\0';
			sm->data = p + 1;
		}

		return 0;
	}

	return SMTP_ERROR_READ;
}

int smtp_write(int fd, const char* buffer, int size) {
	for (int send_num = 0; send_num < size;) {
		int error = send(fd, &buffer[send_num], size - send_num, 0);
		if (error < 0) {
			printf("errno = %d size = %d send_num = %d", errno, size,
					send_num);
			if (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)
				continue;
			return SMTP_ERROR_WRITE;
		} else
			send_num += error;
	}

	printf("CLIENT: %s\n", buffer);

	return 0;
}

static char* explode(struct smtp* sm) {
	char* old = sm->data;
	char* p = old;
	while (*p) {
		if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')
				|| (*p >= '0' && *p <= '9') || *p == '+' || *p == '/'
				|| *p == '=') {
			p++;
		} else {
			sm->data = p;
			*p = '\0';
			break;
		}
	}

	return old;
}

static int hello(struct smtp* sm) {
	char buffer[256];
	int size = sprintf(buffer, "HELO %s\r\n", sm->domain);

	if (smtp_write(sm->socket, buffer, size))
		return SMTP_ERROR_WRITE;

	if (smtp_read(sm) || strcmp(sm->cmd, "250"))
		return SMTP_ERROR_READ;

	sm->status = SMTP_STATUS_AUTH;

	return 0;
}

static int auth(struct smtp* sm) {
	if (smtp_write(sm->socket, "AUTH LOGIN\r\n", strlen("AUTH LOGIN\r\n")))
		return SMTP_ERROR_WRITE;
	if (smtp_read(sm) || strcmp(sm->cmd, "334"))
		return SMTP_ERROR_READ;

	char* p = explode(sm);
	char buffer[256];
	int size = base64_decode(p, strlen(p), buffer, 256);
	if (size < 0)
		return SMTP_ERROR_SERVER_STATUS;

	buffer[size] = 0;

	if (strcasecmp(buffer, "username:"))
		return SMTP_ERROR_SERVER_STATUS;
	size = base64_encode(sm->user_name, strlen(sm->user_name), buffer, 256);
	if (size < 0 || size + 2 > 256)
		return SMTP_ERROR_WRITE;
	buffer[size++] = '\r';
	buffer[size++] = '\n';
	buffer[size] = '\0';
	if (smtp_write(sm->socket, buffer, size))
		return SMTP_ERROR_WRITE;
	if (smtp_read(sm) || strcmp(sm->cmd, "334"))
		return SMTP_ERROR_READ;

	p = explode(sm);
	size = base64_decode(p, strlen(p), buffer, 256);
	if (size < 0)
		return SMTP_ERROR_SERVER_STATUS;
	buffer[size] = 0;
	if (strcasecmp(buffer, "password:"))
		return SMTP_ERROR_SERVER_STATUS;
	size = base64_encode(sm->password, strlen(sm->password), buffer, 256);
	if (size < 0 || size + 2 > 256)
		return SMTP_ERROR_WRITE;
	buffer[size++] = '\r';
	buffer[size++] = '\n';
	buffer[size] = '\0';
	if (smtp_write(sm->socket, buffer, size))
		return SMTP_ERROR_WRITE;

	if (smtp_read(sm) || strcmp(sm->cmd, "235"))
		return SMTP_ERROR_READ;
	sm->status = SMTP_STATUS_SEND;

	return 0;
}

static char* smtp_time(char* buffer) {
	time_t t2;
	time(&t2);

	struct tm t;
	localtime_r(&t2, &t);

	static char* week[] = { "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
	static char* month[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul",
			"Aug", "Sep", "Oct", "Nov", "Dec" };
	sprintf(buffer, "%s, %02d %s %04d %02d:%02d:%02d %s%02d%2d",
			week[t.tm_wday], t.tm_mday, month[t.tm_mon], t.tm_year + 1900,
			t.tm_hour, t.tm_min, t.tm_sec, t.tm_gmtoff >= 0 ? "+" : "-",
			t.tm_gmtoff / 3600);
	return buffer;
}

static int send_mail(struct smtp* sm) {
	// MAIL FROM
	char buffer[256];
	int size = sprintf(buffer, "MAIL FROM: <%s>\r\n", sm->user_name);
	if (smtp_write(sm->socket, buffer, size))
		return SMTP_ERROR_WRITE;
	if (smtp_read(sm) || strcmp(sm->cmd, "250"))
		return SMTP_ERROR_READ;

	// RCPT TO
	int i;
	for (i = 0; i < sm->to_len; i++) {
		size = sprintf(buffer, "RCPT TO: <%s>\r\n", sm->to[i]);
		if (smtp_write(sm->socket, buffer, size))
			return SMTP_ERROR_WRITE;
		if (smtp_read(sm) || strcmp(sm->cmd, "250"))
			return SMTP_ERROR_READ;
	}

	if (smtp_write(sm->socket, "DATA\r\n", strlen("DATA\r\n")))
		return SMTP_ERROR_WRITE;
	if (smtp_read(sm) || strcmp(sm->cmd, "354"))
		return SMTP_ERROR_READ;

	int to_size = 0;
	for (i = 0; i < sm->to_len; i++)
		to_size += strlen(sm->to[i]);

	char header[to_size + 512 + strlen(sm->user_name)];
	int pos = strlen("MIME-Version: 1.0\r\nContent-Type: text/html\r\n");
	memcpy(header, "MIME-Version: 1.0\r\nContent-Type: text/html\r\n", pos);

	for (i = 0; i < sm->to_len; i++) {
		pos += sprintf(&header[pos], "To: %s\r\n", sm->to[i]);
	}
	pos += sprintf(&header[pos], "From: %s<%s>\r\n", sm->user_name,
			sm->user_name);
	pos += sprintf(&header[pos], "Subject: %s\r\n", sm->subject);
	pos += sprintf(&header[pos], "Message-ID: <%d.%s>\r\n", time(NULL),
			sm->user_name);

	char date[100];
	pos += sprintf(&header[pos], "Date: %s\r\n", smtp_time(date));

	if (smtp_write(sm->socket, header, pos))
		return SMTP_ERROR_WRITE;
	if (smtp_write(sm->socket, sm->content, strlen(sm->content)))
		return SMTP_ERROR_WRITE;
	if (smtp_write(sm->socket, "\r\n.\r\n", strlen("\r\n.\r\n")))
		return SMTP_ERROR_WRITE;
	if (smtp_read(sm) || strcmp(sm->cmd, "250"))
		return SMTP_ERROR_READ;

	sm->status = SMTP_STATUS_QUIT;

	return 0;
}

static int quit(struct smtp* sm) {
	if (smtp_write(sm->socket, "QUIT \r\n", strlen("QUIT \r\n")))
		return SMTP_ERROR_WRITE;
	if (smtp_read(sm) || strcmp(sm->cmd, "221"))
		return SMTP_ERROR_READ;

	sm->status = SMTP_STATUS_NULL;

	return 0;
}

typedef int (*SMTP_FUN)(struct smtp*);
static const SMTP_FUN smtp_fun[SMTP_STATUS_MAX] = { NULL, hello, auth,
		send_mail, quit };

int smtp_send(const char* domain, int port, const char* user_name,
		const char* password, const char* subject, const char* content,
		const char** to, int to_len) {
	struct hostent* host = gethostbyname(domain);
	if (!host) {
		printf("domain can not find锛乗n");
		return SMTP_ERROR_DOMAIN;
	}

	if (host->h_addrtype != AF_INET) {
		if (host->h_addrtype == AF_INET6)
			printf("ipv6 is not support!\n");
		else
			printf("address type is not support %d\n ", host->h_addrtype);
		return SMTP_ERROR_DOMAIN;
	}

	int sock_fd = socket(AF_INET, SOCK_STREAM, 0);
	if (-1 == sock_fd) {
		printf("can not create socket!\n");
		return SMTP_ERROR_SOCKET;
	}

	struct sockaddr_in local;
	local.sin_family = AF_INET;
	local.sin_port = htons(port);
	local.sin_addr = *(struct in_addr*) host->h_addr_list[0];
	memset(local.sin_zero, 0, sizeof(local.sin_zero));

	if (-1 == connect(sock_fd, (struct sockaddr*) &local, sizeof(local))) {
		printf("can not connect socket!\n");
		return SMTP_ERROR_CONNECT;
	}

	printf("connect ok ,ip address %s \n", inet_ntoa(local.sin_addr));

	struct smtp sm = { .domain = domain, .user_name = user_name, .password =
			password, .subject = subject, .content = content, .status =
			SMTP_STATUS_EHLO, .socket = sock_fd, .to = to, .to_len = to_len };

	if (smtp_read(&sm) || strcmp(sm.cmd, "220"))
		return SMTP_ERROR_READ;

	while (sm.status != SMTP_STATUS_NULL) {
		int error = smtp_fun[sm.status](&sm);
		if (error) {
			printf("error = %d\n", error);
			return error;
		}
	}

	close(sock_fd);

	return 0;
}
