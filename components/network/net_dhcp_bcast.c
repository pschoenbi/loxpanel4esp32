/*
 * DHCP-Antworten als Broadcast anfordern (RFC 2131, Flag-Bit 15).
 *
 * Hintergrund: Antwortet der DHCP-Server direkt adressiert (an die angebotene, noch nicht
 * vergebene IP), kommt OFFER/ACK auf diesem Board nicht beim P4 an; Broadcasts schon.
 * Beobachtet mit UniFi nach einem Router-Neustart. lwIP setzt das Flag nie, daher wird
 * die IDF-Hook-Funktion dhcp_append_extra_opts per Linker (--wrap) erweitert.
 */

#include <stdint.h>
#include "lwip/dhcp.h"
#include "lwip/prot/dhcp.h"

#define DHCP_FLAG_BROADCAST_BIT 0x8000

void __real_dhcp_append_extra_opts(struct netif *netif, uint8_t state, struct dhcp_msg *msg_out,
                                   uint16_t *options_out_len);

void __wrap_dhcp_append_extra_opts(struct netif *netif, uint8_t state, struct dhcp_msg *msg_out,
                                   uint16_t *options_out_len)
{
    __real_dhcp_append_extra_opts(netif, state, msg_out, options_out_len);
    /* Nur solange noch keine Adresse gilt; Verlängerungen laufen normal per Unicast */
    if (state == DHCP_STATE_SELECTING || state == DHCP_STATE_REQUESTING ||
        state == DHCP_STATE_REBOOTING || state == DHCP_STATE_REBINDING) {
        msg_out->flags = PP_HTONS(DHCP_FLAG_BROADCAST_BIT);
    }
}
