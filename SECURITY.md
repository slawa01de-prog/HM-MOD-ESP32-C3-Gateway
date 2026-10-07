# Security

The current gateway WebUI does **not** implement authentication.

Recommended deployment:

- run only on a trusted local network;
- do not expose HTTP port 80 or UDP port 3008 directly to the Internet;
- use a router DHCP reservation for a stable gateway IP;
- do not publish Wi-Fi credentials, NVS dumps or device-specific secrets;
- stop OpenCCU before using the HM-MOD firmware updater.

If remote access is required, use an authenticated VPN instead of direct port
forwarding.
