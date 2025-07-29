// rp.hpp
#ifndef RP_HPP
#define RP_HPP

#include <stdbool.h>
#include "contiki.h"
#include "net/rime/rime.h"
#include "net/netstack.h"
#include "core/net/linkaddr.h"

#ifndef CLOCK_LT
#define CLOCK_LT(a,b) ((int)((a)-(b)) < 0)
#endif

/*---------------------------------------------------------------------------*/
/*---------------------------------------------------------------------------*/

#define REPORT_DELAY_AFTER_PARENT_SWITCH (CLOCK_SECOND * 1) // now i dont use
#define BEACON_SILENT_LIMIT 20 * CLOCK_SECOND // max time node to be silent after parent switch(update)
/*---------------------------------------------------------------------------*/
// Cleanup old routes from the routing table
static const clock_time_t cleanup_interval = CLOCK_SECOND * 120; // bigger than beacon interval

/*---------------------------------------------------------------------------*/
#define TOPOLOGY_REPORT_INTERVAL (1500 * CLOCK_SECOND) // i dont use 
#define MAX_SUBTREE_SIZE 10 // Maximum number of nodes in the subtree

#define MAX_BUFFERED_REPORTS 7

struct topology_report {
  linkaddr_t node;
  uint16_t metric;
  uint16_t subtree_size;
  linkaddr_t subtree[MAX_SUBTREE_SIZE];
  //uint16_t seqn;

} __attribute__((packed));

struct pending_topology_reports {
  struct topology_report reports[MAX_BUFFERED_REPORTS];
  int count;
};

/*---------------------------------------------------------------------------*/
#define RSSI_THRESHOLD -95
#define MAX_PATH_LENGTH 10  // Maximum number of hops
/*---------------------------------------------------------------------------*/
/* for beacon */
#define MIN_PARENT_SWITCH_INTERVAL (40 * CLOCK_SECOND) // min 40 sec for one parent

#define BEACON_FORWARD_DELAY ((random_rand() % CLOCK_SECOND)) // random delay to avoid collisions when forwarding beacons
#define BEACON_INITIAL_INTERVAL (15 * CLOCK_SECOND) 
#define BEACON_MIN_INTERVAL     (10 * CLOCK_SECOND) 
#define BEACON_MAX_INTERVAL     (70 * CLOCK_SECOND)
#define STABILITY_THRESHOLD     3   // number of beacons to consider parent stable
extern clock_time_t current_beacon_interval;

/*---------------------------------------------------------------------------*/
/* types of routes */
typedef enum {
  ROUTE_TOPOLOGY = 0,
  ROUTE_PARENT = 1,
  ROUTE_NEIGHBOR = 2,
  ROUTE_SELF = 3
} route_type_t; // just to distinguish routes -- different from priority

/*---------------------------------------------------------------------------*/
/* Routing table */
typedef struct {
  linkaddr_t destination;
  linkaddr_t next_hop;
  route_type_t type;
  uint16_t metric;
  int16_t rssi;
  clock_time_t last_updated;
} routing_entry_t;

typedef struct routing_entry_node {
  routing_entry_t entry;
  struct routing_entry_node *next;
} routing_entry_node_t;

/*---------------------------------------------------------------------------*/
/* Callback structure */
struct rp_callbacks {
  void (* recv)(const linkaddr_t *src, uint8_t hops);
};

/* Connection data structure */
struct rp_conn {
  struct broadcast_conn bc;
  struct unicast_conn uc;
  const struct rp_callbacks* callbacks;

  linkaddr_t parent;
  uint8_t parent_stable_counter;
  clock_time_t last_parent_change;

  struct ctimer beacon_timer;
  clock_time_t current_beacon_interval;
  clock_time_t last_beacon_forward;

  uint16_t metric;
  uint16_t beacon_seqn;
  int16_t rssi;
  bool is_sink;
  uint8_t subtree_size;
  linkaddr_t subtree[MAX_SUBTREE_SIZE];
  struct ctimer cleanup_timer;
  struct ctimer report_timer;

  struct pending_topology_reports pending_reports;
  struct ctimer report_delay_timer;
  int report_timer_active;

  /*---------------------------------------------------------------------------*/
  /*
  --- here i tried to order the topology reports like to 
  --- process earlier reports first

  uint16_t report_seqn; */

  /* 
  --- this i tried to use for the parent changes
  --- idea: to find the mutual "ancestor" of the prev and new parent 
            and at this ancestor to stop the topology sending from the prev parent
  --- after tries the problem was with packetbuf

  bool should_send_topology;
  linkaddr_t new_parent_candidate;   */
  /*---------------------------------------------------------------------------*/

};
/*---------------------------------------------------------------------------*/
/* Functions */
void rp_open(
    struct rp_conn* conn, 
    uint16_t channels, 
    bool is_sink, 
    const struct rp_callbacks *callbacks);

int rp_send(struct rp_conn *c, const linkaddr_t *dest);

void bc_recv(struct broadcast_conn *conn, const linkaddr_t *sender);
void uc_recv(struct unicast_conn *c, const linkaddr_t *from);

void beacon_timer_cb(void* ptr);
void cleanup_timer_callback(void *ptr);

/*---------------------------------------------------------------------------*/
/* Functions to delete routes */
void delete_route_by_next_hop(struct rp_conn *conn, const linkaddr_t *next_hop, bool is_sink);
void delete_route(const linkaddr_t *destination, const linkaddr_t *next_hop) ;

/* Return priority of route types */
int route_priority(route_type_t t);

// to lookup a route in the routing table
routing_entry_t *lookup_route(const linkaddr_t *destination, bool is_sink);

// to add a route to the routing table
void add_route(struct rp_conn *conn, const linkaddr_t *destination, const linkaddr_t *next_hop, route_type_t type,
               uint16_t metric,
               int16_t rssi);
void add_new_route(struct rp_conn *conn, const linkaddr_t *destination, const linkaddr_t *next_hop, route_type_t type,
               uint16_t metric,
               int16_t rssi) ;

// to send a topology report
void send_topology_report( void *ptr, char* lol); 

// to update the routing table based on a received topology report
void update_routing_table(struct rp_conn *conn, const struct topology_report *report);

#endif // RP_HPP
