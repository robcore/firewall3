/*
 * firewall3 - 3rd OpenWrt UCI firewall implementation
 *
 *   Copyright (C) 2013 Jo-Philipp Wich <jo@mein.io>
 *
 * Permission to use, copy, modify, and/or distribute this software for any
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

#include "ipsets.h"


const struct fw3_option fw3_ipset_opts[] = {
	FW3_OPT("enabled",       bool,           ipset,     enabled),
	FW3_OPT("reload_set",	 bool,           ipset,     reload_set),
	FW3_OPT("counters",	 bool,           ipset,     counters),
	FW3_OPT("comment",	 bool,           ipset,     comment),

	FW3_OPT("name",          string,         ipset,     name),
	FW3_OPT("family",        family,         ipset,     family),

	FW3_OPT("storage",       ipset_method,   ipset,     method),
	FW3_LIST("match",        ipset_datatype, ipset,     datatypes),

	FW3_OPT("iprange",       address,        ipset,     iprange),
	FW3_OPT("portrange",     port,           ipset,     portrange),

	FW3_OPT("netmask",       int,            ipset,     netmask),
	FW3_OPT("maxelem",       int,            ipset,     maxelem),
	FW3_OPT("hashsize",      int,            ipset,     hashsize),
	FW3_OPT("timeout",       int,            ipset,     timeout),

	FW3_OPT("external",      string,         ipset,     external),

	FW3_LIST("entry",        setentry,       ipset,     entries),
	FW3_OPT("loadfile",      string,         ipset,     loadfile),

	{ }
};

#define T(m, t1, t2, t3, r, o) \
	{ FW3_IPSET_METHOD_##m, \
	  FW3_IPSET_TYPE_##t1 | (FW3_IPSET_TYPE_##t2 << 8) | (FW3_IPSET_TYPE_##t3 << 16), \
	  r, o }

enum ipset_optflag {
	OPT_IPRANGE   = (1 << 0),
	OPT_PORTRANGE = (1 << 1),
	OPT_NETMASK   = (1 << 2),
	OPT_HASHSIZE  = (1 << 3),
	OPT_MAXELEM   = (1 << 4),
	OPT_FAMILY    = (1 << 5),
};

struct ipset_type {
	enum fw3_ipset_method method;
	uint32_t types;
	uint8_t required;
	uint8_t optional;
};

static struct ipset_type ipset_types[] = {
	T(BITMAP, IP,   UNSPEC, UNSPEC, OPT_IPRANGE, OPT_NETMASK),
	T(BITMAP, IP,   MAC,    UNSPEC, OPT_IPRANGE, 0),
	T(BITMAP, PORT, UNSPEC, UNSPEC, OPT_PORTRANGE, 0),

	T(HASH,   IP,   UNSPEC, UNSPEC, 0,
	  OPT_FAMILY | OPT_HASHSIZE | OPT_MAXELEM | OPT_NETMASK),
	T(HASH,   NET,  UNSPEC, UNSPEC, 0,
	  OPT_FAMILY | OPT_HASHSIZE | OPT_MAXELEM),
	T(HASH,   IP,   PORT,   UNSPEC, 0,
	  OPT_FAMILY | OPT_HASHSIZE | OPT_MAXELEM),
	T(HASH,   NET,  PORT,   UNSPEC, 0,
	  OPT_FAMILY | OPT_HASHSIZE | OPT_MAXELEM),
	T(HASH,   IP,   PORT,   IP,     0,
	  OPT_FAMILY | OPT_HASHSIZE | OPT_MAXELEM),
	T(HASH,   IP,   PORT,   NET,    0,
	  OPT_FAMILY | OPT_HASHSIZE | OPT_MAXELEM),

	T(LIST,   SET,  UNSPEC, UNSPEC, 0, OPT_MAXELEM),
};


static bool
check_types(struct uci_element *e, struct fw3_ipset *ipset)
{
	int i = 0;
	uint32_t typelist = 0;
	struct fw3_ipset_datatype *type;

	list_for_each_entry(type, &ipset->datatypes, list)
	{
		if (i >= 3)
		{
			warn_section("ipset", ipset, e, "must not have more than 3 datatypes assigned");
			return false;
		}

		typelist |= (type->type << (i++ * 8));
	}

	/* find a suitable storage method if none specified */
	if (ipset->method == FW3_IPSET_METHOD_UNSPEC)
	{
		for (i = 0; i < ARRAY_SIZE(ipset_types); i++)
		{
			/* skip type for v6 if it does not support family */
			if (ipset->family != FW3_FAMILY_V4 &&
			    !(ipset_types[i].optional & OPT_FAMILY))
				continue;

			if (ipset_types[i].types == typelist)
			{
				ipset->method = ipset_types[i].method;

				warn_section("ipset", ipset, e, "defines no storage method, assuming '%s'",
					fw3_ipset_method_names[ipset->method]);

				break;
			}
		}
	}

	//typelist |= ipset->method;

	for (i = 0; i < ARRAY_SIZE(ipset_types); i++)
	{
		if (ipset_types[i].method == ipset->method &&
		    ipset_types[i].types == typelist)
		{
			if (!ipset->external)
			{
				if ((ipset_types[i].required & OPT_IPRANGE) &&
					!ipset->iprange.set)
				{
					warn_section("ipset", ipset, e, "requires an ip range");
					return false;
				}

				if ((ipset_types[i].required & OPT_PORTRANGE) &&
				    !ipset->portrange.set)
				{
					warn_section("ipset", ipset, e, "requires a port range");
					return false;
				}

				if (!(ipset_types[i].required & OPT_IPRANGE) &&
				    ipset->iprange.set)
				{
					warn_section("ipset", ipset, e, "iprange ignored");
					ipset->iprange.set = false;
				}

				if (!(ipset_types[i].required & OPT_PORTRANGE) &&
				    ipset->portrange.set)
				{
					warn_section("ipset", ipset, e, "portrange ignored");
					ipset->portrange.set = false;
				}

				if (!(ipset_types[i].optional & OPT_NETMASK) &&
				    ipset->netmask > 0)
				{
					warn_section("ipset", ipset, e, "netmask ignored");
					ipset->netmask = 0;
				}

				if (!(ipset_types[i].optional & OPT_HASHSIZE) &&
				    ipset->hashsize > 0)
				{
					warn_section("ipset", ipset, e, "hashsize ignored");
					ipset->hashsize = 0;
				}

				if (!(ipset_types[i].optional & OPT_MAXELEM) &&
				    ipset->maxelem > 0)
				{
					warn_section("ipset", ipset, e, "maxelem ignored");
					ipset->maxelem = 0;
				}

				if (!(ipset_types[i].optional & OPT_FAMILY) &&
				    ipset->family != FW3_FAMILY_V4)
				{
					warn_section("ipset", ipset, e, "family ignored");
					ipset->family = FW3_FAMILY_V4;
				}
			}

			return true;
		}
	}

	warn_section("ipset", ipset, e, "has an invalid combination of storage method and matches");
	return false;
}

static bool
check_ipset(struct fw3_state *state, struct fw3_ipset *ipset, struct uci_element *e)
{
	if (!ipset->enabled) {
		return false;
	}

	if (ipset->external)
	{
		if (!*ipset->external)
			ipset->external = NULL;
		else if (!ipset->name)
			ipset->name = ipset->external;
	}

	if (!ipset->name || !*ipset->name)
	{
		warn_section("ipset", ipset, e, "ipset must have a name assigned");
	}
	//else if (fw3_lookup_ipset(state, ipset->name) != NULL)
	//{
	//	warn_section("ipset", ipset, e, "has duplicated set name", ipset->name);
	//}
	else if (ipset->family == FW3_FAMILY_ANY)
	{
		warn_section("ipset", ipset, e, "must not have family 'any'");
	}
	else if (ipset->iprange.set && ipset->family != ipset->iprange.family)
	{
		warn_section("ipset", ipset, e, "has iprange of wrong address family");
	}
	else if (list_empty(&ipset->datatypes))
	{
		warn_section("ipset", ipset, e, "has no datatypes assigned");
	}
	else if (check_types(e, ipset))
	{
		return true;
	}

	return false;
}

static struct fw3_ipset *
fw3_alloc_ipset(struct fw3_state *state)
{
	struct fw3_ipset *ipset;

	ipset = calloc(1, sizeof(*ipset));
	if (!ipset)
		return NULL;

	INIT_LIST_HEAD(&ipset->datatypes);
	INIT_LIST_HEAD(&ipset->entries);

	ipset->comment    = false;
	ipset->counters   = false;
	ipset->enabled    = true;
	ipset->family     = FW3_FAMILY_V4;
	ipset->reload_set = false;

	list_add_tail(&ipset->list, &state->ipsets);

	return ipset;
}

void
fw3_load_ipsets(struct fw3_state *state, struct uci_package *p,
		struct blob_attr *a)
{
	struct uci_section *s;
	struct uci_element *e;
	struct fw3_ipset *ipset;
	struct blob_attr *entry;
	unsigned rem;

	INIT_LIST_HEAD(&state->ipsets);

	if (state->disable_ipsets)
		return;

	blob_for_each_attr(entry, a, rem)
	{
		const char *type;
		const char *name = "ubus ipset";

		if (!fw3_attr_parse_name_type(entry, &name, &type))
			continue;

		if (strcmp(type, "ipset"))
			continue;

		ipset = fw3_alloc_ipset(state);
		if (!ipset)
			continue;

		if (!fw3_parse_blob_options(ipset, fw3_ipset_opts, entry, name))
		{
			warn_section("ipset", ipset, NULL, "skipped due to invalid options");
			fw3_free_ipset(ipset);
			continue;
		}

		if (!check_ipset(state, ipset, NULL))
			fw3_free_ipset(ipset);
	}

	uci_foreach_element(&p->sections, e)
	{
		s = uci_to_section(e);

		if (strcmp(s->type, "ipset"))
			continue;

		ipset = fw3_alloc_ipset(state);

		if (!ipset)
			continue;

		if (!fw3_parse_options(ipset, fw3_ipset_opts, s))
			warn_elem(e, "has invalid options");

		if (!check_ipset(state, ipset, e))
			fw3_free_ipset(ipset);
	}
}


static void
load_file(struct fw3_ipset *ipset)
{
	FILE *f;
	char line[128];

	if (!ipset->loadfile)
		return;

	info("   * Loading file %s", ipset->loadfile);

	f = fopen(ipset->loadfile, "r");

	if (!f) {
		info("     ! Skipping due to open error: %s", strerror(errno));
		return;
	}

	while (fgets(line, sizeof(line), f))
		fw3_pr("add %s %s", ipset->name, line);

	fclose(f);
}

static void
create_ipset(struct fw3_ipset *ipset, struct fw3_state *state)
{
	bool first = true;
	struct fw3_setentry *entry;
	struct fw3_ipset_datatype *type;

	info(" * Creating ipset %s", ipset->name);

	first = true;
	fw3_pr("create %s %s", ipset->name, fw3_ipset_method_names[ipset->method]);

	list_for_each_entry(type, &ipset->datatypes, list)
	{
		fw3_pr("%c%s", first ? ':' : ',', fw3_ipset_type_names[type->type]);
		first = false;
	}

	if (ipset->method == FW3_IPSET_METHOD_HASH)
		fw3_pr(" family inet%s", (ipset->family == FW3_FAMILY_V4) ? "" : "6");

	if (ipset->iprange.set)
	{
		fw3_pr(" range %s", fw3_address_to_string(&ipset->iprange, false, true));
	}
	else if (ipset->portrange.set)
	{
		fw3_pr(" range %u-%u",
		       ipset->portrange.port_min, ipset->portrange.port_max);
	}

	if (ipset->timeout > 0)
		fw3_pr(" timeout %u", ipset->timeout);

	if (ipset->maxelem > 0)
		fw3_pr(" maxelem %u", ipset->maxelem);

	if (ipset->netmask > 0)
		fw3_pr(" netmask %u", ipset->netmask);

	if (ipset->hashsize > 0)
		fw3_pr(" hashsize %u", ipset->hashsize);

	if (ipset->counters)
		fw3_pr(" counters");

	if (ipset->comment)
		fw3_pr(" comment");

	fw3_pr("\n");

	list_for_each_entry(entry, &ipset->entries, list)
		fw3_pr("add %s %s\n", ipset->name, entry->value);

	load_file(ipset);
}

void
fw3_create_ipsets(struct fw3_state *state, enum fw3_family family,
		  bool reload_set)
{
	unsigned int delay, tries;
	bool exec = false;
	struct fw3_ipset *ipset;

	if (state->disable_ipsets)
		return;

	/* spawn ipsets */
	list_for_each_entry(ipset, &state->ipsets, list)
	{
		if (ipset->family != family ||
		    (reload_set && !ipset->reload_set))
			continue;

		if (ipset->external)
			continue;

		if (!exec)
		{
			exec = fw3_command_pipe(false, "ipset", "-exist", "-");

			if (!exec)
				return;
		}

		create_ipset(ipset, state);
	}

	if (exec)
	{
		fw3_pr("quit\n");
		fw3_command_close();
	}

	/* wait a little expontially for ipsets to appear */
	list_for_each_entry(ipset, &state->ipsets, list)
	{
		if (ipset->external)
			continue;

		delay = 5;
		for (tries = 0; !fw3_check_ipset(ipset) && tries < 10; tries++)
			usleep(delay<<1);
	}
}

void
fw3_destroy_ipsets(struct fw3_state *state, enum fw3_family family,
		   bool reload_set)
{
	unsigned int delay, tries;
	bool exec = false;
	struct fw3_ipset *ipset;

	if (state->disable_ipsets)
		return;

	/* destroy ipsets */
	list_for_each_entry(ipset, &state->ipsets, list)
	{
		if (ipset->family != family ||
		    (reload_set && !ipset->reload_set))
			continue;

		if (!exec)
		{
			exec = fw3_command_pipe(false, "ipset", "-exist", "-");

			if (!exec)
				return;
		}

		info(" * Deleting ipset %s", ipset->name);

		fw3_pr("flush %s\n", ipset->name);
		fw3_pr("destroy %s\n", ipset->name);
	}

	if (exec)
	{
		fw3_pr("quit\n");
		fw3_command_close();
	}

	/* wait for ipsets to disappear */
	list_for_each_entry(ipset, &state->ipsets, list)
	{
		if (ipset->external)
			continue;

		delay = 5;
		for (tries = 0; fw3_check_ipset(ipset) && tries < 10; tries++)
			usleep(delay<<1);
	}
}

struct fw3_ipset *
fw3_lookup_ipset(struct fw3_state *state, const char *name)
{
	struct fw3_ipset *s;

	if (list_empty(&state->ipsets))
		return NULL;

	list_for_each_entry(s, &state->ipsets, list)
	{
		if (strcmp(s->name, name))
			continue;

		return s;
	}

	return NULL;
}

bool
fw3_check_ipset(struct fw3_ipset *set)
{
	bool rv = false;

	socklen_t sz;
	int s = socket(AF_INET, SOCK_RAW, IPPROTO_RAW);
	struct ip_set_req_version req_ver;
	struct ip_set_req_get_set req_name;

	if (s < 0 || fcntl(s, F_SETFD, FD_CLOEXEC))
		goto out;

	sz = sizeof(req_ver);
	req_ver.op = IP_SET_OP_VERSION;

	if (getsockopt(s, SOL_IP, SO_IP_SET, &req_ver, &sz))
		goto out;

	sz = sizeof(req_name);
	req_name.op = IP_SET_OP_GET_BYNAME;
	req_name.version = req_ver.version;
	snprintf(req_name.set.name, IPSET_MAXNAMELEN - 1, "%s",
	         set->external ? set->external : set->name);

	if (getsockopt(s, SOL_IP, SO_IP_SET, &req_name, &sz))
		goto out;

	rv = ((sz == sizeof(req_name)) && (req_name.set.index != IPSET_INVALID_ID));

out:
	if (s >= 0)
		close(s);

	return rv;
}
