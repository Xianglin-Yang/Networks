#include <stdlib.h>
#include <stdio.h>
#include <stdbool.h>
#include "emulator.h"
#include "gbn.h"

/* ******************************************************************
   Selective Repeat protocol implementation based on Go Back N.
   
   Network properties:
   - one way network delay averages five time units (longer if there
   are other messages in the channel for GBN), but can be larger
   - packets can be corrupted (either the header or the data portion)
   or lost, according to user-defined probabilities
   - packets will be delivered in the order in which they were sent
   (although some can be lost).
**********************************************************************/

#define RTT  16.0       /* round trip time.  MUST BE SET TO 16.0 when submitting assignment */
#define WINDOWSIZE 6    /* the maximum number of buffered unacked packet
                          MUST BE SET TO 6 when submitting assignment */
#define SEQSPACE 12     /* the sequence space for SR should be at least 2 * windowsize */
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
static int timers[WINDOWSIZE];         /* Tracks which packets have active timers */
static int windowfirst, windowlast;    /* array indexes of the first/last packet awaiting ACK */
static int windowcount;                /* the number of packets currently awaiting an ACK */
static int A_nextseqnum;               /* the next sequence number to be used by the sender */

/* Maps a sequence number to its position in the sender's window */
int seq_to_index(int seqnum)
{
  return seqnum % WINDOWSIZE;
}

/* Checks if a sequence number is within the current window */
bool is_in_window(int seqnum, int base, int window_size)
{
  return ((seqnum - base + SEQSPACE) % SEQSPACE) < window_size;
}

/* called from layer 5 (application layer), passed the message to be sent to other side */
void A_output(struct msg message)
{
  struct pkt sendpkt;
  int i;

  /* if not blocked waiting on ACK */
  if (windowcount < WINDOWSIZE) {
    if (TRACE > 1)
      printf("----A: New message arrives, send window is not full, send new messge to layer3!\n");

    /* create packet */
    sendpkt.seqnum = A_nextseqnum;
    sendpkt.acknum = NOTINUSE;
    for (i=0; i<20; i++)
      sendpkt.payload[i] = message.data[i];
    sendpkt.checksum = ComputeChecksum(sendpkt);

    /* put packet in window buffer */
    windowlast = (windowfirst + windowcount) % WINDOWSIZE;
    buffer[windowlast] = sendpkt;
    acked[windowlast] = false;
    timers[windowlast] = 1;  /* Mark this packet as having an active timer */
    windowcount++;

    /* send out packet */
    if (TRACE > 0)
      printf("Sending packet %d to layer 3\n", sendpkt.seqnum);
    tolayer3(A, sendpkt);

    /* start timer for this packet */
    starttimer(A, RTT);

    /* get next sequence number, wrap back to 0 */
    A_nextseqnum = (A_nextseqnum + 1) % SEQSPACE;
  }
  /* if blocked, window is full */
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
    int base_seq = buffer[windowfirst].seqnum;
    
    if (is_in_window(packet.acknum, base_seq, windowcount)) {
      /* Calculate the position of this ACK in our window */
      int pos = (packet.acknum - base_seq + SEQSPACE) % SEQSPACE;
      int index = (windowfirst + pos) % WINDOWSIZE;
      
      if (!acked[index]) {
        /* Mark as acknowledged */
        acked[index] = true;
        new_ACKs++;
        
        /* Stop timer for this packet */
        if (timers[index]) {
          stoptimer(A);
          timers[index] = 0;
        }
        
        /* Slide window if possible */
        while (windowcount > 0 && acked[windowfirst]) {
          if (TRACE > 1)
            printf("----A: Sliding window, removing packet %d\n", buffer[windowfirst].seqnum);
          
          acked[windowfirst] = false;
          timers[windowfirst] = 0;
          windowfirst = (windowfirst + 1) % WINDOWSIZE;
          windowcount--;
        }
        
        /* Start timer for the first unacknowledged packet if we have one */
        if (windowcount > 0) {
          int i;
          for (i = 0; i < windowcount; i++) {
            int idx = (windowfirst + i) % WINDOWSIZE;
            if (!acked[idx]) {
              if (!timers[idx]) {
                if (TRACE > 1)
                  printf("----A: Starting timer for packet %d\n", buffer[idx].seqnum);
                starttimer(A, RTT);
                timers[idx] = 1;
              }
              break;
            }
          }
        }
      }
    }
    else {
      if (TRACE > 0)
        printf("----A: ACK %d outside current window, ignoring\n", packet.acknum);
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
    printf("----A: timer interrupt, resend unacked packet!\n");

  if (windowcount > 0) {
    /* Find the first unacknowledged packet */
    int i;
    for (i = 0; i < windowcount; i++) {
      int idx = (windowfirst + i) % WINDOWSIZE;
      if (!acked[idx]) {
        /* Resend this packet */
        if (TRACE > 0)
          printf("---A: resending packet %d\n", buffer[idx].seqnum);
        tolayer3(A, buffer[idx]);
        packets_resent++;
        
        /* Restart timer for this packet */
        timers[idx] = 1;
        starttimer(A, RTT);
        break;
      }
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
  
  int i;
  for (i = 0; i < WINDOWSIZE; i++) {
    acked[i] = false;
    timers[i] = 0;
    buffer[i].seqnum = NOTINUSE;
  }
}

/********* Receiver (B) variables and procedures ************/

static int expectedseqnum;    /* the sequence number expected next by the receiver */
static int B_nextseqnum;      /* the sequence number for the next packets sent by B */
static int rcv_base;          /* base sequence number of receiving window */
static struct pkt recvbuffer[WINDOWSIZE];  /* Buffer for out-of-order packets */
static bool recvacked[WINDOWSIZE];         /* Tracks received packets */

/* called from layer 3, when a packet arrives */
void B_input(struct pkt packet)
{
  struct pkt sendpkt;
  int i;

  if (!IsCorrupted(packet)) {
    if (TRACE > 0)
      printf("----B: packet %d is correctly received\n", packet.seqnum);

    /* Check if packet is within receive window */
    if (is_in_window(packet.seqnum, rcv_base, WINDOWSIZE)) {
      /* Calculate buffer index */
      int index = seq_to_index(packet.seqnum);
      
      /* Store the packet if not already received */
      if (!recvacked[index]) {
        recvbuffer[index] = packet;
        recvacked[index] = true;
        
        /* Deliver in-order packets */
        while (recvacked[seq_to_index(rcv_base)]) {
          /* Deliver to upper layer */
          tolayer5(B, recvbuffer[seq_to_index(rcv_base)].payload);
          
          /* Mark as no longer received */
          recvacked[seq_to_index(rcv_base)] = false;
          
          /* Move receive window */
          rcv_base = (rcv_base + 1) % SEQSPACE;
          packets_received++;
          
          if (TRACE > 1)
            printf("----B: delivered packet %d to layer 5, new rcv_base is %d\n", 
                  (rcv_base - 1 + SEQSPACE) % SEQSPACE, rcv_base);
        }
      }
      
      /* Send ACK for this packet */
      sendpkt.acknum = packet.seqnum;
    }
    else {
      /* Packet outside window - may be a duplicate or too far ahead */
      if (TRACE > 0)
        printf("----B: packet %d outside receive window (%d to %d)\n", 
              packet.seqnum, rcv_base, (rcv_base + WINDOWSIZE - 1) % SEQSPACE);
      
      /* If it's a duplicate from previous window, acknowledge it again */
      if (((packet.seqnum - rcv_base + SEQSPACE) % SEQSPACE) >= WINDOWSIZE &&
          ((rcv_base - packet.seqnum - 1 + SEQSPACE) % SEQSPACE) < WINDOWSIZE) {
        sendpkt.acknum = packet.seqnum;
      } else {
        /* For any other case, just ACK the last packet we successfully received */
        sendpkt.acknum = (rcv_base - 1 + SEQSPACE) % SEQSPACE;
      }
    }
  }
  else {
    if (TRACE > 0)
      printf("----B: packet corrupted, sending ACK for last in-order packet!\n");
    sendpkt.acknum = (rcv_base - 1 + SEQSPACE) % SEQSPACE;
  }

  /* create ACK packet */
  sendpkt.seqnum = B_nextseqnum;
  B_nextseqnum = (B_nextseqnum + 1) % 2;  /* Alternating bit for ACK packets */

  /* we don't have any data to send. fill payload with 0's */
  for (i=0; i<20; i++)
    sendpkt.payload[i] = '0';

  /* compute checksum */
  sendpkt.checksum = ComputeChecksum(sendpkt);

  /* send out packet */
  tolayer3(B, sendpkt);
}

/* Initialize receiver */
void B_init(void)
{
  expectedseqnum = 0;
  rcv_base = 0;
  B_nextseqnum = 1;
  
  int i;
  for (i = 0; i < WINDOWSIZE; i++) {
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
