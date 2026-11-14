# Network assets

These are screenshots of the network surfaces. QR codes are **not** committed
because they encode a specific machine's LAN address.

To generate your own phone-player QR code after launching the app:

```bash
brew install qrencode
IP=$(ipconfig getifaddr en0)
qrencode -o network-output-qr.png "http://$IP:49800/"
open network-output-qr.png
```

The app serves the phone player on port `49800`; the landing page is `/`, and
`/health` is JSON status. See the top-level README for details.
