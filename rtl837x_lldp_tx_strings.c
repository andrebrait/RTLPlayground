/* Keep the existing banked TX text helpers out of the full RX bank. */
#pragma codeseg BANK2
#pragma constseg BANK2

#include "rtl837x_common.h"
#include "rtl837x_lldp.h"
#include "dhcp.h"
#include "machine.h"

extern __code const struct machine machine;

#define LLDP_TYPE(value) (value << 1)

uint8_t lldp_sysname(__xdata uint8_t *p) __banked
{
    uint8_t hostname_length = strlen_x(hostname);

    *p++ = LLDP_TYPE(LLDP_SYSNAME_TLV_TYPE);
    *p++ = hostname_length;
    memcpy(p, hostname, hostname_length);
    return hostname_length + 2;
}

uint8_t lldp_sysdesc(__xdata uint8_t *p) __banked
{
    uint16_t machine_name_length = strlen(machine.machine_name);

    *p++ = LLDP_TYPE(LLDP_SYSDESC_TLV_TYPE);
    *p++ = machine_name_length;
    memcpyc(p, machine.machine_name, machine_name_length);
    return machine_name_length + 2;
}
