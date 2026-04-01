import { ref } from 'vue'

// SHA256 implementation for token generation
// Based on https://github.com/davidtgur/sha256-js
function sha256(str) {
  const k = [
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
    0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
    0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
    0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
    0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
    0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
    0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2
  ]

  const w = new Uint32Array(64)
  const h = new Uint32Array([0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19])

  const buf = new TextEncoder().encode(str)
  const len = buf.length
  const paddedLen = len + 1 + 8 + ((64 - ((len + 1 + 8) % 64)) % 64)
  const msg = new Uint8Array(paddedLen)
  msg.set(buf)
  msg[len] = 0x80
  const view = new DataView(msg.buffer)
  view.setUint32(paddedLen - 4, len * 8, false)

  for (let chunk = 0; chunk < paddedLen / 64; chunk++) {
    for (let i = 0; i < 16; i++) {
      w[i] = view.getUint32(chunk * 64 + i * 4, false)
    }
    for (let i = 16; i < 64; i++) {
      const s0 = ((w[i-15] >>> 7) | (w[i-15] << 25)) ^ ((w[i-15] >>> 18) | (w[i-15] << 14)) ^ (w[i-15] >>> 3)
      const s1 = ((w[i-2] >>> 17) | (w[i-2] << 15)) ^ ((w[i-2] >>> 19) | (w[i-2] << 13)) ^ (w[i-2] >>> 10)
      w[i] = (w[i-16] + s0 + w[i-7] + s1) >>> 0
    }

    let [a, b, c, d, e, f, g, hh] = h

    for (let i = 0; i < 64; i++) {
      const S1 = ((e >>> 6) | (e << 26)) ^ ((e >>> 11) | (e << 21)) ^ ((e >>> 25) | (e << 7))
      const ch = (e & f) ^ (~e & g)
      const t1 = (hh + S1 + ch + k[i] + w[i]) >>> 0
      const S0 = ((a >>> 2) | (a << 30)) ^ ((a >>> 13) | (a << 19)) ^ ((a >>> 22) | (a << 10))
      const maj = (a & b) ^ (a & c) ^ (b & c)
      const t2 = (S0 + maj) >>> 0
      hh = g; g = f; f = e; e = (d + t1) >>> 0
      d = c; c = b; b = a; a = (t1 + t2) >>> 0
    }

    h[0] = (h[0] + a) >>> 0; h[1] = (h[1] + b) >>> 0
    h[2] = (h[2] + c) >>> 0; h[3] = (h[3] + d) >>> 0
    h[4] = (h[4] + e) >>> 0; h[5] = (h[5] + f) >>> 0
    h[6] = (h[6] + g) >>> 0; h[7] = (h[7] + hh) >>> 0
  }

  const hex = []
  for (let i = 0; i < 8; i++) {
    hex.push(h[i].toString(16).padStart(8, '0'))
  }
  return hex.join('')
}

// Auth state
const secret = ref('')
const requireAuth = ref(false)
const authStream = ref(true)
let onAuthFailure = null

// Register callback for when auth fails (401 response)
export function onAuthError(cb) {
  onAuthFailure = cb
}

// Compute token: SHA256(secret + ":" + timestamp)
export function computeToken() {
  if (!secret.value) return ''
  const timestamp = Math.floor(Date.now() / 300000) // 5-minute window
  return sha256(secret.value + ':' + timestamp)
}

// API wrapper with auth
export async function api(method, path, body) {
  const opts = { method }
  opts.headers = {}
  if (body) {
    opts.headers['Content-Type'] = 'application/json'
    opts.body = JSON.stringify(body)
  }
  // Add auth token if needed
  if (requireAuth.value && secret.value) {
    opts.headers['Authorization'] = `Bearer ${computeToken()}`
  }
  const r = await fetch(path, opts)
  const data = await r.json()
  if (r.status === 401) {
    requireAuth.value = true
    clearSecret()
    if (onAuthFailure) onAuthFailure()
  }
  return data
}

// Get stream URL with token (for img src)
export function getStreamUrl() {
  if (requireAuth.value && authStream.value && secret.value) {
    return `/stream/mjpeg?token=${computeToken()}`
  }
  return '/stream/mjpeg'
}

// Check auth status on startup
export async function checkAuth() {
  try {
    const r = await fetch('/api/auth-info')
    const data = await r.json()
    requireAuth.value = data.require_auth === true
    if (requireAuth.value) {
      authStream.value = data.auth_stream !== false
    }
    return data
  } catch {
    return { require_auth: false }
  }
}

// Verify secret against server, returns { ok, error }
export async function verifySecret(s) {
  secret.value = s
  try {
    const token = computeToken()
    const r = await fetch('/api/auth/verify', {
      method: 'POST',
      headers: { 'Authorization': `Bearer ${token}` }
    })
    const data = await r.json()
    if (data.ok) {
      sessionStorage.setItem('fajita_secret', s)
      return { ok: true }
    }
    secret.value = ''
    return { ok: false, error: '密钥无效' }
  } catch {
    secret.value = ''
    return { ok: false, error: '无法连接服务器' }
  }
}

// Set secret directly (used for restoring from sessionStorage)
export function setSecret(s) {
  secret.value = s
}

// Try to restore secret from sessionStorage
export function restoreSecret() {
  const saved = sessionStorage.getItem('fajita_secret')
  if (saved) {
    secret.value = saved
    return true
  }
  return false
}

// Clear secret (on auth failure)
export function clearSecret() {
  secret.value = ''
  sessionStorage.removeItem('fajita_secret')
}

// Auth state exports
export const needsAuth = requireAuth
export const streamNeedsAuth = authStream
export const hasSecret = () => secret.value.length > 0

export const getCameras = () => api('GET', '/api/cameras')
export const getStatus = () => api('GET', '/api/status')
export const selectCamera = (index) => api('POST', '/api/camera/select', { index })
export const setControl = (data) => api('POST', '/api/control', data)
export const setFocus = (data) => api('POST', '/api/focus', data)
export const setAWB = (data) => api('POST', '/api/awb', data)
export const setAE = (data) => api('POST', '/api/ae', data)
export const setStream = (data) => api('POST', '/api/stream', data)