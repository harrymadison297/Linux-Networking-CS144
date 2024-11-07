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

// char log_ok[4] = "\nOK\n";
// conn_output(state->conn, log_ok, 4);

/* ===================================HELPER======================================== */
void ctcp_send_control_segment(ctcp_state_t *state)
{
  ctcp_segment_t ctcp_segment;

  ctcp_segment.seqno = htonl(0); // I don't think seqno matters for pure control segments
  ctcp_segment.ackno = htonl(state->rx_state.last_seqno_accepted + 1);
  ctcp_segment.len = sizeof(ctcp_segment_t);
  ctcp_segment.flags = TH_ACK;
  ctcp_segment.window = 0;
  ctcp_segment.cksum = 0;
  ctcp_segment.cksum = cksum(&ctcp_segment, sizeof(ctcp_segment_t));

  conn_send(state->conn, &ctcp_segment, sizeof(ctcp_segment_t));
}

void ctcp_send_unwrapped_segment(ctcp_state_t *state, wrapped_ctcp_segment_t *new_segment_ptr)
{
  conn_send(state->conn, &new_segment_ptr->ctcp_segment, ntohs(new_segment_ptr->ctcp_segment.len));
  new_segment_ptr->timestamp_of_last_send = current_time();
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
  uint8_t buf[MAX_SEG_DATA_SIZE + 1];
  wrapped_ctcp_segment_t *new_segment_ptr;

  /* Check if EOF */
  if (state->tx_state.has_EOF_been_read)
    return;

  /* Read from input */
  bytes_read = conn_input(state->conn, buf, MAX_SEG_DATA_SIZE);
  if (bytes_read > 0)
  {
    /* Create a new ctcp segment */
    new_segment_ptr = (wrapped_ctcp_segment_t *)calloc(1,
                                                       sizeof(wrapped_ctcp_segment_t) + bytes_read);
    if (NULL == new_segment_ptr)
      return;

    /* Initialize the ctcp segment */
    new_segment_ptr->ctcp_segment.len = htons((uint16_t)sizeof(ctcp_segment_t) + bytes_read);

    /* Set the segment's sequence number. */
    new_segment_ptr->ctcp_segment.seqno = htonl(state->tx_state.last_seqno_read + 1);

    /* Copy the data just readed into new segment */
    memcpy(new_segment_ptr->ctcp_segment.data, buf, bytes_read);

    /* Checksum */
    new_segment_ptr->ctcp_segment.cksum = 0;
    new_segment_ptr->ctcp_segment.cksum = cksum(&new_segment_ptr->ctcp_segment, ntohs(new_segment_ptr->ctcp_segment.len));

    /* Set last_seqno_read. Sequence numbers start at 1 */
    state->tx_state.last_seqno_read += bytes_read;

    /* Send cTCP segment to other side */
    ctcp_send_unwrapped_segment(state, new_segment_ptr);
    new_segment_ptr->num_xmits = 1;

    if (state->tx_state.last_seqno_sent == 0)
    {
      state->tx_state.last_seqno_sent = bytes_read;
    }

    /* Add sended cTCP segment to unacked */
    ll_add(state->tx_state.wrapped_unacked_segments, new_segment_ptr);
  }

  if (bytes_read == -1)
  {
    state->tx_state.has_EOF_been_read = true;

    /* Creat FIN segment to send. */
    new_segment_ptr = (wrapped_ctcp_segment_t *)calloc(1, sizeof(wrapped_ctcp_segment_t));
    if (NULL == new_segment_ptr)
      return;

    new_segment_ptr->ctcp_segment.len = htons((uint16_t)sizeof(ctcp_segment_t));
    new_segment_ptr->ctcp_segment.seqno = htonl(state->tx_state.last_seqno_read + 1);
    new_segment_ptr->ctcp_segment.flags |= TH_FIN;
    new_segment_ptr->ctcp_segment.cksum = 0;
    new_segment_ptr->ctcp_segment.cksum = cksum(&new_segment_ptr->ctcp_segment, ntohs(new_segment_ptr->ctcp_segment.len));

    ctcp_send_unwrapped_segment(state, new_segment_ptr);
    new_segment_ptr->num_xmits = 1;
  }
}

void ctcp_receive(ctcp_state_t *state, ctcp_segment_t *segment, size_t len)
{
   uint16_t num_data_bytes;

  /* If the segment was truncated, ignore it and hopefully retransmission will fix it. */
  if (len < ntohs(segment->len))
  {
    free(segment);
    return;
  }

  /* Check the checksum */
  uint16_t segment_cksum = segment->cksum;
  segment->cksum = 0;
  if (segment_cksum != cksum(segment, ntohs(segment->len)))
  {
    free(segment);
    return;
  }

  /* Check Ack number if receive package from receiver */
  if (ntohl(segment->seqno) == 0 && ntohl(segment->ackno) - state->tx_state.last_seqno_sent != 1)
  {
    free(segment);
    return;
  }

  /* Check if this process is receiver */
  if (ntohl(segment->seqno) != 0)
  {
    if (1 != ntohl(segment->seqno) - state->rx_state.last_seqno_accepted)
    {
      free(segment);
      return;
    }

    num_data_bytes = ntohs(segment->len) - sizeof(ctcp_segment_t);
    state->rx_state.last_seqno_accepted += num_data_bytes;

    ll_add(state->rx_state.segments_to_output, segment);
    ctcp_output(state);
  }

  /* Check if this process is sender */
  if (ntohl(segment->seqno) == 0)
  {
    state->tx_state.last_ackno_rxed = ntohl(segment->ackno);
    ll_node_t *segment_node = ll_front(state->tx_state.wrapped_unacked_segments);

    while (true)
    {
      wrapped_ctcp_segment_t *current_wrapped_segment = segment_node->object;
      if (current_wrapped_segment->ctcp_segment.seqno < state->tx_state.last_ackno_rxed)
      {
        free(current_wrapped_segment);
        ll_node_t *delete_node = segment_node;
        segment_node = segment_node->next;
        ll_remove(state->tx_state.wrapped_unacked_segments, delete_node);
        continue;
      }

      int bytes_read = ntohs(current_wrapped_segment->ctcp_segment.len) - sizeof(ctcp_segment_t);
      ctcp_send_unwrapped_segment(state, current_wrapped_segment);
      current_wrapped_segment->num_xmits = 1;
      state->tx_state.last_seqno_sent = current_wrapped_segment->ctcp_segment.seqno - 1 + bytes_read;
      break;
    }

    free(segment);
  }
}

void ctcp_output(ctcp_state_t *state)
{
  ll_node_t *front_node_ptr;
  ctcp_segment_t *ctcp_segment_ptr;
  int num_data_bytes;
  size_t bufspace;
  // int num_segments_output = 0;
  front_node_ptr = ll_front(state->rx_state.segments_to_output);
  ctcp_segment_ptr = (ctcp_segment_t *)front_node_ptr->object;
  num_data_bytes = ntohs(ctcp_segment_ptr->len) - sizeof(ctcp_segment_t);
  if (num_data_bytes)
  {
    // See if there's enough bufspace right now to output.
    bufspace = conn_bufspace(state->conn);
    if (bufspace < num_data_bytes)
      return;

    conn_output(state->conn, ctcp_segment_ptr->data, num_data_bytes);
    ll_remove(state->rx_state.segments_to_output, front_node_ptr);

    ctcp_send_control_segment(state);
  }

  // If this segment's FIN flag is set, output EOF by setting length to 0,
  // and update state.
  if ((!state->rx_state.has_FIN_been_rxed) && (ctcp_segment_ptr->flags & TH_FIN))
  {
    state->rx_state.has_FIN_been_rxed = true;
    state->rx_state.last_seqno_accepted++;
    conn_output(state->conn, ctcp_segment_ptr->data, 0);
  }
}

void ctcp_timer()
{
  ctcp_state_t * curr_state;

  if (state_list == NULL) return;
	for (curr_state = state_list; curr_state != NULL; curr_state = curr_state->next) {

    // ctcp_output(curr_state);

    ll_node_t *curr_segment;
    for ( curr_segment = ll_front(curr_state->tx_state.wrapped_unacked_segments) ; curr_segment != NULL ; curr_segment = curr_segment->next)
    {
      if (curr_segment->object == NULL)
        continue;
      
      wrapped_ctcp_segment_t *curr_unwrapped_segment = curr_segment->object;
      if (current_time() - curr_unwrapped_segment->timestamp_of_last_send >= MAX_SEG_LIFETIME_MS)
      {
        if (curr_unwrapped_segment->num_xmits == MAX_NUM_XMITS)
        {
          ctcp_destroy(curr_state);
        }
        ctcp_send_unwrapped_segment(curr_state, curr_unwrapped_segment);
        curr_unwrapped_segment->num_xmits += 1;
      }
    }
  }
}
