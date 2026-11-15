# Troubleshooting

## The board hotspot is visible but no input arrives

Open `/api/status` on the board first. If `udp_rx` remains zero, check that the desktop is sending to the displayed IP and port 4210. If `udp_rx` increases while `hid_mounted` is false, move the ESP32 USB cable to the controlled computer's native USB port and select USB-OTG/HID in the board menu.

## STA credentials are wrong

Connect to the generated `AI-KVM-<MAC>` AP and send a new `WIFI,STA,<ssid>,<password>` command over USB serial. The firmware restarts and retries the new credentials. Credentials are stored only in NVS.

## MCP cannot connect

Use the named pipe first. The optional TCP MCP listener is loopback-only by default. Check `status`, then `logs`; do not expose the listener to a public network without an authenticated reverse proxy.
