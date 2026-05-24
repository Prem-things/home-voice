'use strict';
/*
 * SmartHomeAI — Node.js Backend (Gemini API edition)
 * ====================================================
 * AI engine  : Google Gemini (server-side — key never exposed to users)
 * Transport  : WebSocket relay between phone ↔ ESP32
 *
 * Flow:
 *   Phone opens QR URL → page loads from this server
 *   User speaks → text sent to POST /api/voice
 *   Server calls Gemini → gets JSON command
 *   Server pushes command to ESP32 via WebSocket
 *   ESP32 switches relay → sends status back → page updates
 */

const express   = require('express');
const http      = require('http');
const { WebSocketServer, WebSocket } = require('ws');
const { GoogleGenerativeAI }         = require('@google/generative-ai');
const path      = require('path');
require('dotenv').config();

// ── Validate config ─────────────────────────────────────────────
if (!process.env.GEMINI_API_KEY) {
    console.error('\n❌  GEMINI_API_KEY is not set in .env\n');
    process.exit(1);
}

// ── Init ────────────────────────────────────────────────────────
const app    = express();
const server = http.createServer(app);
const wss    = new WebSocketServer({ server, path: '/ws' });
const genAI  = new GoogleGenerativeAI(process.env.GEMINI_API_KEY);

const MODEL  = process.env.GEMINI_MODEL || 'gemini-2.5-flash';

app.use(express.json({ limit: '10kb' }));
app.use(express.static(path.join(__dirname, 'public')));

// ── Room store ──────────────────────────────────────────────────
//   roomId → { esp32: WebSocket|null, phones: Set<WebSocket>, state: {} }
const rooms = new Map();

function getRoom(roomId) {
    if (!rooms.has(roomId)) {
        rooms.set(roomId, {
            esp32  : null,
            phones : new Set(),
            state  : { bulb: false, fan: false, bed_light: false }
        });
    }
    return rooms.get(roomId);
}

function broadcastToPhones(room, payload) {
    const raw = typeof payload === 'string' ? payload : JSON.stringify(payload);
    room.phones.forEach(ws => ws.readyState === WebSocket.OPEN && ws.send(raw));
}

// ── System prompt ───────────────────────────────────────────────
const SYSTEM_PROMPT = `You are an AI smart home controller. Analyse the user's voice command and return ONLY valid JSON — no markdown, no explanation, nothing else.

DEVICES:
  "bulb"      — ceiling light / room light / main lamp
  "fan"       — ceiling fan / room fan / air cooler
  "bed_light" — bed lamp / reading light / night light / bedside lamp
  "all"       — every device simultaneously

ACTIONS: "on" | "off"

RESPONSE FORMATS — pick ONE:
  Single device  : {"device":"bulb","action":"on"}
  Multiple       : {"commands":[{"device":"fan","action":"on"},{"device":"bulb","action":"off"}]}
  Scene mode     : {"scene":"sleep"} | {"scene":"movie"} | {"scene":"reading"}
  Not understood : {"error":"unclear","msg":"one short sentence"}

SCENE DEFINITIONS:
  sleep   → all off
  movie   → bulb off, fan on, bed_light off
  reading → bulb off, fan on, bed_light on

INTENT MAPPING (infer freely):
  dark / dim / can't see / need light   → bulb on
  too bright / glare                    → bulb off
  hot / stuffy / sweating / need breeze → fan on
  cold / freezing / chilly              → fan off
  goodnight / bedtime / tired / sleep   → scene: sleep
  movie / film night                    → scene: movie
  reading / studying / working          → scene: reading
  everything / all lights               → device: all

LANGUAGES: Support all Indian languages — Hindi, Kannada, Tamil, Telugu,
Malayalam, Marathi, Gujarati, Punjabi, Bengali, English, and mixed code-switching.
Understand transliterated text (e.g. "fan band karo", "bulb on maadu").

RULES:
  1. Output ONLY a single JSON object. Nothing before or after it.
  2. No comments or explanations inside the JSON.
  3. Use exactly the device keys above.`;

// ── REST: Serve frontend ────────────────────────────────────────
app.get('/room/:roomId', (_req, res) => {
    res.sendFile(path.join(__dirname, 'public', 'index.html'));
});

// ── REST: Process voice command ─────────────────────────────────
app.post('/api/voice', async (req, res) => {
    const { text, roomId, history = [] } = req.body;

    if (!text?.trim() || !roomId) {
        return res.status(400).json({ error: 'text and roomId are required' });
    }

    console.log(`[AI] Room:${roomId}  Text: "${text}"`);

    try {
        // Build Gemini conversation history
        const geminiHistory = history.slice(-12).map(h => ({
            role  : h.role === 'assistant' ? 'model' : h.role,
            parts : [{ text: h.text }]
        }));

        const model = genAI.getGenerativeModel({
            model            : MODEL,
            systemInstruction: SYSTEM_PROMPT,
            generationConfig : {
                responseMimeType : 'application/json',
                temperature      : 0.1,
                maxOutputTokens  : 150
            }
        });

        const chat   = model.startChat({ history: geminiHistory });
        const result = await chat.sendMessage(text);
        const raw    = result.response.text();

        let cmd;
        try {
            cmd = JSON.parse(raw);
        } catch (_) {
            console.warn('[AI] Non-JSON response:', raw);
            cmd = { error: 'parse_failed', msg: 'Could not understand command' };
        }

        console.log(`[AI] Room:${roomId}  Result:`, JSON.stringify(cmd));

        // Push command to ESP32 via WebSocket relay
        const room = rooms.get(roomId);
        if (room?.esp32?.readyState === WebSocket.OPEN) {
            room.esp32.send(JSON.stringify(cmd));
        } else {
            console.warn(`[AI] Room:${roomId} — ESP32 not connected`);
        }

        res.json({ success: true, command: cmd, esp32Connected: !!room?.esp32 });

    } catch (e) {
        console.error('[Gemini Error]', e.message);

        let msg = e.message;
        if (e.status === 401 || e.status === 403) msg = 'Invalid Gemini API key — check .env file';
        if (e.status === 429) msg = 'Rate limit hit — wait a moment and try again';

        res.status(500).json({ error: msg });
    }
});

// ── REST: Get room state ────────────────────────────────────────
app.get('/api/status/:roomId', (req, res) => {
    const room = rooms.get(req.params.roomId);
    res.json(room
        ? { ...room.state, esp32Connected: !!room.esp32 }
        : { bulb: false, fan: false, bed_light: false, esp32Connected: false }
    );
});

// ── REST: Manual device command (card tap) ──────────────────────
app.post('/api/command/:roomId', (req, res) => {
    const room = rooms.get(req.params.roomId);
    if (!room?.esp32 || room.esp32.readyState !== WebSocket.OPEN) {
        return res.status(202).json({ queued: true, note: 'ESP32 not connected' });
    }
    room.esp32.send(JSON.stringify(req.body));
    res.json({ success: true });
});

// ── WebSocket relay ─────────────────────────────────────────────
wss.on('connection', (ws, req) => {
    let roomId, type;
    try {
        const params = new URL(req.url, 'http://localhost').searchParams;
        roomId = params.get('room');
        type   = params.get('type');   // 'esp32' | 'phone'
    } catch (_) {}

    if (!roomId) { ws.close(1008, 'Missing ?room= param'); return; }

    const room = getRoom(roomId);

    if (type === 'esp32') {
        if (room.esp32) room.esp32.terminate();
        room.esp32 = ws;
        console.log(`[WS] ESP32 connected   room=${roomId}`);
        ws.send(JSON.stringify({ type: 'ack', roomId }));
        broadcastToPhones(room, { ...room.state, esp32Connected: true });

        ws.on('message', raw => {
            try {
                const data = JSON.parse(raw);
                if ('bulb' in data) {
                    room.state = { bulb: !!data.bulb, fan: !!data.fan, bed_light: !!data.bed_light };
                    broadcastToPhones(room, { ...room.state, esp32Connected: true });
                }
            } catch (_) {}
        });

        ws.on('close', () => {
            room.esp32 = null;
            broadcastToPhones(room, { ...room.state, esp32Connected: false });
            console.log(`[WS] ESP32 disconnected  room=${roomId}`);
        });

        ws.on('error', e => console.error(`[WS] ESP32 error:`, e.message));

    } else {
        room.phones.add(ws);
        console.log(`[WS] Phone connected   room=${roomId}  (${room.phones.size} online)`);
        ws.send(JSON.stringify({ ...room.state, esp32Connected: !!room.esp32 }));
        ws.on('close',  () => room.phones.delete(ws));
        ws.on('error',  () => room.phones.delete(ws));
    }
});

// ── Health check ────────────────────────────────────────────────
app.get('/health', (_req, res) => {
    res.json({
        status : 'ok',
        engine : 'Google Gemini',
        model  : MODEL,
        rooms  : rooms.size,
        uptime : Math.floor(process.uptime())
    });
});

// ── Start ───────────────────────────────────────────────────────
const PORT = process.env.PORT || 3000;
server.listen(PORT, () => {
    console.log('\n══════════════════════════════════════════');
    console.log('   SmartHomeAI — Gemini Edition');
    console.log('══════════════════════════════════════════');
    console.log(`  Port   : ${PORT}`);
    console.log(`  Engine : Google Gemini`);
    console.log(`  Model  : ${MODEL}`);
    console.log(`  RoomID : ${process.env.ROOM_ID || 'HOME001'}`);
    console.log('══════════════════════════════════════════\n');
});

process.on('SIGTERM', () => server.close(() => process.exit(0)));
