/*	$OpenBSD: ruleset.c,v 1.34 2017/02/13 12:23:47 gilles Exp $ */

/*
 * Copyright (c) 2009 Gilles Chehade <gilles@poolp.org>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>
#include <sys/tree.h>
#include <sys/socket.h>

#include <netinet/in.h>

#include <errno.h>
#include <event.h>
#include <imsg.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>

#include "smtpd.h"
#include "log.h"


static int ruleset_check_source(struct table *,
    const struct sockaddr_storage *, int);
static int ruleset_check_mailaddr(struct table *, const struct mailaddr *);

struct match *ruleset_match_new(const struct envelope *);

struct rule *
ruleset_match(const struct envelope *evp)
{
	const struct mailaddr		*maddr = &evp->dest;
	const struct sockaddr_storage	*ss = &evp->ss;
	struct rule			*r;
	int				 ret;

	TAILQ_FOREACH(r, env->sc_rules, r_entry) {

		if (r->r_tag[0] != '\0') {
			ret = strcmp(r->r_tag, evp->tag);
			if (ret != 0 && !r->r_nottag)
				continue;
			if (ret == 0 && r->r_nottag)
				continue;
		}

		if ((r->r_wantauth && !r->r_negwantauth) && !(evp->flags & EF_AUTHENTICATED))
			continue;
		if ((r->r_wantauth && r->r_negwantauth) && (evp->flags & EF_AUTHENTICATED))
			continue;

		ret = ruleset_check_source(r->r_sources, ss, evp->flags);
		if (ret == -1) {
			errno = EAGAIN;
			return (NULL);
		}
		if ((ret == 0 && !r->r_notsources) || (ret != 0 && r->r_notsources))
			continue;

		if (r->r_senders) {
			ret = ruleset_check_mailaddr(r->r_senders, &evp->sender);
			if (ret == -1) {
				errno = EAGAIN;
				return (NULL);
			}
			if ((ret == 0 && !r->r_notsenders) || (ret != 0 && r->r_notsenders))
				continue;
		}

		if (r->r_recipients) {
			ret = ruleset_check_mailaddr(r->r_recipients, &evp->dest);
			if (ret == -1) {
				errno = EAGAIN;
				return (NULL);
			}
			if ((ret == 0 && !r->r_notrecipients) || (ret != 0 && r->r_notrecipients))
				continue;
		}

		ret = r->r_destination == NULL ? 1 :
		    table_lookup(r->r_destination, NULL, maddr->domain, K_DOMAIN,
			NULL);
		if (ret == -1) {
			errno = EAGAIN;
			return NULL;
		}
		if ((ret == 0 && !r->r_notdestination) || (ret != 0 && r->r_notdestination))
			continue;

		goto matched;
	}

	errno = 0;
	log_trace(TRACE_RULES, "no rule matched");
	return (NULL);

matched:
	log_trace(TRACE_RULES, "rule matched: %s", rule_to_text(r));
	return r;
}

static int
ruleset_check_source(struct table *table, const struct sockaddr_storage *ss,
    int evpflags)
{
	const char   *key;

	if (evpflags & (EF_AUTHENTICATED | EF_INTERNAL))
		key = "local";
	else
		key = ss_to_text(ss);
	switch (table_lookup(table, NULL, key, K_NETADDR, NULL)) {
	case 1:
		return 1;
	case -1:
		log_warnx("warn: failure to perform a table lookup on table %s",
		    table->t_name);
		return -1;
	default:
		break;
	}

	return 0;
}

static int
ruleset_check_mailaddr(struct table *table, const struct mailaddr *maddr)
{
	const char	*key;

	key = mailaddr_to_text(maddr);
	if (key == NULL)
		return -1;

	switch (table_lookup(table, NULL, key, K_MAILADDR, NULL)) {
	case 1:
		return 1;
	case -1:
		log_warnx("warn: failure to perform a table lookup on table %s",
		    table->t_name);
		return -1;
	default:
		break;
	}
	return 0;
}

/* --- */
static int
ruleset_match_table_lookup(struct table *table, const char *key, enum table_service service)
{
	switch (table_lookup(table, NULL, key, service, NULL)) {
	case 1:
		return 1;
	case -1:
		log_warnx("warn: failure to perform a table lookup on table %s",
		    table->t_name);
		return -1;
	default:
		break;
	}
	return 0;
}

static int
ruleset_match_tag(struct match *m, const struct envelope *evp)
{
	struct table	*table = table_find(m->tag_table, NULL);

	return ruleset_match_table_lookup(table, evp->tag, K_DOMAIN);
}

static int
ruleset_match_from(struct match *m, const struct envelope *evp)
{
	const char	*key;
	struct table	*table = table_find(m->from_table, NULL);

	if (m->from_socket) {
		/* XXX - socket needs to be distinguished from "local" */
		return -1;
	}

	/* XXX - socket should also be considered local */
	if (evp->flags & EF_INTERNAL)
		key = "local";
	else
		key = ss_to_text(&evp->ss);
	return ruleset_match_table_lookup(table, key, K_NETADDR);
}

static int
ruleset_match_to(struct match *m, const struct envelope *evp)
{
	struct table	*table = table_find(m->to_table, NULL);

	return ruleset_match_table_lookup(table, evp->dest.domain, K_DOMAIN);
}

static int
ruleset_match_smtp_helo(struct match *m, const struct envelope *evp)
{
	struct table	*table = table_find(m->smtp_helo_table, NULL);
	
	return ruleset_match_table_lookup(table, evp->helo, K_DOMAIN);
}

static int
ruleset_match_smtp_starttls(struct match *m, const struct envelope *evp)
{
	/* XXX - not until TLS flag is added to envelope */
	return -1;
}

static int
ruleset_match_smtp_auth(struct match *m, const struct envelope *evp)
{
	if (!(evp->flags & EF_AUTHENTICATED))
		return 0;
	
	if (m->smtp_auth_table) {
		/* XXX - not until smtp_session->username is added to envelope */
		/*
		 * table = table_find(m->from_table, NULL);
		 * key = evp->username;
		 * return ruleset_match_table_lookup(table, key, K_CREDENTIALS);
		 */
		return -1;

	}
	return 1;
}

static int
ruleset_match_smtp_mail_from(struct match *m, const struct envelope *evp)
{
	const char	*key;
	struct table	*table = table_find(m->from_table, NULL);
	
	if ((key = mailaddr_to_text(&evp->sender)) == NULL)
		return -1;
	return ruleset_match_table_lookup(table, key, K_MAILADDR);
}

static int
ruleset_match_smtp_rcpt_to(struct match *m, const struct envelope *evp)
{
	const char	*key;
	struct table	*table = table_find(m->from_table, NULL);
	
	if ((key = mailaddr_to_text(&evp->dest)) == NULL)
		return -1;
	return ruleset_match_table_lookup(table, key, K_MAILADDR);
}

struct match *
ruleset_match_new(const struct envelope *evp)
{
	struct match	*m;
	int		ret;
	
	TAILQ_FOREACH(m, env->sc_matches, entry) {
		if (m->tag) {
			if ((ret = ruleset_match_tag(m, evp)) == -1)
				goto tempfail;
			if (!(ret && m->tag > 0))
				continue;
		}
		if (m->from) {
			if ((ret = ruleset_match_from(m, evp)) == -1)
				goto tempfail;
			if (!(ret && m->from > 0))
				continue;
		}
		if (m->to) {
			if ((ret = ruleset_match_to(m, evp)) == -1)
				goto tempfail;
			if (!(ret && m->to > 0))
				continue;
		}
		if (m->smtp_helo) {
			if ((ret = ruleset_match_smtp_helo(m, evp)) == -1)
				goto tempfail;
			if (!(ret && m->smtp_helo > 0))
				continue;
		}
		if (m->smtp_auth) {
			if ((ret = ruleset_match_smtp_auth(m, evp)) == -1)
				goto tempfail;
			if (!(ret && m->smtp_auth > 0))
				continue;
		}
		if (m->smtp_starttls) {
			if ((ret = ruleset_match_smtp_starttls(m, evp)) == -1)
				goto tempfail;
			if (!(ret && m->smtp_starttls > 0))
				continue;
		}
		if (m->smtp_mail_from) {
			if ((ret = ruleset_match_smtp_mail_from(m, evp)) == -1)
				goto tempfail;
			if (!(ret && m->smtp_mail_from > 0))
				continue;
		}
		if (m->smtp_rcpt_to) {
			if ((ret = ruleset_match_smtp_rcpt_to(m, evp)) == -1)
				goto tempfail;
			if (!(ret && m->smtp_rcpt_to > 0))
				continue;
		}
		goto matched;
	}

	errno = 0;
	log_trace(TRACE_RULES, "no rule matched");
	return (NULL);

tempfail:
	errno = EAGAIN;
	log_trace(TRACE_RULES, "temporary failure in processing of a rule");
	return (NULL);
	
matched:
	log_trace(TRACE_RULES, "rule matched");
	return m;
}
