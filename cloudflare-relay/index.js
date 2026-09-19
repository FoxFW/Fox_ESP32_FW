const DISCORD_API = 'https://discord.com/api/v10';
const POST_MIN_INTERVAL_MS = 2000;
const READ_MIN_INTERVAL_MS = 500;
const MAX_BODY_BYTES = 4096;
const READ_LIMIT_MAX = 10;

function checkAppKey(request, env) {
  return request.headers.get('X-App-Key') === env.APP_KEY;
}

async function rateLimited(env, ip, route, minIntervalMs) {
  if (!env.RATE_LIMIT) return false;
  const key = `cooldown:${route}:${ip}`;
  if (await env.RATE_LIMIT.get(key)) return true;
  await env.RATE_LIMIT.put(key, '1', { expirationTtl: Math.ceil(minIntervalMs / 1000) });
  return false;
}

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    const ip = request.headers.get('CF-Connecting-IP') || 'unknown';

    if (!checkAppKey(request, env)) {
      return new Response('Unauthorized', { status: 401 });
    }

    if (url.pathname === '/post' && request.method === 'POST') {
      return await handlePost(request, env, ip);
    }
    if (url.pathname === '/read' && request.method === 'GET') {
      return await handleRead(url, env, ip);
    }
    return new Response('Not found', { status: 404 });
  },
};

async function handlePost(request, env, ip) {
  if (await rateLimited(env, ip, 'post', POST_MIN_INTERVAL_MS)) {
    return new Response('Too many requests', { status: 429 });
  }

  const body = await request.text();
  if (body.length === 0 || body.length > MAX_BODY_BYTES) {
    return new Response('Bad request', { status: 400 });
  }

  const discordRes = await fetch(
    `${DISCORD_API}/channels/${env.DISCORD_CHANNEL_ID}/messages`,
    {
      method: 'POST',
      headers: {
        Authorization: `Bot ${env.DISCORD_BOT_TOKEN}`,
        'Content-Type': 'application/json',
      },
      body,
    },
  );

  return new Response(await discordRes.text(), { status: discordRes.status });
}

// Fox_ESP32_FW's discord.cpp only ever reads two fields out of each message
// object - "content" and "timestamp" (see jsonExtractString calls in
// doRead()) - but Discord's real API response for each message also
// includes id, channel_id, a full author{} object (id/username/avatar/
// discriminator/public_flags/bot/global_name), attachments[], embeds[],
// mentions[], mention_roles[], pinned, mention_everyone, tts,
// edited_timestamp, flags, components[], type. That's easily 400-600+
// bytes of pure overhead per message the ESP32 has to receive into RAM and
// hold in a String before throwing almost all of it away.
//
// S2 RAM/OOM investigation (2026-09-13, see claude/S2_RAM_OOM_ANALYSIS.md
// in the project docs): trimming the response to just what the firmware
// actually uses cuts the body significantly - real measured savings depend
// on message length, but for typical short messages this should take a
// multi-KB raw Discord response down to well under 1KB for a 5-10 message
// read. This does NOT touch the fixed TLS-handshake-overhead portion of
// the RAM cost (mbedTLS's own connection setup, which dominated the
// measured ~54KB trough independent of body size in the classic-hardware
// test) - treat this as a real, safe, easy win worth shipping, not a
// complete fix for the S2 heap-exhaustion risk on its own.
async function handleRead(url, env, ip) {
  if (await rateLimited(env, ip, 'read', READ_MIN_INTERVAL_MS)) {
    return new Response('Too many requests', { status: 429 });
  }

  let limit = parseInt(url.searchParams.get('limit') || '5', 10);
  if (!Number.isFinite(limit) || limit < 1) limit = 5;
  if (limit > READ_LIMIT_MAX) limit = READ_LIMIT_MAX;

  const discordRes = await fetch(
    `${DISCORD_API}/channels/${env.DISCORD_CHANNEL_ID}/messages?limit=${limit}`,
    { headers: { Authorization: `Bot ${env.DISCORD_BOT_TOKEN}` } },
  );

  if (discordRes.status !== 200) {
    return new Response(await discordRes.text(), { status: discordRes.status });
  }

  let trimmed;
  try {
    const messages = await discordRes.json();
    trimmed = messages.map((m) => ({ content: m.content, timestamp: m.timestamp }));
  } catch (e) {
    // If Discord ever changes shape underneath us, fail safe by forwarding
    // nothing rather than crashing the relay - discord.cpp treats an empty
    // array as zero messages (DISCORDREADDONE with no DISCORDMSG lines),
    // not an error.
    trimmed = [];
  }

  return new Response(JSON.stringify(trimmed), {
    status: 200,
    headers: {
      'Content-Type': 'application/json',
      Date: discordRes.headers.get('Date') || new Date().toUTCString(),
    },
  });
}
