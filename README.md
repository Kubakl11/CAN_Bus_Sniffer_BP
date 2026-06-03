# CAN Bus Sniffer — Bachelor Thesis - Jakub Klášterka

> **Design and Implementation of a System for CAN Bus Data Analysis in Automotive Monitoring**
>
> Czech Technical University in Prague · Faculty of Electrical Engineering · 2025

---

## About

This project implements a **complete vehicle telemetry pipeline** — from reading OBD-II data off the car's CAN bus, through cellular transmission, to a live web dashboard.

```
   Vehicle CAN Bus (500 kbit/s)
       │
       ▼  OBD-II
  ┌─────────────────────────┐
  │  ESP32-S3 + MCP2518FD   │  ← Custom 4-layer PCB
  │  + A7670E LTE/GPS       │
  └────────┬────────────────┘
           │  HTTPS (LTE)
           ▼
  ┌─────────────────────────┐
  │  Flask Backend           │  ← Railway.app
  │  (REST API + SQLite)     │
  └────────┬────────────────┘
           │  fetch() @ 5 Hz
           ▼
  ┌─────────────────────────┐
  │  Web Dashboard           │  ← Chart.js + Leaflet
  │  (Vanilla HTML/CSS/JS)   │
  └─────────────────────────┘
```

---

## Hardware

| Component | Role |
|---|---|
| **ESP32-S3-MINI-1-N8** | Main MCU (dual-core, 240 MHz) |
| **MCP2518FD** | CAN controller (SPI, CAN 2.0B @ 500 kbit/s) |
| **ATA6563** | CAN transceiver (3.3 V logic / 5 V bus) |
| **SIMCom A7670E** | LTE Cat 1 modem + GNSS (GPS/GLONASS) |

PCB designed in **KiCad**,

---

## Firmware
- **CAN library:** ACAN2517FD
- **OBD-II:** PID discovery (SCAN) + cyclic reading (LOOP) at 150 ms
- **LTE:** AT command driver for A7670E — HTTPS POST with Bearer token + nonce
- **GPS:** NMEA parsing via `AT+CGNSSINFO`
- **WiFi:** SoftAP + captive portal for local configuration



## Backend

Flask app deployed on **Railway.app**.

| Endpoint | Method | Description |
|---|---|---|
| `/data` | `POST` | ESP sends OBD-II JSON (Bearer + nonce auth) |
| `/data` | `GET` | Dashboard reads latest telemetry |
| `/command` | `POST` | Queue command for ESP (SCAN / LOOP / STOP) |

**Security:** HTTPS · Bearer token · nonce replay protection

---

## Dashboard
Written using **HTML / CSS / JavaScript**

| Page | What it shows |
|---|---|
|  `dashboard.html` | Cockpit — gauges (RPM, speed), mini charts, alarms |
|  `vehicle-data.html` | Full PID table with live values |
|  `graphs.html` | Time-series charts (Chart.js) |
|  `alarms.html` | Configurable alarm thresholds |
|  `consumption.html` | Fuel consumption analytics |
|  `map.html` | Live GPS track (Leaflet + Mapbox) |
|  `diagnostic.html` | CAN frame builder, live monitor, UDS, serial console |

**Design:** Dark theme (navy + blue accent), JetBrains Mono for numerics, responsive layout.

`data.js` serves as the **single integration point** 

---

## Quick Start

```bash
# Firmware (PlatformIO)
pio run -e obd_reader -t upload
pio device monitor

# Backend (local)
pip install -r requirements.txt
flask run

# Dashboard
# Open dashboard.html with Live Server
```

---

## 📝 Thesis

Written in **LaTeX** using the `ctuthesis` template. The thesis covers a review of automotive communication buses (CAN, LIN, FlexRay, Ethernet), the OBD-II diagnostic standard, hardware design, firmware implementation, server architecture, and the web dashboard.

---

<p align="center">
  <sub>Jakub Klášterka · CTU FEE Prague · 2025</sub>
</p>