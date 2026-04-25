# dtmc_linux

[![CI](https://github.com/david-erb/dtmc_linux/actions/workflows/ci.yml/badge.svg)](https://github.com/david-erb/dtmc_linux/actions/workflows/ci.yml) [![Coverage](https://codecov.io/gh/david-erb/dtmc_linux/branch/main/graph/badge.svg)](https://codecov.io/gh/david-erb/dtmc_linux) [![Version](https://img.shields.io/github/v/tag/david-erb/dtmc_linux)](https://github.com/david-erb/dtmc_linux/tags) [![Platforms](https://img.shields.io/badge/platform-Linux-lightgrey)](https://david-erb.github.io/dtmc_linux/)

Linux implementations of the `dtmc_base` porting contracts — CAN bus, Modbus RTU/TCP, TCP sockets, TTY, WebSocket, CoAP, MQTT, HTTP, SDL display, and file-backed NVS — plus a set of demo and service applications that exercise each one.

`dtmc_linux` is part of the **[Embedded Applications Lab](https://david-erb.github.io/embedded)** — a set of working applications based on a set of reusable libraries across MCU, Linux, and RTOS targets.

---

## What's inside

| Area | Headers |
|------|---------|
| Serial / CAN | `dtiox_linux_tty`, `dtiox_linux_canbus` |
| TCP / WebSocket | `dtiox_linux_tcp`, `dtiox_linux_websocket` |
| Modbus RTU / TCP | `dtiox_linux_modbus_rtu_master`, `dtiox_linux_modbus_tcp_master`, `dtiox_linux_modbus_tcp_slave` |
| Networking | `dtnetportal_mosquitto`, `dtnetportal_coap` |
| HTTP | `dthttpd_linux_socket` |
| Display | `dtdisplay_linux_sdl` |
| Storage | `dtnvblob_linux_file` |
| Utilities | `dtsha1` |

---

## Demo and service applications

| App | What it shows |
|-----|--------------|
| `demo_iox_tty` / `demo_iox_canbus` | TTY and CAN I/O streams |
| `demo_iox_tcp` / `demo_iox_modbus` | TCP socket and Modbus I/O |
| `demo_netportal_modbus` / `demo_netportal_tty` | Netportal over Modbus and TTY transports |
| `demo_netportal_mosquitto` / `demo_netportal_coap` | Netportal over MQTT and CoAP |
| `benchmark_netportal_simplex_modbus` / `benchmark_netportal_duplex_modbus` | Netportal throughput benchmarks |
| `demo_display_hello` / `demo_lvgl_card` / `hello_sdl2` | SDL2 display and LVGL widget demos |
| `demo_write_kvp` | File-backed NVS key-value persistence |
| `run_test_coap` / `run_test_mosquitto` / `run_test_matter` | Protocol integration test runners |
| `test_all` | Full test suite |

---

## Dependencies

`dtmc_linux` depends on `dtcore`, `dtmc_base`, and `dtmc_services`, which are included as git submodules under `submodules/`. After cloning, initialize them with:

```sh
git submodule update --init --recursive
```

---

## Docs

See the [dtmc_linux documentation site](https://david-erb.github.io/dtmc_linux/) for the full API reference.
