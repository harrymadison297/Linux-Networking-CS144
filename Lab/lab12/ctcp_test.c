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
	 linked_list_t* wrapped_unacked_segments;
	 
 } tx_state_t;
 
 
 typedef struct 
 {
	 uint32_t last_seqno_accepted;
	 bool has_FIN_been_rxed;
	 linked_list_t* segments_to_output;
	 
 } rx_state_t;
 
struct ctcp_state {
  struct ctcp_state *next;  /* Next in linked list */
  struct ctcp_state **prev; /* Prev in linked list */

  conn_t *conn;             /* Connection object -- needed in order to figure
                               out destination when sending */

  /* FIXME: Add other needed fields. */
	  long FIN_WAIT_start_time;
  ctcp_config_t ctcp_config;
  tx_state_t tx_state;
	rx_state_t rx_state;
	
  
};

typedef struct {
  uint32_t         num_xmits;
  long             timestamp_of_last_send;
  ctcp_segment_t   ctcp_segment;
} wrapped_ctcp_segment_t;


/**
 * Linked list of connection states. Go through this in ctcp_timer() to
 * resubmit segments and tear down connections.
 */
static ctcp_state_t *state_list;

/* FIXME: Feel free to add as many helper functions as needed. Don't repeat
          code! Helper functions make the code clearer and cleaner. */

uint16_t ctcp_get_num_data_bytes(ctcp_segment_t* ctcp_segment_ptr);
void ctcp_send_control_segment(ctcp_state_t *state);
void ctcp_send_segment(ctcp_state_t *state, wrapped_ctcp_segment_t* wrapped_segment);
void ctcp_clean_up_unacked_segment_list(ctcp_state_t *state);
void ctcp_send_what_we_can(ctcp_state_t *state);

ctcp_state_t *ctcp_init(conn_t *conn, ctcp_config_t *cfg) {
  /* Connection could not be established. */
  if (conn == NULL) {
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
  /* FIXME: Do any other initialization here. */
  state->FIN_WAIT_start_time = 0;
////////////////////////////////
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
void ctcp_send_what_we_can(ctcp_state_t *state) {

  wrapped_ctcp_segment_t *wrapped_ctcp_segment_ptr;
  ll_node_t *curr_node_ptr;
  long ms_since_last_send;
  unsigned int i, length;
  uint32_t last_seqno_of_segment, last_allowable_seqno;

  if (state == NULL)
    return;

  length = ll_length(state->tx_state.wrapped_unacked_segments);
  if (length == 0)
    /* todo - this will have to be 'continue' or something for multiple connections */
    return;

  for (i = 0; i < length; ++i) {
    if (i == 0) {
      curr_node_ptr = ll_front(state->tx_state.wrapped_unacked_segments);
    } else {
      curr_node_ptr = curr_node_ptr->next;
    }

    wrapped_ctcp_segment_ptr = (wrapped_ctcp_segment_t *) curr_node_ptr->object;

    // Empty segments shouldn't make it into wrapped_unacked_segments.
    assert (wrapped_ctcp_segment_ptr->ctcp_segment.len != 0);

    last_seqno_of_segment = ntohl(wrapped_ctcp_segment_ptr->ctcp_segment.seqno)
      + ctcp_get_num_data_bytes(&wrapped_ctcp_segment_ptr->ctcp_segment) - 1;

    // Subtract 1 because the ackno is byte they want next, not the last byte
    // they've received.
    last_allowable_seqno = state->tx_state.last_ackno_rxed - 1
      + state->ctcp_config.send_window;

    if (state->tx_state.last_ackno_rxed == 0) {
      ++last_allowable_seqno; // last_ackno_rxed starts at 0
    }

    // If the segment is outside of the sliding window, then we're done.
    // "maintain invariant (LSS-LAR <= SWS)"
    if (last_seqno_of_segment > last_allowable_seqno) {
      return;
    }

    // If we got to this point, then we have a segment that's within the send
    // window. Any segments here that have not been sent can now be sent. The
    // first segment can be retransmitted if it timed out.
    if (wrapped_ctcp_segment_ptr->num_xmits == 0) {
      ctcp_send_segment(state, wrapped_ctcp_segment_ptr);
    } else if (i == 0) {
      // Check and see if we need to retrasnmit the first segment.
      ms_since_last_send = current_time() - wrapped_ctcp_segment_ptr->timestamp_of_last_send;
      if (ms_since_last_send > state->ctcp_config.rt_timeout) {
        // Timeout. Resend the segment.
        ctcp_send_segment(state, wrapped_ctcp_segment_ptr);
      }
    }
  }


#if 0
  /*
  **  Lab 1 - Stop and Wait
  */
  curr_state = state_list; /* Only one state for lab 1. */

  /* Make sure we actually have something to send. */
  if (ll_length(curr_state->tx_state.wrapped_unacked_segments) == 0)
    return;

  ll_node_ptr = ll_front(curr_state->tx_state.wrapped_unacked_segments);
  wrapped_ctcp_segment_ptr = (wrapped_ctcp_segment_t *) ll_node_ptr->object;

  if (curr_state->tx_state.last_ackno_rxed > curr_state->tx_state.last_seqno_sent) {
    /* The last data byte sent has been acknowledged, so try to send the next segment. */
    ctcp_send_segment(curr_state, wrapped_ctcp_segment_ptr);
  } else {
    /* We're still waiting for an acknowledgment. We might need to retransmit. */
    ms_since_last_send = current_time() - wrapped_ctcp_segment_ptr->timestamp_of_last_send;
    if (ms_since_last_send > curr_state->ctcp_config.rt_timeout) {
      // Timeout. Resend the segment.
      ctcp_send_segment(curr_state, wrapped_ctcp_segment_ptr);
    }
  }
#endif
}
void ctcp_destroy(ctcp_state_t *state) {
  /* Update linked list. */
	unsigned int len, i;
	if (state) {
		
  if (state->next)
    state->next->prev = state->prev;

  *state->prev = state->next;
  conn_remove(state->conn);

  /* FIXME: Do any other cleanup here. */
	len = ll_length(state->tx_state.wrapped_unacked_segments);
	for (i = 0; i < len; ++i)
    {
			ll_node_t *front_node_ptr = ll_front(state->tx_state.wrapped_unacked_segments);
      free(front_node_ptr->object);
      ll_remove(state->tx_state.wrapped_unacked_segments, front_node_ptr);
    }
    ll_destroy(state->tx_state.wrapped_unacked_segments);
		
	
	for (i = 0; i < len; ++i)
    {
      ll_node_t *front_node_ptr = ll_front(state->rx_state.segments_to_output);
      free(front_node_ptr->object);
      ll_remove(state->rx_state.segments_to_output, front_node_ptr);
    }
    ll_destroy(state->rx_state.segments_to_output);
		
  free(state);
	}
  end_client();
}

void ctcp_read(ctcp_state_t *state) {
  /* FIXME */
	  int bytes_read;
  // I add 1 here so that I can add null terminator in debug code
  uint8_t buf[MAX_SEG_DATA_SIZE+1];
  wrapped_ctcp_segment_t* new_segment_ptr;
	
	  if (state->tx_state.has_EOF_been_read)
    return;
  while ((bytes_read = conn_input(state->conn, buf, MAX_SEG_DATA_SIZE)) > 0)
  {
    /*
    ** Create a new ctcp segment.
    **
    ** An implementation that would lead to less memory fragmentation
    ** would be to allocate consistent sized blocks. Not sure if I'll have
    ** time for that.
    */
    new_segment_ptr = (wrapped_ctcp_segment_t*) calloc(1,
                                  sizeof(wrapped_ctcp_segment_t) + bytes_read);

    /* Initialize the ctcp segment. Remember that calloc init'd everything to zero.
    ** Most headers should be set by whatever fn actually ships this segment out. */
    new_segment_ptr->ctcp_segment.len = htons((uint16_t) sizeof(ctcp_segment_t) + bytes_read);

    /* Set the segment's sequence number. */
    new_segment_ptr->ctcp_segment.seqno = htonl(state->tx_state.last_seqno_read + 1);

    /* Copy the data we just read into the segment we just allocated. */
    memcpy(new_segment_ptr->ctcp_segment.data, buf, bytes_read);

    /* Set last_seqno_read. Sequence numbers start at 1, not 0, so we don't need
    ** to subtract 1 here. */
    state->tx_state.last_seqno_read += bytes_read;

    /* Add new ctcp segment to our list of unacknowledged segments. */
    //ll_add(state->tx_state.wrapped_unacked_segments, new_segment_ptr);
  }	
	
  if (bytes_read == -1)
  {
    state->tx_state.has_EOF_been_read = true;

    /* Create a FIN segment. */
    new_segment_ptr = (wrapped_ctcp_segment_t*) calloc(1, sizeof(wrapped_ctcp_segment_t));
    assert(new_segment_ptr != NULL);
    new_segment_ptr->ctcp_segment.len = htons((uint16_t) sizeof(ctcp_segment_t));
    new_segment_ptr->ctcp_segment.seqno = htonl(state->tx_state.last_seqno_read + 1);
    new_segment_ptr->ctcp_segment.flags |= TH_FIN;
    /* Add new ctcp segment to our list of unacknowledged segments. */
    //ll_add(state->tx_state.wrapped_unacked_segments, new_segment_ptr);
  }
  /* Try to send the data we just read. */
  ctcp_send_segment(state_list,new_segment_ptr);	
	
}
void ctcp_send_segment(ctcp_state_t *state, wrapped_ctcp_segment_t* wrapped_segment)
{
  long timestamp;
  uint16_t segment_cksum;
  int bytes_sent;

  if (wrapped_segment->num_xmits >= MAX_NUM_XMITS) {
    // Assume the other side is unresponsive and destroy the connection.
    ctcp_destroy(state);
    return;
  }

  /* Set the segment's ctcp header fields. */
  wrapped_segment->ctcp_segment.ackno = htonl(state->rx_state.last_seqno_accepted + 1);
  wrapped_segment->ctcp_segment.flags |= TH_ACK;
  wrapped_segment->ctcp_segment.window = htons(state->ctcp_config.recv_window);

  wrapped_segment->ctcp_segment.cksum = 0;
  segment_cksum = cksum(&wrapped_segment->ctcp_segment, ntohs(wrapped_segment->ctcp_segment.len));
  wrapped_segment->ctcp_segment.cksum = segment_cksum;

  /* Try to send the segment. */
  bytes_sent = conn_send(state->conn, &wrapped_segment->ctcp_segment,
                         ntohs(wrapped_segment->ctcp_segment.len));
  timestamp = current_time();
  wrapped_segment->num_xmits++;

  /*if (bytes_sent == 0)*/
  if (bytes_sent < ntohs(wrapped_segment->ctcp_segment.len) ) {
    return; // can't send for some reason, try again later.
  }
  if (bytes_sent == -1) {
    ctcp_destroy(state); // ya done now
    return;
  }

  /* Update state */
  state->tx_state.last_seqno_sent += bytes_sent;
  wrapped_segment->timestamp_of_last_send = timestamp;
}


void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len) {
  /* FIXME */
	uint16_t  num_data_bytes;
	uint32_t last_seqno_of_segment, largest_allowable_seqno, smallest_allowable_seqno;
	//ctcp_segment_t* ctcp_segment_ptr;
	
  /* If the segment was truncated, ignore it and hopefully retransmission will fix it. */
  if (len < ntohs(segment->len)) {
    free(segment);
    return;
  }
	
  // Check the checksum.
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
        (ntohl(segment->seqno) < smallest_allowable_seqno)) {
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

void ctcp_output(ctcp_state_t *state) {
  /* FIXME */
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

uint16_t ctcp_get_num_data_bytes(ctcp_segment_t* ctcp_segment_ptr)
{
  return ntohs(ctcp_segment_ptr->len) - sizeof(ctcp_segment_t);
}


void ctcp_timer() {
  /* FIXME */
	ctcp_state_t * curr_state;

  if (state_list == NULL) return;
	for (curr_state = state_list; curr_state != NULL; curr_state = curr_state->next) {

    ctcp_output(curr_state);
    ctcp_send_segment(curr_state,NULL);

    /* See if we need close down the connection. We can do this if:
     *   - FIN has been received from the other end (i.e., they have no more data
     *     to send us)
     *   - EOF has been read (i.e., user has no more data to send)
     *   - wrapped_unacked_segments is empty (i.e., all data we've sent
     *     (including the final FIN) has been acked)
     *   - segments_to_output is empty (i.e., we've nothing more to output)
     */

    if (   (curr_state->rx_state.has_FIN_been_rxed)
        && (curr_state->tx_state.has_EOF_been_read)
        && (ll_length(curr_state->tx_state.wrapped_unacked_segments) == 0)
        && (ll_length(curr_state->rx_state.segments_to_output) == 0)) {

      // Wait twice the maximum segment lifetime before tearing down the connection.
      if (curr_state->FIN_WAIT_start_time == 0) {
        curr_state->FIN_WAIT_start_time = current_time();
      } else if ((current_time() - curr_state->FIN_WAIT_start_time) > (2*MAX_SEG_LIFETIME_MS)) {
        ctcp_destroy(curr_state);
      }
    }
  }
}

// We'll need to call this after successfully receiving a segment to clean
// acknowledged segments out of wrapped_unacked_segments
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