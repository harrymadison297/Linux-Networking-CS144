/******************************************************************************
 * ctcp.c
 * ------
 * Implementation of cTCP done here. This is the only file you need to change.
 * Look at the following files for references and useful functions:
 *   - ctcp.h: Headers for this file.
 *   - ctcp_iinked_list.h: Linked list functions for managing a linked list.
 *   - ctcp_sys.h: Connection-related structs and functions, cTCP segment
 *                 definition.
 *   - ctcp_utils.h: Checksum computation, getting the current time.
 *
 *****************************************************************************/

#include "ctcp.h"
#include "ctcp_linked_list.h"
#include "ctcp_sys.h"
#include "ctcp_utils.h"

/**
 * Connection state.
 *
 * Stores per-connection information such as the current sequence number,
 * unacknowledged packets, etc.
 *
 * You should add to this to store other fields you might need.
 */
typedef struct
{
  uint32_t last_ackno_rxed;
  bool has_EOF_been_read;
  uint32_t last_seqno_read;
  uint32_t last_seqno_sent;
  linked_list_t *wrapped_unacked_segments;

} tx_state_t;

typedef struct
{
  uint32_t last_seqno_accepted;
  bool has_FIN_been_rxed;
  linked_list_t *segments_to_output;

} rx_state_t;

struct ctcp_state
{
  struct ctcp_state *next;  /* Next in linked list */
  struct ctcp_state **prev; /* Prev in linked list */

  conn_t *conn;            /* Connection object -- needed in order to figure
                              out destination when sending */
  linked_list_t *segments; /* Linked list of segments sent to this connection.
                              It may be useful to have multiple linked lists
                              for unacknowledged segments, segments that
                              haven't been sent, etc. Lab 1 uses the
                              stop-and-wait protocol and therefore does not
                              necessarily need a linked list. You may remove
                              this if this is the case for you */

  /* FIXME: Add other needed fields. */
  long FIN_WAIT_start_time;
  ctcp_config_t ctcp_config;
  tx_state_t tx_state;
  rx_state_t rx_state;
};

typedef struct
{
  uint32_t num_xmits;
  long timestamp_of_last_send;
  ctcp_segment_t ctcp_segment;
} wrapped_ctcp_segment_t;

/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

uint16_t ctcp_get_num_data_bytes(ctcp_segment_t *ctcp_segment_ptr);
void ctcp_send_control_segment(ctcp_state_t *state);
void ctcp_send_segment(ctcp_state_t *state, wrapped_ctcp_segment_t *wrapped_segment);
void ctcp_clean_up_unacked_segment_list(ctcp_state_t *state);
void ctcp_send_what_we_can(ctcp_state_t *state);

/* ===================================HELPER======================================== */
void ctcp_send_control_segment(ctcp_state_t *state) {
  ctcp_segment_t ctcp_segment;

  ctcp_segment.seqno = htonl(0); // I don't think seqno matters for pure control segments
  ctcp_segment.ackno = htonl(state->rx_state.last_seqno_accepted + 1);
  ctcp_segment.len   = sizeof(ctcp_segment_t);
  ctcp_segment.flags = TH_ACK;
  ctcp_segment.window = htons(state->ctcp_config.recv_window);
  ctcp_segment.cksum = 0;
  ctcp_segment.cksum = cksum(&ctcp_segment, sizeof(ctcp_segment_t));

  // deliberately ignore return value
  conn_send(state->conn, &ctcp_segment, sizeof(ctcp_segment_t));
}

void ctcp_clean_up_unacked_segment_list(ctcp_state_t *state) {
  ll_node_t* front_node_ptr;
  wrapped_ctcp_segment_t* wrapped_ctcp_segment_ptr;
  uint32_t seqno_of_last_byte;
  uint16_t num_data_bytes;

  while (ll_length(state->tx_state.wrapped_unacked_segments) != 0) {
    front_node_ptr = ll_front(state->tx_state.wrapped_unacked_segments);
    wrapped_ctcp_segment_ptr = (wrapped_ctcp_segment_t*) front_node_ptr->object;
    num_data_bytes = ntohs(wrapped_ctcp_segment_ptr->ctcp_segment.len) - sizeof(ctcp_segment_t);
    seqno_of_last_byte =   ntohl(wrapped_ctcp_segment_ptr->ctcp_segment.seqno)
                         + num_data_bytes - 1;

    if (seqno_of_last_byte < state->tx_state.last_ackno_rxed) {
      // This segment has been acknowledged.
      free(wrapped_ctcp_segment_ptr);
      ll_remove(state->tx_state.wrapped_unacked_segments, front_node_ptr);
    } else {
      // This segment has not been acknowledged, so our cleanup is done.
      return;
    }
  }
}

/* ===================================CORE======================================== */
ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg)
{
  /* Connection could not be established. */
  if (conn == NULL)
  {
    return NULL;
  }

  /* Established a connection. Create a new state and update the linked list
     of connection states. */
  ctcp_state_t *state = calloc(sizeof(ctcp_state_t), 1);
  state->next = state_list;
  state->prev = &state_list;
  if (state_list)
    state_list->prev = &state->next;
  state_list = state;

  /* Set fields. */
  state->conn = conn;
  state->FIN_WAIT_start_time = 0;

  /* Initialize ctcp_config */
  state->ctcp_config.recv_window = cfg->recv_window;
  state->ctcp_config.send_window = cfg->send_window;
  state->ctcp_config.timer = cfg->timer;
  state->ctcp_config.rt_timeout = cfg->rt_timeout;

  /* Initialize tx_state */
  state->tx_state.last_ackno_rxed = 0;
  state->tx_state.has_EOF_been_read = false;
  state->tx_state.last_seqno_read = 0;
  state->tx_state.last_seqno_sent = 0;
  state->tx_state.wrapped_unacked_segments = ll_create();

  /* Initialize rx_state */
  state->rx_state.last_seqno_accepted = 0;
  state->rx_state.has_FIN_been_rxed = false;
  state->rx_state.segments_to_output = ll_create();

  free(cfg);

  return state;
}

void ctcp_destroy(ctcp_state_t *state)
{
  /* Update linked list. */
  if (state->next)
    state->next->prev = state->prev;

  *state->prev = state->next;
  conn_remove(state->conn);

  /* FIXME: Do any other cleanup here. */

  free(state);
  end_client();
}

void ctcp_read(ctcp_state_t *state)
{
  int bytes_read;
  uint8_t buf[MAX_SEG_DATA_SIZE];
  wrapped_ctcp_segment_t *new_segment_ptr;
  linked_list_t *segment_list = ll_create();
  ll_node_t *segment_node;

  /* Check if EOF */
  if (state->tx_state.has_EOF_been_read)
    return;

  /* Read from input */
  while ((bytes_read = conn_input(state->conn, buf, MAX_SEG_DATA_SIZE)) > 0)
  {
    /* Create a new ctcp segment */
    new_segment_ptr = (wrapped_ctcp_segment_t *)calloc(1,
                                                       sizeof(wrapped_ctcp_segment_t) + bytes_read);

    /* Initialize the ctcp segment */
    new_segment_ptr->ctcp_segment.len = htons((uint16_t)sizeof(ctcp_segment_t) + bytes_read);

    /* Set the segment's sequence number. */
    new_segment_ptr->ctcp_segment.seqno = htonl(state->tx_state.last_seqno_read + 1);

    /* Copy the data just readed into new segment */
    memcpy(new_segment_ptr->ctcp_segment.data, buf, bytes_read);

    /* Set last_seqno_read. Sequence numbers start at 1 */
    state->tx_state.last_seqno_read += bytes_read;

    /* Add node to send linked list */
    ll_add(segment_list, new_segment_ptr);
  }

  /* Send linked list to other side */
  segment_node = segment_list->head;
  while (segment_node)
  {
    /* Add new ctcp segment to our list of unacknowledged segments. Send it to other side */
    int send_state = conn_send(state->conn,
                               &((wrapped_ctcp_segment_t *)segment_node->object)->ctcp_segment,
                               ntohs(((wrapped_ctcp_segment_t *)segment_node->object)->ctcp_segment.len));
    
    /* Check if send is successfully, move to next segment and free heap */
    if (send_state > 0)
    {
      segment_node = segment_node->next;
      free((wrapped_ctcp_segment_t *)segment_node->object);
    }
  }

  /* Free up memory for linked list */
  ll_destroy(segment_list);
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len)
{
  uint16_t  num_data_bytes;
	uint32_t last_seqno_of_segment, largest_allowable_seqno, smallest_allowable_seqno;

  /* If the segment was truncated, ignore it and hopefully retransmission will fix it. */
  if (len < ntohs(segment->len)) {
    free(segment);
    return;
  }

  /* Check the checksum */ 
  if (segment->cksum != cksum(segment, ntohs(segment->len)))
  {
    free(segment);
    return;
  }

  num_data_bytes = ntohs(segment->len) - sizeof(ctcp_segment_t);	

  // Reject the segment if it's outside of the receive window.
  if (num_data_bytes) 
	{
    last_seqno_of_segment = ntohl(segment->seqno) + num_data_bytes - 1;
    smallest_allowable_seqno = state->rx_state.last_seqno_accepted + 1;
    largest_allowable_seqno = state->rx_state.last_seqno_accepted
      + state->ctcp_config.recv_window;

    if ((last_seqno_of_segment > largest_allowable_seqno) ||
        (ntohl(segment->seqno) < smallest_allowable_seqno)) 
    {
      free(segment);
      // Let the sender know our state, since they sent a wonky packet. Maybe
      // our previous ack was lost.
      ctcp_send_control_segment(state);
      return;
    }
  }

  /* If the segment arrived out of order, ignore it. */
  if (   (ntohl(segment->seqno) != (state->rx_state.last_seqno_accepted + 1))
      && (num_data_bytes != 0)) {
    free(segment);
    // Our 'ACK' of the out of order segment might have been lost, so send it
    // again.
    ctcp_send_control_segment(state);
    return;
  }

  // if ACK flag is set, update tx_state.last_ackno_rxed
  if (segment->flags & TH_ACK) {
    state->tx_state.last_ackno_rxed = ntohl(segment->ackno);
  }
	
	if (segment->flags & TH_FIN)
	{
		free(segment);
	}
	ctcp_output(state);	
	  /* The ackno has probably advanced, so clean up our list of unacked segments. */
  ctcp_clean_up_unacked_segment_list(state);
}

/* ================================================================================== */
void ctcp_output(ctcp_state_t *state)
{
  ll_node_t* front_node_ptr;
  ctcp_segment_t* ctcp_segment_ptr;	
  int num_data_bytes;
	size_t bufspace;
  int return_value;		
  int num_segments_output = 0;
	    front_node_ptr = ll_front(state->rx_state.segments_to_output);
    ctcp_segment_ptr = (ctcp_segment_t*) front_node_ptr->object;
   num_data_bytes = ntohs(ctcp_segment_ptr->len) - sizeof(ctcp_segment_t);
    if (num_data_bytes) {

      // Check the segment's sequence number. There might be a hole in
      // segments_to_output, in which case we should give up.
      if ( ntohl(ctcp_segment_ptr->seqno) != state->rx_state.last_seqno_accepted + 1)
      {
        return;
      }

      // See if there's enough bufspace right now to output.
      bufspace = conn_bufspace(state->conn);
      if (bufspace < num_data_bytes) {
        // can't send right now, give up and try later.
        return;
      }

      return_value = conn_output(state->conn, ctcp_segment_ptr->data, num_data_bytes);
      if (return_value == -1) {
        ctcp_destroy(state);
        return;
      }
      num_segments_output++;
    } 
    // update rx_state.last_seqno_accepted
    if (num_data_bytes) {
      state->rx_state.last_seqno_accepted += num_data_bytes;
    }
		
    // If this segment's FIN flag is set, output EOF by setting length to 0,
    // and update state.
    if ((!state->rx_state.has_FIN_been_rxed) && (ctcp_segment_ptr->flags & TH_FIN)) {
      state->rx_state.has_FIN_been_rxed = true;
      state->rx_state.last_seqno_accepted++;
      conn_output(state->conn, ctcp_segment_ptr->data, 0);
      num_segments_output++;
    }
	free(ctcp_segment_ptr);

  if (num_segments_output) {
    // Send an ack. Acking here (instead of in ctcp_receive) flow controls the
    // sender until buffer space is available.
    ctcp_send_control_segment(state);
  }
}

/* ================================================================================== */
void ctcp_timer()
{
  /* FIXME */
}
ok
