// SPDX-License-Identifier: MPL-2.0
// Copyright (C) 2026 Charles Vestal
// Radio's part of the emulator's worklet (fm1-base web/emu/worklet.js loads it first). The app asks for the network
// through generation numbers (app.c, PLAT_WEB): a stream, a place's stations, the list of places. This passes each
// request to the page (app.js), which fetches, and hands what comes back to the app: the stream's bytes into its
// ring as there is room, the stations, the places a batch at a time.

const BATCH = 16384;                              // the places, bytes a quantum

function cstr(f, p) {
  const m = new Uint8Array(f.mem.buffer, p);
  let s = "";
  for (let i = 0; m[i]; i++) s += String.fromCharCode(m[i]);
  return s;
}
function toScratch(f, strs) {                     // ASCII strings, each with its NUL
  const max = f.ex.web_scratch_size(), m = new Uint8Array(f.mem.buffer, f.ex.web_net_scratch(), max);
  let n = 0;
  for (const s of strs) {
    for (let i = 0; i < s.length && n + 1 < max; i++) m[n++] = s.charCodeAt(i) & 127;
    m[n++] = 0;
  }
}
const post = (f, m) => f.port.postMessage({ type: "app", ...m });

globalThis.fm1App = {
  load(f) {
    f.radio = { net: f.ex.web_net_gen(), rgn: f.ex.web_rgn_gen(), rgp: f.ex.web_rgp_gen(), q: [], off: 0,
                places: null, size: 0, at: -1 };
  },

  message(f, m) {
    const ex = f.ex, r = f.radio;
    if (m.op === "head" && m.gen === r.net) {
      toScratch(f, [m.name || ""]);
      ex.web_net_head(m.gen, m.aac ? 1 : 0);
    } else if (m.op === "data" && m.gen === r.net) {
      if (r.q.length < 256) r.q.push(m.bytes);   // (a live stream comes in at its own pace: no more than a burst)
    } else if (m.op === "end" && m.gen === r.net) {
      r.q.push({ end: m.state, why: m.why });     // after what is still queued
    } else if (m.op === "stations" && m.gen === ex.web_rgn_gen()) {
      for (const [id, title] of m.list || []) {
        toScratch(f, [id, title]);
        ex.web_rgn_add(m.gen);
      }
      toScratch(f, [m.why || ""]);
      ex.web_rgn_end(m.gen, m.ok ? 1 : 0);
    } else if (m.op === "places") {
      r.places = m.packed;                        // "title\0country\0"..., kept for every pass the app makes
      r.size = m.size;
      r.why = m.why;
      if (ex.web_rgp_busy()) r.at = 0;
    }
  },

  process(f) {
    const ex = f.ex, r = f.radio;
    const ng = ex.web_net_gen();
    if (ng !== r.net) {                           // a stream wanted, or let go
      r.net = ng;
      r.q = [];
      r.off = 0;
      post(f, { op: "stream", gen: ng, url: ex.web_net_want() ? cstr(f, ex.web_net_url()) : null });
    }
    const gg = ex.web_rgn_gen();
    if (gg !== r.rgn) {
      r.rgn = gg;
      if (ex.web_rgn_busy())
        post(f, { op: "stations", gen: gg, place: cstr(f, ex.web_rgn_place()), country: cstr(f, ex.web_rgn_country()) });
    }
    const pg = ex.web_rgp_gen();
    if (pg !== r.rgp) {
      r.rgp = pg;
      r.at = r.places ? 0 : -1;
      if (!r.places) post(f, { op: "places" });
    }

    // the stream: into the ring while it has room
    while (r.q.length) {
      const c = r.q[0];
      if (c.end !== undefined) {
        toScratch(f, [c.why || ""]);
        ex.web_net_end(r.net, c.end);
        r.q = [];
        break;
      }
      const room = ex.web_net_room();
      if (!room) break;
      const n = Math.min(c.length - r.off, room, ex.web_scratch_size());
      new Uint8Array(f.mem.buffer, ex.web_net_scratch(), n).set(c.subarray(r.off, r.off + n));
      r.off += ex.web_net_put(r.net, n);
      if (r.off >= c.length) { r.q.shift(); r.off = 0; }
    }

    // the places: a batch, ending on a whole pair
    if (r.at >= 0 && r.places && ex.web_rgp_busy()) {
      const p = r.places;
      if (!p.length) {
        toScratch(f, [r.why || "no places"]);
        ex.web_rgp_end(pg, 0);
        r.at = -1;
        return;
      }
      let end = Math.min(r.at + BATCH, p.length);
      if (end < p.length) {
        let z = 0, i = r.at;
        for (let k = r.at; k < end; k++) if (!p[k] && ++z % 2 === 0) i = k + 1;
        end = i;
      }
      new Uint8Array(f.mem.buffer, ex.web_net_scratch(), end - r.at).set(p.subarray(r.at, end));
      ex.web_rgp_feed(pg, end - r.at, Math.round((end - r.at) * r.size / p.length));
      r.at = end;
      if (r.at >= p.length) {
        ex.web_rgp_end(pg, 1);
        r.at = -1;
      }
    }
  },
};
