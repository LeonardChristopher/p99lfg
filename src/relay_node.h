#ifndef P99LFG_RELAY_NODE_H
#define P99LFG_RELAY_NODE_H
/* Embedded Nostr relay ("host a node for your group"). A group member
 * can opt in to running a real relay on their own machine: the app
 * listens on a port, speaks NIP-01 (EVENT/REQ/CLOSE, OK verdicts),
 * stores #t p99lfg events, and broadcasts new events to every
 * connected member in real time. Members point their client at
 * ws://<host-machine>:<port>/ (added via the Join box relay list or
 * guild_relay) - zero third-party dependency, LAN-group friendly.
 *
 * Security posture: OPT-IN only, p99lfg-scoped (stores/forwards ONLY
 * events tagged #t p99lfg - it will not relay arbitrary Nostr
 * traffic), ring-buffer event store (bounded memory), max 8 clients.
 * The listener binds 0.0.0.0 by design - hosting for the group is the
 * point of the feature - so the Windows Firewall prompt is expected
 * and must be allowed. */
#include <stdint.h>

#define RELAY_NODE_STORE_MAX 512 /* events kept (ring) */
#define RELAY_NODE_CLIENTS_MAX 8

/* Start the node. port 0 = pick an ephemeral port (see
 * relay_node_port). Returns 0 on success. */
int relay_node_start(int port);

/* The port actually bound (valid after a successful start). */
int relay_node_port(void);

/* Stop the node and free the listener. */
void relay_node_stop(void);

/* Diagnostics: connected client count / stored event count. */
int relay_node_clients(void);
int relay_node_events(void);
#endif
