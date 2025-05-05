#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdbool.h>
#include "simulator.h"

#define WINDOWSIZE 6
#define SEQSPACE 11
#define TIMEOUT 20.0
#define PKT_DATA_SIZE 20

struct pkt sender_buffer[SEQSPACE];
bool sender_buffer_ack[SEQSPACE];
int sender_window_base = 0;
int sender_nextseqnum = 0;

struct pkt receiver_buffer[SEQSPACE];
bool receiver_packet_received[SEQSPACE];
int receiver_window_base = 0;

// Calculate checksum
int calculate_checksum(struct pkt packet) {
    int checksum = packet.seqnum + packet.acknum;
    for (int i = 0; i < PKT_DATA_SIZE; i++) {
        checksum += packet.payload[i];
    }
    return checksum;
}

bool is_corrupt(struct pkt packet) {
    return packet.checksum != calculate_checksum(packet);
}

bool is_in_window(int base, int seq) {
    if (base <= seq)
        return seq < base + WINDOWSIZE;
    return seq < (base + WINDOWSIZE) % SEQSPACE || seq >= base;
}

void A_output(struct msg message) {
    if (!is_in_window(sender_window_base, sender_nextseqnum)) {
        return;
    }
    struct pkt packet;
    packet.seqnum = sender_nextseqnum;
    packet.acknum = -1;
    memcpy(packet.payload, message.data, PKT_DATA_SIZE);
    packet.checksum = calculate_checksum(packet);

    sender_buffer[sender_nextseqnum] = packet;
    sender_buffer_ack[sender_nextseqnum] = false;

    tolayer3(0, packet);
    if (sender_window_base == sender_nextseqnum) {
        starttimer(0, TIMEOUT);
    }
    sender_nextseqnum = (sender_nextseqnum + 1) % SEQSPACE;
}

void A_input(struct pkt packet) {
    if (is_corrupt(packet)) return;
    int ack = packet.acknum;
    if (!is_in_window(sender_window_base, ack)) return;

    sender_buffer_ack[ack] = true;
    while (sender_buffer_ack[sender_window_base]) {
        sender_buffer_ack[sender_window_base] = false;
        sender_window_base = (sender_window_base + 1) % SEQSPACE;
    }

    if (sender_window_base == sender_nextseqnum) {
        stoptimer(0);
    } else {
        stoptimer(0);
        starttimer(0, TIMEOUT);
    }
}

void A_timerinterrupt() {
    starttimer(0, TIMEOUT);
    for (int i = 0; i < WINDOWSIZE; i++) {
        int seq = (sender_window_base + i) % SEQSPACE;
        if (!sender_buffer_ack[seq] && is_in_window(sender_window_base, seq)) {
            tolayer3(0, sender_buffer[seq]);
        }
    }
}

void A_init() {
    memset(sender_buffer_ack, 0, sizeof(sender_buffer_ack));
    sender_window_base = 0;
    sender_nextseqnum = 0;
}

void B_input(struct pkt packet) {
    if (is_corrupt(packet)) {
        return;
    }

    int seq = packet.seqnum;
    if (is_in_window(receiver_window_base, seq)) {
        if (!receiver_packet_received[seq]) {
            receiver_packet_received[seq] = true;
            receiver_buffer[seq] = packet;
        }

        struct pkt ackpkt;
        ackpkt.seqnum = 0;
        ackpkt.acknum = seq;
        memset(ackpkt.payload, 0, PKT_DATA_SIZE);
        ackpkt.checksum = calculate_checksum(ackpkt);
        tolayer3(1, ackpkt);

        while (receiver_packet_received[receiver_window_base]) {
            tolayer5(1, receiver_buffer[receiver_window_base].payload);
            receiver_packet_received[receiver_window_base] = false;
            receiver_window_base = (receiver_window_base + 1) % SEQSPACE;
        }
    } else {
        struct pkt ackpkt;
        ackpkt.seqnum = 0;
        ackpkt.acknum = seq;
        memset(ackpkt.payload, 0, PKT_DATA_SIZE);
        ackpkt.checksum = calculate_checksum(ackpkt);
        tolayer3(1, ackpkt);
    }
}

void B_init() {
    memset(receiver_packet_received, 0, sizeof(receiver_packet_received));
    receiver_window_base = 0;
}
