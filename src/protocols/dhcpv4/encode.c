/*
 *   This library is free software; you can redistribute it and/or
 *   modify it under the terms of the GNU Lesser General Public
 *   License as published by the Free Software Foundation; either
 *   version 2.1 of the License, or (at your option) any later version.
 *
 *   This library is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 *   Lesser General Public License for more details.
 *
 *   You should have received a copy of the GNU Lesser General Public
 *   License along with this library; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @file protocols/dhcpv4/encode.c
 * @brief Functions to encode DHCP options.
 *
 * @copyright 2008,2017 The FreeRADIUS server project
 * @copyright 2008 Alan DeKok <aland@deployingradius.com>
 * @copyright 2015,2017 Arran Cudbard-Bell <a.cudbardb@freeradius.org>
 */
#include <stdint.h>
#include <stddef.h>
#include <talloc.h>
#include <freeradius-devel/util/pair.h>
#include <freeradius-devel/util/types.h>
#include <freeradius-devel/util/proto.h>
#include <freeradius-devel/io/test_point.h>
#include "dhcpv4.h"
#include "attrs.h"

static inline bool is_encodable(fr_dict_attr_t const *root, VALUE_PAIR *vp)
{
	if (!vp) return false;
	if (vp->da->flags.internal) return false;
	if (!fr_dict_parent_common(root, vp->da, true)) return false;

	return true;
}

/** Find the next attribute to encode
 *
 * @param[in] cursor		to iterate over.
 * @param[in] encoder_ctx	Containing DHCPv4 dictionary.
 * @return
 *	- encodable VALUE_PAIR.
 *	- NULL if none available.
 */
static inline VALUE_PAIR *next_encodable(fr_cursor_t *cursor, void *encoder_ctx)
{
	VALUE_PAIR	*vp;
	fr_dhcpv4_ctx_t	*packet_ctx = encoder_ctx;

	while ((vp = fr_cursor_next(cursor))) if (is_encodable(packet_ctx->root, vp)) break;
	return fr_cursor_current(cursor);
}

/** Determine if the current attribute is encodable, or find the first one that is
 *
 * @param[in] cursor		to iterate over.
 * @param[in] encoder_ctx	Containing DHCPv4 dictionary.
 * @return
 *	- encodable VALUE_PAIR.
 *	- NULL if none available.
 */
static inline VALUE_PAIR *first_encodable(fr_cursor_t *cursor, void *encoder_ctx)
{
	VALUE_PAIR	*vp;
	fr_dhcpv4_ctx_t	*packet_ctx = encoder_ctx;

	vp = fr_cursor_current(cursor);
	if (is_encodable(packet_ctx->root, vp)) return vp;

	return next_encodable(cursor, encoder_ctx);
}

/** Write DHCP option value into buffer
 *
 * Does not include DHCP option length or number.
 *
 * @param[out] out		buffer to write the option to.
 * @param[in] outlen		length of the output buffer.
 * @param[in] tlv_stack		Describing nesting of options.
 * @param[in] depth		in tlv_stack.
 * @param[in,out] cursor	Current attribute we're encoding.
 * @param[in] encoder_ctx	Containing DHCPv4 dictionary.
 * @return
 *	- The length of data writen.
 *	- -1 if out of buffer.
 *	- -2 if unsupported type.
 */
static ssize_t encode_value(uint8_t *out, size_t outlen,
			    fr_dict_attr_t const **tlv_stack, unsigned int depth,
			    fr_cursor_t *cursor, fr_dhcpv4_ctx_t *encoder_ctx)
{
	uint32_t lvalue;

	VALUE_PAIR *vp = fr_cursor_current(cursor);
	uint8_t *p = out;

	FR_PROTO_STACK_PRINT(tlv_stack, depth);
	FR_PROTO_TRACE("%zu byte(s) available for value", outlen);

	if (outlen < vp->vp_length) return -1;	/* Not enough output buffer space. */

	switch (tlv_stack[depth]->type) {
	case FR_TYPE_UINT8:
		p[0] = vp->vp_uint8;
		p ++;
		break;

	case FR_TYPE_UINT16:
		p[0] = (vp->vp_uint16 >> 8) & 0xff;
		p[1] = vp->vp_uint16 & 0xff;
		p += 2;
		break;

	case FR_TYPE_UINT32:
		lvalue = htonl(vp->vp_uint32);
		memcpy(p, &lvalue, 4);
		p += 4;
		break;

	case FR_TYPE_IPV4_ADDR:
		memcpy(p, &vp->vp_ipv4addr, 4);
		p += 4;
		break;

	case FR_TYPE_IPV6_ADDR:
		memcpy(p, &vp->vp_ipv6addr, 16);
		p += 16;
		break;

	case FR_TYPE_ETHERNET:
		memcpy(p, vp->vp_ether, 6);
		p += 6;
		break;

	case FR_TYPE_STRING:
		memcpy(p, vp->vp_strvalue, vp->vp_length);
		p += vp->vp_length;
		break;

	case FR_TYPE_OCTETS:
		memcpy(p, vp->vp_octets, vp->vp_length);
		p += vp->vp_length;
		break;

	default:
		fr_strerror_printf("Unsupported option type %d", vp->vp_type);
		(void)next_encodable(cursor, encoder_ctx);
		return -2;
	}
	vp = next_encodable(cursor, encoder_ctx);	/* We encoded a leaf, advance the cursor */
	fr_proto_tlv_stack_build(tlv_stack, vp ? vp->da : NULL);

	FR_PROTO_STACK_PRINT(tlv_stack, depth);
	FR_PROTO_HEX_DUMP(out, (p - out), "Value");

	return p - out;
}

/** Write out an RFC option header and option data
 *
 * @note May coalesce options with fixed width values
 *
 * @param[out] out		buffer to write the TLV to.
 * @param[in] outlen		length of the output buffer.
 * @param[in] tlv_stack		Describing nesting of options.
 * @param[in] depth		in the tlv_stack.
 * @param[in,out] cursor	Current attribute we're encoding.
 * @param[in] encoder_ctx	Containing DHCPv4 dictionary.
 * @return
 *	- >0 length of data encoded.
 *	- 0 if we ran out of space.
 *	- < 0 on error.
 */
static ssize_t encode_rfc_hdr(uint8_t *out, ssize_t outlen,
			      fr_dict_attr_t const **tlv_stack, unsigned int depth,
			      fr_cursor_t *cursor, fr_dhcpv4_ctx_t *encoder_ctx)
{
	ssize_t			len;
	uint8_t			*p = out;
	fr_dict_attr_t const	*da = tlv_stack[depth];
	VALUE_PAIR		*vp = fr_cursor_current(cursor);

	if (outlen < 3) return 0;	/* No space */

	FR_PROTO_STACK_PRINT(tlv_stack, depth);

	/*
	 *	Write out the option number
	 */
	out[0] = da->attr & 0xff;
	out[1] = 0;	/* Length of the value only (unlike RADIUS) */

	outlen -= 2;
	p += 2;

	/*
	 *	Check here so we get the full 255 bytes
	 */
	if (outlen > UINT8_MAX) outlen = UINT8_MAX;

	/*
	 *	DHCP options with the same number (and array flag set)
	 *	get coalesced into a single option.
	 *
	 *	Note: This only works with fixed length attributes,
	 *	because there's no separate length fields.
	 */
	do {
		VALUE_PAIR *next;

		len = encode_value(p, outlen - out[1], tlv_stack, depth, cursor, encoder_ctx);
		if (len < -1) return len;
		if (len == -1) {
			FR_PROTO_TRACE("No more space in option");
			if (out[1] == 0) {
				/* Couldn't encode anything: don't leave behind these two octets. */
				p -= 2;
			}
			break; /* Packed as much as we can */
		}

		FR_PROTO_STACK_PRINT(tlv_stack, depth);
		FR_PROTO_TRACE("Encoded value is %zu byte(s)", len);
		FR_PROTO_HEX_DUMP(out, (p - out), NULL);

		p += len;
		out[1] += len;

		FR_PROTO_TRACE("%zu byte(s) available in option", outlen - out[1]);

		next = fr_cursor_current(cursor);
		if (!next || (vp->da != next->da)) break;
		vp = next;
	} while (vp->da->flags.array);

	return p - out;
}

/** Write out a TLV header (and any sub TLVs or values)
 *
 * @param[out] out		buffer to write the TLV to.
 * @param[in] outlen		length of the output buffer.
 * @param[in] tlv_stack		Describing nesting of options.
 * @param[in] depth		in the tlv_stack.
 * @param[in,out] cursor	Current attribute we're encoding.
 * @param[in] encoder_ctx	Containing DHCPv4 dictionary.
 * @return
 *	- >0 length of data encoded.
 *	- 0 if we ran out of space.
 *	- < 0 on error.
 */
static ssize_t encode_tlv_hdr(uint8_t *out, ssize_t outlen,
			      fr_dict_attr_t const **tlv_stack, unsigned int depth,
			      fr_cursor_t *cursor, fr_dhcpv4_ctx_t *encoder_ctx)
{
	ssize_t			len;
	uint8_t			*p = out;
	VALUE_PAIR const	*vp = fr_cursor_current(cursor);
	fr_dict_attr_t const	*da = tlv_stack[depth];

	if (outlen < 5) return 0;	/* No space */

	FR_PROTO_STACK_PRINT(tlv_stack, depth);

	/*
	 *	Write out the option number
	 */
	out[0] = da->attr & 0xff;
	out[1] = 0;	/* Length of the value only (unlike RADIUS) */

	outlen -= 2;
	p += 2;

	/*
	 *	Check here so we get the full 255 bytes
	 */
	if (outlen > UINT8_MAX) outlen = UINT8_MAX;

	/*
	 *	Encode any sub TLVs or values
	 */
	while (outlen >= 3) {
		/*
		 *	Determine the nested type and call the appropriate encoder
		 */
		if (tlv_stack[depth + 1]->type == FR_TYPE_TLV) {
			len = encode_tlv_hdr(p, outlen - out[1], tlv_stack, depth + 1, cursor, encoder_ctx);
		} else {
			len = encode_rfc_hdr(p, outlen - out[1], tlv_stack, depth + 1, cursor, encoder_ctx);
		}
		if (len < 0) return len;
		if (len == 0) break;		/* Insufficient space */

		p += len;
		out[1] += len;

		FR_PROTO_STACK_PRINT(tlv_stack, depth);
		FR_PROTO_HEX_DUMP(out, (p - out), "TLV header and sub TLVs");

		/*
		 *	If nothing updated the attribute, stop
		 */
		if (!fr_cursor_current(cursor) || (vp == fr_cursor_current(cursor))) break;

		/*
	 	 *	We can encode multiple sub TLVs, if after
	 	 *	rebuilding the TLV Stack, the attribute
	 	 *	at this depth is the same.
	 	 */
		if (da != tlv_stack[depth]) break;
		vp = fr_cursor_current(cursor);
	}

	return p - out;
}

/** Encode a DHCP option and any sub-options.
 *
 * @param[out] out		Where to write encoded DHCP attributes.
 * @param[in] outlen		Length of out buffer.
 * @param[in] cursor		with current VP set to the option to be encoded.
 *				Will be advanced to the next option to encode.
 * @param[in] encoder_ctx	Containing DHCPv4 dictionary.
 * @return
 *	- > 0 length of data written.
 *	- < 0 error.
 *	- 0 not valid option for DHCP (skipping).
 */
ssize_t fr_dhcpv4_encode_option(uint8_t *out, size_t outlen, fr_cursor_t *cursor, void *encoder_ctx)
{
	VALUE_PAIR		*vp;
	unsigned int		depth = 0;
	fr_dict_attr_t const	*tlv_stack[FR_DICT_MAX_TLV_STACK + 1];
	ssize_t			len;

	vp = first_encodable(cursor, encoder_ctx);
	if (!vp) return -1;

	if (vp->da == attr_dhcp_message_type) goto next; /* already done */
	if ((vp->da->attr > 255) && (vp->da->attr != FR_DHCP_OPTION_82)) {
	next:
		fr_strerror_printf("Attribute \"%s\" is not a DHCP option", vp->da->name);
		next_encodable(cursor, encoder_ctx);
		return 0;
	}

	fr_proto_tlv_stack_build(tlv_stack, vp->da);

	FR_PROTO_STACK_PRINT(tlv_stack, depth);

	/*
	 *	We only have two types of options in DHCPv4
	 */
	switch (tlv_stack[depth]->type) {
	case FR_TYPE_TLV:
		len = encode_tlv_hdr(out, outlen, tlv_stack, depth, cursor, encoder_ctx);
		break;

	default:
		len = encode_rfc_hdr(out, outlen, tlv_stack, depth, cursor, encoder_ctx);
		break;
	}

	if (len < 0) return len;

	FR_PROTO_TRACE("Complete option is %zu byte(s)", len);
	FR_PROTO_HEX_DUMP(out, len, NULL);

	return len;
}

static int _encode_test_ctx(UNUSED fr_dhcpv4_ctx_t *test_ctx)
{
	fr_dhcpv4_global_free();

	return 0;
}

static int encode_test_ctx(void **out, TALLOC_CTX *ctx)
{
	fr_dhcpv4_ctx_t *test_ctx;

	if (fr_dhcpv4_global_init() < 0) return -1;

	test_ctx = talloc_zero(ctx, fr_dhcpv4_ctx_t);
	if (!test_ctx) return -1;
	test_ctx->root = fr_dict_root(dict_dhcpv4);
	talloc_set_destructor(test_ctx, _encode_test_ctx);

	*out = test_ctx;

	return 0;
}

/*
 *	Test points
 */
extern fr_test_point_pair_encode_t dhcpv4_tp_encode;
fr_test_point_pair_encode_t dhcpv4_tp_encode = {
	.test_ctx	= encode_test_ctx,
	.func		= fr_dhcpv4_encode_option
};