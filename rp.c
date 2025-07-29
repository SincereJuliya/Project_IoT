#include <stddef.h>
#include "rp.h"
#include <stdio.h>
#include <stdlib.h>
#include <inttypes.h>
#include "lib/random.h" 
#include "sys/clock.h"
#include "sys/ctimer.h"

/*---------------------------------------------------------------------------*/
/* Initial values */
static routing_entry_node_t *routing_table = NULL;

/*---------------------------------------------------------------------------*/
// Function to print the routing table for debugging purposes
void print_routing_table(void) {
  routing_entry_node_t *current = routing_table;
  printf("RT [Node %02x:%02x] Routing Table:\n", linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1]);
  while (current != NULL) {
    printf("RT [Node %02x:%02x] Destination: %02x:%02x, Next Hop: %02x:%02x, Type: %d, Last Updated: %" PRIu32 "\n",
           linkaddr_node_addr.u8[0], linkaddr_node_addr.u8[1],
           current->entry.destination.u8[0], current->entry.destination.u8[1],
           current->entry.next_hop.u8[0], current->entry.next_hop.u8[1],
           current->entry.type, current->entry.last_updated);
    current = current->next;
  }
}

/*---------------------------------------------------------------------------*/
struct broadcast_callbacks bc_cb = {
  .recv = bc_recv,
  .sent = NULL
};
struct unicast_callbacks uc_cb = {
  .recv = uc_recv,
  .sent = NULL
};

/*---------------------------------------------------------------------------*/
void 
rp_open( struct rp_conn* conn, uint16_t channels, 
         bool is_sink, const struct rp_callbacks *callbacks) 
{
  linkaddr_copy(&conn->parent, &linkaddr_null);
  conn->metric = is_sink ? 0 : 65535;  // Sink = 0, others start with high metric
  conn->beacon_seqn = 0;
  conn->is_sink = is_sink;
  conn->callbacks = callbacks;

  // new! for the parent changes
  conn->last_parent_change = 0; 
  conn->parent_stable_counter = 0;

  broadcast_open(&conn->bc, channels, &bc_cb);
  unicast_open(&conn->uc, channels + 1, &uc_cb);

  /* Initialize timers */

  /* Beacon sending */
  if (conn->is_sink) 
  {
    conn->current_beacon_interval = BEACON_INITIAL_INTERVAL;
    ctimer_set(&conn->beacon_timer, conn->current_beacon_interval, beacon_timer_cb, conn);
  }

  /* Start route cleanup timer for both sink and non-sink */
            // I thought to call it when I work with the routing table in functions
            // , but I decided to call it periodically: should be less computations

  ctimer_set(&conn->cleanup_timer, cleanup_interval, cleanup_timer_callback, conn);

  /* Initialize subtree with self */
  conn->subtree_size = 1;
  linkaddr_copy(&conn->subtree[0], &linkaddr_node_addr);
  add_route(conn, &linkaddr_node_addr, &linkaddr_node_addr, ROUTE_SELF, 0, 0);
  
  /* Initialize toology report timer */
                // I decided to not use the timer for the reports
                // , instead i call it:
                //         when parent changes (i call it from the child)
                //         when a child is removed (from prev parent)
                //         when node receive a report
  /* if (!is_sink) {
      ctimer_set(&conn->report_timer, TOPOLOGY_REPORT_INTERVAL, send_topology_report, conn);
  }  */

}

/*---------------------------------------------------------------------------*/
/*                              Beacon Handling                              */
/*---------------------------------------------------------------------------*/
/* Beacon message structure */
struct beacon_msg {
  uint16_t seqn;
  uint16_t metric;
} __attribute__((packed));
/*---------------------------------------------------------------------------*/
/* Send beacon using the current seqn and metric */
void
send_beacon(struct rp_conn* c)
{
  /* Prepare the beacon message */
  struct beacon_msg beacon = {
    .seqn = c->beacon_seqn, 
    .metric = c->metric
  };

  /* Send the beacon message in broadcast */
  //packetbuf_clear();
  packetbuf_copyfrom(&beacon, sizeof(beacon));

  broadcast_send(&c->bc);
}
/*---------------------------------------------------------------------------*/
/* Beacon timer callback */
void
beacon_timer_cb(void* ptr)
{
  struct rp_conn *c = (struct rp_conn *)ptr;
  send_beacon(c);
  if (c->is_sink) 
  {
    c->beacon_seqn++; 
    ctimer_set(&c->beacon_timer, c->current_beacon_interval, beacon_timer_cb, c);
  } 

}
/*---------------------------------------------------------------------------*/
/* Send a remove child message to the parent */
struct child_msg {
  uint8_t type;
  linkaddr_t child; // The child to remove 
} __attribute__((packed));

/*---------------------------------------------------------------------------*/
/* Send a message to remove a child from the prev parent */
void send_remove_child(struct unicast_conn *uc, const linkaddr_t *to, const linkaddr_t *child_to_remove) {
  struct child_msg msg;
  memset(&msg, 0, sizeof(msg)); 
  msg.type = 0xA2; // REMOVE_CHILD -- this type bc with 0x01 i had problem with packets
  //linkaddr_copy(&msg.child, child_to_remove); 
  memcpy(&msg.child, child_to_remove, sizeof(linkaddr_t));

  if (packetbuf_copyfrom(&msg, sizeof(msg)) < sizeof(msg)) {
    return;
  }
  unicast_send(uc, to);
}

/* Send a message to add a child to the new parent */
void send_add_child(struct unicast_conn *uc, const linkaddr_t *to) {
  struct child_msg msg;
  memset(&msg, 0, sizeof(msg)); 
  msg.type = 0xA1; // ADD_CHILD
  //linkaddr_copy(&msg.child, &linkaddr_node_addr);
  memcpy(&msg.child, &linkaddr_node_addr, sizeof(linkaddr_t));

  if (packetbuf_copyfrom(&msg, sizeof(msg)) < sizeof(msg)) {
    return;
  }
  unicast_send(uc, to);
}

/*---------------------------------------------------------------------------*/
/* Check if a node is in the subtree - for the parent changing*/
bool is_in_subtree(struct rp_conn* conn, const linkaddr_t *node) {
  uint16_t i;
  for (i = 0; i < conn->subtree_size; i++) {
    if (linkaddr_cmp(node, &conn->subtree[i])) {
      return true;
    }
  }
  return false;
}

/*---------------------------------------------------------------------------*/
/* Beacon receive callback */
void
bc_recv(struct broadcast_conn *bc_conn, const linkaddr_t *sender)
{
  struct beacon_msg beacon;
  int16_t rssi;

  /* Get the pointer to the overall structure rp_conn from its field bc */
  struct rp_conn* conn = (struct rp_conn*)(((uint8_t*)bc_conn) - offsetof(struct rp_conn, bc));

  /* ------------------------------------------------------- */
  /* Check if the received broadcast packet looks legitimate */
  if (packetbuf_datalen() != sizeof(struct beacon_msg))
  {
    return;
  }

  memcpy(&beacon, packetbuf_dataptr(), sizeof(struct beacon_msg));
  bool parent_set = false;
  bool should_forward = false;

  /* ------------------------------------------------------- */
  /*                    evaluate a beacon                    */
  /* ------------------------------------------------------- */
  rssi = packetbuf_attr(PACKETBUF_ATTR_RSSI);
  if (rssi < RSSI_THRESHOLD || beacon.seqn < conn->beacon_seqn)
  { 
    return; // The beacon is either too weak or too old, ignore it
  }
  
  /* ------------------------------------------------------- */
  /*                    evaluate as a parent                 */
  /* ------------------------------------------------------- */
  if ( !conn->is_sink && beacon.seqn == conn->beacon_seqn ) 
  {
    if ( beacon.metric + 1 <= conn->metric )
    {
      if (!linkaddr_cmp(&conn->parent, sender) && !is_in_subtree(conn, sender) 
              && ( (clock_time() - conn->last_parent_change) > MIN_PARENT_SWITCH_INTERVAL || conn->last_parent_change == 0 ) ) 
      {
        if(!linkaddr_cmp(&conn->parent, &linkaddr_null)) 
        {
          send_remove_child(&conn->uc, &conn->parent, &linkaddr_node_addr); // send remove child message to the old parent
          delete_route(&conn->parent, &conn->parent); // delete the old parent route from RT
        }

        /* Memorize the new parent, the metric, and the seqn */
        linkaddr_copy(&conn->parent, sender);

        // to keep for a while one parent
        conn->last_parent_change = clock_time();

        // dynamic beacon interval
        conn->current_beacon_interval = BEACON_MIN_INTERVAL;  
        conn->last_beacon_forward = clock_time();
        conn->parent_stable_counter = 0;

        conn->metric = beacon.metric + 1;
        conn->beacon_seqn = beacon.seqn;
        conn->rssi = rssi;

        should_forward = true;

        add_route(conn, sender, sender, ROUTE_PARENT, beacon.metric + 1, rssi); // add new parent route to the RT

        if (!linkaddr_cmp(&conn->parent, &linkaddr_null)) {
          send_add_child(&conn->uc, sender); // send a message to the new parent to add this node as a child
        }

        send_topology_report(conn, "new parent"); // send a topology report to the new parent
        parent_set = true;

      }
      else 
      {
        conn->parent_stable_counter++;

        if (conn->parent_stable_counter >= STABILITY_THRESHOLD) 
        {
          conn->current_beacon_interval *= 2;
          if (conn->current_beacon_interval > BEACON_MAX_INTERVAL) conn->current_beacon_interval = BEACON_MAX_INTERVAL;
          conn->parent_stable_counter = 0;
        }
        parent_set = true;
        
        // if parent is so stable that it didn't change for a while
        if (clock_time() - conn->last_beacon_forward > BEACON_SILENT_LIMIT) 
        {
          should_forward = true;
          conn->last_beacon_forward = clock_time();

          send_topology_report(conn, "stable parent didnt change for a while"); // send a topology report to the parent

        }

      }

    }
  }
  /* ------------------------------------------------------- */
  /*                     add as a neigbor                    */
  /* ------------------------------------------------------- */
  if(!parent_set)
  { // add the neighbor route to the routing table if its not parent
    add_route(conn, sender, sender, ROUTE_NEIGHBOR, beacon.metric + 1, rssi); 
  }
  /* ------------------------------------------------------- */
  /* Schedule beacon propagation */
  ctimer_stop(&conn->beacon_timer);
  if (!conn->is_sink) 
  {
    if (should_forward) 
    {
      ctimer_set(&conn->beacon_timer, BEACON_FORWARD_DELAY, beacon_timer_cb, conn);
    } 
    else 
    {
      ctimer_set(&conn->beacon_timer, conn->current_beacon_interval, beacon_timer_cb, conn);
    }

  } 
  else 
  {
    ctimer_set(&conn->beacon_timer, conn->current_beacon_interval, beacon_timer_cb, conn);
  }
}

/*---------------------------------------------------------------------------*/
/*                               Data Handling                               */
/*---------------------------------------------------------------------------*/
/* Header structure for data packets */
struct collect_header {
  linkaddr_t source;
  linkaddr_t dest;
  uint8_t hops;
} __attribute__((packed));
/*---------------------------------------------------------------------------*/
typedef struct {
  uint16_t seqn;
} __attribute__((packed)) test_msg_t;

/*---------------------------------------------------------------------------*/
/* Data Collection: send function */
int
rp_send(struct rp_conn *conn, const linkaddr_t *dest)
{
  routing_entry_t *route = lookup_route(dest, conn->is_sink);

  if (route == NULL) {
    printf("rp_send: ERROR, route is null\n");
    return -1; // No route, cannot send
  } 

  struct collect_header hdr;
  memcpy(&hdr.source, &linkaddr_node_addr, sizeof(linkaddr_t));
  memcpy(&hdr.dest, dest, sizeof(linkaddr_t));
  hdr.hops = 0;

  if (packetbuf_hdralloc(sizeof(struct collect_header))) 
  {
    memcpy(packetbuf_hdrptr(), &hdr, sizeof(hdr));
    return unicast_send(&conn->uc, &route->next_hop); // send the packet to the next hop

  } else {
    printf("rp_send: ERROR, packet buffer too small for header\n");
    return -2;

  } 

}
/*---------------------------------------------------------------------------*/
/* Old - was a mistake in my impl - To modify the report to send to a parent*/
/* void
tr_modify(struct topology_report *report)
{
  memcpy(&report->node, &linkaddr_node_addr, sizeof(linkaddr_t));
  return;
} */
/*---------------------------------------------------------------------------*/
/* Applying pending topology reports */
static void
delayed_send_topology_report_cb(void *ptr)
{
  struct rp_conn *conn = (struct rp_conn *)ptr;
  conn->report_timer_active = 0;

  //printf("Applying %d pending topology reports\n", conn->pending_reports.count);

  uint16_t i;
  for ( i = 0; i < conn->pending_reports.count; ++i) 
  {
    update_routing_table(conn, &conn->pending_reports.reports[i]);
  }

  conn->pending_reports.count = 0;

  if (!conn->is_sink && !linkaddr_cmp(&conn->parent, &linkaddr_null)) 
  {
    send_topology_report(conn, "delayed batch");
  }
}

/*---------------------------------------------------------------------------*/
/* Node receives a Topology report */
void
tr_recv(struct rp_conn* conn)
{
  // my implementation before: each report is proceed independently 
  // actually PDR in cooja is more with this implementation

  /* 
  struct topology_report report;
  memcpy(&report, packetbuf_dataptr(), sizeof(report));

  update_routing_table(conn, &report);

  //If the node is a sink, it does not forward topology reports
  if (!conn->is_sink && !linkaddr_cmp(&conn->parent, &linkaddr_null)) 
  {
    //tr_modify(&report); // Modify the report if needed
    //packetbuf_copyfrom(&report, sizeof(report));  
    //unicast_send(&conn->uc, &conn->parent); 

    send_topology_report(conn, "after report from child"); // send a topology report to the parent

  } 
  return; */

  // i wanted to buffer the reports and send them in a batch
  struct topology_report report;
  memcpy(&report, packetbuf_dataptr(), sizeof(report));

  // буфер
  if (conn->pending_reports.count < MAX_BUFFERED_REPORTS) 
  {
    conn->pending_reports.reports[conn->pending_reports.count++] = report;
  } 
  else 
  {
    printf("tr_recv: Report buffer full, dropping report\n");
  }

  if (!conn->report_timer_active) 
  {
    conn->report_timer_active = 1;
    ctimer_set(&conn->report_delay_timer, CLOCK_SECOND * 6, delayed_send_topology_report_cb, conn);
  }
  
}

/*---------------------------------------------------------------------------*/
/* Add a child to the subtree */
void add_to_subtree(struct rp_conn *conn, const linkaddr_t *child) 
{
  uint16_t i;
  for ( i = 0; i < conn->subtree_size; i++) 
    if (linkaddr_cmp(&conn->subtree[i], child)) return;

  if (conn->subtree_size < MAX_SUBTREE_SIZE) 
  {
    memcpy(&conn->subtree[conn->subtree_size], child, sizeof(linkaddr_t));
    conn->subtree_size++;
  } 
  else printf("add_to_subtree: Subtree full, cannot add %02x:%02x\n", child->u8[0], child->u8[1]);
  
}
/* Remove from the subtree */
void remove_from_subtree(struct rp_conn *conn, const linkaddr_t *child) {
  uint16_t i;
  for (i = 0; i < conn->subtree_size; i++) {
    if (linkaddr_cmp(&conn->subtree[i], child)) {
      // Сдвигаем остальные элементы влево
      uint16_t j;
      for ( j = i; j < conn->subtree_size - 1; j++) {
        memcpy(&conn->subtree[j], &conn->subtree[j + 1], sizeof(linkaddr_t));
      }
      // Обнуляем последний
      memset(&conn->subtree[conn->subtree_size - 1], 0, sizeof(linkaddr_t));
      conn->subtree_size--;
      return;
    }
  }
}
/*---------------------------------------------------------------------------*/
/* Unicast receive callback */
void
uc_recv(struct unicast_conn *uc_conn, const linkaddr_t *from)
{
  /* Get the pointer to the overall structure rp_conn from its field uc */
  struct rp_conn* conn = (struct rp_conn*)(((uint8_t*)uc_conn) - offsetof(struct rp_conn, uc));

  /* ------------------------------------------------------ */
  /*             check if it is a child message             */
  /* ------------------------------------------------------ */
  if (packetbuf_datalen() == sizeof(struct child_msg)) 
  {
    struct child_msg msg;
    memset(&msg, 0, sizeof(msg));
    memcpy(&msg, packetbuf_dataptr(), sizeof(msg));

    linkaddr_t child_aligned;
    memcpy(&child_aligned, &msg.child, sizeof(linkaddr_t)); 

    switch (msg.type) {
      case 0xA1: // ADD_CHILD 
        add_route(conn, &child_aligned, from, ROUTE_TOPOLOGY, 100, -95);
        if(!conn->is_sink) add_to_subtree(conn, &child_aligned); 

        break;

      case 0xA2: // REMOVE_CHILD
        // im deleting from the prev parent subtree from this child
        delete_route_by_next_hop(conn, &child_aligned, conn->is_sink); // delete all subtree
        delete_route(&child_aligned, &child_aligned); // delete the route to the child
        // also add this child as a neighbor (for sure they are neighbors)
        add_route(conn, &child_aligned, from, ROUTE_NEIGHBOR, 100, -95); 

        send_topology_report(conn, "remove_child"); // send a topology report to the new parent

        break;

      default:
        break;
    }

    return;
  }

  /* ------------------------------------------------------ */
  /*             check if it is a topology report           */
  /* ------------------------------------------------------ */
  if (packetbuf_datalen() == sizeof(struct topology_report)) 
  {
    tr_recv(conn);
    return;
  }

  /* ------------------------------------------------------ */
  /*                check if it is a data packet            */
  /* ------------------------------------------------------ */
  if (packetbuf_datalen() < sizeof(struct collect_header)) 
  {
    printf("uc_recv: too short unicast packet %d\n", packetbuf_datalen());
    return;
  }

  struct collect_header hdr;
  memcpy(&hdr, packetbuf_dataptr(), sizeof(hdr));

  // Check hop count limit
  if(hdr.hops + 1 > MAX_PATH_LENGTH) 
  {
    printf("uc_recv: drop bc hop-limit exceeded (%d):\n", hdr.hops);
    return;
  }

  hdr.hops += 1; // Increment hop count
  memcpy(packetbuf_dataptr(), &hdr, sizeof(hdr));
  
  linkaddr_t tmp_dest;
  memcpy(&tmp_dest, &hdr.dest, sizeof(linkaddr_t));

  /* Am I a destination? */
  if(linkaddr_cmp(&linkaddr_node_addr, &tmp_dest)) 
  {
    if (packetbuf_hdrreduce(sizeof(struct collect_header))) 
    {
      if (packetbuf_datalen() != sizeof(test_msg_t)) 
      {
        return;
      }

      test_msg_t msg;
      memcpy(&msg, packetbuf_dataptr(), sizeof(msg));

      linkaddr_t tmp_src;
      memcpy(&tmp_src, &hdr.source, sizeof(linkaddr_t));
      conn->callbacks->recv(&tmp_src, hdr.hops);

    }
    else printf("uc_recv: Header reduction failed!\n");
    return;

  }
  else 
  { /* Im not a destination, lets forward it */

    /*Check where to send with searching in the routing table*/
    linkaddr_t tmp_dest;
    memcpy(&tmp_dest, &hdr.dest, sizeof(linkaddr_t));
    routing_entry_t *route = lookup_route(&tmp_dest, conn->is_sink);

    //print everything -- debugging
    /* print_routing_table();
    printf("uc_recv: Forwarding packet from hdr %02x:%02x -> %02x:%02x; from RT next-hop %02x:%02x -> dest %02x:%02x, hops %d\n",
           hdr.source.u8[0], hdr.source.u8[1], hdr.dest.u8[0], hdr.dest.u8[1],
           route->destination.u8[0], route->destination.u8[1],
           route->next_hop.u8[0], route->next_hop.u8[1], hdr.hops); */

    if (route == NULL) 
    {
      printf("uc_recv: ERROR, route is null\n");
      return; // No route, cannot send
    }

    unicast_send(&conn->uc, &route->next_hop);

    linkaddr_t tmp_src2;
    memcpy(&tmp_src2, &hdr.source, sizeof(linkaddr_t));
    
    return; 
  }

}
/*---------------------------------------------------------------------------*/
/*                              Route Management                             */
/*---------------------------------------------------------------------------*/
/* Return priority of route types */
int route_priority(route_type_t t) {
  switch (t) {
    case ROUTE_SELF: return 4;     // highest (never overwritten)
    case ROUTE_PARENT: return 3;
    case ROUTE_TOPOLOGY: return 2;
    case ROUTE_NEIGHBOR: return 1;
    default: return 0;
  }
}
/*---------------------------------------------------------------------------*/
/* Add or update a route in the routing table */
void add_route(struct rp_conn *conn, const linkaddr_t *destination, const linkaddr_t *next_hop, route_type_t type,
               uint16_t metric,
               int16_t rssi) 
{
  if (linkaddr_cmp(&linkaddr_node_addr, destination) && !linkaddr_cmp(&linkaddr_node_addr, next_hop)) 
  {
    if (type != ROUTE_SELF) return; // Only allow self route with ROUTE_SELF
  }

  routing_entry_node_t *current = routing_table;
  clock_time_t now = clock_time();

  while (current != NULL) 
  {
    if (linkaddr_cmp(&current->entry.destination, destination)) 
    {
      int new_prio = route_priority(type);
      int old_prio = route_priority(current->entry.type);

      bool same_nexthop = linkaddr_cmp(&current->entry.next_hop, next_hop);

      if (same_nexthop) 
      {
        if (new_prio >= old_prio /* || (new_prio == old_prio && rssi >= current->entry.rssi)  */)
        {
          current->entry.type = type;
          current->entry.metric = metric;
          current->entry.rssi = rssi;
        }
        current->entry.last_updated = now;
        return;

      } 
      else 
      {
        if (new_prio >= old_prio /* || (new_prio == old_prio && rssi >= current->entry.rssi)  */) 
        {
          linkaddr_copy(&current->entry.next_hop, next_hop);
          current->entry.type = type;
          current->entry.metric = metric;
          current->entry.rssi = rssi;
          current->entry.last_updated = now;
          return;

        } 
        else 
        {
          break;
        }
      }
      break;
    }

    current = current->next;
  }

  // route not found - create new
  add_new_route(conn, destination, next_hop, type, metric, rssi);

} 
/*---------------------------------------------------------------------------*/
/*  Adding a new route */
void add_new_route(struct rp_conn *conn, const linkaddr_t *destination, const linkaddr_t *next_hop, route_type_t type,
               uint16_t metric,
               int16_t rssi) 
{
  clock_time_t now = clock_time();
  routing_entry_node_t *node = (routing_entry_node_t *)malloc(sizeof(routing_entry_node_t));

  if (node == NULL) {
    printf("add_route: ERROR - Out of memory adding route to %02x:%02x\n", destination->u8[0], destination->u8[1]);
    return;
  }
  
  linkaddr_copy(&node->entry.destination, destination);
  linkaddr_copy(&node->entry.next_hop, next_hop);
  node->entry.last_updated = now;
  node->entry.type = type; 
  node->entry.metric = metric; 
  node->entry.rssi = rssi;
  
  node->next = routing_table;
  routing_table = node;
}

/*---------------------------------------------------------------------------*/
/* Lookup a route in the routing table */
routing_entry_t 
*lookup_route(const linkaddr_t *destination, bool is_sink) 
{
  routing_entry_node_t *current = routing_table;

  // 1. Try to find a direct match
  while (current != NULL) 
  {
    if (linkaddr_cmp(&current->entry.destination, destination)) 
    {
      return &current->entry;
    }
    current = current->next;
  }

  // 2. Fallback: look for parent
  if(!is_sink){
    routing_entry_node_t *parent = routing_table;
    while (parent != NULL) 
    {
      if (parent->entry.type == 1 || parent->entry.type == ROUTE_PARENT) return &parent->entry;
      parent = parent->next;

    }
    
  }

  // 3. No route found
  //printf("lookup_route: No route found for destination %02x:%02x\n", destination->u8[0], destination->u8[1]);
  return NULL;
}
/*---------------------------------------------------------------------------*/
/* Purge old routes from the routing table */
void 
purge_old_routes(struct rp_conn *conn) 
{
  routing_entry_node_t *prev = NULL;
  routing_entry_node_t *current = routing_table;
  clock_time_t now = clock_time();  

  while (current != NULL) {
    routing_entry_node_t *next = current->next;

    if (  CLOCK_LT(current->entry.last_updated + cleanup_interval, now) 
           && current->entry.type != 3 && current->entry.type != 1 
           && current->entry.type != ROUTE_PARENT && current->entry.type != ROUTE_SELF // Do not purge self route and parent
       )  
    { 
      if (prev == NULL) 
      {
        routing_table = next;
      } 
      else 
      {
        prev->next = next;
      }
      
      remove_from_subtree(conn, &current->entry.destination);
      free(current);  // Free memory

    } 
    else 
    {
      prev = current;
    }

    current = next;  // Move to next route
  }
}

/*---------------------------------------------------------------------------*/
//void func to delete route by destination and next hop
void delete_route(const linkaddr_t *destination, const linkaddr_t *next_hop) 
{
  routing_entry_node_t *current = routing_table;
  routing_entry_node_t *prev = NULL;

  while (current != NULL) {
    if (linkaddr_cmp(&current->entry.destination, destination) && 
        linkaddr_cmp(&current->entry.next_hop, next_hop)) 
    {
      if (prev == NULL) 
      {
        routing_table = current->next; // Remove from head
      } 
      else 
      {
        prev->next = current->next; // Remove from middle or end
      }

      free(current); // Free memory
      return;

    }
    prev = current;
    current = current->next;
  }

}
/*---------------------------------------------------------------------------*/
// Function to delete routes by next hop
void delete_route_by_next_hop(struct rp_conn *conn, const linkaddr_t *next_hop, bool is_sink) 
{
  routing_entry_node_t *current = routing_table;
  routing_entry_node_t *prev = NULL;

  while (current != NULL) {
    routing_entry_node_t *next = current->next;  // Save next before potentially freeing
    if (linkaddr_cmp(&current->entry.next_hop, next_hop)) 
    {

      if (is_sink && current->entry.type == 0) {
        prev = current; // Do not delete self route in sink
        current = next;
        continue;
      }

      if (prev == NULL) 
      {
        routing_table = next; // Remove head
      } 
      else 
      {
        prev->next = next; // Bypass current
      }
      
      // delete destination from subtree
      remove_from_subtree(conn, &current->entry.destination);

      free(current);

    } 
    else 
    {
      prev = current;
    }
    current = next;

  }

}
/*---------------------------------------------------------------------------*/
/* Cleanup timer callback */
void 
cleanup_timer_callback(void *ptr) 
{
  struct rp_conn *conn = (struct rp_conn *)ptr;
  if(!conn->is_sink) purge_old_routes(conn);
  ctimer_reset(&conn->cleanup_timer);  // Reschedule the timer
} 
/*---------------------------------------------------------------------------*/
/* Sending topology reports */
void
send_topology_report(void *ptr, char* lol) 
{
  struct rp_conn *conn = (struct rp_conn *)ptr;
  if (conn->is_sink) return; // Sink doesn't send reports

  struct topology_report report;
  memset(&report, 0, sizeof(report));  // clear garbage

  // Fill the report with current node and metric
  memcpy(&report.node, &linkaddr_node_addr, sizeof(linkaddr_t));
  report.metric = conn->metric; 

  uint8_t subtree_index = 0; 

  routing_entry_node_t *current = routing_table;

  while (current != NULL) 
  {
    const linkaddr_t *dest = &current->entry.destination;

    if 
    ( subtree_index < MAX_SUBTREE_SIZE && !linkaddr_cmp(dest, &linkaddr_null) 
      && ( current->entry.type == ROUTE_TOPOLOGY || current->entry.type == ROUTE_SELF ) // only add subtree
    )
    { 
      printf("debug: [%s] send_topology_report: Adding subtree node %02x:%02x \n", lol, dest->u8[0], dest->u8[1]);

      memcpy(&report.subtree[subtree_index], dest, sizeof(linkaddr_t));
      subtree_index++;
    }
    current = current->next;
    
  }

  // Set the subtree size in the report
  report.subtree_size = subtree_index; 

  //packetbuf_clear();
  packetbuf_copyfrom(&report, sizeof(struct topology_report));

  // Send the report to the parent
  if (!linkaddr_cmp(&conn->parent, &linkaddr_null)) 
  { 
    unicast_send(&conn->uc, &conn->parent);
  } 
  
  // Reschedule the next report -- now I do not use timer for reports
  //ctimer_set(&conn->report_timer, TOPOLOGY_REPORT_INTERVAL, send_topology_report, conn);
}

/*---------------------------------------------------------------------------*/
/*  To update RT from the report */
void 
update_routing_table(struct rp_conn *conn, const struct topology_report *report) 
{
  // i want to delete all routes which has next hop as the node in the report
  linkaddr_t node_aligned;
  memcpy(&node_aligned, &report->node, sizeof(linkaddr_t));

  delete_route_by_next_hop(conn, &node_aligned, conn->is_sink);
  add_route(conn, &node_aligned, &node_aligned, ROUTE_TOPOLOGY, report->metric, -95);

  // Add routes from the report
  uint8_t i;
  for (i = 0; i < report->subtree_size && i < MAX_SUBTREE_SIZE; i++) 
  {
    linkaddr_t tmp_addr;
    memcpy(&tmp_addr, &report->subtree[i], sizeof(linkaddr_t));
    if(!linkaddr_cmp(&tmp_addr, &linkaddr_null))
    {
      linkaddr_t tmp_parent;
      memcpy(&tmp_parent, &report->node, sizeof(linkaddr_t));
      add_route(conn, &tmp_addr, &tmp_parent, ROUTE_TOPOLOGY, report->metric + 1, -95); // dummy RSSI

    }
  }

  // Add subtree nodes to the connection's subtree
  for (i = 0; i < report->subtree_size && conn->subtree_size < MAX_SUBTREE_SIZE; i++) 
  {
    //const linkaddr_t *node = &report->subtree[i];
    linkaddr_t node;
    memcpy(&node, &report->subtree[i], sizeof(linkaddr_t));


    if (!linkaddr_cmp(&node, &linkaddr_null)) 
    { 
      bool already_present = false;
      uint16_t j;
      for (  j = 0; j < conn->subtree_size; j++) 
      {
        if (linkaddr_cmp(&conn->subtree[j], &node)) 
        {
          already_present = true;
          break;

        }
      }
 
      if (!already_present && conn->subtree_size < MAX_SUBTREE_SIZE) 
      {
        memcpy(&conn->subtree[conn->subtree_size], &node, sizeof(linkaddr_t));
        conn->subtree_size++;

      }
    }
  }

}
