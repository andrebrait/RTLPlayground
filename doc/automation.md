# Automation

## Upload

You can automate upload of the firmware via WEB with curl:

1. Authorize with /login endpoint and save cookie:

   ```bash
   curl -c cookies.txt  http://${SWITCH_IP}/login -d pwd=${PASSWORD} -i
   ```

   This will save session cookie in cookies.txt
2. Send the firmware via form:

    ```bash
    curl -b cookies.txt http://${SWITCH_IP}/upload -F "uploadedfile=@${FIRMWARE_FILE_PATH}" -i
    ```

    You can expect that server will close connection, without responding to request.
    Wait for SWITCH_IP to be responding again.

## Port status

In similar way to upload, you can fetch the configuration and state of the
ports, and their link and counters. [The HTTP API](api.md) lists every endpoint.

1. Get the session cookie as for upload.
2. Hit `/ports.json` or `/traffic.json` with the cookie:

    ```bash
    curl -b cookies.txt http://${SWITCH_IP}/ports.json
    curl -b cookies.txt http://${SWITCH_IP}/traffic.json
    ```
