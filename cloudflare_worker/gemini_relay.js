// FoxFW Gemini Relay — Cloudflare Worker
//
// Holds a Google Gemini API key server-side so no FoxFW user's ESP32 ever
// needs (or sees) a real API key. Every "Fox AI Chat" request goes through
// this Worker instead of straight to Google.
//
// ── Deploy (no CLI needed) ──────────────────────────────────────────────
//   1. dash.cloudflare.com -> Workers & Pages -> Create -> Create Worker
//   2. Name it (e.g. foxfw-gemini-relay), deploy the default "Hello World"
//   3. Edit code -> replace everything with this file -> Save and deploy
//   4. Settings -> Variables and Secrets -> Add secret:
//        name: GEMINI_API_KEY
//        value: a Gemini API key from https://ai.google.dev/gemini-api/docs/api-key
//               (create it under the foxcustomfirmware@gmail.com account)
//   5. Settings -> Variables and Secrets -> Add secret:
//        name: APP_KEY
//        value: any string you choose - must match GEMINI_RELAY_APP_KEY in
//               Fox_ESP32_FW/config.h exactly. This isn't a real secret
//               (it ships inside the firmware), it just keeps casual
//               scanners from finding this URL and hammering it directly.
//   6. Settings -> Bindings -> KV Namespace -> create one named
//      RATE_LIMIT and bind it under the variable name RATE_LIMIT.
//      Without this bound, the Worker still works but has no per-IP
//      cooldown - don't skip this step for a public relay.
//   7. Copy the Worker's URL (https://<name>.<your-subdomain>.workers.dev)
//      into Fox_ESP32_FW/config.h's GEMINI_RELAY_BASE_URL.
//
// ── Why this exists ───────────────────────────────────────────────────────
// Fox AI Chat used to ask each user for their own Gemini API key. This
// Worker replaces that with one shared key (yours), the same pattern the
// Discord relay (discord.cpp / FOXCHAT_RELAY_BASE_URL) already uses for
// Fox Chat. The ESP32 also enforces its own client-side cooldown before
// ever reaching this Worker (see gemini.cpp) - the KV rate limit here is
// the backstop that protects your quota even from a modified client.

const ALLOWED_MODEL = 'gemini-flash-latest';
const GEMINI_URL = `https://generativelanguage.googleapis.com/v1beta/models/${ALLOWED_MODEL}:generateContent`;
const COOLDOWN_SECONDS = 60; // per-IP

function json(data, status = 200) {
  return new Response(JSON.stringify(data), {
    status,
    headers: { 'Content-Type': 'application/json' },
  });
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (url.pathname !== '/ask' || request.method !== 'POST') {
      return json({ error: 'Not found' }, 404);
    }

    if (env.APP_KEY && request.headers.get('X-App-Key') !== env.APP_KEY) {
      return json({ error: 'Forbidden' }, 403);
    }

    const ip = request.headers.get('CF-Connecting-IP') || 'unknown';
    if (env.RATE_LIMIT) {
      const key = `cooldown:${ip}`;
      if (await env.RATE_LIMIT.get(key)) {
        return json({ error: 'Rate limited - try again shortly.' }, 429);
      }
      await env.RATE_LIMIT.put(key, '1', { expirationTtl: COOLDOWN_SECONDS });
    }

    let body;
    try {
      body = await request.json();
    } catch {
      return json({ error: 'Invalid JSON body' }, 400);
    }

    const prompt = (body.prompt || '').toString();
    if (!prompt) return json({ error: 'prompt is required' }, 400);

    const geminiRes = await fetch(`${GEMINI_URL}?key=${env.GEMINI_API_KEY}`, {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ contents: [{ parts: [{ text: prompt }] }] }),
    });

    if (!geminiRes.ok) {
      const errBody = await geminiRes.text();
      console.error(`Gemini API error ${geminiRes.status}: ${errBody}`);
      return json({ error: `Gemini error ${geminiRes.status}: ${errBody.slice(0, 200)}` }, geminiRes.status);
    }

    const data = await geminiRes.json();
    const blockReason = data && data.promptFeedback && data.promptFeedback.blockReason;
    if (blockReason) {
      return json({ blockReason });
    }

    const text =
      data &&
      data.candidates &&
      data.candidates[0] &&
      data.candidates[0].content &&
      data.candidates[0].content.parts &&
      data.candidates[0].content.parts[0] &&
      data.candidates[0].content.parts[0].text;

    if (!text) return json({ error: 'No reply text in Gemini response' }, 502);

    return json({ text });
  },
};
