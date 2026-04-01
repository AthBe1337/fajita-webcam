export async function api(method, path, body) {
  const opts = { method }
  if (body) {
    opts.headers = { 'Content-Type': 'application/json' }
    opts.body = JSON.stringify(body)
  }
  const r = await fetch(path, opts)
  return r.json()
}

export const getCameras = () => api('GET', '/api/cameras')
export const getStatus = () => api('GET', '/api/status')
export const selectCamera = (index) => api('POST', '/api/camera/select', { index })
export const setControl = (data) => api('POST', '/api/control', data)
export const setFocus = (data) => api('POST', '/api/focus', data)
export const setAWB = (data) => api('POST', '/api/awb', data)
export const setAE = (data) => api('POST', '/api/ae', data)
export const setStream = (data) => api('POST', '/api/stream', data)
