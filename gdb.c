#include "gdb.h"
#include "cpu/cpu.h"
#include "cpu/core.h"

#include <sys/socket.h>
#include <netinet/in.h>

#define GDB_MSG_MAX 128

static int sock = 0;
static char escape_chars[] = "}$#*";
//static char empty_resp[] = "$#00";

void gdb_init(int port) {
	assert((sock == 0) && "Multiple calls to gdb_init");

	int serv_sock;
	serv_sock = socket(AF_INET, SOCK_STREAM, 0);
	if (serv_sock == -1) {
		perror("opening gdb socket");
		exit(errno);
	}

	struct sockaddr_in server;
	server.sin_family = AF_INET;
	server.sin_addr.s_addr = htonl(INADDR_ANY);
	server.sin_port = htons(port);

	if (-1 == bind(serv_sock, (struct sockaddr*) &server, sizeof server)) {
		perror("binding gdb socket");
		exit(errno);
	}

	socklen_t length = sizeof server;
	if (-1 == getsockname(serv_sock, (struct sockaddr*) &server, &length)) {
		perror("getting gdb port");
		exit(errno);
	}

	listen(serv_sock, 1);

	INFO("Waiting for gdb connection on port %d\n",
			ntohs(server.sin_port));

	sock = accept(serv_sock, NULL, 0);
	if (sock == -1) {
		perror("accepting gdb connection");
		exit(errno);
	}

	// Grab the opening '+'
	char c;
	assert(1 == recv(sock, &c, 1, 0));
	assert(c == '+');

	// Get the initial gdb support message
	const char *msg;
	int len;
	msg = gdb_get_message(&len);

	printf("Got msg >>>%s<<<, len %d, strlen %zd\n", msg, len, strlen(msg));

	// Respond with out message
	const char resp[] = "qSupported:PacketSize="VAL2STR(GDB_MSG_MAX-1)";qReverseContinue+;qReverseStep+";

	gdb_send_message(resp);
}

char* gdb_get_message(int *ext_len) {
	static char buf[GDB_MSG_MAX];
	memset(buf, 0, GDB_MSG_MAX);

	int read = 0;
	int len = 0;

	do {
		assert(read < GDB_MSG_MAX);

		int ret;
		ret = recv(sock, buf+read, GDB_MSG_MAX-read, 0);

		if (ret < 0) {
			if (ret == EINTR) {
				ERR(E_NOT_IMPLEMENTED, "TODO: GDB handle EINTR case\n");
			} else
			if (ret == ENOMEM) {
				// try again
				;
			} else {
				perror("receving from gdb");
				exit(errno);
			}
		} else
		if (ret == 0) {
			ERR(E_UNKNOWN, "Connection closed unexpectedly\n");
		} else {
			read += ret;
		}

		char *end = memchr(buf, '#', GDB_MSG_MAX);
		len = end ? (end - buf + 1) : 0;
	} while (len == 0);

	assert((len+2) < GDB_MSG_MAX);

	if (read > (len + 2)) {
		WARN("Read beyond one message boundary\n");
		WARN("read %d len %d (len + 2 %d)\n", read, len, len+2);
		WARN(">>>%s<<<\n", buf);
		WARN("This should not be possible, as gdb commands are single messages\n");
		ERR(E_UNPREDICTABLE, "Lost protocol sync?\n");
	} else
	if (read < (len + 2)) {
		assert(recv(sock, buf+read, (len+2)-read, MSG_WAITALL) == (len+2)-read);
	}

	unsigned char csum = 0;
	char *cur = buf + 1;
	while (*cur != '#')
		csum += *cur++;

	if (csum != strtol(buf + len, NULL, 16)) {
		WARN("Checksum mismatch!\n");
		WARN("Expected: %2x Got: %s\n", csum, buf+len);
		WARN("buf: >>>%s<<<\n", buf);
		ERR(E_UNPREDICTABLE, "Bad Checksum\n");
	} else {
		// Send ACK
		send(sock, "+", 1, 0);
	}

	printf("Got message >>>%s<<<, len %d, my_csum %2x\n", buf, len+2, csum);

	if (NULL != ext_len)
		*ext_len = len - 2; // omit $ and #

	buf[len - 1] = '\0'; // overwrite #

	return buf + 1; // point ahead of $
}

static void _gdb_send(const char c) {
	static int csum = 0;
	if (c == '$') {
		printf("Sending: >$");
		csum = 0;
	}
	else if (c == '#') {
		printf("#");
		csum = 2;
	}
	else if (csum > 1) {
		printf("%c", c);
		csum--;
	}
	else if (csum == 1) {
		printf("%c<\n", c);
		csum--;
	} else {
		printf("%c", c);
	}

	int ret = send(sock, &c, 1, 0);
	if (ret != 1) {
		perror(PP_STRING" W");
		ERR(E_UNKNOWN, "Failed to send message to gdb\n");
	}
}

static void gdb_send(const char *msg, int len) {
	while (len) {
		const char *c = escape_chars;
		while (*c != '\0') {
			if (*c == *msg) {
				_gdb_send('}');
				_gdb_send(*msg ^ 0x20);
				break;
			}
			c++;
		}
		if (*c == '\0') {
			_gdb_send(*msg);
		}
		msg++;
		len--;
	}
}

void gdb_send_message(const char *msg) {
	unsigned char csum = 0;
	const char *cur = msg;
	while (*cur != '\0')
		csum += *cur++;

	char csum_str[3];
	sprintf(csum_str, "%02x", csum);

	_gdb_send('$');
	gdb_send(msg, strlen(msg));
	_gdb_send('#');
	_gdb_send(csum_str[0]);
	_gdb_send(csum_str[1]);

	char c;
	assert(1 == recv(sock, &c, 1, 0));
	if (c == '+')
		return;
	else if (c == '-')
		return gdb_send_message(msg);
	else
		ERR(E_UNKNOWN, "Expected + or -, got %c\n", c);
}

void stop_and_wait_for_gdb(void) {
	gdb_send_message("S05");
	wait_for_gdb();
}

/* We expect gdb to issue some form of command to indicate how
   long we should run. This function (or its delegates) should
   set variables such that the simulator stops as requested, and
   should return when prepared for the simulator to run freely */
void wait_for_gdb(void) {
	// In general, the empty response is allowed for unknown messages. For now,
	// we try to explicitly identify the messages we don't recognize, and bail
	// out on completely new ones.
	while (true) {
		printf("Waiting for a message from gdb...\n");
		char *cmd = gdb_get_message(NULL);

		switch (cmd[0]) {
			case '?':
				// Report why the target halted
				// Right now SIGTRAP is the only option
				gdb_send_message("S05");
				break;

			case 'g':
			{
				const int reg_str_len = 16 * 8 + 1;
				char buf[reg_str_len];

				snprintf(buf, reg_str_len,
						"%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x%08x", 
						htonl(CORE_reg_read(0)),
						htonl(CORE_reg_read(1)),
						htonl(CORE_reg_read(2)),
						htonl(CORE_reg_read(3)),
						htonl(CORE_reg_read(4)),
						htonl(CORE_reg_read(5)),
						htonl(CORE_reg_read(6)),
						htonl(CORE_reg_read(7)),
						htonl(CORE_reg_read(8)),
						htonl(CORE_reg_read(9)),
						htonl(CORE_reg_read(10)),
						htonl(CORE_reg_read(11)),
						htonl(CORE_reg_read(12)),
						htonl(CORE_reg_read(13)),
						htonl(CORE_reg_read(14)),
						htonl(CORE_reg_read(15))
					);

				gdb_send_message(buf);
				break;
			}

			case 'H':
			{
				if (0 == strcmp("Hc-1", cmd)) {
					// Indicates that all future commands should be
					// applied to all running threads, we only have
					// the one, so we're cool with that
					gdb_send_message("OK");
				} else
				if (0 == strcmp("Hc0", cmd)) {
					// Future step/cont commands on any thread
					gdb_send_message("OK");
				} else {
					goto unknown_gdb;
				}
				break;
			}

			case 'k':
				// kill
				INFO("Killed by remote debugger, dying\n");
				sim_terminate();

			case 'm':
			{
				// m addr,length

				// XXX: Make efficient / pretty
				const char *address = strtok(cmd+1, ",");
				const char *length = strtok(NULL, ",");
				assert(NULL != length);
				int addr = strtol(address, NULL, 16);
				int len = strtol(length, NULL, 16);
				int resp_len = len * 2 + 1;

				printf("addr %d (%s) len %d (%s)\n", addr, address, len, length);
				{
					char buf[resp_len];
					char *head = buf;
					while (len) {
						sprintf(head, "%02x", read_byte(addr));
						head += 2;
						addr++;
						len--;
					}

					gdb_send_message(buf);
				}
				break;
			}

			case 'q':
			{
				if (0 == strcmp("qC", cmd)) {
					// Asking for info on current thread, we don't
					// support multithread, so an empty response says
					// the 'default' thread is running, which is okay
					gdb_send_message("");
				} else {
					goto unknown_gdb;
				}
				break;
			}

			case 's':
			{
				if (0 == strcmp("s", cmd)) {
					dumpatcycle = cycle + 1;
					return;
				} else {
					goto unknown_gdb;
				}
				break;
			}

			/* Okay, never mind this for now, this interface is just too
			   poorly documented to be worth it.
			case 'v':
			{
				if (0 == strcmp("vCont?", cmd)) {
					// Probing for supported vCont commands
					// Which is a lie, since gdb will only succeed if
					// you report that you support all of them. Largely
					// we do however (signals make no sense in this
					// context), so let's lie a bit
					gdb_send_message("vCont;c;C;s;S;t");
				} else
				if (0 == strcmp("vCont;c", cmd)) {
					// "Continue" command
					return;
				} else
				if (0 == strcmp("vCont;s", cmd)) {
					dumpatcycle = cycle+1;
				} else {
					goto unknown_gdb;
				}
				break;
			}
			*/

			default:
unknown_gdb:
				WARN("Unknown gdb command: %s\n", cmd);
				gdb_send_message("");
		}
	}
}