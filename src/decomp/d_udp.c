/*
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 59 Temple Place, Suite 330, Boston, MA  02111-1307  USA
 */

/**
 * @file d_udp.c
 * @brief ROHC decompression context for the UDP profile.
 * @author Didier Barvaux <didier.barvaux@toulouse.viveris.com>
 * @author The hackers from ROHC for Linux
 */

#include "d_udp.h"
#include "rohc_bit_ops.h"
#include "rohc_traces.h"
#include "rohc_debug.h"
#include "crc.h"

#include <netinet/ip.h>
#include <netinet/udp.h>


/*
 * Private function prototypes.
 */

static void d_udp_destroy(void *const context)
	__attribute__((nonnull(1)));

static int udp_parse_dynamic_udp(struct d_generic_context *context,
                                 const unsigned char *packet,
                                 unsigned int length,
                                 unsigned char *dest);


static int udp_parse_uo_tail_udp(struct d_generic_context *context,
                                 const unsigned char *packet,
                                 unsigned int length,
                                 unsigned char *dest);


/**
 * @brief Create the UDP decompression context.
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @return The newly-created UDP decompression context
 */
void * d_udp_create(void)
{
	struct d_generic_context *context;
	struct d_udp_context *udp_context;

	/* create the generic context */
	context = d_generic_create();
	if(context == NULL)
	{
		goto quit;
	}

	/* create the UDP-specific part of the context */
	udp_context = malloc(sizeof(struct d_udp_context));
	if(udp_context == NULL)
	{
		rohc_debugf(0, "cannot allocate memory for the UDP-specific context\n");
		goto destroy_context;
	}
	bzero(udp_context, sizeof(struct d_udp_context));
	context->specific = udp_context;

	/* create the LSB decoding context for SN */
	context->sn_lsb_ctxt = rohc_lsb_new(ROHC_LSB_SHIFT_SN);
	if(context->sn_lsb_ctxt == NULL)
	{
		rohc_debugf(0, "failed to create the LSB decoding context for SN\n");
		goto free_udp_context;
	}

	/* the UDP checksum field present flag will be initialized
	 * with the IR packets */
	udp_context->udp_checksum_present = -1;

	/* some UDP-specific values and functions */
	context->next_header_len = sizeof(struct udphdr);
	context->build_next_header = udp_build_uncompressed_udp;
	context->parse_static_next_hdr = udp_parse_static_udp;
	context->parse_dyn_next_hdr = udp_parse_dynamic_udp;
	context->parse_uo_tail = udp_parse_uo_tail_udp;
	context->compute_crc_static = udp_compute_crc_static;
	context->compute_crc_dynamic = udp_compute_crc_dynamic;

	/* create the UDP-specific part of the header changes */
	context->outer_ip_changes->next_header_len = sizeof(struct udphdr);
	context->outer_ip_changes->next_header = malloc(sizeof(struct udphdr));
	if(context->outer_ip_changes->next_header == NULL)
	{
		rohc_debugf(0, "cannot allocate memory for the UDP-specific "
		            "part of the outer IP header changes\n");
		goto free_lsb_sn;
	}
	bzero(context->outer_ip_changes->next_header, sizeof(struct udphdr));

	context->inner_ip_changes->next_header_len = sizeof(struct udphdr);
	context->inner_ip_changes->next_header = malloc(sizeof(struct udphdr));
	if(context->inner_ip_changes->next_header == NULL)
	{
		rohc_debugf(0, "cannot allocate memory for the UDP-specific "
		            "part of the inner IP header changes\n");
		goto free_outer_ip_changes_next_header;
	}
	bzero(context->inner_ip_changes->next_header, sizeof(struct udphdr));

	/* set next header to UDP */
	context->next_header_proto = IPPROTO_UDP;

	return context;

free_outer_ip_changes_next_header:
	zfree(context->outer_ip_changes->next_header);
free_lsb_sn:
	rohc_lsb_free(context->sn_lsb_ctxt);
free_udp_context:
	zfree(udp_context);
destroy_context:
	d_generic_destroy(context);
quit:
	return NULL;
}


/**
 * @brief Destroy the context.
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * @param context The compression context
 */
static void d_udp_destroy(void *const context)
{
	struct d_generic_context *g_context;

	assert(context != NULL);
	g_context = (struct d_generic_context *) context;

	/* clean UDP-specific memory */
	assert(g_context->outer_ip_changes != NULL);
	assert(g_context->outer_ip_changes->next_header != NULL);
	zfree(g_context->outer_ip_changes->next_header);
	assert(g_context->inner_ip_changes != NULL);
	assert(g_context->inner_ip_changes->next_header != NULL);
	zfree(g_context->inner_ip_changes->next_header);

	/* destroy the LSB decoding context for SN */
	rohc_lsb_free(g_context->sn_lsb_ctxt);

	/* destroy the resources of the generic context */
	d_generic_destroy(context);
}


/**
 * @brief Get the size of the static part of an IR packet
 * @return the size
 */
int udp_get_static_size(void)
{
	return 4;
}


/**
 * @brief Find the length of the IR header.
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * \verbatim

 Basic structure of the IR packet (5.7.7.1):

      0   1   2   3   4   5   6   7
     --- --- --- --- --- --- --- ---
 1  |         Add-CID octet         |  if for small CIDs and CID != 0
    +---+---+---+---+---+---+---+---+
 2  | 1   1   1   1   1   1   0 | D |
    +---+---+---+---+---+---+---+---+
    |                               |
 3  /    0-2 octets of CID info     /  1-2 octets if for large CIDs
    |                               |
    +---+---+---+---+---+---+---+---+
 4  |            Profile            |  1 octet
    +---+---+---+---+---+---+---+---+
 5  |              CRC              |  1 octet
    +---+---+---+---+---+---+---+---+
    |                               |
 6  |         Static chain          |  variable length
    |                               |
    +---+---+---+---+---+---+---+---+
    |                               |
 7  |         Dynamic chain         |  present if D = 1, variable length
    |                               |
    +---+---+---+---+---+---+---+---+
 8  |             SN                |  2 octets if not RTP
    +---+---+---+---+---+---+---+---+
    |                               |
 9  |           Payload             |  variable length
    |                               |
     - - - - - - - - - - - - - - - -

\endverbatim
 *
 * The function computes the length of the fields 2 + 4-8, ie. the first byte,
 * the Profile and CRC fields, the static and dynamic chains (outer and inner
 * IP headers + UDP header) and the SN.
 *
 * @param context         The decompression context
 * @param packet          The pointer on the IR packet minus the Add-CID byte
 *                        (ie. the field 2 in the figure)
 * @param plen            The length of the IR packet minus the Add-CID byte
 * @param large_cid_len   The size of the large CID field
 *                        (ie. field 3 in the figure)
 * @return                The length of the IR header,
 *                        0 if an error occurs
 */
unsigned int udp_detect_ir_size(struct d_context *context,
                                unsigned char *packet,
                                unsigned int plen,
                                unsigned int large_cid_len)
{
	unsigned int length, d;

	/* Profile and CRC fields + IP static & dynamic chains + SN */
	length = ip_detect_ir_size(context, packet, plen, large_cid_len);
	if(length == 0)
	{
		goto quit;
	}

	/* UDP static part (see 5.7.7.5 in RFC 3095) */
	length += udp_get_static_size();

	/* UDP dynamic part if included (see 5.7.7.5 in RFC 3095) */
	d = GET_BIT_0(packet);
	if(d)
	{
		length += 2;
	}

quit:
	return length;
}


/**
 * @brief Find the length of the IR-DYN header.
 *
 * This function is one of the functions that must exist in one profile for the
 * framework to work.
 *
 * \verbatim

 Basic structure of the IR-DYN packet (5.7.7.2):

      0   1   2   3   4   5   6   7
     --- --- --- --- --- --- --- ---
 1  :         Add-CID octet         : if for small CIDs and CID != 0
    +---+---+---+---+---+---+---+---+
 2  | 1   1   1   1   1   0   0   0 | IR-DYN packet type
    +---+---+---+---+---+---+---+---+
    :                               :
 3  /     0-2 octets of CID info    / 1-2 octets if for large CIDs
    :                               :
    +---+---+---+---+---+---+---+---+
 4  |            Profile            | 1 octet
    +---+---+---+---+---+---+---+---+
 5  |              CRC              | 1 octet
    +---+---+---+---+---+---+---+---+
    |                               |
 6  /         Dynamic chain         / variable length
    |                               |
    +---+---+---+---+---+---+---+---+
 7  |             SN                | 2 octets if not RTP
    +---+---+---+---+---+---+---+---+
    :                               :
 8  /           Payload             / variable length
    :                               :
     - - - - - - - - - - - - - - - -

\endverbatim
 *
 * The function computes the length of the fields 2 + 4-8, ie. the first byte,
 * the Profile and CRC fields, the dynamic chains (outer and inner IP headers +
 * UDP header) and the SN.
 *
 * @param context         The decompression context
 * @param packet          The IR-DYN packet after the Add-CID byte if present
 *                        (ie. the field 2 in the figure)
 * @param plen            The length of the IR-DYN packet minus the Add-CID byte
 * @param large_cid_len   The size of the large CID field
 *                        (ie. field 3 in the figure)
 * @return                The length of the IR-DYN header,
 *                        0 if an error occurs
 */
unsigned int udp_detect_ir_dyn_size(struct d_context *context,
                                    unsigned char *packet,
                                    unsigned int plen,
                                    unsigned int large_cid_len)
{
	unsigned int length;

	/* Profile and CRC fields + IP dynamic chains */
	length = ip_detect_ir_dyn_size(context, packet, plen, large_cid_len);
	if(length == 0)
	{
		goto quit;
	}

	/* UDP dynamic part (see 5.7.7.5 in RFC 3095) */
	length += 2;

quit:
	return length;
}


/**
 * @brief Parse the UDP static part of the ROHC packet.
 *
 * @param context The generic decompression context
 * @param packet  The ROHC packet to parse
 * @param length  The length of the ROHC packet
 * @param dest    The decoded UDP header
 * @return        The number of bytes read in the ROHC packet,
 *                -1 in case of failure
 */
int udp_parse_static_udp(struct d_generic_context *context,
                         const unsigned char *packet,
                         unsigned int length,
                         unsigned char *dest)
{
	struct udphdr *udp = (struct udphdr *) dest;
	int read = 0; /* number of bytes read from the packet */

	/* check the minimal length to decode the UDP static part */
	if(length < 4)
	{
		rohc_debugf(0, "ROHC packet too small (len = %d)\n", length);
		goto error;
	}

	udp->source = GET_NEXT_16_BITS(packet);
	rohc_debugf(3, "UDP source port = 0x%04x\n", ntohs(udp->source));
	packet += 2;
	read += 2;

	udp->dest = GET_NEXT_16_BITS(packet);
	rohc_debugf(3, "UDP destination port = 0x%04x\n", ntohs(udp->dest));
	packet += 2;
	read += 2;

	return read;

error:
	return -1;
}


/**
 * @brief Parse the UDP dynamic part of the ROHC packet.
 *
 * @param context      The generic decompression context
 * @param packet       The ROHC packet to parse
 * @param length       The length of the ROHC packet
 * @param dest         The decoded UDP header
 * @return             The number of bytes read in the ROHC packet,
 *                     -1 in case of failure
 */
static int udp_parse_dynamic_udp(struct d_generic_context *context,
                                 const unsigned char *packet,
                                 unsigned int length,
                                 unsigned char *dest)
{
	struct d_udp_context *udp_context;
	struct udphdr *udp;
	int read = 0; /* number of bytes read from the packet */
	int ret;

	udp_context = context->specific;
	udp = (struct udphdr *) dest;

	/* UDP checksum */
	if(length < 2)
	{
		rohc_debugf(0, "ROHC packet too small (len = %d)\n", length);
		goto error;
	}
	udp->check = GET_NEXT_16_BITS(packet);
	rohc_debugf(3, "UDP checksum = 0x%04x\n", ntohs(udp->check));
	packet += 2;
	read += 2;

	/* determine whether the UDP checksum will be present in UO packets */
	udp_context->udp_checksum_present = (udp->check > 0);

	/* SN field */
	ret = ip_parse_dynamic_ip(context, packet, length - read, dest + read);
	if(ret == -1)
	{
		goto error;
	}
	packet += ret;
	read += ret;

	return read;

error:
	return -1;
}


/**
 * @brief Parse the UDP tail of the UO* ROHC packets.
 *
 * @param context      The generic decompression context
 * @param packet       The ROHC packet to parse
 * @param length       The length of the ROHC packet
 * @param dest         The decoded UDP header
 * @return             The number of bytes read in the ROHC packet,
 *                     -1 in case of failure
 */
static int udp_parse_uo_tail_udp(struct d_generic_context *context,
                                 const unsigned char *packet,
                                 unsigned int length,
                                 unsigned char *dest)
{
	struct d_udp_context *udp_context;
	struct udphdr *udp;
	int read = 0; /* number of bytes read from the packet */

	udp_context = context->specific;
	udp = (struct udphdr *) dest;

	/* UDP checksum if necessary:
	 *  udp_checksum_present < 0 <=> not initialized
	 *  udp_checksum_present = 0 <=> UDP checksum field not present
	 *  udp_checksum_present > 0 <=> UDP checksum field present */
	if(udp_context->udp_checksum_present > 0)
	{
		/* check the minimal length to decode the UDP checksum */
		if(length < 2)
		{
			rohc_debugf(0, "ROHC packet too small (len = %d)\n", length);
			goto error;
		}

		/* retrieve the UDP checksum from the ROHC packet */
		udp->check = GET_NEXT_16_BITS(packet);
		rohc_debugf(3, "UDP checksum = 0x%04x\n", ntohs(udp->check));
		packet += 2;
		read += 2;
	}
	else if(udp_context->udp_checksum_present < 0)
	{
		rohc_debugf(0, "udp_checksum_present not initialized and "
		            "packet is not one IR packet\n");
		goto error;
	}

	return read;

error:
	return -1;
}


/**
 * @brief Build an uncompressed UDP header.
 *
 * @param context      The generic decompression context
 * @param active       The UDP header changes
 * @param dest         The buffer to store the UDP header (MUST be at least
 *                     of sizeof(struct udphdr) length)
 * @param payload_size The length of the UDP payload
 * @return             The length of the next header (ie. the UDP header),
 *                     -1 in case of error
 */
int udp_build_uncompressed_udp(struct d_generic_context *context,
                               struct d_generic_changes *active,
                               unsigned char *dest,
                               int payload_size)
{
	struct d_udp_context *udp_context = context->specific;
	struct udphdr *udp_active = (struct udphdr *) active->next_header;
	struct udphdr *udp = (struct udphdr *) dest;

	/* static + checksum */
	memcpy(dest, udp_active, sizeof(struct udphdr));

	/* UDP checksum:
	 *  - error if udp_checksum_present not initialized,
	 *    ie. udp_checksum_present < 0
	 *  - already copied if checksum is present,
	 *    ie. udp_checksum_present > 0
	 *  - set checksum to zero if checksum is not present,
	 *    ie. udp_checksum_present = 0  */
	if(udp_context->udp_checksum_present < 0)
	{
		rohc_debugf(0, "udp_checksum_present not initialized\n");
		goto error;
	}
	else if(udp_context->udp_checksum_present == 0)
	{
		udp->check = 0;
	}
	rohc_debugf(3, "UDP checksum = 0x%04x\n", ntohs(udp->check));

	/* interfered fields */
	udp->len = htons(payload_size + sizeof(struct udphdr));
	rohc_debugf(3, "UDP length = 0x%04x\n", ntohs(udp->len));

	return sizeof(struct udphdr);

error:
	return -1;
}


/**
 * @brief Define the decompression part of the UDP profile as described
 *        in the RFC 3095.
 */
struct d_profile d_udp_profile =
{
	ROHC_PROFILE_UDP,       /* profile ID (see 8 in RFC 3095) */
	"UDP / Decompressor",   /* profile description */
	d_generic_decode,       /* profile handlers */
	d_generic_decode_ir,
	d_udp_create,
	d_udp_destroy,
	udp_detect_ir_size,
	udp_detect_ir_dyn_size,
	udp_get_static_size,
	d_generic_get_sn,
};

