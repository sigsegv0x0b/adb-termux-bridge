#!/usr/bin/env node
/**
 * test.js — Connect to Chrome DevTools via termux-adb-bridge forward and control Chrome.
 *
 * Prerequisites:
 *   1. Bridge running: ./inject.sh or ./start.sh
 *   2. Forward set up: this script creates it automatically
 *   3. Chrome with debuggable WebView / remote debugging enabled
 *
 * Usage: node test.js [--url https://www.nofrills.com]
 */

const http = require('http');
const net = require('net');
const crypto = require('crypto');

const BRIDGE_HOST = '127.0.0.1';
const BRIDGE_PORT = 10099;
const CDP_PORT = 9223;
const UNIX_PATH = '@chrome_devtools_remote';
const DEFAULT_URL = 'https://www.nofrills.com';

// Certificate discovery — matches adb-termux.sh logic
const CERT_DIR = process.env.BRIDGE_CERT_DIR || process.env.HOME + '/.termux-adb-bridge/certs';
let CA_CRT, CLIENT_CRT, CLIENT_KEY;

function findCerts() {
    const fs = require('fs');
    const path = require('path');
    let certDir = CERT_DIR;

    // Look for fingerprint subdirectory first
    try {
        const dirs = fs.readdirSync(certDir);
        for (const d of dirs) {
            const full = path.join(certDir, d);
            if (fs.statSync(full).isDirectory()) {
                const ca = path.join(full, 'ca.crt');
                const cert = path.join(full, 'client.crt');
                const key = path.join(full, 'client.key');
                if (fs.existsSync(ca) && fs.existsSync(cert) && fs.existsSync(key)) {
                    CA_CRT = fs.readFileSync(ca);
                    CLIENT_CRT = fs.readFileSync(cert);
                    CLIENT_KEY = fs.readFileSync(key);
                    console.log('  Found certs in', full);
                    return;
                }
            }
        }
    } catch (e) {}

    // Fallback: direct cert dir
    const ca = path.join(certDir, 'ca.crt');
    const cert = path.join(certDir, 'client.crt');
    const key = path.join(certDir, 'client.key');
    if (fs.existsSync(ca) && fs.existsSync(cert) && fs.existsSync(key)) {
        CA_CRT = fs.readFileSync(ca);
        CLIENT_CRT = fs.readFileSync(cert);
        CLIENT_KEY = fs.readFileSync(key);
        console.log('  Found certs in', certDir);
        return;
    }

    console.error('Error: certificates not found in', certDir);
    console.error('Run inject.sh first to deploy the bridge.');
    process.exit(1);
}

function bridgeApi(method, path, body) {
    return new Promise((resolve, reject) => {
        const https = require('https');
        const options = {
            hostname: BRIDGE_HOST,
            port: BRIDGE_PORT,
            path: path,
            method: method,
            ca: CA_CRT,
            cert: CLIENT_CRT,
            key: CLIENT_KEY,
            rejectUnauthorized: false,
            agent: new https.Agent({ keepAlive: false }),
            headers: { 'Content-Type': 'application/json' }
        };
        if (body) options.headers['Content-Length'] = Buffer.byteLength(body);
        const req = https.request(options, (res) => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => resolve(data));
        });
        req.on('error', reject);
        if (body) req.write(body);
        req.end();
    });
}

async function setupForward() {
    console.log('[1/5] Setting up CDP forward...');
    const addResp = await bridgeApi('POST', '/api/tcpforward',
        JSON.stringify({
            unix_path: UNIX_PATH,
            host: '127.0.0.1',
            port: CDP_PORT
        }));
    console.log('  Forward:', addResp);
    const id = JSON.parse(addResp).id;

    // Cleanup on exit
    process.on('exit', () => {
        bridgeApi('DELETE', `/api/tcpforward?id=${id}`).catch(() => {});
    });
    process.on('SIGINT', () => {
        bridgeApi('DELETE', `/api/tcpforward?id=${id}`).then(() => process.exit());
    });
    process.on('SIGTERM', () => {
        bridgeApi('DELETE', `/api/tcpforward?id=${id}`).then(() => process.exit());
    });

    return id;
}

async function launchChrome(url) {
    console.log('[2/5] Opening Chrome with ' + url + '...');
    const resp = await bridgeApi('POST', '/api/exec',
        JSON.stringify({
            command: `am start -a android.intent.action.VIEW -d ${url} com.android.chrome`
        }));
    console.log('  Chrome:', JSON.parse(resp).stdout.trim());
}

function cdpGet(path) {
    return new Promise((resolve, reject) => {
        const req = http.get(`http://127.0.0.1:${CDP_PORT}${path}`, (res) => {
            let data = '';
            res.on('data', chunk => data += chunk);
            res.on('end', () => resolve(JSON.parse(data)));
        });
        req.on('error', reject);
        req.setTimeout(5000, () => { req.destroy(); reject(new Error('timeout')); });
    });
}

function wsConnect(url) {
    return new Promise((resolve, reject) => {
        const parsed = new URL(url);
        const key = crypto.randomBytes(16).toString('base64');
        const socket = net.createConnection({ host: '127.0.0.1', port: CDP_PORT }, () => {
            socket.write(
                `GET ${parsed.pathname} HTTP/1.1\r\n` +
                `Host: 127.0.0.1:${CDP_PORT}\r\n` +
                'Upgrade: websocket\r\n' +
                'Connection: Upgrade\r\n' +
                `Sec-WebSocket-Key: ${key}\r\n` +
                'Sec-WebSocket-Version: 13\r\n' +
                '\r\n'
            );
        });

        let buf = '';
        socket.on('data', (data) => {
            buf += data.toString();
            // Wait for WebSocket upgrade response (ends with \r\n\r\n)
            if (buf.includes('\r\n\r\n')) {
                const lines = buf.split('\r\n');
                if (lines[0] && lines[0].includes('101')) {
                    resolve(new CDPSocket(socket));
                } else {
                    socket.destroy();
                    reject(new Error('WebSocket upgrade failed: ' + lines[0]));
                }
            }
        });
        socket.on('error', reject);
        socket.setTimeout(5000, () => {
            socket.destroy();
            reject(new Error('WebSocket connect timeout'));
        });
    });
}

class CDPSocket {
    constructor(socket) {
        this.socket = socket;
        this.id = 0;
        this.callbacks = new Map();
        this.buffer = Buffer.alloc(0);
        this.socket.on('data', (data) => this._onData(data));
        this.socket.on('error', () => {});
    }

    _onData(data) {
        this.buffer = Buffer.concat([this.buffer, data]);
        while (this.buffer.length >= 2) {
            const opcode = this.buffer[0] & 0x0f;
            const masked = (this.buffer[1] & 0x80) !== 0;
            let payloadLen = this.buffer[1] & 0x7f;
            let offset = 2;

            if (payloadLen === 126) {
                if (this.buffer.length < 4) return;
                payloadLen = this.buffer.readUInt16BE(2);
                offset = 4;
            } else if (payloadLen === 127) {
                if (this.buffer.length < 10) return;
                payloadLen = Number(this.buffer.readBigUInt64BE(2));
                offset = 10;
            }

            const maskLen = masked ? 4 : 0;
            if (this.buffer.length < offset + maskLen + payloadLen) return;

            const mask = masked ? this.buffer.slice(offset, offset + 4) : null;
            offset += maskLen;
            let payload = this.buffer.slice(offset, offset + payloadLen);
            if (masked) {
                for (let i = 0; i < payload.length; i++) {
                    payload[i] ^= mask[i % 4];
                }
            }

            this.buffer = this.buffer.slice(offset + payloadLen);

            if (opcode === 1) { // Text frame
                const msg = JSON.parse(payload.toString());
                if (msg.id && this.callbacks.has(msg.id)) {
                    this.callbacks.get(msg.id)(msg);
                    this.callbacks.delete(msg.id);
                }
            } else if (opcode === 8) { // Close
                this.socket.destroy();
                return;
            }
        }
    }

    send(method, params) {
        const id = ++this.id;
        return new Promise((resolve) => {
            this.callbacks.set(id, resolve);
            const msg = JSON.stringify({ id, method, params });
            this._sendFrame(msg);
        });
    }

    _sendFrame(data) {
        const payload = Buffer.from(data, 'utf8');
        let frame;
        if (payload.length < 126) {
            frame = Buffer.alloc(2 + payload.length);
            frame[0] = 0x81; // FIN + Text
            frame[1] = payload.length;
            payload.copy(frame, 2);
        } else if (payload.length < 65536) {
            frame = Buffer.alloc(4 + payload.length);
            frame[0] = 0x81;
            frame[1] = 126;
            frame.writeUInt16BE(payload.length, 2);
            payload.copy(frame, 4);
        } else {
            frame = Buffer.alloc(10 + payload.length);
            frame[0] = 0x81;
            frame[1] = 127;
            frame.writeBigUInt64BE(BigInt(payload.length), 2);
            payload.copy(frame, 10);
        }
        this.socket.write(frame);
    }

    close() { this.socket.destroy(); }
}

async function getTargets() {
    console.log('[3/5] Getting CDP targets...');
    try {
        const targets = await cdpGet('/json');
        console.log('  Targets:', JSON.stringify(targets.map(t => ({
            title: t.title, url: t.url, type: t.type
        }))));
        return targets;
    } catch (e) {
        console.log('  /json failed:', e.message, '- trying /json/list');
        try {
            const targets = await cdpGet('/json/list');
            console.log('  Targets:', JSON.stringify(targets.map(t => ({
                title: t.title, url: t.url, type: t.type
            }))));
            return targets;
        } catch (e2) {
            console.error('  No targets found:', e2.message);
            console.error('  Chrome debugging may not be enabled on this device.');
            console.error('  Check: settings put global webview_debugging_enabled 1');
            process.exit(1);
        }
    }
}

async function controlBrowser(wsUrl, url) {
    console.log('[4/5] Connecting to page via CDP...');
    const cdp = await wsConnect(wsUrl);

    console.log('[5/5] Controlling browser...');

    // Enable Page domain
    await cdp.send('Page.enable');

    // Navigate to URL
    const navResult = await cdp.send('Page.navigate', { url });
    const frameId = navResult.result ? navResult.result.frameId : null;
    console.log('  Navigated to:', url, frameId ? `(frameId: ${frameId})` : '');

    // Wait for load
    await cdp.send('Page.loadEventFired');

    // Get page title
    const evalResult = await cdp.send('Runtime.evaluate', {
        expression: 'document.title',
        returnByValue: true
    });
    const title = evalResult.result?.result?.value || 'N/A';
    console.log('  Page title:', title);

    // Get the URL
    const urlResult = await cdp.send('Runtime.evaluate', {
        expression: 'window.location.href',
        returnByValue: true
    });
    const currentUrl = urlResult.result?.result?.value || 'N/A';
    console.log('  Current URL:', currentUrl);

    // Take a screenshot
    console.log('  Taking screenshot...');
    const screenshot = await cdp.send('Page.captureScreenshot', {
        format: 'jpeg',
        quality: 80
    });
    if (screenshot.result && screenshot.result.data) {
        const fs = require('fs');
        fs.writeFileSync('/tmp/chrome_screenshot.jpg', screenshot.result.data, 'base64');
        console.log('  Screenshot saved to /tmp/chrome_screenshot.jpg');
    }

    // Get page metrics
    const metrics = await cdp.send('Page.getLayoutMetrics');
    console.log('  Viewport:', JSON.stringify(metrics.result?.layoutViewport || {}));

    cdp.close();
    console.log('\n  Done!');
}

async function main() {
    findCerts();
    const url = process.argv[2] || DEFAULT_URL;

    try {
        await setupForward();
        await launchChrome(url);
        await new Promise(r => setTimeout(r, 3000)); // Wait for Chrome to load

        const targets = await getTargets();

        // Find a page target
        const pageTarget = targets.find(t => t.type === 'page' && t.webSocketDebuggerUrl);
        if (!pageTarget) {
            console.log('  No debuggable page found.');
            console.log('  Available targets:', JSON.stringify(targets.map(t => ({ type: t.type, url: t.url }))));
            process.exit(1);
        }

        await controlBrowser(pageTarget.webSocketDebuggerUrl, url);
    } catch (e) {
        console.error('Error:', e.message);
        process.exit(1);
    }
}

main();
