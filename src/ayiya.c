/**************************************
 SixXSd - SixXS PoP Daemon
 by Jeroen Massar <jeroen@sixxs.net>
***************************************
 $Author: jeroen $
 $Id: ayiya.c,v 1.5 2006-02-14 15:41:36 jeroen Exp $
 $Date: 2006-02-14 15:41:36 $

 SixXSd AYIYA (Anything in Anything) code
**************************************/

#include "sixxsd.h"

const char module_ayiya[] = "ayiya";
#define module module_ayiya

struct pseudo_ayh
{
	struct ayiyahdr	ayh;
	struct in6_addr	identity;
	sha1_byte	hash[SHA1_DIGEST_LENGTH];
	unsigned char	payload[2048];
};

struct ayiya_socket
{
	const char	*title;
	unsigned int	port;
	const char	*sport;
	int		socket;
} ayiya_socket[] = {
	{"AYIYA",	0,	"5072",	-1},
	{"AYIYA 8374",	0,	"8374",	-1},
	{"AYIYA 80",	0,	"80",	-1},
	{NULL,		0,	NULL,	-1}
};

/*
 * AYIYA Log Rate limiting
 * Remember the last 10 hosts, this could cause messages
 * to be dropped but that is better as a log flood
 * This applies only to Warning & Error messages
 */

/*
struct sockaddr_storage lastlogs[10];
int log_last = 0;
*/
void ayiya_log(int level, struct sockaddr_storage *clientaddr, socklen_t addrlen, const char *fmt, ...);
void ayiya_log(int level, struct sockaddr_storage *clientaddr, socklen_t addrlen, const char *fmt, ...)
{
	char	buf[1024];
	char	clienthost[NI_MAXHOST];
	char	clientservice[NI_MAXSERV];
	va_list	ap;

	/* First check for ratelimiting */
	if (level == LOG_ERR || level == LOG_WARNING)
	{

	}

	/* Clear them just in case */
	memset(buf, 0, sizeof(buf));
	memset(clienthost, 0, sizeof(clienthost));
	memset(clientservice, 0, sizeof(clientservice));

	if (0 != getnameinfo((struct sockaddr *)clientaddr, addrlen,
		clienthost, sizeof(clienthost),
		clientservice, sizeof(clientservice), NI_NUMERICHOST|NI_NUMERICSERV))
	{
		mdolog(LOG_ERR, "Resolve Error: %d : %s\n", errno, strerror(errno));
		strncpy(clienthost, "unknown", sizeof(clienthost));
		strncpy(clientservice, "unknown", sizeof(clientservice));
	}
	
	/* Print the host+port this is coming from */
	snprintf(buf, sizeof(buf), "[%s]:%s : ", clienthost, clientservice);

	/* Print the log message behind it */
	va_start(ap, fmt);
	vsnprintf(buf+strlen(buf), sizeof(buf)-strlen(buf), fmt, ap);
	va_end(ap);
	
	/* Actually Log it */
	mdolog(level, buf);

#if 0
	/* Add this one */
	memcpy(&lastlogs[log_last], &clientaddr, sizeof(lastlogs[log_last]));

	/* Cycle(tm) */
	log_last++;
	log_last%=(sizeof(lastlogs)/sizeof(struct in_addr));
#endif
}

/* From the interface (kernel) -> the other side of the tunnel */
void *ayiya_process_outgoing(void *arg);
void *ayiya_process_outgoing(void *arg)
{
	struct sixxs_interface	*iface = (struct sixxs_interface *)arg;
	SHA_CTX			sha1;
	sha1_byte		hash[SHA1_DIGEST_LENGTH];
	struct sockaddr_in	target;
	int			lenin, lenout;
	unsigned int		i;
	struct in_addr		in_any;

	struct pseudo_ayh	s;

	/* in_any == 0.0.0.0 */
	memset(&in_any, 0, sizeof(in_any));

	/* We tunnel over IPv4 */
	target.sin_family = AF_INET;

	/* Prefill some standard AYIYA values */
	memset(&s, 0, sizeof(s));
	s.ayh.ayh_idlen = 4;			/* 2^4 = 16 bytes = 128 bits (IPv6 address) */
	s.ayh.ayh_idtype = ayiya_id_integer;
	s.ayh.ayh_siglen = 5;			/* 5*4 = 20 bytes = 160 bits (SHA1) */
	s.ayh.ayh_hshmeth = ayiya_hash_sha1;
	s.ayh.ayh_autmeth = ayiya_auth_sharedsecret;
	s.ayh.ayh_opcode = ayiya_op_forward;
	s.ayh.ayh_nextheader = IPPROTO_IPV6;

	/* Our IPv6 side of this tunnel */
	memcpy(&s.identity, &iface->ipv6_us, sizeof(s.identity));

	while (iface->running)
	{
		lenin = read(iface->ayiya_fd, s.payload, sizeof(s.payload));
		if (lenin <= 0)
		{
			mdolog(LOG_ERR, "[outgoing] Error reading from %s (%d): %s\n", iface->name, errno, strerror(errno));
			continue;
		}

		if (lenin < 2)
		{
			mdolog(LOG_ERR, "[outgoing] Short packet of %u\n", lenin);
			continue;
		}

		if (((int)lenin) > ((int)sizeof(s.payload)))
		{
			mdolog(LOG_ERR, "[outgoing] Long packet of %d vs %u\n", lenin, sizeof(s.payload));
			continue;
		}

		/* Move around the bytes */
		memmove(&s.payload, &s.payload[4], lenin-4);
		lenin-=4;

		/*
		 * Check if the tunnel has a remote address
		 * This indeed requires action from the remote site first...
		 * and requires one of the two parties to have both addresses
		 * The other side knows the static PoP address so guess who takes it easy ;)
		 */

		if (memcmp(&iface->ipv4_them, &in_any, sizeof(iface->ipv4_them)) == 0)
		{
			/*
			 * Drop the packet, we should actually have disconfigured
			 * the thing though and thus this thread should not be running
			 */
			mdolog(LOG_ERR, "[outgoing] %s Received a packet while no remote address configured\n", iface->name);
			continue;
		}

		/* Fill in the current time */
		s.ayh.ayh_epochtime = htonl(time(NULL));

		/*
		 * The hash of the shared secret needs to be in the
		 * spot where we later put the complete hash
		 */
		memcpy(&s.hash, &iface->ayiya_hash, sizeof(s.hash));

		/* Generate a SHA1 */
		SHA1_Init(&sha1);
		/* Hash the complete AYIYA packet */
		SHA1_Update(&sha1, (unsigned char *)&s, sizeof(s)-sizeof(s.payload)+lenin);
		/* Store the hash in the packets hash */
		SHA1_Final(hash, &sha1);

		/* Store the hash in the packet */
		memcpy(&s.hash, &hash, sizeof(s.hash));

		/* Update the sockaddr, just in case it changed */
		target.sin_port = htons(iface->ayiya_port);
		memcpy(&target.sin_addr, &iface->ipv4_them, sizeof(target.sin_addr));

		/* Send it onto the network */
		lenin = sizeof(s)-sizeof(s.payload)+lenin;
		for (i=0; ayiya_socket[i].title; i++)
		{
			if (ayiya_socket[i].port != iface->ayiya_sport) continue;
			break;
		}
		lenout = sendto(ayiya_socket[i].socket, &s, lenin, 0, (struct sockaddr *)&target, sizeof(target));
		if (lenout < 0)
		{
			mdolog(LOG_ERR, "[outgoing] %s Error while sending %u bytes sent to network using process %s socket %d (%d): %s)\n", iface->name, lenin, ayiya_socket[i].title, ayiya_socket[i].socket, errno, strerror(errno));
		}
		else if (lenin != lenout)
		{
			mdolog(LOG_ERR, "[outgoing] %s Only %u of %u bytes sent to network (%d): %s)\n", iface->name, lenout, lenin, errno, strerror(errno));
		}
	}
	return NULL;
}

/* epochtime = epochtime as received in the packet */
/* Don't forget to convert byteorder using ntohl() */
int ayiya_checktime(time_t epochtime);
int ayiya_checktime(time_t epochtime)
{
	/* Number of seconds we allow the clock to be off */
	#define CLOCK_OFF 120
	int i;

	/* Get the current time */
	time_t curr_time = time(NULL);

	/* Is one of the times in the loop range? */
	if (	(curr_time >= -CLOCK_OFF) ||
        	(epochtime >= -CLOCK_OFF))
	{
		/* Shift the times out of the loop range */
		i =	(curr_time + (CLOCK_OFF*2)) -
			(epochtime + (CLOCK_OFF*2));
	}
	else i = curr_time - epochtime;

	/* The clock may be faster, thus flip the sign */
	if (i < 0) i = -i;

	/* Compare the clock offset */
	if (i > CLOCK_OFF)
	{
		/* Time is off, silently drop the packet */
		return i;
	}

	/* Time is in the allowed range */
	return 0;
}

/*
 * From the other side of the tunnel -> interface (kernel)
 * buf        = buffer containing the packet
 * clienthost = client information used for logging
 * clientlen  = length of the client information structure
 * protocol   = the protocol in which AYIYA was carried
*/
void ayiya_process_incoming(char *header, unsigned int length, struct sockaddr_storage *ci, socklen_t cl, unsigned int protocol, unsigned int sport);
void ayiya_process_incoming(char *header, unsigned int length, struct sockaddr_storage *ci, socklen_t cl, unsigned int protocol, unsigned int sport)
{
	SHA_CTX			sha1;
	sha1_byte		their_hash[SHA1_DIGEST_LENGTH],
				our_hash[SHA1_DIGEST_LENGTH];
	char			buf[1024];
	struct pseudo_ayh	*s = (struct pseudo_ayh *)header;
	int			i;
	unsigned int		j, payloadlen = 0;
	struct sixxs_interface	*iface = NULL;
	struct sixxs_prefix	*pfx = NULL;
	struct sockaddr_in	*ci4 = (struct sockaddr_in *)ci;

	/*
	 * - idlen must be 4 (2^4 = 16 bytes = 128 bits = IPv6 address)
	 * - It must be an integer identity
	 * - siglen must be 5 (5*4 = 20 bytes = 160 bits = SHA1)
	 * - Hash Method == SHA1
	 * - Authentication Method must be Shared Secret
	 * - Next header must be IPv6 or IPv6 No Next Header
	 * - Opcode must be 0 - 2
	 */
        if (	s->ayh.ayh_idlen != 4 ||
		s->ayh.ayh_idtype != ayiya_id_integer ||
		s->ayh.ayh_siglen != 5 ||
		s->ayh.ayh_hshmeth != ayiya_hash_sha1 ||
		s->ayh.ayh_autmeth != ayiya_auth_sharedsecret ||
		(s->ayh.ayh_nextheader != IPPROTO_IPV6 &&
		 s->ayh.ayh_nextheader != IPPROTO_NONE) ||
		(s->ayh.ayh_opcode != ayiya_op_forward &&
		 s->ayh.ayh_opcode != ayiya_op_echo_request &&
		 s->ayh.ayh_opcode != ayiya_op_echo_request_forward))
	{
		/* Invalid AYIYA packet */
		ayiya_log(LOG_ERR, ci, cl, "[incoming] Dropping invalid AYIYA packet\n");
		ayiya_log(LOG_ERR, ci, cl, "idlen:   %u != %u\n", s->ayh.ayh_idlen, 4);
		ayiya_log(LOG_ERR, ci, cl, "idtype:  %u != %u\n", s->ayh.ayh_idtype, ayiya_id_integer);
		ayiya_log(LOG_ERR, ci, cl, "siglen:  %u != %u\n", s->ayh.ayh_siglen, 5);
		ayiya_log(LOG_ERR, ci, cl, "hshmeth: %u != %u\n", s->ayh.ayh_hshmeth, ayiya_hash_sha1);
		ayiya_log(LOG_ERR, ci, cl, "autmeth: %u != %u\n", s->ayh.ayh_autmeth, ayiya_auth_sharedsecret);
		ayiya_log(LOG_ERR, ci, cl, "nexth  : %u != %u || %u\n", s->ayh.ayh_nextheader, IPPROTO_IPV6, IPPROTO_NONE);
		ayiya_log(LOG_ERR, ci, cl, "opcode : %u != %u || %u || %u\n", s->ayh.ayh_opcode, ayiya_op_forward, ayiya_op_echo_request, ayiya_op_echo_request_forward);
		return;
	}

	pfx = pfx_get(&s->identity, 128);
	if (!pfx || !pfx->is_tunnel)
	{
		memset(buf, 0, sizeof(buf));
		inet_ntop(AF_INET6, &s->identity, buf, sizeof(buf));
		ayiya_log(LOG_WARNING, ci, cl, "Unknown endpoint \"%s\"\n", buf);
		if (pfx) OS_Mutex_Release(&pfx->mutex, "ayiya_process_incoming");
		return;
	}

	/* The interface */
	iface = int_get(pfx->interface_id);
	OS_Mutex_Release(&pfx->mutex, "ayiya_process_incoming");
	if (!iface) return;
	
	/* Is this an AYIYA tunnel? */
	if (iface->type != IFACE_AYIYA)
	{
		ayiya_log(LOG_WARNING, ci, cl, "[incoming] Received AYIYA packet for non-AYIYA tunnel\n");
		OS_Mutex_Release(&iface->mutex, "ayiya_process_incoming");
		return;
	}

	/* Verify the epochtime */
	i = ayiya_checktime(ntohl(s->ayh.ayh_epochtime));
	if (i != 0)
	{
		memset(buf, 0, sizeof(buf));
		inet_ntop(AF_INET6, &iface->ipv6_them, buf, sizeof(buf));
		ayiya_log(LOG_WARNING, ci, cl, "[incoming] Time is %d seconds off for %u / %s \n", i, iface->interface_id, buf);
		OS_Mutex_Release(&iface->mutex, "ayiya_process_incoming");
		return;
	}

	/* How long is the payload? */
	payloadlen = length - (sizeof(*s) - sizeof(s->payload));

	/* Save their hash */
	memcpy(&their_hash, &s->hash, sizeof(their_hash));

	/* Copy in our SHA1 hash */
	memcpy(&s->hash, &iface->ayiya_hash, sizeof(s->hash));

	/* Generate a SHA1 of the header + identity + shared secret */
	SHA1_Init(&sha1);
	/* Hash the Packet */
	SHA1_Update(&sha1, (unsigned char *)s, length);
	/* Store the hash */
	SHA1_Final(our_hash, &sha1);

	/* Generate a SHA1 of the header + identity + shared secret */
	/* Compare the SHA1's */
	if (memcmp(&their_hash, &our_hash, sizeof(their_hash)) != 0)
	{
		ayiya_log(LOG_WARNING, ci, cl, "[incoming] Incorrect Hash received\n");
		OS_Mutex_Release(&iface->mutex, "ayiya_process_incoming");
		return;
	}

	/* Is it still the same host? No -> Change the endpoint */
	if (memcmp(&ci4->sin_addr, &iface->ipv4_them, sizeof(iface->ipv4_them)) != 0)
	{
		/* Modify the endpoint */
		int_set_endpoint(iface, ci4->sin_addr);
	}

	/* Changed the port? */
	j = ntohs(ci4->sin_port);
	if (j != iface->ayiya_port)
	{
		int_set_port(iface, j);
	}

	if (sport != iface->ayiya_sport) iface->ayiya_sport = sport;
	if (protocol != iface->ayiya_protocol) iface->ayiya_protocol = protocol;

	int_beat(iface);

	if (s->ayh.ayh_nextheader == IPPROTO_IPV6)
	{
		struct
		{
			struct tun_pi	pi;
			char		payload[2048];
		} packet;

		memset(&packet, 0, sizeof(packet));

		packet.pi.proto = htons(ETH_P_IPV6);
		memcpy(&packet.payload, &s->payload, payloadlen);

		/* Forward the packet to the kernel */
		write(iface->ayiya_fd, &packet, payloadlen+sizeof(struct tun_pi));
	}
	else
	{
		ayiya_log(LOG_WARNING, ci, cl, "[incoming] Not processing %u\n", (int)s->ayh.ayh_nextheader);
	}

	OS_Mutex_Release(&iface->mutex, "ayiya_process_incoming");
}

/* This thread handles incoming AYIYA packets */
void *ayiya_thread(void *arg);
void *ayiya_thread(void *arg)
{
	int			n;
	struct sockaddr_storage	ci;
	socklen_t		cl;
	char			buf[2048];
	struct ayiya_socket	*as = (struct ayiya_socket *)arg;
	SOCKET			s;

	/* Show that we have started */
	mdolog(LOG_INFO, "Anything in Anything Handler (%s)\n", as->title);

	/* Clear the lastlog table */
/*	memset(&lastlogs, 0, sizeof(lastlogs)); */

	if (!inet_ntop(AF_INET, &g_conf->pop_ipv4, buf, sizeof(buf)))
	{
		mdolog(LOG_ERR, "Configuration error, pop_ipv4 not set to a valid IPv4 address\n");
		return NULL;
	}

	/* Setup listening socket */
	s = listen_server("ayiya", buf, as->sport, AF_INET, SOCK_DGRAM, IPPROTO_UDP, NULL, 0);
	if (s < 0)
	{
		mdolog(LOG_ERR, "listen_server error:: could not create listening socket\n");
		return NULL;
	}

	as->socket = s;
	mddolog("%s using socket %d\n", as->title, s);

	while (g_conf->running)
	{
		cl = sizeof(ci);
		memset(buf, 0, sizeof(buf));
		n = recvfrom(s, buf, sizeof(buf), 0, (struct sockaddr *)&ci, &cl);

		if (n < 0) continue;

		if (n < (int)sizeof(struct ayiyahdr))
		{
			ayiya_log(LOG_WARNING, &ci, cl, "Packet too short\n");
			continue;
		}

		/* We got what could be a valid packet over UDP from a client */
		ayiya_process_incoming(buf, n, &ci, cl, IPPROTO_UDP, as->port);
	}

	return NULL;
}

bool ayiya_start(struct sixxs_interface *iface)
{
	struct ifreq ifr;
	char desc[128];

	/* Create a new tap device */
	iface->ayiya_fd = open("/dev/net/tun", O_RDWR);
	if (iface->ayiya_fd == -1)
	{
		mdolog(LOG_ERR, "Couldn't open device %s (%d): %s\n", "/dev/net/tun", errno, strerror(errno));
		/*
		 * Abort as we can't function properly
		 * on Linux do 'cd /dev/; ./MAKEDEV tun' + modprobe tun
		 */
		exit(-1);
	}
	
	memset(&ifr, 0, sizeof(ifr));
	/* Request a TUN device */
	ifr.ifr_flags = IFF_TUN;
	/* Set the interface name */
	strncpy(ifr.ifr_name, iface->name, sizeof(ifr.ifr_name));

	if (ioctl(iface->ayiya_fd, TUNSETIFF, &ifr))
	{
		mdolog(LOG_ERR, "Couldn't set interface name of %s (%d): %s\n", iface->name, errno, strerror(errno));
		return false;
	}

	iface->running = true;

	/* Add a thread for handling outgoing packets */
	snprintf(desc, sizeof(desc), "AYIYA-Out [%s]", iface->name);
	thread_add(desc, ayiya_process_outgoing, iface, true);

	return true;
}

bool ayiya_stop(struct sixxs_interface *iface)
{
	if (!iface->running) return true;

	iface->running = false;

	close(iface->ayiya_fd);
	iface->ayiya_fd = -1;

	return true;
}

void ayiya_init(void)
{
	unsigned int i;

        /* Create a thread for the AYIYA Handler */
	for (i=0; ayiya_socket[i].title; i++)
	{
		ayiya_socket[i].port = atoi(ayiya_socket[i].sport);
		thread_add(ayiya_socket[i].title, ayiya_thread, (void *)&ayiya_socket[i], true);
	}
}

