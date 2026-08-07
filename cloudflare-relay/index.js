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

  const body = await discordRes.text();
  return new Response(body, {
    status: 200,
    headers: {
      'Content-Type': 'application/json',
      Date: discordRes.headers.get('Date') || new Date().toUTCString(),
    },
  });
}
