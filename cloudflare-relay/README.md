# FoxChat Discord Relay

Small Cloudflare Worker (index.js) that holds the FoxFW Discord bot token
and channel ID server-side, so no FoxFW device (Flipper or ESP32) stores
the real Discord credentials.

Every FoxFW ESP32 talks to this relay instead of discord.com directly.

- POST /post - forwards the message JSON to Discord's channels/{id}/messages endpoint using the token held in the Worker.
- GET /read?limit=N - proxies Discord's recent-messages response (body + Date header) back unmodified.

Both routes require an X-App-Key header matching the Worker's APP_KEY
secret. This is not a high-value secret, it's baked into
Fox_ESP32_FW/config.h as FOXCHAT_RELAY_APP_KEY in plain text. Its only job
is to stop randoms who find the Worker URL from hitting it - the real
Discord bot token lives only in the Worker.

Deploy steps (Cloudflare dashboard, no CLI needed):

1. dash.cloudflare.com -> Workers & Pages -> Create -> Create Worker.
2. Name it (e.g. foxfw-chat-relay), deploy the default "Hello World".
3. Edit code, replace everything with index.js, save and deploy.
4. Settings -> Variables and Secrets -> Add secret, three times:
   - DISCORD_BOT_TOKEN - the real Discord bot token
   - DISCORD_CHANNEL_ID - the Discord channel ID
   - APP_KEY - any string, must match FOXCHAT_RELAY_APP_KEY in config.h
5. (Recommended) Settings -> Bindings -> KV Namespace -> create one named
   RATE_LIMIT and bind it under the variable name RATE_LIMIT. Without
   this, requests are only rate-limited by the ESP32 itself.
6. Copy the Worker's URL (https://name.subdomain.workers.dev) into
   FOXCHAT_RELAY_BASE_URL in config.h.

No CORS handling - this is only ever called by the ESP32 firmware's
HTTPClient, never by a browser.
