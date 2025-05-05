#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include "emulator.h"
#include "sr.h"

/* ******************************************************************
   Selective Repeat protocol. Adapted from Go-Back-N implementation.

   Network properties:
   - One-way network delay averages five time units (longer if there
     are other messages in the channel), but can be larger.
   - Packets can be corrupted (header or data) or lost, based on user-defined probabilities.
   - Packets are delivered in the order they were sent (though some may be lost).
**********************************************************************/

#define RTT  16.0       /* Round trip time. MUST BE SET TO 16.0 for submission */
#define WINDOWSIZE 6    /* Maximum number of buffered unacked packets. MUST BE SET TO 6 */
#define SEQSPACE 7      /* Sequence space for SR, must be at least 2 * WINDOWSIZE (but 7 is used here) */
#define NOTINUSE (-1)   /* Filler for unused header fields */

/* Compute checksum for a packet */
int ComputeChecksum(struct pkt packet)
{
  int checksum = 0;
  int i;

  checksum = packet.seqnum;
  checksum += packet.acknum;
  for (i = 0; i < 20; i++)
    checksum += (int)(packet.payload[i]);

  return checksum;
}

/* Check if a packet is corrupted */
bool IsCorrupted(struct pkt packet)
{
  return (packet.checksum != ComputeChecksum(packet));
}

/********* Sender (A) Variables and Functions ************/

static struct pkt buffer[WINDOWSIZE];  /* Buffer for packets awaiting ACK */
static bool acked[WINDOWSIZE];         /* Tracks ACK status of packets */
static int windowfirst, windowlast;    /* Indexes of first/last packet in window */
static int windowcount;                /* Number of packets awaiting ACK */
static int A_nextseqnum;               /* Next sequence number to use */

/* Called from layer 5 to send a message */
void A_output(struct msg message)
{
  struct pkt sendpkt;
  int i;

  if (windowcount < WINDOWSIZE) {
    if (TRACE > 1)
      printf("----A: New message arrives, send window is not full, send new messge to layer3!\n");

    /* Create packet */
    sendpkt.seqnum = A_nextseqnum;
    sendpkt.acknum = NOTINUSE;
    for (i = 0; i < 20; i++)
      sendpkt.payload[i] = message.data[i];
    sendpkt.checksum = ComputeChecksum(sendpkt);

    /* Store in buffer */
    windowlast = (windowlast + 1) % WINDOWSIZE;
    buffer[windowlast] = sendpkt;
    acked[windowlast] = false;
    windowcount++;

    /* Send packet */
    if (TRACE > 0)
      printf("Sending packet %d to layer 3\n", sendpkt.seqnum);
    tolayer3(A, sendpkt);

    /* Start timer only if this is the first packet in the window */
    if (windowcount == 1)
      starttimer(A, RTT);

    /* Increment sequence number */
    A_nextseqnum = (A_nextseqnum + 1) % SEQSPACE;
  }
  else {
    if (TRACE > 0)
      printf("----A: New message arrives, send window is full\n");
    window_full++;
  }
}

/* Called from layer 3 when an ACK arrives */
void A_input(struct pkt packet)
{
  if (!IsCorrupted(packet)) {
    if (TRACE > 0)
      printf("----A: uncorrupted ACK %d is received\n", packet.acknum);
    total_ACKs_received++;

    /* Check if ACK is within the current window */
    int seqfirst = buffer[windowfirst].seqnum;
    int ackindex = (packet.acknum - seqfirst + SEQSPACE) % SEQSPACE;
    if (ackindex < WINDOWSIZE && !acked[(windowfirst + ackindex) % WINDOWSIZE]) {
      acked[(windowfirst + ackindex) % WINDOWSIZE] = true;
      new_ACKs++;

      /* Slide window if the first packet is ACKed */
      while (acked[windowfirst] && windowcount > 0) {
        windowcount--;
        windowfirst = (windowfirst + 1) % WINDOWSIZE;
      }

      /* Manage timer */
      if (windowcount > 0) {
        /* Restart timer for the new earliest unacked packet */
        stoptimer(A);
        starttimer(A, RTT);
      } else {
        /* Stop timer if all packets are ACKed */
        stoptimer(A);
      }
    }
  }
  else {
    if (TRACE > 0)
      printf("----A: corrupted ACK is received, do nothing!\n");
  }
}

/* Called when A's timer expires */
void A_timerinterrupt(void)
{
  if (TRACE > 0)
    printf("----A: time out, resend earliest unacked packet!\n");

  if (windowcount > 0) {
    int index = windowfirst;
    if (!acked[index]) {
      if (TRACE > 0)
        printf("---A: resending packet %d\n", buffer[index].seqnum);
      tolayer3(A, buffer[index]);
      packets_resent++;
      /* Restart timer for this packet */
      starttimer(A, RTT);
    }
  }
}

/* Initialize sender */
void A_init(void)
{
  A_nextseqnum = 0;
  windowfirst = 0;
  windowlast = -1;
  windowcount = 0;
  for (int i = 0; i < WINDOWSIZE; i++) {
    acked[i] = false;
    buffer[i].seqnum = NOTINUSE;
  }
}

/********* Receiver (B) Variables and Functions ************/

static int expectedseqnum;             /* Next expected sequence number */
static int B_nextseqnum;               /* Next sequence number for B's packets */
static struct pkt recvbuffer[WINDOWSIZE]; /* Buffer for out-of-order packets */
static bool recvacked[WINDOWSIZE];     /* Tracks received packets */

/* Called from layer 3 when a packet arrives */
void B_input(struct pkt packet)
{
  struct pkt sendpkt;
  int i;

  if (!IsCorrupted(packet)) {
    if (TRACE > 0)
      printf("----B: packet %d is correctly received\n", packet.seqnum);

    /* Check if packet is within receive window */
    int recvindex = (packet.seqnum - expectedseqnum + SEQSPACE) % SEQSPACE;
    if (recvindex < WINDOWSIZE) {
      int bufindex = (expectedseqnum + recvindex) % WINDOWSIZE;
      if (!recvacked[bufindex]) {
        recvbuffer[bufindex] = packet;
        recvacked[bufindex] = true;
      }

      /* Deliver in-order packets */
      while (recvacked[expectedseqnum % WINDOWSIZE]) {
        tolayer5(B, recvbuffer[expectedseqnum % WINDOWSIZE].payload);
        recvacked[expectedseqnum % WINDOWSIZE] = false;
        expectedseqnum = (expectedseqnum + 1) % SEQSPACE;
        packets_received++;
      }

      /* Send ACK for this packet */
      sendpkt.acknum = packet.seqnum;
    }
    else {
      /* Packet outside window, ACK last in-order packet */
      sendpkt.acknum = (expectedseqnum - 1 + SEQSPACE) % SEQSPACE;
    }
  }
  else {
    if (TRACE > 0)
      printf("----B: packet corrupted, resend ACK!\n");
    sendpkt.acknum = (expectedseqnum - 1 + SEQSPACE) % SEQSPACE;
  }

  /* Create ACK packet */
  sendpkt.seqnum = B_nextseqnum;
  B_nextseqnum = (B_nextseqnum + 1) % 2;
  for (i = 0; i < 20; i++)
    sendpkt.payload[i] = '0';
  sendpkt.checksum = ComputeChecksum(sendpkt);

  /* Send ACK */
  tolayer3(B, sendpkt);
}

/* Initialize receiver */
void B_init(void)
{
  expectedseqnum = 0;
  B_nextseqnum = 1;
  for (int i = 0; i < WINDOWSIZE; i++) {
    recvacked[i] = false;
  }
}

/********* Bidirectional Stubs (Not Required) ************/

void B_output(struct msg message)
{
}

void B_timerinterrupt(void)
{
}
