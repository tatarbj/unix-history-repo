/*
 * Copyright (c) 1995
 *	Bill Paul <wpaul@ctr.columbia.edu>.  All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 * 1. Redistributions of source code must retain the above copyright
 *    notice, this list of conditions and the following disclaimer.
 * 2. Redistributions in binary form must reproduce the above copyright
 *    notice, this list of conditions and the following disclaimer in the
 *    documentation and/or other materials provided with the distribution.
 * 3. All advertising materials mentioning features or use of this software
 *    must display the following acknowledgement:
 *	This product includes software developed by Bill Paul.
 * 4. Neither the name of the author nor the names of any co-contributors
 *    may be used to endorse or promote products derived from this software
 *    without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY Bill Paul AND CONTRIBUTORS ``AS IS'' AND
 * ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
 * IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
 * ARE DISCLAIMED.  IN NO EVENT SHALL Bill Paul OR CONTRIBUTORS BE LIABLE
 * FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL
 * DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS
 * OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION)
 * HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT
 * LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY
 * OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF
 * SUCH DAMAGE.
 *
 */

#include <rpc/rpc.h>
#include <rpcsvc/yp.h>
#include <rpcsvc/yppasswd.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/stat.h>
#include <paths.h>
#include <errno.h>
#include <sys/param.h>
#include "yp_extern.h"
#ifdef TCP_WRAPPER
#include "tcpd.h"
#endif

#ifndef lint
static const char rcsid[] = "$Id$";
#endif

extern int debug;

char *yp_procs[] = {	"ypproc_null" ,
			"ypproc_domain",
			"ypproc_domain_nonack",
			"ypproc_match",
			"ypproc_first",
			"ypproc_next",
			"ypproc_xfr",
			"ypproc_clear",
			"ypproc_all",
			"ypproc_master",
			"ypproc_order",
			"ypproc_maplist"
		   };

#ifdef TCP_WRAPPER
void load_securenets()
{
}
#else
struct securenet {
	struct in_addr net;
	struct in_addr mask;
	struct securenet *next;
};

struct securenet *securenets;

#define LINEBUFSZ 1024

/*
 * Read /var/yp/securenets file and initialize the securenets
 * list. If the file doesn't exist, we set up a dummy entry that
 * allows all hosts to connect.
 */
void load_securenets()
{
	FILE *fp;
	char path[MAXPATHLEN + 2];
	char linebuf[1024 + 2];
	struct securenet *tmp;

	/*
	 * If securenets is not NULL, we are being called to reload
	 * the list; free the existing list before re-reading the
	 * securenets file.
	 */
	if (securenets != NULL) {
		while(securenets) {
			tmp = securenets->next;
			free(securenets->net);
			free(securenets->mask);
			free(securenets);
			securenets = tmp;
		}
	}

	snprintf(path, MAXPATHLEN, "%s/securenets", yp_dir);

	if ((fp = fopen(path, "r")) == NULL) {
		if (errno == ENOENT) {
			securenets = (struct securenet *)malloc(sizeof(struct securenet));
			securenets->net.s_addr = INADDR_ANY;
			securenets->mask.s_addr = INADDR_BROADCAST;
			securenets->next = NULL;
			return;
		} else {
			yp_error("fopen(%s) failed: %s", path, strerror(errno));
			exit(1);
		}
	}

	securenets = NULL;

	while(fgets(linebuf, LINEBUFSZ, fp)) {
		char addr1[20], addr2[20];

		if (linebuf[0] == '#')
			continue;
		if (sscanf(linebuf, "%s %s", addr1, addr2) < 2) {
			yp_error("badly formatted securenets entry: %s",
							linebuf);
			continue;
		}

		tmp = (struct securenet *)malloc(sizeof(struct securenet));

		if (!inet_aton((char *)&addr1, (struct in_addr *)&tmp->net)) {
			yp_error("badly formatted securenets entry: %s", addr1);
			free(tmp);
			continue;
		}

		if (!inet_aton((char *)&addr2, (struct in_addr *)&tmp->mask)) {
			yp_error("badly formatted securenets entry: %s", addr2);
			free(tmp);
			continue;
		}

		tmp->next = securenets;
		securenets = tmp;
	}

	fclose(fp);
	
}
#endif

/*
 * Access control functions.
 *
 * yp_access() checks the mapname and client host address and watches for
 * the following things:
 *
 * - If the client is referencing one of the master.passwd.* maps, it must
 *   be using a privileged port to make its RPC to us. If it is, then we can
 *   assume that the caller is root and allow the RPC to succeed. If it
 *   isn't access is denied.
 *
 * - The client's IP address is checked against the securenets rules.
 *   There are two kinds of securenets support: the built-in support,
 *   which is very simple and depends on the presense of a
 *   /var/yp/securenets file, and tcp-wrapper support, which requires
 *   Wietse Venema's libwrap.a and tcpd.h. (Since the tcp-wrapper
 *   package does not ship with FreeBSD, we use the built-in support
 *   by default. Users can recompile the server the tcp-wrapper library
 *   if they already have it installed and want to use hosts.allow and
 *   hosts.deny to control access instead od having a seperate securenets
 *   file.)
 *
 *   If no /var/yp/securenets file is present, the host access checks
 *   are bypassed and all hosts are allowed to connect.
 *
 * The yp_validdomain() functions checks the domain specified by the caller
 * to make sure it's actually served by this server. This is more a sanity
 * check than an a security check, but this seems to be the best place for
 * it.
 */

int yp_access(map, rqstp)
	const char *map;
	const struct svc_req *rqstp;
{
	struct sockaddr_in *rqhost;
	int status = 0;
	unsigned long oldaddr;
#ifndef TCP_WRAPPER
	struct securenet *tmp;
#endif

	rqhost = svc_getcaller(rqstp->rq_xprt);

	if (debug) {
		yp_error("Procedure %s called from %s:%d",
			/* Hack to allow rpc.yppasswdd to use this routine */
			rqstp->rq_prog == YPPASSWDPROG ?
			"yppasswdproc_update" :
			yp_procs[rqstp->rq_proc], inet_ntoa(rqhost->sin_addr),
			ntohs(rqhost->sin_port));
		if (map != NULL)
			yp_error("Client is referencing map \"%s\".", map);
	}

	/* Check the map name if one was supplied. */
	if (map != NULL) {
		if ((strstr(map, "master.passwd.") ||
		    rqstp->rq_proc == YPPROC_XFR) &&
		    ntohs(rqhost->sin_port) > 1023) {
			yp_error("Access to %s denied -- client not privileged", map);
			return(1);
		}
	}

#ifdef TCP_WRAPPER
	status = hosts_ctl(progname, STRING_UNKNOWN,
			   inet_ntoa(rqhost->sin_addr), "");
#else
	tmp = securenets;
	while(tmp) {
		if (((rqhost->sin_addr.s_addr & ~tmp->mask.s_addr)
		    | tmp->net.s_addr) == rqhost->sin_addr.s_addr) {
			status = 1;
			break;
		}
		tmp = tmp->next;
	}
#endif

	if (!status) {
		if (rqhost->sin_addr.s_addr != oldaddr) {
			yp_error("connect from %s:%d to procedure %s refused",
					inet_ntoa(rqhost->sin_addr),
					ntohs(rqhost->sin_port),
					rqstp->rq_prog == YPPASSWDPROG ?
					"yppasswdproc_update" :
					yp_procs[rqstp->rq_proc]);
			oldaddr = rqhost->sin_addr.s_addr;
		}
		return(1);
	}
	return(0);

}

int yp_validdomain(domain)
	const char *domain;
{
	struct stat statbuf;
	char dompath[MAXPATHLEN + 2];

	if (domain == NULL || strstr(domain, "binding") ||
	    !strcmp(domain, ".") || !strcmp(domain, "..") ||
	    strchr(domain, '/') || strlen(domain) > YPMAXDOMAIN)
		return(1);

	snprintf(dompath, sizeof(dompath), "%s/%s", yp_dir, domain);

	if (stat(dompath, &statbuf) < 0 || !S_ISDIR(statbuf.st_mode))
		return(1);

	return(0);
}
