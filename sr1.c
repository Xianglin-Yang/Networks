#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include "emulator.h"
#include "gbn.h"

/* ******************************************************************
   Go Back N protocol.  Adapted from J.F.Kurose
   ALTERNATING BIT AND GO-BACK-N NETWORK EMULATOR: VERSION 1.2

   Network properties:
   - one way network delay averages five time units (longer if there
   are other messages in the channel for GBN), but can be larger
   - packets can be corrupted (either the header or the data portion)
   or lost, according to user-defined probabilities
   - packets will be delivered in the order in which they were sent
   (although some can be lost).

   Modifications:
   - removed bidirectional GBN code and other code not used by prac.
   - fixed C style to adhere to current programming style
   - added GBN implementation
**********************************************************************/

#define RTT  16.0       /* round trip time.  MUST BE SET TO 16.0 when submitting assignment */
#define WINDOWSIZE 6    /* the maximum number of buffered unacked packet
                          MUST BE SET TO 6 when submitting assignment */
#define SEQSPACE 7      /* the min sequence space for GBN must be at least windowsize + 1 */
#define NOTINUSE (-1)   /* used to fill header fields that are not being used */

/* generic procedure to compute the checksum of a packet.  Used by both sender and receiver
   the simulator will overwrite part of your packet with 'z's.  It will not overwrite your
   original checksum.  This procedure must generate a different checksum to the original if
   the packet is corrupted.
*/
int ComputeChecksum(struct pkt packet)
{
  int checksum = 0;
  int i;

  checksum = packet.seqnum;
  checksum += packet.acknum;
  for ( i=0; i<20; i++ )
    checksum += (int)(packet.payload[i]);

  return checksum;
}

bool IsCorrupted(struct pkt packet)
{
  return (packet.checksum != ComputeChecksum(packet));
}


/********* Sender (A) variables and functions ************/

static struct pkt buffer[WINDOWSIZE];  /* array for storing packets waiting for ACK */
static bool acked[WINDOWSIZE];         /* Tracks ACK status of packets */
static int windowfirst, windowlast;    /* array indexes of the first/last packet awaiting ACK */
static int windowcount;                /* the number of packets currently awaiting an ACK */
static int A_nextseqnum;               /* the next sequence number to be used by the sender */

/* called from layer 5 (application layer), passed the message to be sent to other side */
void A_output(struct msg message)
{
  struct pkt sendpkt;
  int i;

  /* if not blocked waiting on ACK */
  if ( windowcount < WINDOWSIZE) {
    if (TRACE > 1)
      printf("----A: New message arrives, send window is not full, send new messge to layer3!\n");

    /* create packet */
    sendpkt.seqnum = A_nextseqnum;
    sendpkt.acknum = NOTINUSE;
    for ( i=0; i<20 ; i++ )
      sendpkt.payload[i] = message.data[i];
    sendpkt.checksum = ComputeChecksum(sendpkt);

    /* put packet in window buffer */
    /* windowlast will always be 0 for alternating bit; but not for GoBackN */
    windowlast = (windowlast + 1) % WINDOWSIZE;
    buffer[windowlast] = sendpkt;
    acked[windowlast] = false;
    windowcount++;

    /* send out packet */
    if (TRACE > 0)
      printf("Sending packet %d to layer 3\n", sendpkt.seqnum);
    tolayer3 (A, sendpkt);

    /* start timer if first packet in window */
    if (windowcount == 1)
      starttimer(A,RTT);

    /* get next sequence number, wrap back to 0 */
    A_nextseqnum = (A_nextseqnum + 1) % SEQSPACE;
  }
  /* if blocked,  window is full */
  else {
    if (TRACE > 0)
      printf("----A: New message arrives, send window is full\n");
    window_full++;
  }
}


/* called from layer 3, when a packet arrives for layer 4
   In this practical this will always be an ACK as B never sends data.
*/

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

/* called when A's timer goes off */
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



/********* Receiver (B)  variables and procedures ************/

static int expectedseqnum; /* the sequence number expected next by the receiver */
static int B_nextseqnum;   /* the sequence number for the next packets sent by B */
static struct pkt recvbuffer[WINDOWSIZE]; /* Buffer for out-of-order packets */
static bool recvacked[WINDOWSIZE];     /* Tracks received packets */

/* called from layer 3, when a packet arrives*/
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

  /* create ACK packet */
  sendpkt.seqnum = B_nextseqnum;
  B_nextseqnum = (B_nextseqnum + 1) % 2;

  /* we don't have any data to send.  fill payload with 0's */
  for ( i=0; i<20 ; i++ )
    sendpkt.payload[i] = '0';

  /* computer checksum */
  sendpkt.checksum = ComputeChecksum(sendpkt);

  /* send out packet */
  tolayer3 (B, sendpkt);
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

/******************************************************************************
 * The following functions need be completed only for bi-directional messages *
 *****************************************************************************/

/* Note that with simplex transfer from a-to-B, there is no B_output() */
void B_output(struct msg message)
{
}

/* called when B's timer goes off */
void B_timerinterrupt(void)
{
}
