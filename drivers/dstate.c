/* dstate.c - Network UPS Tools driver-side state management

   Copyright (C)
	2003	Russell Kroll <rkroll@exploits.org>
	2008	Arjen de Korte <adkorte-guest@alioth.debian.org>

   This program is free software; you can redistribute it and/or modify
   it under the terms of the GNU General Public License as published by
   the Free Software Foundation; either version 2 of the License, or
   (at your option) any later version.

   This program is distributed in the hope that it will be useful,
   but WITHOUT ANY WARRANTY; without even the implied warranty of
   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
   GNU General Public License for more details.

   You should have received a copy of the GNU General Public License
   along with this program; if not, write to the Free Software
   Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA 02111-1307 USA
*/

#include <stdio.h>
#ifndef WIN32
#include <stdarg.h>
#include <sys/stat.h>
#include <pwd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/un.h>
#else
#include <stdio.h> 
#include <strings.h> 
#endif

#include "common.h"
#include "dstate.h"
#include "state.h"
#include "parseconf.h"

#ifndef WIN32
	static int	sockfd = -1, stale = 1, alarm_active = 0;
	static char	*sockfn = NULL;
#else
	static HANDLE 	sockfd = INVALID_HANDLE_VALUE; 
	static int 	stale = 1, alarm_active = 0;
	static OVERLAPPED connect_overlapped;
	static char	*pipename = NULL;
#endif
	static char	status_buf[ST_MAX_VALUE_LEN], alarm_buf[ST_MAX_VALUE_LEN];
	static st_tree_t	*dtree_root = NULL;
	static conn_t	*connhead = NULL;
	static cmdlist_t *cmdhead = NULL;

	struct ups_handler	upsh;

/* this may be a frequent stumbling point for new users, so be verbose here */
static void sock_fail(const char *fn)
{
#ifndef WIN32
/*FIXME*/
	int	sockerr;
	struct passwd	*user;

	/* save this so it doesn't get overwritten */
	sockerr = errno;

	/* dispense with the usual upslog stuff since we have stderr here */

	printf("\nFatal error: unable to create listener socket\n\n");
	printf("bind %s failed: %s\n", fn, strerror(sockerr));

	user = getpwuid(getuid());

	if (!user) {
		fatal_with_errno(EXIT_FAILURE, "getpwuid");
	}

	/* deal with some common problems */
	switch (errno)
	{
	case EACCES:
		printf("\nCurrent user: %s (UID %d)\n\n",
			user->pw_name, (int)user->pw_uid);

		printf("Things to try:\n\n");
		printf(" - set different owners or permissions on %s\n\n", 
			dflt_statepath());
		printf(" - run this as some other user "
			"(try -u <username>)\n");
		break;

	case ENOENT:
		printf("\nThings to try:\n\n");
		printf(" - mkdir %s\n", dflt_statepath());
		break;

	case ENOTDIR:
		printf("\nThings to try:\n\n");
		printf(" - rm %s\n\n", dflt_statepath());
		printf(" - mkdir %s\n", dflt_statepath());
		break;
	}
	
	/*
	 * there - that wasn't so bad.  every helpful line of code here 
	 * prevents one more "help me" mail to the list a year from now
	 */
#endif
	printf("\n");
	fatalx(EXIT_FAILURE, "Exiting.");
}

#ifndef WIN32
static int sock_open(const char *fn)
{
	int	ret, fd;
	struct sockaddr_un	ssaddr;

	fd = socket(AF_UNIX, SOCK_STREAM, 0);

	if (fd < 0) {
		fatal_with_errno(EXIT_FAILURE, "Can't create a unix domain socket");
	}

	/* keep this around for the unlink() when exiting */
	sockfn = xstrdup(fn);

	ssaddr.sun_family = AF_UNIX;
	snprintf(ssaddr.sun_path, sizeof(ssaddr.sun_path), "%s", sockfn);

	unlink(sockfn);

	/* group gets access so upsd can be a different user but same group */
	umask(0007);

	ret = bind(fd, (struct sockaddr *) &ssaddr, sizeof ssaddr);

	if (ret < 0) {
		sock_fail(sockfn);
	}

	ret = chmod(sockfn, 0660);

	if (ret < 0) {
		fatal_with_errno(EXIT_FAILURE, "chmod(%s, 0660) failed", sockfn);
	}

	ret = listen(fd, DS_LISTEN_BACKLOG);

	if (ret < 0) {
		fatal_with_errno(EXIT_FAILURE, "listen(%d, %d) failed", fd, DS_LISTEN_BACKLOG);
	}
#else
static void sock_connect(HANDLE sock);
static HANDLE sock_open(const char *fn)
{
	HANDLE fd;

	fd = CreateNamedPipe( 
			fn,			// pipe name 
			PIPE_ACCESS_DUPLEX |  // read/write access 
			FILE_FLAG_OVERLAPPED, // async IO
			PIPE_TYPE_BYTE |	 
			PIPE_READMODE_BYTE |	 
			PIPE_WAIT,		 
			PIPE_UNLIMITED_INSTANCES, // max. instances  
			ST_SOCK_BUF_LEN,	// output buffer size 
			ST_SOCK_BUF_LEN,	// input buffer size 
			0,			// client time-out 
			NULL);			// FIXME: default security attribute 

	if (fd == INVALID_HANDLE_VALUE) {
		fatal_with_errno(EXIT_FAILURE, "Can't create a state socket (windows named pipe)");
	}

	/* Prepare an async wait on a connection on the pipe */
	memset(&connect_overlapped,0,sizeof(connect_overlapped));
	connect_overlapped.hEvent = CreateEvent(NULL, /*Security*/
			FALSE, /* auto-reset*/
			FALSE, /* inital state = non signaled*/
			NULL /* no name*/);
	if(connect_overlapped.hEvent == NULL ) {
		printf("CreateEvent failed.\n");
		fatal_with_errno(EXIT_FAILURE, "Can't create event");
	}

	/* Wait for a connection */
	BOOL ret;
	ret = ConnectNamedPipe(fd,&connect_overlapped);

#endif


	return fd;
}

static void sock_disconnect(conn_t *conn)
{
#ifndef WIN32
	close(conn->fd);
#else
	/* FIXME not sure if this is the right way to close a connection */
	if( conn->read_overlapped.hEvent != INVALID_HANDLE_VALUE) {
		CloseHandle(conn->read_overlapped.hEvent);
		conn->read_overlapped.hEvent = INVALID_HANDLE_VALUE;
	}
	DisconnectNamedPipe(conn->fd);
#endif

	pconf_finish(&conn->ctx);

	if (conn->prev) {
		conn->prev->next = conn->next;
	} else {
		connhead = conn->next;
	}

	if (conn->next) {
		conn->next->prev = conn->prev;
	} else {
		/* conntail = conn->prev; */
	}

	free(conn);
}

static void send_to_all(const char *fmt, ...)
{
	int	ret;
	char	buf[ST_SOCK_BUF_LEN];
	va_list	ap;
	conn_t	*conn, *cnext;

	va_start(ap, fmt);
	ret = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (ret < 1) {
		upsdebugx(2, "%s: nothing to write", __func__);
		return;
	}

	upsdebugx(5, "%s: %.*s", __func__, ret-1, buf);

	for (conn = connhead; conn; conn = cnext) {
		cnext = conn->next;

#ifndef WIN32
		ret = write(conn->fd, buf, strlen(buf));
#else
		DWORD bytesWritten = 0;
		BOOL  result = FALSE;

		result = WriteFile (conn->fd,buf,strlen(buf),&bytesWritten,NULL);
		if( result == 0 ) {
			upsdebugx(2, "write failed on %d, disconnecting", (int)conn->fd);
			sock_disconnect(conn);
			continue;
		}
		else  {
			ret = (int)bytesWritten;
		}

#endif
		if (ret != (int)strlen(buf)) {
			upsdebugx(2, "write %d bytes to socket %d failed", (int)strlen(buf), (int)conn->fd);
			sock_disconnect(conn);
		}
	}
}

static int send_to_one(conn_t *conn, const char *fmt, ...)
{
	int	ret;
	va_list	ap;
	char	buf[ST_SOCK_BUF_LEN];
#ifdef WIN32
        DWORD bytesWritten = 0;
        BOOL  result = FALSE;
#endif

	va_start(ap, fmt);
	ret = vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);

	if (ret < 1) {
		upsdebugx(2, "%s: nothing to write", __func__);
		return 1;
	}

	upsdebugx(5, "%s: %.*s", __func__, ret-1, buf);

#ifndef WIN32
	ret = write(conn->fd, buf, strlen(buf));
#else
        result = WriteFile (conn->fd,buf,strlen(buf),&bytesWritten,NULL);
	if( result == 0 ) {
		printf("Write failed\n");
		ret = 0;
	}
	else  {
		ret = (int)bytesWritten;
	}
#endif

	if (ret != (int)strlen(buf)) {
		upsdebugx(2, "write %d bytes to socket %d failed", (int)strlen(buf), (int)conn->fd);
		sock_disconnect(conn);
		return 0;	/* failed */
	}

	return 1;	/* OK */
}

#ifndef WIN32
static void sock_connect(int sock)
#else
static void sock_connect(HANDLE sock)
#endif
{
	conn_t	*conn;

#ifndef WIN32
	int	ret;
	int fd;

	struct sockaddr_un sa;
	socklen_t	salen;

	salen = sizeof(sa);
	fd = accept(sock, (struct sockaddr *) &sa, &salen);

	if (fd < 0) {
		upslog_with_errno(LOG_ERR, "accept on unix fd failed");
		return;
	}

	/* enable nonblocking I/O */

	ret = fcntl(fd, F_GETFL, 0);

	if (ret < 0) {
		upslog_with_errno(LOG_ERR, "fcntl get on unix fd failed");
		close(fd);
		return;
	}

	ret = fcntl(fd, F_SETFL, ret | O_NDELAY);

	if (ret < 0) {
		upslog_with_errno(LOG_ERR, "fcntl set O_NDELAY on unix fd failed");
		close(fd);
		return;
	}	

	conn = xcalloc(1, sizeof(*conn));
	conn->fd = fd;

	pconf_init(&conn->ctx, NULL);

	if (connhead) {
		conn->next = connhead;
		connhead->prev = conn;
	}

	connhead = conn;

	upsdebugx(3, "new connection on fd %d", fd);
#else
	/* We have detected a connection on the opened pipe. So we start by saving its handle  and cretae a new pipe for future connection */
	conn = xcalloc(1, sizeof(*conn));
	conn->fd = sock;

	/* sockfd is the handle of the connection pending pipe */
	sockfd = CreateNamedPipe( 
			pipename,			// pipe name 
			PIPE_ACCESS_DUPLEX |  // read/write access 
			FILE_FLAG_OVERLAPPED, // async IO
			PIPE_TYPE_BYTE |	 
			PIPE_READMODE_BYTE |	 
			PIPE_WAIT,		 
			PIPE_UNLIMITED_INSTANCES, // max. instances  
			ST_SOCK_BUF_LEN,	// output buffer size 
			ST_SOCK_BUF_LEN,	// input buffer size 
			0,			// client time-out 
			NULL);			// FIXME: default security attribute 

	if (sockfd == INVALID_HANDLE_VALUE) {
		fatal_with_errno(EXIT_FAILURE, "Can't create a state socket (windows named pipe)");
	}

	/* Prepare a new async wait for a connection on the pipe */
	CloseHandle(connect_overlapped.hEvent);
	memset(&connect_overlapped,0,sizeof(connect_overlapped));
	connect_overlapped.hEvent = CreateEvent(NULL, /*Security*/
			FALSE, /* auto-reset*/
			FALSE, /* inital state = non signaled*/
			NULL /* no name*/);
	if(connect_overlapped.hEvent == NULL ) {
		fatal_with_errno(EXIT_FAILURE, "Can't create event");
	}

	/* Wait for a connection */
	BOOL ret;
	ret = ConnectNamedPipe(sockfd,&connect_overlapped);

	/* A new pipe waiting for new client connection has been created. We could manage the current connection now */
	/* Start a read operation on the newly connected pipe so we could wait on the event associated to this IO */
	memset(&conn->read_overlapped,0,sizeof(conn->read_overlapped));
	memset(conn->buf,0,sizeof(conn->buf));
	conn->read_overlapped.hEvent = CreateEvent(NULL, /*Security*/
			FALSE, /* auto-reset*/
			FALSE, /* inital state = non signaled*/
			NULL /* no name*/);
	if(conn->read_overlapped.hEvent == NULL ) {
		fatal_with_errno(EXIT_FAILURE, "Can't create event");
	}

	ReadFile (conn->fd,conn->buf,sizeof(conn->buf)-1,NULL,&(conn->read_overlapped)); /* -1 to be sure to have a trailling 0 */

	pconf_init(&conn->ctx, NULL);

	if (connhead) {
		conn->next = connhead;
		connhead->prev = conn;
	}

	connhead = conn;

	upsdebugx(3, "new connection on fd %d", (int)sock);
#endif

}

static int st_tree_dump_conn(st_tree_t *node, conn_t *conn)
{
	int	ret;
	enum_t	*etmp;

	if (!node) {
		return 1;	/* not an error */
	}

	if (node->left) {
		ret = st_tree_dump_conn(node->left, conn);

		if (!ret) {
			return 0;	/* write failed in the child */
		}
	}

	if (!send_to_one(conn, "SETINFO %s \"%s\"\n", node->var, node->val)) {
		return 0;	/* write failed, bail out */
	}

	/* send any enums */
	for (etmp = node->enum_list; etmp; etmp = etmp->next) {
		if (!send_to_one(conn, "ADDENUM %s \"%s\"\n", node->var, etmp->val)) {
			return 0;
		}
	}

	/* provide any auxiliary data */
	if (node->aux) {
		if (!send_to_one(conn, "SETAUX %s %d\n", node->var, node->aux)) {
			return 0;
		}
	}

	/* finally report any flags */
	if (node->flags) {
		char	flist[SMALLBUF];

		/* build the list */
		snprintf(flist, sizeof(flist), "%s", node->var);

		if (node->flags & ST_FLAG_RW) {
			snprintfcat(flist, sizeof(flist), " RW");
		}
		if (node->flags & ST_FLAG_STRING) {
			snprintfcat(flist, sizeof(flist), " STRING");
		}

		send_to_one(conn, "SETFLAGS %s\n", flist);
	}

	if (node->right) {
		return st_tree_dump_conn(node->right, conn);
	}

	return 1;	/* everything's OK here ... */
}

static int cmd_dump_conn(conn_t *conn)
{
	cmdlist_t	*cmd;

	for (cmd = cmdhead; cmd; cmd = cmd->next) {
		if (!send_to_one(conn, "ADDCMD %s\n", cmd->name)) {
			return 0;
		}
	}

	return 1;
}

static int sock_arg(conn_t *conn, int numarg, char **arg)
{
	if (numarg < 1) {
		return 0;
	}

	if (!strcasecmp(arg[0], "DUMPALL")) {

		/* first thing: the staleness flag */
		if ((stale == 1) && !send_to_one(conn, "DATASTALE\n")) {
			return 1;
		}

		if (!st_tree_dump_conn(dtree_root, conn)) {
			return 1;
		}

		if (!cmd_dump_conn(conn)) {
			return 1;
		}

		if ((stale == 0) && !send_to_one(conn, "DATAOK\n")) {
			return 1;
		}

		send_to_one(conn, "DUMPDONE\n");
		return 1;
	}

	if (!strcasecmp(arg[0], "PING")) {
		send_to_one(conn, "PONG\n");
		return 1;
	}

	if (numarg < 2) {
		return 0;
	}

	/* INSTCMD <cmdname> [<value>]*/
	if (!strcasecmp(arg[0], "INSTCMD")) {

		/* try the new handler first if present */
		if (upsh.instcmd) {
			if (numarg > 2) {
				upsh.instcmd(arg[1], arg[2]);
				return 1;
			}

			upsh.instcmd(arg[1], NULL);
			return 1;
		}

		upslogx(LOG_NOTICE, "Got INSTCMD, but driver lacks a handler");
		return 1;
	}

	if (numarg < 3) {
		return 0;
	}

	/* SET <var> <value> */
	if (!strcasecmp(arg[0], "SET")) {

		/* try the new handler first if present */
		if (upsh.setvar) {
			upsh.setvar(arg[1], arg[2]);
			return 1;
		}

		upslogx(LOG_NOTICE, "Got SET, but driver lacks a handler");
		return 1;
	}

	/* unknown */
	return 0;
}

static void sock_read(conn_t *conn)
{
	int	i, ret;

#ifndef WIN32
	char	buf[SMALLBUF];

	ret = read(conn->fd, buf, sizeof(buf));

	if (ret < 0) {
		switch(errno)
		{
		case EINTR:
		case EAGAIN:
			return;

		default:
			sock_disconnect(conn);
			return;
		}
	}
#else
	char *buf = conn->buf;
	DWORD bytesRead;
	BOOL res;
	res = GetOverlappedResult(conn->fd, &conn->read_overlapped, &bytesRead, FALSE);
	if( res == 0 ) {
		upslogx(LOG_INFO, "Read error : %d",(int)GetLastError());
		sock_disconnect(conn);
		return;
	}
	ret = bytesRead;
#endif

	for (i = 0; i < ret; i++) {

		switch(pconf_char(&conn->ctx, buf[i]))
		{
		case 0: /* nothing to parse yet */
			continue;

		case 1: /* try to use it, and complain about unknown commands */
			if (!sock_arg(conn, conn->ctx.numargs, conn->ctx.arglist)) {
				size_t	arg;

				upslogx(LOG_INFO, "Unknown command on socket: ");

				for (arg = 0; arg < conn->ctx.numargs; arg++) {
					upslogx(LOG_INFO, "arg %d: %s", (int)arg, conn->ctx.arglist[arg]);
				}
			}
			continue;

		default: /* nothing parsed */
			upslogx(LOG_NOTICE, "Parse error on sock: %s", conn->ctx.errmsg);
			return;
		}
	}

#ifdef WIN32
	/* Restart async read */
	memset(conn->buf,0,sizeof(conn->buf));
	ReadFile(conn->fd,conn->buf,sizeof(conn->buf)-1,NULL,&(conn->read_overlapped)); /* -1 to be sure to have a trailling 0 */
#endif
}

static void sock_close(void)
{
	conn_t	*conn, *cnext;
#ifndef WIN32
	if (sockfd != -1) {
		close(sockfd);
		sockfd = -1;

		if (sockfn) {
			unlink(sockfn);
			free(sockfn);
			sockfn = NULL;
		}
	}

#else
	if (sockfd != INVALID_HANDLE_VALUE) {
		FlushFileBuffers(sockfd);
		CloseHandle(sockfd);
		sockfd = INVALID_HANDLE_VALUE;
		printf("sock_close Close %d\n",(int)sockfd);
	}
#endif

	for (conn = connhead; conn; conn = cnext) {
		cnext = conn->next;
		sock_disconnect(conn);
	}

	connhead = NULL;
	/* conntail = NULL; */
}

/* interface */

void dstate_init(const char *prog, const char *devname)
{
	char	sockname[SMALLBUF];

#ifndef WIN32
	/* do this here for now */
	signal(SIGPIPE, SIG_IGN);

	if (devname) {
		snprintf(sockname, sizeof(sockname), "%s/%s-%s", dflt_statepath(), prog, devname);
	} else {
		snprintf(sockname, sizeof(sockname), "%s/%s", dflt_statepath(), prog);
	}
#else
	/* FIXME: use dflt_statepath() instead of hardcoded value ?*/
	if (devname) {
		snprintf(sockname, sizeof(sockname), "\\\\.\\pipe\\%s-%s", prog, devname);
	} else {
		snprintf(sockname, sizeof(sockname), "\\\\.\\pipe\\%s", prog);
	}
	pipename = strdup(sockname);
#endif

	sockfd = sock_open(sockname);

	upsdebugx(2, "dstate_init: sock %s open on fd %d", sockname, (int)sockfd);
}

#ifndef WIN32
/* returns 1 if timeout expired or data is available on UPS fd, 0 otherwise */
int dstate_poll_fds(struct timeval timeout, int extrafd)
{
	int	ret, maxfd, overrun = 0;
	fd_set	rfds;
	struct timeval	now;
	conn_t	*conn, *cnext;

	FD_ZERO(&rfds);
	FD_SET(sockfd, &rfds);

	maxfd = sockfd;

	if (extrafd != -1) {
		FD_SET(extrafd, &rfds);

		if (extrafd > maxfd) {
			maxfd = extrafd;
		}
	}

	for (conn = connhead; conn; conn = conn->next) {
		FD_SET(conn->fd, &rfds);

		if (conn->fd > maxfd) {
			maxfd = conn->fd;
		}
	}

	gettimeofday(&now, NULL);

	/* number of microseconds should always be positive */
	if (timeout.tv_usec < now.tv_usec) {
		timeout.tv_sec -= 1;
		timeout.tv_usec += 1000000;
	}

	if (timeout.tv_sec < now.tv_sec) {
		timeout.tv_sec = 0;
		timeout.tv_usec = 0;
		overrun = 1;	/* no time left */
	} else {
		timeout.tv_sec -= now.tv_sec;
		timeout.tv_usec -= now.tv_usec;
	}
	
	ret = select(maxfd + 1, &rfds, NULL, NULL, &timeout);

	if (ret == 0) {
		return 1;	/* timer expired */
	}

	if (ret < 0) {
		switch (errno)
		{
		case EINTR:
		case EAGAIN:
			/* ignore interruptions from signals */
			break;

		default:
			upslog_with_errno(LOG_ERR, "select unix sockets failed");
		}

		return overrun;
	}

	if (FD_ISSET(sockfd, &rfds)) {
		sock_connect(sockfd);
	}

	for (conn = connhead; conn; conn = cnext) {
		cnext = conn->next;

		if (FD_ISSET(conn->fd, &rfds)) {
			sock_read(conn);
		}
	}

	/* tell the caller if that fd woke up */
	if ((extrafd != -1) && (FD_ISSET(extrafd, &rfds))) {
		return 1;
	}

	return overrun;
}
#else
/* returns 1 if timeout expired or data is available on UPS fd, 0 otherwise */
int dstate_poll_fds(struct timeval timeout, HANDLE extrafd)
{
	DWORD	ret;
	int	maxfd = 0, overrun = 0;
	HANDLE	rfds[32];
	conn_t	*conn;
        struct timeval  now;

/*
	if (extrafd != -1) {
		rfds[maxfd] = extrafd;
		maxfd++;
	}
*/

	gettimeofday(&now, NULL);

	/* number of microseconds should always be positive */
	if (timeout.tv_usec < now.tv_usec) {
		timeout.tv_sec -= 1;
		timeout.tv_usec += 1000000;
	}

	if (timeout.tv_sec < now.tv_sec) {
		timeout.tv_sec = 0;
		timeout.tv_usec = 0;
		overrun = 1;	/* no time left */
	} else {
		timeout.tv_sec -= now.tv_sec;
		timeout.tv_usec -= now.tv_usec;
	}

/* Wait on the read IO of each connections */
	for (conn = connhead; conn; conn = conn->next) {
		rfds[maxfd] = conn->read_overlapped.hEvent;
		maxfd++;
	}
	/* Add the connect event */
	rfds[maxfd] = connect_overlapped.hEvent;
	maxfd++;

	ret = WaitForMultipleObjects( 
				maxfd,	// number of objects in array
				rfds,	// array of objects
				FALSE,	// wait for any object
				timeout.tv_sec); // five-second wait

	if (ret == WAIT_TIMEOUT) {
		return 1;	/* timer expired */
	}

	if (ret == WAIT_FAILED) {
		printf("waitfor failed\n");
		return overrun;
	}

	/* Retrieve the signaled connection */
	for(conn = connhead; conn != NULL; conn = conn->next) {
		if( conn->read_overlapped.hEvent == rfds[ret-WAIT_OBJECT_0]) {
			break;
		}
	}

	/* the connection event handle has been signaled */
	if (rfds[ret] == connect_overlapped.hEvent) {
		sock_connect(sockfd);
	}
	/* one of the read event handle has been signaled */
	else {
		if( conn != NULL) {
			sock_read(conn);
		}
	}
		
	/* tell the caller if that fd woke up */
/*
	if ((extrafd != -1) && (ret == extrafd)) {
		return 1;
	}
*/
	return overrun;
}
#endif

/******************************************************************
 * COMMON
 ******************************************************************/

int dstate_setinfo(const char *var, const char *fmt, ...)
{
	int	ret;
	char	value[ST_MAX_VALUE_LEN];
	va_list	ap;

	va_start(ap, fmt);
	vsnprintf(value, sizeof(value), fmt, ap);
	va_end(ap);

	ret = state_setinfo(&dtree_root, var, value);

	if (ret == 1) {
		send_to_all("SETINFO %s \"%s\"\n", var, value);
	}

	return ret;
}

int dstate_addenum(const char *var, const char *fmt, ...)
{
	int	ret;
	char	value[ST_MAX_VALUE_LEN];
	va_list	ap;

	va_start(ap, fmt);
	vsnprintf(value, sizeof(value), fmt, ap);
	va_end(ap);

	ret = state_addenum(dtree_root, var, value);

	if (ret == 1) {
		send_to_all("ADDENUM %s \"%s\"\n", var, value);
	}

	return ret;
}

void dstate_setflags(const char *var, int flags)
{
	st_tree_t	*sttmp;
	char	flist[SMALLBUF];

	/* find the dtree node for var */
	sttmp = state_tree_find(dtree_root, var);

	if (!sttmp) {
		upslogx(LOG_ERR, "dstate_setflags: base variable (%s) does not exist", var);
		return;
	}

	if (sttmp->flags == flags) {
		return;		/* no change */
	}

	sttmp->flags = flags;

	/* build the list */
	snprintf(flist, sizeof(flist), "%s", var);

	if (flags & ST_FLAG_RW) {
		snprintfcat(flist, sizeof(flist), " RW");
	}

	if (flags & ST_FLAG_STRING) {
		snprintfcat(flist, sizeof(flist), " STRING");
	}

	/* update listeners */
	send_to_all("SETFLAGS %s\n", flist);
}

void dstate_setaux(const char *var, int aux)
{
	st_tree_t	*sttmp;

	/* find the dtree node for var */
	sttmp = state_tree_find(dtree_root, var);

	if (!sttmp) {
		upslogx(LOG_ERR, "dstate_setaux: base variable (%s) does not exist", var);
		return;
	}

	if (sttmp->aux == aux) {
		return;		/* no change */
	}

	sttmp->aux = aux;

	/* update listeners */
	send_to_all("SETAUX %s %d\n", var, aux);
}

const char *dstate_getinfo(const char *var)
{
	return state_getinfo(dtree_root, var);
}

void dstate_addcmd(const char *cmdname)
{
	int	ret;

	ret = state_addcmd(&cmdhead, cmdname);

	/* update listeners */
	if (ret == 1) {
		send_to_all("ADDCMD %s\n", cmdname);
	}
}

int dstate_delinfo(const char *var)
{
	int	ret;

	ret = state_delinfo(&dtree_root, var);

	/* update listeners */
	if (ret == 1) {
		send_to_all("DELINFO %s\n", var);
	}

	return ret;
}

int dstate_delenum(const char *var, const char *val)
{
	int	ret;

	ret = state_delenum(dtree_root, var, val);

	/* update listeners */
	if (ret == 1) {
		send_to_all("DELENUM %s \"%s\"\n", var, val);
	}

	return ret;
}

int dstate_delcmd(const char *cmd)
{
	int	ret;

	ret = state_delcmd(&cmdhead, cmd);

	/* update listeners */
	if (ret == 1) {
		send_to_all("DELCMD %s\n", cmd);
	}

	return ret;
}

void dstate_free(void)
{
	state_infofree(dtree_root);
	dtree_root = NULL;
	
	state_cmdfree(cmdhead);
	cmdhead = NULL;

	sock_close();
}

const st_tree_t *dstate_getroot(void)
{
	return dtree_root;
}

const cmdlist_t *dstate_getcmdlist(void)
{
	return cmdhead;
}

void dstate_dataok(void)
{
	if (stale == 1) {
		stale = 0;
		send_to_all("DATAOK\n");
	}
}

void dstate_datastale(void)
{
	if (stale == 0) {
		stale = 1;
		send_to_all("DATASTALE\n");
	}
}

int dstate_is_stale(void)
{
	return stale;
}

/* ups.status management functions - reducing duplication in the drivers */

/* clean out the temp space for a new pass */
void status_init(void)
{
	memset(status_buf, 0, sizeof(status_buf));
}

/* add a status element */
void status_set(const char *buf)
{
	/* separate with a space if multiple elements are present */
	if (strlen(status_buf) > 0) {
		snprintfcat(status_buf, sizeof(status_buf), " %s", buf);
	} else {
		snprintfcat(status_buf, sizeof(status_buf), "%s", buf);
	}
}

/* write the status_buf into the externally visible dstate storage */
void status_commit(void)
{
	if (alarm_active) {
		dstate_setinfo("ups.status", "ALARM %s", status_buf);
	} else {
		dstate_setinfo("ups.status", "%s", status_buf);
	}
}

/* similar handlers for ups.alarm */

void alarm_init(void)
{
	memset(alarm_buf, 0, sizeof(alarm_buf));
}

void alarm_set(const char *buf)
{
	if (strlen(alarm_buf) > 0) {
		snprintfcat(alarm_buf, sizeof(alarm_buf), " %s", buf);
	} else {
		snprintfcat(alarm_buf, sizeof(alarm_buf), "%s", buf);
	}
}

/* write the status_buf into the info array */
void alarm_commit(void)
{
	if (strlen(alarm_buf) > 0) {
		dstate_setinfo("ups.alarm", "%s", alarm_buf);
		alarm_active = 1;
	} else {
		dstate_delinfo("ups.alarm");
		alarm_active = 0;
	}
}
