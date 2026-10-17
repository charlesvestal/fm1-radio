// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 Charles Vestal
// Radio's emulator proxy, a Cloudflare Worker (wrangler deploy, in this directory). Radio Garden's API answers only
// its own site, so the emulator's page (../emu/app.js) asks here:
//   GET /api/...             Radio Garden's API (search, a place's channels, the places), passed through
//   GET /listen/ID           {"url": the station's stream}: where Radio Garden's listen link redirects
//   GET /stream?u=URL        a stream whose server the page cannot read itself (no CORS, or plain http), passed
//                            through when it is audio
// Only for the emulator's pages (ORIGINS); nothing is kept.

const ORIGINS = [/^https:\/\/charlesvestal\.github\.io$/, /^https?:\/\/(localhost|127\.0\.0\.1)(:\d+)?$/];
const RG = "https://radio.garden";
const UA = "FM-1 Radio emulator (+https://github.com/charlesvestal/fm1-radio)";

function cors(req, h = new Headers()) {
  const o = req.headers.get("Origin") || "";
  if (ORIGINS.some((r) => r.test(o))) {
    h.set("Access-Control-Allow-Origin", o);
    h.set("Access-Control-Expose-Headers", "icy-name, content-type");
    h.set("Vary", "Origin");
  }
  return h;
}
const fail = (req, status, msg) => new Response(msg, { status, headers: cors(req) });

export default {
  async fetch(req) {
    const url = new URL(req.url), origin = req.headers.get("Origin") || "";
    if (req.method === "OPTIONS") return new Response(null, { headers: cors(req) });
    if (req.method !== "GET") return fail(req, 405, "GET only");
    if (origin && !ORIGINS.some((r) => r.test(origin))) return fail(req, 403, "not for this site");

    if (url.pathname.startsWith("/api/")) {
      const places = url.pathname === "/api/ara/content/places";
      const r = await fetch(RG + url.pathname + url.search, {
        headers: { "User-Agent": UA, Accept: "application/json" },
        cf: { cacheTtl: places ? 86400 : 3600, cacheEverything: true },
      });
      const h = cors(req);
      h.set("Content-Type", r.headers.get("Content-Type") || "application/json");
      h.set("Cache-Control", `public, max-age=${places ? 86400 : 600}`);
      return new Response(r.body, { status: r.status, headers: h });
    }

    const m = url.pathname.match(/^\/listen\/([A-Za-z0-9_-]{1,16})$/);
    if (m) {
      const r = await fetch(`${RG}/api/ara/content/listen/${m[1]}/channel.mp3`,
                            { headers: { "User-Agent": UA }, redirect: "manual" });
      const loc = r.headers.get("Location");
      if (!loc) return fail(req, 404, "station not found");
      const h = cors(req);
      h.set("Content-Type", "application/json");
      h.set("Cache-Control", "public, max-age=600");
      return new Response(JSON.stringify({ url: new URL(loc, RG).href }), { headers: h });
    }

    if (url.pathname === "/stream") {
      let u;
      try { u = new URL(url.searchParams.get("u") || ""); } catch { return fail(req, 400, "no url"); }
      if (u.protocol !== "http:" && u.protocol !== "https:") return fail(req, 400, "http(s) only");
      const r = await fetch(u.href, { headers: { "User-Agent": UA, "Icy-MetaData": "0" }, signal: req.signal });
      const type = (r.headers.get("Content-Type") || "").toLowerCase();
      if (!/^(audio\/|application\/(ogg|octet-stream|x-mpegurl|vnd\.apple\.mpegurl))/.test(type)) {
        r.body?.cancel();
        return fail(req, r.ok ? 415 : r.status, r.ok ? "not audio" : `upstream ${r.status}`);
      }
      const h = cors(req);
      h.set("Content-Type", type);
      h.set("Cache-Control", "no-store");
      if (r.headers.get("icy-name")) h.set("icy-name", r.headers.get("icy-name"));
      return new Response(r.body, { status: r.status, headers: h });
    }
    return fail(req, 404, "not here");
  },
};
