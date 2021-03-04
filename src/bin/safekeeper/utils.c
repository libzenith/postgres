#include "safekeeper.h"
#include "common/logging.h"
#include "common/ip.h"
#include <netinet/tcp.h>
#include <unistd.h>

int CompareNodeId(NodeId* id1, NodeId* id2)
{
	return
		(id1->term < id2->term)
		? -1
		: (id1->term > id2->term)
		   ? 1
   		   : memcmp(&id1->uuid, &id1->uuid, sizeof(pg_uuid_t));
}

static bool
SetSocketOptions(pgsocket sock)
{
	char sebuf[PG_STRERROR_R_BUFLEN];
	int on = 1;
	if (setsockopt(sock, IPPROTO_TCP, TCP_NODELAY,
				   (char *) &on, sizeof(on)) < 0)
	{
		pg_log_info("setsockopt(%s) failed: %s", "TCP_NODELAY",
					SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
		closesocket(sock);
		return false;
	}
	return true;
}

pgsocket
ConnectSocketAsync(char const* host, char const* port, bool* established)
{
	struct addrinfo *addrs = NULL,
		*addr,
		hints;
	int	tries = 0;
	int	ret;
	pgsocket sock = PGINVALID_SOCKET;
	char sebuf[PG_STRERROR_R_BUFLEN];

	/*
	 * Create the UDP socket for sending and receiving statistic messages
	 */
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;
	hints.ai_addrlen = 0;
	hints.ai_addr = NULL;
	hints.ai_canonname = NULL;
	hints.ai_next = NULL;
	ret = pg_getaddrinfo_all(host, port, &hints, &addrs);
	if (ret || !addrs)
	{
		pg_log_error("Could not resolve \"localhost\": %s",
					 gai_strerror(ret));
		return -1;
	}
	for (addr = addrs; addr; addr = addr->ai_next)
	{
		sock = PGINVALID_SOCKET;
		if (++tries > 1)
			pg_log_info("trying another address for the statistics collector");

		sock = socket(addr->ai_family, SOCK_STREAM|SOCK_NONBLOCK, 0);
		if (sock == PGINVALID_SOCKET)
		{
			pg_log_info("could not create socket for statistics collector: %s",
						SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
			continue;
		}
		if (!SetSocketOptions(sock))
			continue;

		/*
		 * Bind it to a kernel assigned port on localhost and get the assigned
		 * port via getsockname().
		 */
		while ((ret = connect(sock, addr->ai_addr, addr->ai_addrlen)) < 0 && errno == EINTR);
		if (ret < 0)
		{
			if (errno == EWOULDBLOCK || errno == EAGAIN)
			{
				*established = false;
				break;
			}
			pg_log_info("Could not establish connection to %s:%s: %s",
						host, port, SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
			closesocket(sock);
		}
		else
		{
			*established = true;
			break;
		}
	}
	return sock;
}

pgsocket
CreateSocket(char const* host, char const* port, int n_peers)
{
	struct addrinfo *addrs = NULL,
		*addr,
		hints;
	int	tries = 0;
	int	ret;
	char sebuf[PG_STRERROR_R_BUFLEN];
	pgsocket sock = PGINVALID_SOCKET;

	/*
	 * Create the UDP socket for sending and receiving statistic messages
	 */
	hints.ai_flags = AI_PASSIVE;
	hints.ai_family = AF_UNSPEC;
	hints.ai_socktype = SOCK_STREAM;
	hints.ai_protocol = 0;
	hints.ai_addrlen = 0;
	hints.ai_addr = NULL;
	hints.ai_canonname = NULL;
	hints.ai_next = NULL;
	ret = pg_getaddrinfo_all(host, port, &hints, &addrs);
	if (ret || !addrs)
	{
		pg_log_error("Could not resolve \"localhost\": %s",
					 gai_strerror(ret));
		return -1;
	}
	for (addr = addrs; addr; addr = addr->ai_next)
	{
		sock = PGINVALID_SOCKET;
		if (++tries > 1)
			pg_log_info("trying another address for the statistics collector");

		sock = socket(addr->ai_family, SOCK_STREAM|SOCK_NONBLOCK, 0);
		if (sock == PGINVALID_SOCKET)
		{
			pg_log_info("could not create socket for statistics collector: %s",
						SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
			continue;
		}
		/*
		 * Bind it to a kernel assigned port on localhost and get the assigned
		 * port via getsockname().
		 */
		if (bind(sock, addr->ai_addr, addr->ai_addrlen) < 0)
		{
			pg_log_info("Could not bind socket for statistics collector: %s",
						SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
			closesocket(sock);
			continue;
		}
		ret = listen(sock, n_peers);
		if (ret < 0)
		{
			pg_log_info("Could not listen: %s", SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
			closesocket(sock);
			continue;
		}
		if (SetSocketOptions(sock))
			break;
	}
	return sock;
}

bool
WriteSocket(pgsocket sock, void const* buf, size_t size)
{
	char* src = (char*)buf;
	char sebuf[PG_STRERROR_R_BUFLEN];

	while (size != 0)
	{
		ssize_t rc = send(sock, src, size, 0);
		if (rc < 0)
		{
			if (errno == EINTR)
				continue;
			pg_log_error("Socket write failed: %s",
						 SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
			return false;
		}
		size -= rc;
		src += rc;
	}
	return true;
}

ssize_t
ReadSocketAsync(pgsocket sock, void* buf, size_t size)
{
	size_t offs = 0;
	char sebuf[PG_STRERROR_R_BUFLEN];

	while (size != offs)
	{
		ssize_t rc = recv(sock, (char*)buf + offs, size - offs, 0);
		if (rc < 0)
		{
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return offs;
			pg_log_error("Socket write failed: %s",
						 SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
			return -1;
		}
		offs += rc;
	}
	return offs;
}

ssize_t
WriteSocketAsync(pgsocket sock, void const* buf, size_t size)
{
	size_t offs = 0;
	char sebuf[PG_STRERROR_R_BUFLEN];

	while (size != offs)
	{
		ssize_t rc = send(sock, (char const*)buf + offs, size - offs, 0);
		if (rc < 0)
		{
			if (errno == EINTR)
				continue;
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				return offs;
			pg_log_error("Socket write failed: %s",
						 SOCK_STRERROR(SOCK_ERRNO, sebuf, sizeof(sebuf)));
			return -1;
		}
		offs += rc;
	}
	return offs;
}

bool
SaveData(char const* path, void const* data, size_t size)
{
	int fd = open(path, O_WRONLY | O_CREAT | O_TRUNC | PG_BINARY, 0);
	if (fd < 0)
	{
		pg_log_error("Failed to create file %s: %s",
					 path, strerror(errno));
		return false;
	}
	if ((size_t)write(fd, data, size) != size)
	{
		pg_log_error("Failed to write file %s: %s",
					 path, strerror(errno));
		close(fd);
		return false;
	}
	if (fsync(fd) < 0)
	{
		pg_log_error("Failed to fsync file %s: %s",
					 path, strerror(errno));
		close(fd);
		return false;
	}
	close(fd);
	return true;
}

bool
LoadData(char const* path, void* data, size_t size)
{
	int fd = open(path, O_RDONLY | PG_BINARY, 0);
	if (fd < 0)
	{
		pg_log_error("Failed to open file %s: %s",
					 path, strerror(errno));
		return false;
	}
	if ((size_t)read(fd, data, size) != size)
	{
		pg_log_error("Failed to write file %s: %s",
					 path, strerror(errno));
		close(fd);
		return false;
	}
	close(fd);
	return true;
}

int
CompareLsn(const void *a, const void *b)
{
	XLogRecPtr	lsn1 = *((const XLogRecPtr *) a);
	XLogRecPtr	lsn2 = *((const XLogRecPtr *) b);

	if (lsn1 > lsn2)
		return -1;
	else if (lsn1 == lsn2)
		return 0;
	else
		return 1;
}
