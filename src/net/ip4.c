/* ip4.c: Internet Protocol version 4
 * Copyright © 2011-2015 Lukas Martini
 *
 * This file is part of Xelix.
 *
 * Xelix is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * Xelix is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with Xelix. If not, see <http://www.gnu.org/licenses/>.
 */

#include "ip4.h"
#include <lib/log.h>
#include <lib/endian.h>
#include <lib/string.h>
#include <net/ether.h>
#include <net/udp.h>
#include <memory/kmalloc.h>
#include <lib/panic.h>
#include <lib/print.h>

/* These flags and the offset are stored in one uint16_t. The flags are bits
 * one to three, the offset occupies the remaining bits. The offset is also
 * stored in multiples of 8.
 */
#define PKG_MORE_FRAGMENTS(pkg) ((bool)(endian_swap16(pkg ->off) & 0x2000))
#define PKG_DONT_FRAGMENT(pkg) ((bool)(endian_swap16(pkg ->off) & 0x4000))
#define PKG_FRAGMENT_OFFSET(pkg) ((endian_swap16(pkg ->off) & 0x1fff) * 8)

// This is per IP/ID tuple
// FIXME Should be dynamic
#define MAX_PKGS_IN_FRAGMENT_STORAGE 500

// Source IP and ID are stored in network endian in this for simplicity
struct fragment_entry {
	ip4_addr_t source;
	uint16_t id;
	uint32_t stored_packets;
	ip4_header_t** packets;
	struct fragment_entry* next;
};

struct fragment_entry* first_fragment = NULL;
struct fragment_entry* last_fragment = NULL;
uint32_t stored_fragments = 0;

char* ip4_split_ip(char* out, int ip)
{
	unsigned char bytes[4];
	bytes[0] = ip & 0xFF;
	bytes[1] = (ip >> 8) & 0xFF;
	bytes[2] = (ip >> 16) & 0xFF;
	bytes[3] = (ip >> 24) & 0xFF;

	// FIXME We neeeeeeed snprintf!
	strcat(out, itoa(bytes[3], 10));
	strcat(out, ".");
	strcat(out, itoa(bytes[2], 10));
	strcat(out, ".");
	strcat(out, itoa(bytes[1], 10));
	strcat(out, ".");
	strcat(out, itoa(bytes[0], 10));
	return out;
}

void prepare_packet_to_send(ip4_header_t* packet) {
	packet->version = 4;
	packet->id = (uint16_t)(pit_getTickNum() % 65535);
	packet->checksum = 0;
	packet->checksum = endian_swap16(net_calculate_checksum((uint8_t*)packet, sizeof(ip4_header_t), 0));
}

void ip4_send_ether(net_device_t *target, size_t size, ip4_header_t *packet, ether_frame_hdr_t *hdr)
{
	if (target->proto != NET_PROTO_ETH)
	{
		ip4_send(target, size, packet);
		return;
	}

	prepare_packet_to_send(packet);

	ether_frame_hdr_t *etherhdr = kmalloc(sizeof(ether_frame_hdr_t) + size);
	memset(etherhdr, 0, sizeof(ether_frame_hdr_t));
	if (hdr != NULL)
		memcpy(etherhdr, hdr, sizeof(ether_frame_hdr_t));
	memcpy(etherhdr + 1, packet, size);

	net_send(target, size + sizeof(ether_frame_hdr_t), (uint8_t*)etherhdr);
}

void ip4_send(net_device_t* target, size_t size, ip4_header_t* packet)
{
	prepare_packet_to_send(packet);

	if (target->proto == NET_PROTO_ETH)
	{
		/* TODO Implement some ARP things */
		ip4_send_ether(target, size, packet, NULL);
		return;
	}

	net_send(target, size, (void*)packet);
}

static void handle_icmp(net_device_t* origin, size_t size, ip4_header_t* ip_packet, ether_frame_hdr_t *etherhdr)
{
	ip4_icmp_header_t* packet = (ip4_icmp_header_t*)(ip_packet + 1);
	size_t packet_size = size - sizeof(ip4_header_t);

	//if(packet->type != 8)
	//	return;
	
	if(endian_swap16(packet->sequence) == 1)
	{
		char* ip = ip4_split_ip((char*)kmalloc(sizeof(char) * 15), endian_swap32(ip_packet->src));
		log(LOG_INFO, "net: ip4: %s started ICMP pinging this host.\n", ip, endian_swap16(packet->sequence));
		kfree(ip);
	}

	// We can reuse the existing packet as the most stuff stays unmodified
	uint32_t orig_src = ip_packet->src;
	ip_packet->src = ip_packet->dst;
	ip_packet->dst = orig_src;

	packet->type = 0;
	packet->code = 0;

	packet->checksum = 0;
	packet->checksum = endian_swap16(net_calculate_checksum((uint8_t*)packet, packet_size, 0));

	char destination[6];
	memcpy(destination, etherhdr->source, 6);
	memcpy(etherhdr->source, etherhdr->destination, 6);
	memcpy(etherhdr->destination, destination, 6);
	
	ip4_send_ether(origin, size, ip_packet, etherhdr);
}

static struct fragment_entry* locate_fragment(ip4_header_t* packet) {
	if(!first_fragment) {
		return NULL;
	}

	struct fragment_entry* fragment = first_fragment;
	for(; fragment; fragment = fragment->next) {
		if(fragment->source == packet->src && fragment->id == packet->id) {
			return fragment;
		}
	}

	return NULL;
}

static struct fragment_entry* create_fragment_store(ip4_addr_t src, uint16_t id) {
	struct fragment_entry* fragment = (struct fragment_entry*)kmalloc(sizeof(struct fragment_entry));
	fragment->packets = (ip4_header_t**)kmalloc(sizeof(void*) * MAX_PKGS_IN_FRAGMENT_STORAGE);
	fragment->source = src;
	fragment->id = id;
	fragment->stored_packets = 0;
	fragment->next = NULL;

	if(last_fragment) {
		assert(last_fragment->next != last_fragment);
		last_fragment->next = fragment;
	}

	last_fragment = fragment;

	if(!first_fragment) {
		first_fragment = fragment;
	}

	stored_fragments++;
	return fragment;
}

static struct fragment_entry* store_fragment(ip4_header_t* packet, struct fragment_entry* fragment) {
	if(!fragment) {
		fragment = create_fragment_store(packet->src, packet->id);
	}

	if(fragment->stored_packets > MAX_PKGS_IN_FRAGMENT_STORAGE) {
		// FIXME Do some reasonable error handling here or make dynamic
		return NULL;
	}

	fragment->packets[fragment->stored_packets] = packet;
	fragment->packets[++fragment->stored_packets] = NULL;
	return fragment;
}

void ip4_sort_packet(net_device_t* origin, size_t size, ip4_header_t* packet) {
	switch(packet->proto) {
		case IP4_TOS_ICMP:
			handle_icmp(origin, size, packet, NULL);
			break;
		case IP4_TOS_UDP:
			udp_receive(origin, size, packet);
			break;
	}
}

// FIXME This does not remove the packet from the storage atm. Obviously this is bad.
static void reassemble_packet(struct fragment_entry* fragment, net_device_t* origin) {
	uint32_t full_length = 0;

	// Calculate complete packet length
	for(int i = 0; i < fragment->stored_packets; i++) {
		full_length += endian_swap16(fragment->packets[i]->len);

		// We will only include the header of the first packet
		if(PKG_FRAGMENT_OFFSET(fragment->packets[i]) != 0) {
			//full_length -= fragment->packets[i]->hl * 8; // FIXME
		}
	}

	ip4_header_t* full_packet = (ip4_header_t*)kmalloc(full_length);

	for(int i = 0; i < fragment->stored_packets; i++) {
		char* ip = ip4_split_ip((char*)kmalloc(sizeof(char) * 15), endian_swap32(fragment->packets[i]->src));

		ip4_header_t* packet = fragment->packets[i];
		uint8_t* packet_data = (void*)packet;

		uint16_t size = endian_swap16(packet->len);
		uint32_t offset = PKG_FRAGMENT_OFFSET(packet);

		// Unless this is the first packet, chop off the header.
		if(PKG_FRAGMENT_OFFSET(packet) != 0) {
			packet_data = (char*)packet_data + packet->hl * 4;
			size -= packet->hl * 4;
			offset += packet->hl * 4;
		}

		if(size > full_length || offset + size > full_length) {
			log(LOG_WARN, "ip4: Received invalidly fragmented packet.\n");
			kfree(full_packet);
			return;
		}

		// FIXME This can probably be exploited in approximately one million ways
		memcpy((void*)((intptr_t)full_packet + offset), packet_data, size);
	}

	// Reinject the assembled package
	ip4_sort_packet(origin, full_length, full_packet);
}

void ip4_receive(net_device_t* origin, net_l2proto_t proto, size_t size, void* raw)
{
	ip4_header_t* packet = NULL;
	ether_frame_hdr_t* etherhdr = NULL;

	// This should not be done here. Move to net.c!
	if (proto == NET_PROTO_ETH)
	{
		packet = net_ether_getPayload(raw);
		etherhdr = raw;
		size -= sizeof(ether_frame_hdr_t);
	}
	else if (proto == NET_PROTO_RAW)
	{
		packet = raw;
	}

	log(LOG_INFO, "pkg in  \n");

	// TODO Send an ICMP TTL exceeded packet here
	if(unlikely(packet->ttl <= 0))
		return;
	packet->ttl--;

	log(LOG_INFO, "pkg in after ttl\n");

	// Check if this is part of a fragmented packet
	struct fragment_entry* fragment_entry = locate_fragment(packet);
	if(PKG_MORE_FRAGMENTS(packet) || PKG_FRAGMENT_OFFSET(packet) > 0 || fragment_entry) {
		fragment_entry = store_fragment(packet, fragment_entry);
		if(!fragment_entry) {
			return; // FIXME Should have proper error handling
		}

		/* This was the last packet of the fragment group
		 * FIXME This does not account for potential packet order changes in
		 * transit and will break if the last packet arrives before any other
		 * packet. Should probably be fixed.
		 */
		if(!PKG_MORE_FRAGMENTS(packet)) {
			reassemble_packet(fragment_entry, origin);
		}

		return;
	}

	ip4_sort_packet(origin, size, packet);
}
