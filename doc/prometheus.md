# Prometheus exporter

Build with `make MACHINE=<profile> PROMETHEUS=1`. Without the flag the
firmware has no exporter code; the Web UI's Prometheus controls stay hidden. It
can be combined with `SNMP=1`; both use the same bounded hardware readers in
`snmp_hw.c`.

The exporter serves `GET /metrics` on its own TCP port, 9100, in the
Prometheus text format (version 0.0.4). It has its own connection, so a scrape
and the web interface can run at the same time. The web server on port 80 does
not serve `/metrics`. Metric names, types and labels follow
`prometheus-node-exporter-lua` on OpenWrt `realtek` switches, so dashboards and
alerting rules written for those switches also apply here.

## Turning it on

The exporter starts disabled. Turn on **Prometheus exporter** in **System → Services**,
or use the console or UART:

```
prometheus on
prometheus show
prometheus off
```

The change applies immediately. **Save to flash** keeps it after a reboot.
While the exporter is off, the switch resets connections to port 9100 once the
handshake completes; a request that arrives after `prometheus off` on an open
connection gets 404. Anything other than a `GET /metrics` request line in one
TCP segment returns 404, including other methods such as `HEAD`. The MIB
families are added when the query has a `mib=1` parameter; other query
parameters are ignored.

`/metrics` needs no login: Prometheus cannot hold a web session. Anyone who
can reach port 9100 can read the counters, port names, SFP
module details and hostname while the exporter is on. Restrict management
access accordingly.

## Scrape configuration

```yaml
scrape_configs:
  - job_name: rtlplayground
    static_configs:
      - targets: ["192.0.2.10:9100"]
  # Optional: the full ASIC MIB counter set, at a slower interval.
  - job_name: rtlplayground-mib
    scrape_interval: 5m
    params:
      mib: ["1"]
    static_configs:
      - targets: ["192.0.2.10:9100"]
```

## Metrics

Each port is labelled `device="lan<N>"`, where N is the front-panel port
number. The configured port name is reported only as `ifalias` in
`node_network_info`, so renaming a port does not start new series.

| Family | Notes |
|---|---|
| `node_uname_info` | `sysname="RTLPlayground"`, firmware version and build date, board name, hostname |
| `node_boot_time_seconds`, `node_time_seconds` | Only while NTP is synchronised |
| `node_hwmon_chip_names`, `node_hwmon_temp_celsius` | SoC sensor as `chip="thermal_thermal_zone0"`; SFP modules as `chip="sfp_lan<N>"` |
| `node_thermal_zone_temp` | SoC temperature, `type="cpu-thermal"`, `zone="0"` |
| `node_network_info` | `operstate` is `up`, `down` (PHY powered down) or `lowerlayerdown`; `address`, `ifalias` |
| `node_network_carrier` | 1 with link |
| `node_network_speed_bytes` | Omitted while the link is down |
| `node_network_receive_bytes_total`, `node_network_transmit_bytes_total` | Include the FCS |
| `node_network_receive_packets_total`, `node_network_transmit_packets_total` | Unicast + multicast + broadcast |
| `node_network_receive_multicast_total` | |
| `node_network_receive_errs_total`, `node_network_receive_frame_total` | RX CRC/alignment errors |
| `node_network_receive_drop_total` | Drop events |
| `node_network_transmit_drop_total` | Out discards |
| `node_network_transmit_errs_total`, `node_network_transmit_carrier_total` | Excessive plus late collisions |
| `node_network_transmit_colls_total` | Collisions |
| `rtlplayground_sfp_info` | `vendor`, `model`, `serial` of an inserted module |
| `rtlplayground_sfp_los` | Loss of signal, on boards with a LOS pin |
| `rtlplayground_sfp_voltage_volts`, `rtlplayground_sfp_tx_bias_amperes`, `rtlplayground_sfp_tx_power_watts`, `rtlplayground_sfp_rx_power_watts` | Digital diagnostics |
| `rtlplayground_sfp_external_calibration` | 1 for a module whose diagnostics are externally calibrated |

The error and drop mapping is the one the OpenWrt realtek DSA driver uses
(`otto_stats_update_link()`). Unlike that driver, byte counters include the
4-byte FCS and transmitted packets are not reduced by discards.

With a `mib=1` query parameter the scrape also contains every named ASIC MIB
counter as an `untyped` `node_ethtool_*` family, the counterpart of OpenWrt's ethtool
collector. Counters that exist in the OpenWrt RTL930x driver keep its names,
for example `node_ethtool_transmitted_crcalignerrors`; the others take their
Web UI name, for example `node_ethtool_received_ether_crc_align_errors`.

## Limits

- A hardware read that does not complete in its bounded time leaves that one
  sample out of the scrape. No value is invented.
- Digital diagnostics are reported only for internally calibrated modules.
  Externally calibrated modules report `rtlplayground_sfp_external_calibration 1`
  and no diagnostic values.
- `node_network_info` has no `duplex` label.
- The response is generated while it is sent, one buffer (one MSS) at a time,
  so a scrape is not a single snapshot.
- One scrape runs at a time. A second connection's SYN is dropped until the
  first connection is closed, and its client retries.
