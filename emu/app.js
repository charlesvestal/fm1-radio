// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 Charles Vestal
// Radio's part of the emulator's page (fm1-base web/emu/index.html imports it): the network the FM-1 has over WiFi,
// done with fetch. Radio Garden answers only its own site, so its API goes through a small proxy (web/proxy/: a
// Cloudflare Worker), which also turns a station's listen link into its stream's address. The stream is fetched
// directly when its server allows it (most send Access-Control-Allow-Origin), else through the proxy. Its bytes
// go to the worklet (app_worklet.js) and the app decodes them as on the FM-1, effects and all.

const PROXY = new URLSearchParams(location.search).get("proxy") || "https://fm1-radio-proxy.charlesv-fb3.workers.dev";

// text as the FM-1 keeps it (net/radio_net.c fold_cp): ASCII, Latin letters with marks as the plain letter, the rest
// dropped; at most 31 characters (its fields are 32 bytes)
const FOLD = "AAAAAA\0CEEEEIIII\0NOOOOO\0\0UUUUY\0\0aaaaaa\0ceeeeiiii\0nooooo\0\0uuuuy\0y" +
  "AaAaAaCcCcCcCcDd\0\0EeEeEeEeEeGgGgGgGgHh\0\0IiIiIiIiI\0IiJjKk\0LlLlLlLl\0\0NnNnNnn\0\0OoOoOo\0\0RrRrRrSsSsSs" +
  "SsTtTt\0\0UuUuUuUuUuUuWwYyYZzZzZzs";
function fold(s) {
  let o = "";
  for (const ch of String(s || "").replace(/[\n\t\r]/g, " ")) {
    const cp = ch.codePointAt(0);
    const c = cp >= 32 && cp < 127 ? ch : cp >= 0xc0 && cp < 0x180 ? FOLD[cp - 0xc0] : "\0";
    if (c !== "\0") o += c;
  }
  return o.slice(0, 31);
}
const ieq = (a, b) => a.toLowerCase() === b.toLowerCase();
const lastSeg = (u) => u.split("/").filter(Boolean).pop() || "";

let node = null, say = () => {}, cur = null, placesP = null;
const post = (m, t) => node.port.postMessage({ type: "app", ...m }, t || []);

async function getJSON(path, signal) {
  const r = await fetch(PROXY + path, { signal });
  if (!r.ok) throw new Error(`http status ${r.status}`);
  return r.json();
}

// ---- the stream
async function stream(gen, url) {
  if (cur) cur.ctl.abort();
  cur = null;
  if (!url) return;
  const ctl = new AbortController(), signal = ctl.signal;
  cur = { gen, ctl };
  const end = (state, why) => { if (!signal.aborted) post({ op: "end", gen, state, why }); };
  try {
    let u = url;
    const m = url.match(/^https:\/\/radio\.garden\/api\/ara\/content\/listen\/([^/]+)\//);
    if (m) u = (await getJSON(`/listen/${encodeURIComponent(m[1])}`, signal)).url;
    if (!u) return end(3, "station not found");
    let res = null;
    if (u.startsWith("https://")) {               // straight from the station, if it lets this page read it
      try { res = await fetch(u, { signal, cache: "no-store" }); } catch (e) { if (signal.aborted) return; }
    }
    if (!res || !res.ok)
      res = await fetch(`${PROXY}/stream?u=${encodeURIComponent(u)}`, { signal, cache: "no-store" });
    if (!res.ok) return end(3, `http status ${res.status}`);
    const type = (res.headers.get("content-type") || "").toLowerCase(), has = (w) => type.includes(w);
    if (has("mp4") || has("mpegurl") || has("ogg") || has("html")) {   // as the FM-1 refuses them (radio_net.c)
      ctl.abort();
      return post({ op: "end", gen, state: 5,
                    why: `can't play ${has("mp4") ? "MP4" : has("mpegurl") ? "HLS" : has("ogg") ? "Ogg" : "a page"}` });
    }
    post({ op: "head", gen, aac: has("aac") && !has("mp4"), name: fold(res.headers.get("icy-name")).slice(0, 47) });
    const rd = res.body.getReader();
    for (;;) {
      const { done, value } = await rd.read();
      if (done) return end(4, "stream ended");
      post({ op: "data", gen, bytes: value }, [value.buffer]);
    }
  } catch (e) {
    if (!signal.aborted) end(3, /status/.test(e && e.message) ? e.message : "stream stalled");
  }
}

// ---- a place's stations: Radio Garden's search for the place, then its channels page (as radio_net.c rgn_task)
async function stations(gen, place, country) {
  try {
    const q = encodeURIComponent(`${place} ${country}`).replace(/%20/g, "+");
    const hits = ((await getJSON(`/api/search?q=${q}`)).hits?.hits || []).map((h) => h._source?.page)
      .filter((p) => p && typeof p.url === "string" && p.url.startsWith("/visit/"));
    const same = (p) => ieq(fold(p.title), place);
    const found = hits.find((p) => same(p) && (!p.subtitle || ieq(fold(p.subtitle), country))) || hits.find(same);
    if (!found) return post({ op: "stations", gen, ok: false, why: "place not found" });
    const list = [], seen = new Set();
    (function walk(o) {                           // every object with a /listen/ link, in the page's order
      if (!o || typeof o !== "object" || list.length >= 100) return;
      if (typeof o.url === "string" && o.url.startsWith("/listen/")) {
        const id = lastSeg(o.url);
        if (!seen.has(id)) { seen.add(id); list.push([id.slice(0, 11), fold(o.title) || id.slice(0, 11)]); }
      }
      for (const v of Object.values(o)) walk(v);
    })(await getJSON(`/api/ara/content/page/${encodeURIComponent(lastSeg(found.url))}/channels`));
    post({ op: "stations", gen, ok: true, list });
  } catch (e) {
    say("Radio Garden could not be reached: the places' stations need the network.");
    post({ op: "stations", gen, ok: false, why: "connection lost" });
  }
}

// ---- every place (as radio_net.c rgp_task), "title\0country\0" each, fetched once a session
async function places() {
  if (!placesP) placesP = (async () => {
    const r = await fetch(`${PROXY}/api/ara/content/places`);
    if (!r.ok) throw new Error(`http status ${r.status}`);
    const text = await r.text(), list = JSON.parse(text).data?.list || [];
    const out = [];
    for (const p of list)
      if (typeof p.url === "string" && p.url.startsWith("/visit/")) {
        const t = fold(p.title), c = fold(p.country);
        if (t && c) out.push(t, c);
      }
    const s = out.join("\0") + "\0", packed = new Uint8Array(s.length);
    for (let i = 0; i < s.length; i++) packed[i] = s.charCodeAt(i);
    return { packed, size: text.length };
  })();
  try {
    const { packed, size } = await placesP;
    post({ op: "places", packed: packed.slice(), size });
  } catch (e) {
    placesP = null;
    say("Radio Garden could not be reached, so the emulator has no places to browse.");
    post({ op: "places", packed: new Uint8Array(0), size: 0, why: "connection lost" });
  }
}

export function attach(o) {
  node = o.node;
  say = o.status || say;
}
export function message(m) {
  if (m.op === "stream") stream(m.gen, m.url);
  else if (m.op === "stations") stations(m.gen, m.place, m.country);
  else if (m.op === "places") places();
}
