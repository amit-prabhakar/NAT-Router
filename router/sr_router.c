/**********************************************************************
 * file:  sr_router.c
 * date:  Mon Feb 18 12:50:42 PST 2002
 * Contact: casado@stanford.edu
 *
 * Description:
 *
 * This file contains all the functions that interact directly
 * with the routing table, as well as the main entry method
 * for routing.
 *
 **********************************************************************/
#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <string.h>
#include "sr_if.h"
#include "sr_rt.h"
#include "sr_router.h"
#include "sr_protocol.h"
#include "sr_arpcache.h"
#include "sr_utils.h"
#include "sr_nat.h"
/*Global*/
int sanity_check(sr_ip_hdr_t *ipheader);
struct sr_rt *longest_prefix_match(struct sr_instance *sr, uint32_t ipdest);
/*---------------------------------------------------------------------
 * Method: sr_init(void)
 * Scope:  Global
 *
 * Initialize the routing subsystem
 *
 *---------------------------------------------------------------------*/
void sr_init(struct sr_instance* sr)
{
    /* REQUIRES */
    assert(sr);
    
    /* Initialize cache and cache cleanup thread */
    sr_arpcache_init(&(sr->cache));
    
    pthread_attr_init(&(sr->arp_attr));
    pthread_attr_setdetachstate(&(sr->arp_attr), PTHREAD_CREATE_JOINABLE);
    pthread_attr_setscope(&(sr->arp_attr), PTHREAD_SCOPE_SYSTEM);
    pthread_attr_setscope(&(sr->arp_attr), PTHREAD_SCOPE_SYSTEM);
    pthread_t arp_thread;
    
    pthread_create(&arp_thread, &(sr->arp_attr), sr_arpcache_timeout, sr);
    
} /* -- sr_init -- */
/*---------------------------------------------------------------------
 * Method: sr_handlepacket(uint8_t* p,char* interface)
 * Scope:  Global
 *
 * This method is called each time the router receives a packet on the
 * interface.  The packet buffer, the packet length and the receiving
 * interface are passed in as parameters. The packet is complete with
 * ethernet headers.
 *
 * Note: Both the packet buffer and the character's memory are handled
 * by sr_vns_comm.c that means do NOT delete either.  Make a copy of the
 * packet instead if you intend to keep it around beyond the scope of
 * the method call.
 *
 *---------------------------------------------------------------------*/
void sr_handlepacket(struct sr_instance* sr,
                     uint8_t * packet/* lent */,
                     unsigned int len,
                     char* interface/* lent */)
{
    /* REQUIRES */
    assert(sr);
    assert(packet);
    assert(interface);
    
    printf("*** -> Received packet of length %d \n", len);
    
    
    /* Ethernet Header */
    sr_ethernet_hdr_t *eth_hdr = (sr_ethernet_hdr_t*) packet;
    char *ever_pointed = ever_pointer(sr, interface);
    
    uint16_t ethernet_type = ethertype((uint8_t*) eth_hdr);
    
    
    if (ethernet_type == ethertype_arp){
        
        fprintf(stdout,"ARP Packet Received\n");
        handle_arp(sr, packet, len, ever_pointed);
    }
    else if (ethernet_type == ethertype_ip){
        
        fprintf(stdout,"IP Packet Received\n");
        handle_ip(sr, packet, len, ever_pointed);
    }
    
    else{
        fprintf(stderr,"Unknown Packet Type Dropping Packet\n");
        return;
    }
}/* end sr_ForwardPacket */
/* Keeping methods out of the sr_handle
 Make another file for it?*/
/* Takes an ARP packet and deals with it */
void handle_arp(struct sr_instance* sr, uint8_t * incoming_packet, unsigned int incoming_len, char* incoming_interface)
{
    /*easy references to the incoming packet internals*/
    sr_ethernet_hdr_t *incoming_eth = (sr_ethernet_hdr_t*) incoming_packet;
    sr_arp_hdr_t *incoming_arp = (sr_arp_hdr_t*)(incoming_packet + sizeof(sr_ethernet_hdr_t));
    
    /*double check this arp packet is for the router if not, no point of continuing*/
    struct sr_if *interface_listing = sr->if_list;
    while (interface_listing != NULL)
    {
        if (interface_listing->ip == incoming_arp->ar_dest_ip)
        {
            break;
        }
        interface_listing = interface_listing->next;
    }
    
    if(interface_listing == NULL)
    {
        /*printf("arp packet for unknown gateway. can't do anything\n");*/
        return;
    }
    
    if(ntohs(incoming_arp->ar_op) == arp_op_request)
    {
        /*printf("got an incoming arp request\n");
         print_hdrs(incoming_packet, incoming_len);*/
        
        /*now that you have the mac for the ip, make a reply*/
        int reply_size = sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t);
        uint8_t *reply = malloc(reply_size);
        
        /*get info about the incoming interface for source info*/
        struct sr_if *incoming_info = sr_get_interface(sr, incoming_interface);
        
        /*Assemble ethernet header.*/
        sr_ethernet_hdr_t *reply_eheader = (sr_ethernet_hdr_t*) reply;
        memcpy(reply_eheader->ether_dhost, incoming_eth->ether_shost, 6);
        memcpy(reply_eheader->ether_shost, sr_get_interface(sr, incoming_interface)->mac, 6);
        reply_eheader->ether_type = incoming_eth->ether_type;
        
        /* Assemble arp header*/
        sr_arp_hdr_t *reply_arp = (sr_arp_hdr_t*)(reply + sizeof(sr_ethernet_hdr_t));
        reply_arp->ar_hardware_type = incoming_arp->ar_hardware_type;
        reply_arp->ar_protocol_type = incoming_arp->ar_protocol_type;
        reply_arp->ar_mac_addr_len = incoming_arp->ar_mac_addr_len;
        reply_arp->ar_ip_addr_len = incoming_arp->ar_ip_addr_len;
        reply_arp->ar_op = htons(arp_op_reply);
        memcpy(reply_arp->ar_src_mac, incoming_info->mac, 6);
        reply_arp->ar_src_ip = incoming_info->ip;
        memcpy(reply_arp->ar_dest_mac, incoming_arp->ar_src_mac, 6);
        reply_arp->ar_dest_ip = incoming_arp->ar_src_ip;
        
        /*Print what's in the reply before sending it*/
        /*printf("the arp request reply\n");
         print_hdrs(reply, reply_size);*/
        
        /*send it*/
        int result = sr_send_packet(sr, reply, reply_size, incoming_interface);
        if (result != 0)
        {
            fprintf(stderr, "ARP: error has occurred sending the packet.\n");
        }
        free(reply);
    }
    else if (ntohs(incoming_arp->ar_op) == arp_op_reply)
    {
        /*printf("got an incoming arp reply\n");
         print_hdrs(incoming_packet, incoming_len);*/
        
        /*BLINDLY add it to the arp cache*/
        struct sr_arpreq *arp_reply_backlog;
        arp_reply_backlog = sr_arpcache_insert(&(sr->cache), incoming_arp->ar_src_mac, incoming_arp->ar_src_ip);
        
        /*straight up copy and paste from ar_op == arp_op_request*/
        /*only process the backlog if there is one. otherwise backlog->packet will give a memory read exception*/
        if(arp_reply_backlog != NULL)
        {
            /*process the backlog of packets*/
            struct sr_packet *backlog_packet = arp_reply_backlog->packets;
            while (backlog_packet != NULL)
            {
                sr_ethernet_hdr_t *backlog_eheader = (sr_ethernet_hdr_t*)(backlog_packet->buf);
                memcpy(backlog_eheader->ether_dhost, incoming_arp->ar_src_mac, 6);
                memcpy(backlog_eheader->ether_shost, sr_get_interface(sr, backlog_packet->iface)->mac, 6);
                
                /*printf("sending out backlog packet: \n");
                 print_hdrs(backlog_packet->buf, backlog_packet->len);*/
                sr_send_packet(sr, backlog_packet->buf, backlog_packet->len, backlog_packet->iface);
                
                backlog_packet = backlog_packet->next;
            }
            /*backlog has been completed, get rid of this request*/
            /*printf("backlog pointer %p fulfilled\n", arp_reply_backlog);*/
            sr_arpreq_destroy(&(sr->cache), arp_reply_backlog);
        }
        else
        {
            /*printf("no backlog to process\n");*/
        }
    }
}
/* Takes an IP packet and deals with it */
void handle_ip(struct sr_instance* sr, uint8_t * packet, unsigned int len, char* interface) {
    
    /* REQUIRES */
    assert(sr);
    assert(packet);
    assert(interface);
    
    /* Get ethernet header */
    sr_ethernet_hdr_t *ethernet_header = (sr_ethernet_hdr_t *) packet;
    
    /* Get IP header */
    sr_ip_hdr_t *ip_header = (sr_ip_hdr_t *) (packet + sizeof(sr_ethernet_hdr_t));
    
    
    
    /* Get this packet out early if the length is too short*/
    if (len < sizeof(sr_ip_hdr_t)) {
        fprintf(stderr, "length too short, dropping packet\n");
        return;
    }
    
    printf("got an ip packet\n");
    
    int check_packet = sanity_check(ip_header);
    if (check_packet == 1) {
        fprintf(stderr, "Packet dropped\n");
        return;
    }
    
    /* Try to short circuit processing if this packet is intended for router */
    struct sr_if *node = 0;
    node = sr->if_list;
    while(node){
        if(node->ip == ip_header->ip_dst){
            break;
        }
        node = node->next;
    }
    
    if (ip_header->ip_ttl > 1) {
        fprintf(stdout, "TTL Decremented\n");
        ip_header->ip_ttl = ip_header->ip_ttl - 1;
        ip_header->ip_sum = 0;
        ip_header->ip_sum = cksum(ip_header, ip_header->ip_hl * 4);
        
        
        struct sr_arpentry *arpl = sr_arpcache_lookup(&sr->cache, ip_header->ip_src);
        if (!arpl) {
            sr_arpcache_insert(&sr->cache, ethernet_header->ether_shost, ip_header->ip_src);
            free(arpl);
        }
        
    }
    else if (ip_header->ip_ttl == 1 && node) {
        ;
    }
    else {
        fprintf(stdout, "TTL EXCEEDED!!!!!!!!!!!!!!!!\n");
        send_icmp(sr, interface, packet, ip_header,len, ICMP_TIME_EXCEEDED, ICMP_ECHO_REPLY, false);
        return;
    }
    
    /************************* NAT CODE START *************************/
    
    if (sr->nat_mode) {
        uint8_t ip_type = ip_protocol(packet + sizeof(sr_ethernet_hdr_t));
        fprintf(stdout, "NAT MODE ENABLED.\n");
        
        /*Internal interfaces*/
        if (strncmp(interface, "eth1", 5) == 0){
            /* Packet for this router */
            if (node){
                fprintf(stdout,"Received at internal interface for this router\n");
                if (ip_type == ip_protocol_icmp){
                    fprintf(stdout,"Got an ICMP\n");
                    sr_icmp_hdr_t *icmp_header = (sr_icmp_hdr_t*)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
                    print_hdr_icmp((uint8_t*)icmp_header);
                    if (icmp_header->icmp_type == ICMP_ECHO_REQ){
                        send_icmp(sr, interface, packet, ip_header, len, 0, 0, true);
                        return;
                    }
                }
                else if(ip_type == ip_protocol_tcp){
                    fprintf(stdout,"GOT A TCP\n");
                    send_icmp(sr, interface, packet, ip_header, len, 3, 3, false);
                    return;
                }
                else{
                    fprintf(stdout,"Recieved an unknown ICMP. Dropping packet\n");
                    return;
                }
            }
            
            fprintf(stdout,"Packet not for this router -- NAT ENABLED\n");
            if (ip_type == ip_protocol_icmp){ /*icmp from lan --> wan using nat*/
                
                /*get icmp header*/
                sr_icmp_tping_hdr_t *icmp_header = (sr_icmp_tping_hdr_t*)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
                
                /*get external interface*/
                struct sr_if *external_interface = sr_get_interface(sr, "eth2");
                
                /*get mapping*/
                struct sr_nat_mapping *mapping = sr_nat_lookup_internal(&(sr->the_nat), ip_header->ip_src, icmp_header->identifier, nat_mapping_icmp);
                
                /*if mapping doesn't exist insert it*/
                if (!mapping){
                    mapping = sr_nat_insert_mapping(&(sr->the_nat), ip_header->ip_src, icmp_header->identifier, nat_mapping_icmp, NULL);
                    mapping->ip_ext = external_interface->ip;
                }
                
                struct sr_rt *prefix_match = longest_prefix_match(sr, ip_header->ip_dst);
                
                if (prefix_match){
                    ip_header->ip_src = external_interface->ip;
                }
                
                icmp_header->identifier = mapping->aux_ext;
                
                /*update cksum*/
                ip_header->ip_sum = 0;
                icmp_header->icmp_sum = 0;
                ip_header->ip_sum = cksum(ip_header, sizeof(sr_ip_hdr_t));
                icmp_header->icmp_sum = cksum(icmp_header, len-sizeof(sr_ethernet_hdr_t)-sizeof(sr_ip_hdr_t));
            }
            
            else if(ip_type == ip_protocol_tcp){
                /*get TCP Header*/
                fprintf(stdout,"Got a TCP Packet on eth1\n");
                sr_tcp_hdr_t *tcp_header = (sr_tcp_hdr_t *) (packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_tcp_hdr_t));
                
                struct sr_nat_mapping *mapping = sr_nat_lookup_internal(&(sr->the_nat), ip_header->ip_src, tcp_header->src_port, nat_mapping_tcp_old);
                struct sr_if *external_interface = sr_get_interface(sr, "eth2");
                if (!mapping) {
                    fprintf(stdout,"no nat_mapping_tcp_old for TCP Packet check for nat_mapping_tcp_new_s2\n");
                    mapping = sr_nat_lookup_internal(&(sr->the_nat), ip_header->ip_src, tcp_header->src_port, nat_mapping_tcp_new_s2);
                    
                    if (!mapping){
                        fprintf(stdout,"no nat_mapping_tcp_new_s2 for TCP Packet inserting  nat_mapping_tcp_new_s1\n");
                        mapping = sr_nat_insert_mapping(&(sr->the_nat), ip_header->ip_src, tcp_header->src_port, nat_mapping_tcp_new_s1, NULL);
                        mapping->ip_ext = external_interface->ip;
                    }
                    else{
                        fprintf(stdout,"nat_mapping_tcp_new_s2 exists for TCP Packet changing to nat_mapping_tcp_old\n");
                        mapping->type = nat_mapping_tcp_old;
                    }
                }
                fprintf(stdout,"TCP packet has an old mapping\n");
                if (longest_prefix_match(sr, ip_header->ip_dst)){
                    ip_header->ip_src = external_interface->ip;
                }
                
                tcp_header->src_port = mapping->aux_ext;
                
                
                ip_header->ip_sum = 0;
                ip_header->ip_sum = cksum(ip_header, sizeof(sr_ip_hdr_t));
                
                tcp_header->checksum = tcp_cksum(ip_header, tcp_header, len);
            }
            fprintf(stdout,"TCP packet ----- Starting send\n");
            /*in both cases, we alter the original packet so now its time to send it off*/
            struct sr_rt *match = longest_prefix_match(sr, ip_header->ip_dst);
            if (match){
                printf("Found the match in the routing table\n");
                struct sr_arpentry *entry = sr_arpcache_lookup(&sr->cache, match->gw.s_addr);
                if (entry){
                    printf("Found the ARP in the cache\n");
                    
                    struct sr_if *sr_interface_instance = sr_get_interface(sr, match->interface);
                    
                    /* Make ethernet header */
                    sr_ethernet_hdr_t *reply_ethernet_header = (sr_ethernet_hdr_t *)packet;
                    memcpy(reply_ethernet_header->ether_dhost, entry->mac, sizeof(unsigned char)*6);
                    memcpy(reply_ethernet_header->ether_shost, sr_interface_instance->mac, sizeof(uint8_t)*ETHER_ADDR_LEN);
                    reply_ethernet_header->ether_type = ethernet_header->ether_type;
                    
                    printf("Sending lan --> wan out interface: %s\n", sr_interface_instance->name);
                    sr_send_packet(sr, packet, len, sr_interface_instance->name);
                    free(entry);
                    
                } else {
                    printf("ARP Cache miss\n");
                    struct sr_arpreq *request = sr_arpcache_queuereq(&(sr->cache), ip_header->ip_dst, packet, len, match->interface);
                    handle_qreq(sr, request);
                    return;
                }
            }
            else {
                fprintf(stdout,"No match. Sending ICMP net unreachable...\n");
                send_icmp(sr, interface, packet, ip_header,len, ICMP_UNREACHABLE, ICMP_ECHO_REPLY, false);
                return;
            }
            
        }
        /* Received packet from outside */
        else if (strncmp(interface, "eth2", 5) == 0) {
            fprintf(stdout,"External host reply/connection attempt\n");
            
            /*print_hdrs(packet, len);*/
            
            
            
            if (node){
                
                switch (ip_type) {
                        
                    case ip_protocol_tcp: {
                        sr_tcp_hdr_t *tcp_header = (sr_tcp_hdr_t *) (packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_tcp_hdr_t));
                        
                        struct sr_nat_mapping *mapping = sr_nat_lookup_external(&(sr->the_nat), tcp_header->dst_port, nat_mapping_tcp_old);
                        if (mapping == NULL) {
                            mapping = sr_nat_lookup_external(&(sr->the_nat), tcp_header->dst_port, nat_mapping_tcp_new_s1);
                            if (mapping){
                                mapping->type = nat_mapping_tcp_new_s2;
                                fprintf(stdout,"got a TCP packet on eth2 with a mapping for s1 changing to s2\n");
                            }
                            else{
                                printf("Mapping is NULL\n");
                                /*A: unsol syn */
                                if (tcp_header->syn){
                                    fprintf(stdout,"***** Got an Unsol syn in eth2 *****\n");
                                    if (ntohs(tcp_header->dst_port) == 22 ){
                                        send_icmp(sr, interface, packet, ip_header, len, 3, 3, false);
                                        return;
                                    }
                                    /*get mapping HAX*/
                                    mapping = sr_nat_lookup_internal(&(sr->the_nat), ip_header->ip_src, tcp_header->src_port, nat_mapping_tcp_unsolicited);
                                    if (mapping){
                                        fprintf(stdout,"***** dup unsol syn *****\n");
                                        return;
                                    }
                                    else{
                                        /*NAT DOESNT KNOW OF UNSOL SYN, insert and drop*/
                                        fprintf(stdout,"***** new unsol syn *****\n");
                                        sr_nat_insert_mapping(&(sr->the_nat), ip_header->ip_src, tcp_header->src_port, nat_mapping_tcp_unsolicited, packet);
                                        return;
                                    }
                                    
                                }
                            }
                            if (mapping == NULL){
                                return;
                            }
                        }
                        fprintf(stdout,"got a TCP packet on eth2 with a mapping\n");
                        mapping->last_updated = time(NULL);
                        
                        ip_header->ip_dst = mapping->ip_int;
                        tcp_header->dst_port = mapping->aux_int;
                        
                        ip_header->ip_sum = 0;
                        ip_header->ip_sum = cksum(ip_header, sizeof(sr_ip_hdr_t));
                        tcp_header->checksum = tcp_cksum(ip_header, tcp_header, len);
                        
                        fprintf(stdout,"TCP packet ----- Starting send\n");
                        struct sr_rt *prefix_match = longest_prefix_match(sr, ip_header->ip_dst);
                        if (prefix_match){
                            printf("Found the match in routing table\n");
                            struct sr_arpentry *entry = sr_arpcache_lookup(&sr->cache, prefix_match->gw.s_addr);
                            if (entry){
                                printf("Found the ARP in the cache\n");
                                
                                struct sr_if *router_if = sr_get_interface(sr, prefix_match->interface);
                                
                                /* Make ethernet header */
                                sr_ethernet_hdr_t *reply_ethernet_header = (sr_ethernet_hdr_t *)packet;
                                memcpy(reply_ethernet_header->ether_dhost, entry->mac, sizeof(unsigned char)*6);
                                memcpy(reply_ethernet_header->ether_shost, router_if->mac, sizeof(uint8_t)*ETHER_ADDR_LEN);
                                reply_ethernet_header->ether_type = ethernet_header->ether_type;
                                
                                /* print_hdrs(packet, len); */
                                sr_send_packet(sr, packet, len, router_if->name);
                                free(entry);
                                
                            } else {
                                printf("ARP Cache miss\n");
                                struct sr_arpreq *req = sr_arpcache_queuereq(&(sr->cache), ip_header->ip_dst, packet, len, prefix_match->interface);
                                handle_qreq(sr, req);
                                
                            }
                        }
                        break;
                    }
                        
                    case ip_protocol_icmp: {
                        fprintf(stdout, "Got an ICMP on ext int\n");
                        sr_icmp_tping_hdr_t *icmp_header = (sr_icmp_tping_hdr_t*)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
                        struct sr_nat_mapping *extmapping = sr_nat_lookup_external(&(sr->the_nat), icmp_header->identifier, nat_mapping_icmp);
                        /* Unsolicited connection, drop packet */
                        if (!extmapping) {
                            fprintf(stdout, "No external mappings for this ICMP packet, dropping...\n");
                            /* Is this the start of a memory leak? Do I have to free packet? */
                            /*free(packet);*/
                            return;
                        }
                        /* Modify ICMP header */
                        icmp_header->identifier = extmapping->aux_int; /* Comes in network byte order already */
                        icmp_header->icmp_sum = 0;
                        icmp_header->icmp_sum = cksum(icmp_header, len - sizeof(sr_ethernet_hdr_t) - sizeof(sr_ip_hdr_t));
                        
                        /* Modify IP header */
                        ip_header->ip_dst = extmapping->ip_int; /* Comes in network byte order already */
                        /*ip_header->ip_src = sr_get_interface(sr, "eth1")->ip;  Magic: is in network byte order */
                        ip_header->ip_sum = 0;
                        ip_header->ip_sum = cksum(ip_header, sizeof(sr_ip_hdr_t));
                        
                        /* Modify ethernet header */
                        memcpy(ethernet_header->ether_shost, sr_get_interface(sr, "eth1")->mac, 6);
                        print_addr_ip_int(extmapping->ip_int);
                        struct sr_arpentry *entry = sr_arpcache_lookup(&sr->cache, extmapping->ip_int);
                        /* make arp request here if no entry found */
                        if (entry) {
                            memcpy(ethernet_header->ether_dhost, entry->mac, 6);
                            /*print_hdrs(packet, len);*/
                            sr_send_packet(sr, packet, len, "eth1");
                            return;
                        }
                        /*might need to jump to arp cache being lazy for now*/
                        break;
                    }
                        
                    default:
                        fprintf(stdout, "Unsupported protocol (not ICMP or TCP)");
                        break;
                }
            }
            else
            {
                
            }
            /*print_hdrs(packet, len);*/
            
            return;
        }
        
        return; /*when in nat mode, stay in nat mode code*/
    }
    
    /************************** NAT CODE END **************************/
    
    /* Packet destined for this router */
    if (node != NULL) {
        uint8_t ip_type = ip_protocol(packet + sizeof(sr_ethernet_hdr_t));
        
        /* Get ICMP header */
        sr_icmp_hdr_t* icmp_header = (sr_icmp_hdr_t*)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
        switch (ip_type) {
                
            case ip_protocol_tcp:
            case ip_protocol_udp:
                
                fprintf(stdout,"Recieved TCP or UDP Packet. Sending 'ICMP: Port Unreachable' to Source. \n");
                /*send_icmp(sr, interface, packet, ip_header, len, ICMP_UNREACHABLE, ICMP_UNREACHABLE);*/
                
                int size = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
                uint8_t *response_packet = malloc(size);
                
                sr_ethernet_hdr_t *ethernet_hdr = malloc(sizeof(sr_ethernet_hdr_t));
                memcpy(ethernet_hdr, (sr_ethernet_hdr_t *) packet, sizeof(sr_ethernet_hdr_t));
                
                /* Make ethernet header */
                sr_ethernet_hdr_t *response_ethernet_header = (sr_ethernet_hdr_t *)response_packet;
                memcpy(response_ethernet_header->ether_dhost, ethernet_hdr->ether_shost, sizeof(sr_ethernet_hdr_t));
                memcpy(response_ethernet_header->ether_shost, sr_get_interface(sr, interface)->mac, sizeof(sr_ethernet_hdr_t));
                response_ethernet_header->ether_type = htons(ethertype_ip);
                
                
                /* Make IP header */
                sr_ip_hdr_t *response_ip_header = (sr_ip_hdr_t *)(response_packet + sizeof(sr_ethernet_hdr_t));
                response_ip_header->ip_v = 4;
                response_ip_header->ip_hl = sizeof(sr_ip_hdr_t)/4;
                response_ip_header->ip_tos = 0;
                response_ip_header->ip_len = htons(sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t));
                response_ip_header->ip_id = htons(0);
                response_ip_header->ip_off = htons(IP_DF);
                response_ip_header->ip_ttl = 64;
                response_ip_header->ip_dst = ip_header->ip_src;
                response_ip_header->ip_p = ip_protocol_icmp;
                response_ip_header->ip_src = node->ip;
                response_ip_header->ip_sum = 0;
                response_ip_header->ip_sum = cksum(response_ip_header, sizeof(sr_ip_hdr_t));
                
                /* Make ICMP Header */
                sr_icmp_t3_hdr_t *response_icmp_header = (sr_icmp_t3_hdr_t *)(response_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
                response_icmp_header->icmp_type = ICMP_UNREACHABLE;
                response_icmp_header->icmp_code = 3;
                response_icmp_header->unused = 0;
                response_icmp_header->next_mtu = 0;
                memcpy(response_icmp_header->data, ip_header, ICMP_DATA_SIZE);
                response_icmp_header->icmp_sum = 0;
                response_icmp_header->icmp_sum = cksum(response_icmp_header, sizeof(sr_icmp_t3_hdr_t));
                sr_send_packet(sr, response_packet, len, interface);
                break;
                
            case ip_protocol_icmp:
                
                ethernet_header = (sr_ethernet_hdr_t *)packet;
                if (icmp_header->icmp_type == ICMP_ECHO_REQ) {
                    send_icmp(sr, interface, packet, ip_header, len, 0, 0, true);
                }
                break;
                
            default:
                fprintf(stdout,"Recieved unknown ICMP message type.\n");
                break;
        }
        return;
    }
    else {
        fprintf(stdout,"The packet wasn't directed to this router\n");
        struct sr_rt *prefix_match = longest_prefix_match(sr, ip_header->ip_dst);
        
        if (prefix_match){
            /* Prepare packet for sending */
            fprintf(stdout,"longest prefix match found\n");
            struct sr_arpentry *entry = sr_arpcache_lookup(&sr->cache, prefix_match->gw.s_addr);
            struct sr_if *interface = sr_get_interface(sr, prefix_match->interface);
            print_addr_ip_int(prefix_match->gw.s_addr);
            print_addr_eth(interface->mac);
            print_addr_ip_int(interface->ip);
            
            if (entry){
                struct sr_if *interface = sr_get_interface(sr, prefix_match->interface);
                
                /* Make ethernet header */
                sr_ethernet_hdr_t *reply_ethernet_header = (sr_ethernet_hdr_t *)packet;
                memcpy(reply_ethernet_header->ether_dhost, entry->mac, sizeof(unsigned char)*6);
                memcpy(reply_ethernet_header->ether_shost, interface->mac, sizeof(uint8_t)*ETHER_ADDR_LEN);
                reply_ethernet_header->ether_type = ethernet_header->ether_type;
                
                /* print_hdrs(packet, len);*/
                sr_send_packet(sr, packet, len, interface->name);
                free(entry);
            } else {
                fprintf(stdout,"ARP Cache miss\n");
                struct sr_arpreq *request = sr_arpcache_queuereq(&(sr->cache), ip_header->ip_dst, packet, len, prefix_match->interface);
                handle_qreq(sr, request);
            }
        } else {
            fprintf(stdout,"No match. Sending ICMP net unreachable...\n");
            
            send_icmp(sr, interface, packet, ip_header,len, ICMP_UNREACHABLE, ICMP_ECHO_REPLY, false);
        }
        
    }
    return;
}
/* Finds excuses to get rid of an IP packet */
int sanity_check(sr_ip_hdr_t *ip_header) {
    
    uint16_t received_checksum = ip_header->ip_sum;
    ip_header->ip_sum = 0;
    uint16_t computed_checksum = cksum(ip_header, ip_header->ip_hl * 4);
    ip_header->ip_sum = received_checksum;
    
    printf("Original CS: %d\n", received_checksum);
    printf("Computed CS: %d\n", computed_checksum);
    
    
    if (received_checksum != computed_checksum) {
        fprintf(stderr, "checksum does not match, dropping packet\n");
        return 1;
    }
    
    fprintf(stdout, "Sanity Checks Passed\n");
    return 0;
}
/* longest prefix match lookup...please fuckin work */
struct sr_rt *longest_prefix_match(struct sr_instance *sr, uint32_t ipdest)
{
    /* -- REQUIRES -- */
    assert(sr);
    
    if(sr->routing_table == 0){
        printf("No entries in routing table. Dropping packet\n");
        return NULL;
    }
    
    int len = 0;
    struct sr_rt *entry = 0;
    struct sr_rt *lprefix = 0;
    
    entry = sr->routing_table;
    while(entry != NULL){
        if ((ipdest & entry->mask.s_addr) == (entry->dest.s_addr & entry->mask.s_addr)){
            if((entry->mask.s_addr & ipdest) > len){
                lprefix = entry;
                len = (entry->mask.s_addr & ipdest);
            }
        }
        entry = entry->next;
    }
    return lprefix;
}
/* sends ICMP message for net unreachable */
void send_icmp(struct sr_instance* sr, char* interface, uint8_t * packet, sr_ip_hdr_t *ip_header,unsigned int len, int type, int code, bool hax) {
    /* REQUIRES */
    assert(sr);
    assert(interface);
    assert(packet);
    
    /* Keeping this for reference */
    sr_icmp_tping_hdr_t *icmp_header = (sr_icmp_tping_hdr_t*)(packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
    sr_ethernet_hdr_t *ethernet_header = (sr_ethernet_hdr_t *)packet;
    
    if (hax) {
        
        memcpy(ethernet_header->ether_dhost, ethernet_header->ether_shost, sizeof(uint8_t)*ETHER_ADDR_LEN);
        memcpy(ethernet_header->ether_shost, sr_get_interface(sr, interface)->mac, sizeof(uint8_t)*ETHER_ADDR_LEN);
        
        /* update ICMP Header from current packet */
        icmp_header->icmp_type = type;
        icmp_header->icmp_code = code;
        icmp_header->icmp_sum = 0;
        icmp_header->icmp_sum = cksum(icmp_header, len - sizeof(sr_ethernet_hdr_t) - sizeof(sr_ip_hdr_t));
        
        /* update IP header from current packet*/
        ip_header->ip_sum = 0;
        ip_header->ip_sum = cksum(ip_header, sizeof(sr_ip_hdr_t));
        uint32_t new_dest = ip_header->ip_src;
        ip_header->ip_src = ip_header->ip_dst;
        ip_header->ip_dst = new_dest;
        /*DT: will cause duplicate responses because when the response comes back, it will send the reply again*/
        sr_send_packet(sr, packet, len, interface);
        return;
    }
    
    
    
    int size = 0;
    if (type == ICMP_UNREACHABLE) {
        size = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
    }
    
    else if ( type == ICMP_TIME_EXCEEDED) {
        size = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t11_hdr_t);
    }
    else {
        size = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_hdr_t);
    }
    
    uint8_t *response_packet = malloc(size);
    
    /* build ethernet header */
    sr_ethernet_hdr_t *response_ethernet_header = (sr_ethernet_hdr_t *)response_packet;
    memcpy(response_ethernet_header->ether_dhost, ethernet_header->ether_shost, 6);
    memcpy(response_ethernet_header->ether_shost, ethernet_header->ether_dhost, 6);
    response_ethernet_header->ether_type = htons(ethertype_ip);
    
    
    /* build IP header */
    sr_ip_hdr_t *response_ip_header = (sr_ip_hdr_t *)(response_packet + sizeof(sr_ethernet_hdr_t));
    response_ip_header->ip_v = 4;
    response_ip_header->ip_hl = sizeof(sr_ip_hdr_t)/4;
    response_ip_header->ip_tos = 0;
    response_ip_header->ip_len = htons(sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t11_hdr_t));
    response_ip_header->ip_id = htons(0);
    response_ip_header->ip_off = htons(IP_DF);
    response_ip_header->ip_ttl = 64;
    response_ip_header->ip_dst = ip_header->ip_src;
    response_ip_header->ip_p = ip_protocol_icmp;
    response_ip_header->ip_src = sr_get_interface(sr, interface)->ip;
    response_ip_header->ip_sum = 0;
    response_ip_header->ip_sum = cksum(response_ip_header, sizeof(sr_ip_hdr_t));
    
    /* build ICMP Header */
    
    if (type == ICMP_UNREACHABLE) {
        sr_icmp_t3_hdr_t *response_icmp_header = (sr_icmp_t3_hdr_t *)(response_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
        response_icmp_header->icmp_type = ICMP_UNREACHABLE;
        response_icmp_header->icmp_code = code;
        response_icmp_header->unused = 0;
        response_icmp_header->next_mtu = 0;
        response_icmp_header->icmp_sum = 0;
        memcpy(response_icmp_header->data, ip_header, ICMP_DATA_SIZE);
        response_icmp_header->icmp_sum = cksum(response_icmp_header, sizeof(sr_icmp_t3_hdr_t));
    }
    else if (type == ICMP_TIME_EXCEEDED) {
        sr_icmp_t11_hdr_t *response_icmp_header = (sr_icmp_t11_hdr_t *)(response_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
        response_icmp_header->icmp_type = ICMP_TIME_EXCEEDED;
        response_icmp_header->icmp_code = code;
        response_icmp_header->unused = 0;
        response_icmp_header->icmp_sum = 0;
        memcpy(response_icmp_header->data, ip_header, ICMP_DATA_SIZE);
        response_icmp_header->icmp_sum = (cksum(response_icmp_header, sizeof(sr_icmp_t11_hdr_t)));
    }
    /* Build type8/0 ICMP here */
    else
    {
        sr_icmp_tping_hdr_t *response_icmp_header = (sr_icmp_tping_hdr_t *)(response_packet + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
        response_icmp_header->icmp_type = type;
        response_icmp_header->icmp_code = code;
        response_icmp_header->icmp_sum = 0;
        memcpy(response_icmp_header->data, ip_header, ICMP_DATA_SIZE);
        response_icmp_header->icmp_sum = cksum(response_icmp_header, sizeof(sr_icmp_hdr_t));
    }
    
    if (type == ICMP_TIME_EXCEEDED){
        struct sr_arpentry *entry = sr_arpcache_lookup(&sr->cache, response_ip_header->ip_dst);
        if (entry) {
            memcpy(response_ethernet_header->ether_dhost, entry->mac, 6);
            memcpy(response_ethernet_header->ether_shost, sr_get_interface(sr, interface)->mac, 6);
            printf("time exceeded notice\n");
            print_hdrs(response_packet, size);
            sr_send_packet (sr, response_packet, size, interface);
            free(response_packet);
        } else {
            struct sr_arpreq *request = sr_arpcache_queuereq(&sr->cache, response_ip_header->ip_dst, response_packet, size, interface);
            handle_qreq(sr, request);
        }
        
        
    }
    
    else{
        sr_send_packet (sr, response_packet, size, interface);
        free(response_packet);
    }
    
}
/*generates an arp request for the packet and asks handle_arp to take care of it*/
void handle_qreq(struct sr_instance *sr, struct sr_arpreq *request)
{
    /*printf("handling arp request pointer %p\n", request);*/
    time_t now = time(NULL); /*DT* in seconds since new years 1970*/
    time_t diff = now - request->sent; /*OH* supposed to be "now -" not "now =" right? */
    
    if (request->times_sent >= 5)
    {
        int doable = 0;
        /*printf("request has exceeded 5x limit\n");*/
        struct sr_packet *failed_packet = request->packets;
        while (failed_packet != NULL)
        {
            /*printf("mailing out failure for packet: \n");
             print_hdrs(failed_packet->buf, failed_packet->len);*/
            
            /*get the ip of the failed packet based on whether it's ip or arp*/
            sr_ip_hdr_t *orig_ipheader = (sr_ip_hdr_t*) (failed_packet->buf + sizeof(sr_ethernet_hdr_t));
            
            /*DT*create icmp fail and make its internals easily accessible*/
            int fail_length = sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t) + sizeof(sr_icmp_t3_hdr_t);
            uint8_t *fail = malloc(fail_length);
            sr_ethernet_hdr_t *fail_eheader = (sr_ethernet_hdr_t*) fail;
            sr_ip_hdr_t *fail_ipheader = (sr_ip_hdr_t*) (fail + sizeof(sr_ethernet_hdr_t));
            sr_icmp_t3_hdr_t *fail_icmp = (sr_icmp_t3_hdr_t*) (fail + sizeof(sr_ethernet_hdr_t) + sizeof(sr_ip_hdr_t));
            
            /*DT*set ethernet header type. source/dest won't be known until going to the cache*/
            fail_eheader->ether_type = htons(ethertype_ip);
            
            /*DT*fill in ip header*/
            fail_ipheader->ip_v = 4;
            fail_ipheader->ip_hl = sizeof(sr_ip_hdr_t) / 4;
            fail_ipheader->ip_tos = 0;
            fail_ipheader->ip_len = htons(fail_length - sizeof(sr_ethernet_hdr_t)); /*DT*everything but the ethernet header*/
            fail_ipheader->ip_id = 0;
            fail_ipheader->ip_off = htons(IP_DF);
            fail_ipheader->ip_ttl = 64;
            fail_ipheader->ip_p = ip_protocol_icmp;
            fail_ipheader->ip_src = sr_get_interface(sr, failed_packet->iface)->ip;
            fail_ipheader->ip_dst = orig_ipheader->ip_src;
            fail_ipheader->ip_sum = 0; /*zero out before calculating*/
            uint16_t ip_checksum = cksum(fail_ipheader, sizeof(sr_ip_hdr_t));
            fail_ipheader->ip_sum = ip_checksum;
            
            /*DT* fill in the icmp stuff*/
            /*DT* wikipedia: https://en.wikipedia.org/wiki/Internet_Control_Message_Protocol */
            fail_icmp->icmp_type = 3;
            fail_icmp->icmp_code = 1;
            fail_icmp->unused = 0; /*DT* zero out to make sure old heap garbage doesn't screw this up */
            fail_icmp->next_mtu = 0; /*DT* according to wikipedia, you only fill this in for code 4*/
            memcpy(fail_icmp->data, orig_ipheader, ICMP_DATA_SIZE);
            fail_icmp->icmp_sum = 0;
            uint16_t icmp_checksum = cksum(fail_icmp, sizeof(sr_icmp_t3_hdr_t));
            fail_icmp->icmp_sum = icmp_checksum;
            
            /*find out where the client is connected to*/
            struct sr_rt *best_match = longest_prefix_match(sr, orig_ipheader->ip_src);
            if(best_match != NULL)
            {
                struct sr_arpentry *sender = sr_arpcache_lookup(&(sr->cache), orig_ipheader->ip_src);
                if(sender != NULL)
                {
                    /*printf("sender info found\n");*/
                    doable = 1;
                    
                    /*set the ethernet header*/
                    memcpy(fail_eheader->ether_dhost, sender->mac, 6);
                    memcpy(fail_eheader->ether_shost, sr_get_interface(sr, best_match->interface)->mac, 6);
                    
                    /*printf("failure to be mailed out:\n");
                     print_hdrs(fail, fail_length);*/
                    sr_send_packet(sr, fail, fail_length, best_match->interface);
                    free(fail);
                    
                    failed_packet = failed_packet->next;
                }
                else
                {
                    /*printf("no cache entry on the sender. wait for cache\n");*/
                    struct sr_arpreq *request = sr_arpcache_queuereq(&(sr->cache), orig_ipheader->ip_src, fail, fail_length, best_match->interface);
                    handle_qreq(sr, request);
                    break;
                }
            }
            else
            {
                /*printf("no gateway match to mail out failure\n");*/
            }
            
        }
        /*only remove the request if sender info was available to mail failures*/
        if(doable)
        {
            /*printf("request pointer %p was doable\n", request);*/
            sr_arpreq_destroy(&(sr->cache), request); /*DT* this request is hopeless. failures have been sent. get rid of it*/
        }
        else
        {
            /*printf("request pointer %p was NOT doable\n", request);*/
        }
    }
    else if (diff >= 1)
    {
        /*update the counters*/
        request->sent = now;
        request->times_sent++;
        
        struct sr_packet *first_packet = request->packets;
        
        /*printf("sending out arp request for the first packet in the request list\n");
         printf("first packet contents:\n");
         print_hdrs(first_packet->buf, first_packet->len);*/
        
        int arp_request_size = sizeof(sr_ethernet_hdr_t) + sizeof(sr_arp_hdr_t);
        uint8_t *arp_request = malloc(arp_request_size);
        sr_ethernet_hdr_t *request_eheader = (sr_ethernet_hdr_t*) arp_request;
        sr_arp_hdr_t *request_aheader = (sr_arp_hdr_t*) (arp_request + sizeof(sr_ethernet_hdr_t));
        
        /*assemble request header*/
        memcpy(request_eheader->ether_shost, sr_get_interface(sr, first_packet->iface)->mac, 6);
        uint8_t mac_broadcast[6] = {0xff, 0xff, 0xff, 0xff, 0xff, 0xff};
        memcpy(request_eheader->ether_dhost, mac_broadcast, 6); /*make sure it is sent to the broadcast mac*/
        request_eheader->ether_type = htons(ethertype_arp);
        
        /*make the arp request*/
        request_aheader->ar_hardware_type = htons(arp_hdr_ethernet);
        request_aheader->ar_protocol_type = htons(arp_hdr_ip);
        request_aheader->ar_mac_addr_len = 6;
        request_aheader->ar_ip_addr_len = 4;
        request_aheader->ar_op = htons(arp_op_request);
        memcpy(request_aheader->ar_src_mac, sr_get_interface(sr, first_packet->iface)->mac, 6);
        request_aheader->ar_src_ip = sr_get_interface(sr, first_packet->iface)->ip;
        memcpy(request_aheader->ar_dest_mac, mac_broadcast, 6);
        request_aheader->ar_dest_ip = request->ip;
        
        /*printf("helper Qreq arp request headers\n");
         print_hdrs(arp_request, arp_request_size);*/
        
        /*send the arp request for the first packet from the interface it came from*/
        sr_send_packet(sr, arp_request, arp_request_size, first_packet->iface);
        free(arp_request);
        
    }
    else
    {
        /*printf("difference is less than 1 (%ld). it's too soon to try again\n", diff);*/
    }
}
