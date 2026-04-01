# Backend API Reference

HTTP server provides REST API for camera control and MJPEG streaming.

## Base URL

```
http://localhost:8080
```

Port configurable via `-p` or `--port` command line option.

---

## Endpoints

### GET /api/cameras

List all available cameras on the device.

**Response**

```json
[
  {
    "index": 0,
    "name": "imx519",
    "width": 4656,
    "height": 3496,
    "has_af": true,
    "rotation": 270,
    "bayer": "rggb",
    "exposure": { "min": 20, "max": 6737, "default": 1000 },
    "analogue_gain": { "min": 0, "max": 960, "default": 0 },
    "digital_gain": { "min": 256, "max": 65535, "default": 256 }
  },
  {
    "index": 1,
    "name": "imx376k",
    "width": 2592,
    "height": 1940,
    "has_af": true,
    "rotation": 270,
    "bayer": "bggr",
    "exposure": { "min": 4, "max": 65515, "default": 1600 },
    "analogue_gain": { "min": 0, "max": 480, "default": 0 },
    "digital_gain": { "min": 0, "max": 4096, "default": 1024 }
  },
  {
    "index": 2,
    "name": "imx371",
    "width": 4656,
    "height": 3496,
    "has_af": false,
    "rotation": 90,
    "bayer": "bggr",
    "exposure": { "min": 4, "max": 65515, "default": 1600 },
    "analogue_gain": { "min": 0, "max": 480, "default": 0 },
    "digital_gain": { "min": 0, "max": 4096, "default": 1024 }
  }
]
```

**Fields**

| Field | Type | Description |
|-------|------|-------------|
| `index` | int | Camera index for selection |
| `name` | string | Sensor name |
| `width` | int | Native sensor width |
| `height` | int | Native sensor height |
| `has_af` | bool | Has autofocus motor |
| `rotation` | int | Sensor rotation angle (90/270) |
| `bayer` | string | Bayer pattern: `rggb`, `bggr`, `grbg`, `gbrg` |
| `exposure` | object | Exposure control range |
| `analogue_gain` | object | Analog gain control range |
| `digital_gain` | object | Digital gain control range |

---

### GET /api/status

Get current streaming and control status.

**Response**

```json
{
  "camera": "imx519",
  "camera_index": 0,
  "streaming": true,
  "fps": 6.2,
  "jpeg_size": 45678,
  "timing": {
    "isp_ms": 28.5,
    "jpeg_ms": 12.3
  },
  "downsample": 4,
  "rotation": 0,
  "jpeg_quality": 80,
  "exposure": 2000,
  "analogue_gain": 400,
  "digital_gain": 1024,
  "focus": 150,
  "awb": {
    "auto": true,
    "r_gain": 1.25,
    "b_gain": 0.85
  },
  "ae": {
    "auto": true,
    "target": 128.0,
    "current": 125.5
  },
  "af": {
    "mode": "continuous",
    "state": "locked",
    "position": 150
  }
}
```

**Fields**

| Field | Type | Description |
|-------|------|-------------|
| `camera` | string | Current active camera name |
| `camera_index` | int | Current camera index |
| `streaming` | bool | Pipeline running state |
| `fps` | float | Current frame rate |
| `jpeg_size` | int | Last JPEG frame size (bytes) |
| `timing.isp_ms` | float | ISP processing time (unpack + demosaic) |
| `timing.jpeg_ms` | float | JPEG encoding time |
| `downsample` | int | Current downsampling factor (1/2/4) |
| `rotation` | int | Current rotation angle (0/90/180/270) |
| `jpeg_quality` | int | JPEG quality (1-100) |
| `exposure` | int | Current exposure value |
| `analogue_gain` | int | Current analog gain |
| `digital_gain` | int | Current digital gain |
| `focus` | int | Current focus position (or -1 if N/A) |
| `awb.auto` | bool | Auto white balance enabled |
| `awb.r_gain` | float | Red channel gain |
| `awb.b_gain` | float | Blue channel gain |
| `ae.auto` | bool | Auto exposure enabled |
| `ae.target` | float | Target brightness (0-255) |
| `ae.current` | float | Current measured brightness |
| `af.mode` | string | Focus mode: `manual`, `oneshot`, `continuous` |
| `af.state` | string | Focus state: `idle`, `scanning`, `locked`, `failed` |
| `af.position` | int | Best/most recent focus position |

---

### POST /api/camera/select

Switch to a different camera.

**Request**

```json
{
  "index": 1
}
```

**Response (success)**

```json
{
  "ok": true,
  "camera": "imx376k"
}
```

**Response (error)**

```json
{
  "error": "Failed to select camera"
}
```

**Notes**

- Switching camera stops streaming, reconfigures media pipeline, and restarts
- Autofocus state is reset on camera switch
- May take 1-2 seconds to complete

---

### POST /api/control

Set camera exposure and gain controls.

**Request**

```json
{
  "exposure": 3000,
  "analogue_gain": 600,
  "digital_gain": 2048
}
```

All fields optional; only provided fields are updated.

**Response**

```json
{
  "ok": true
}
```

**Field ranges**

| Field | Range | Unit |
|-------|-------|------|
| `exposure` | sensor-specific | exposure lines |
| `analogue_gain` | sensor-specific | gain units |
| `digital_gain` | sensor-specific | gain units |

Check `/api/cameras` for each sensor's valid range.

---

### POST /api/focus

Control autofocus system.

**Request (set position)**

```json
{
  "position": 200
}
```

**Request (set mode)**

```json
{
  "mode": "continuous"
}
```

**Mode values**

| Mode | Description |
|------|-------------|
| `manual` | Stay at current position, disable AF |
| `oneshot` | Single focus scan, then lock |
| `continuous` | Continuous focus adjustment |

**Response**

```json
{
  "ok": true
}
```

**Response (error - no AF)**

```json
{
  "error": "Camera has no AF"
}
```

**Notes**

- Setting `position` automatically switches to manual mode
- `oneshot` triggers a single focus sweep and locks at best position
- Focus position range depends on lens hardware (typically 0-255)

---

### POST /api/awb

Control auto white balance.

**Request**

```json
{
  "auto": true,
  "r_gain": 1.2,
  "b_gain": 0.9
}
```

**Response**

```json
{
  "ok": true
}
```

**Fields**

| Field | Type | Description |
|-------|------|-------------|
| `auto` | bool | Enable/disable auto WB algorithm |
| `r_gain` | float | Manual red gain (when auto=false) |
| `b_gain` | float | Manual blue gain (when auto=false) |

When `auto=true`, gains are calculated from frame analysis. When `auto=false`, manual gains are applied directly.

---

### POST /api/ae

Control auto exposure.

**Request**

```json
{
  "auto": true,
  "target": 120.0
}
```

**Response**

```json
{
  "ok": true
}
```

**Fields**

| Field | Type | Description |
|-------|------|-------------|
| `auto` | bool | Enable/disable auto exposure algorithm |
| `target` | float | Target brightness (0-255, default 128) |

When `auto=true`, exposure and gain are adjusted to reach target brightness.

---

### POST /api/stream

Configure stream parameters.

**Request**

```json
{
  "quality": 90,
  "fps": 15,
  "downsample": 2,
  "rotation": 90
}
```

All fields optional.

**Response**

```json
{
  "ok": true
}
```

**Fields**

| Field | Range | Description |
|-------|-------|-------------|
| `quality` | 1-100 | JPEG compression quality |
| `fps` | 0-30 | Target frame rate (0 = unlimited) |
| `downsample` | 1, 2, 4 | Downsampling factor |
| `rotation` | 0, 90, 180, 270 | Rotation angle (server-side) |

**Notes**

- Changing `downsample` requires pipeline restart (brief interruption)
- `rotation` is applied server-side before JPEG encoding (no restart needed)
- Higher quality = larger JPEG, slower encoding
- Lower downsample = higher resolution, slower processing

---

### GET /snapshot

Get a single JPEG frame.

**Response**

- Content-Type: `image/jpeg`
- Body: JPEG binary data

**Error response**

```json
{
  "error": "No frame available"
}
```

HTTP status 503 when pipeline not running or no frames yet captured.

---

### GET /stream/mjpeg

MJPEG video stream endpoint.

**Response**

- Content-Type: `multipart/x-mixed-replace; boundary=frame`
- Body: Continuous MJPEG stream

**Stream format**

```
--frame
Content-Type: image/jpeg
Content-Length: 45678

<JPEG binary data>
--frame
Content-Type: image/jpeg
Content-Length: 45890

<JPEG binary data>
...
```

**Usage in HTML**

```html
<img src="/stream/mjpeg" />
```

**Usage in JavaScript**

```javascript
const img = new Image();
img.src = '/stream/mjpeg';
document.body.appendChild(img);
```

---

## Error Responses

All error responses follow this format:

```json
{
  "error": "Error message description"
}
```

Common HTTP status codes:

| Status | Meaning |
|--------|---------|
| 400 | Invalid request (missing field, invalid JSON) |
| 500 | Server error (failed to apply control) |
| 503 | Service unavailable (no frames) |

---

## Frontend Integration Example

### React Component

```jsx
import React, { useState, useEffect } from 'react';

function CameraControl() {
  const [status, setStatus] = useState(null);
  const [cameras, setCameras] = useState([]);

  useEffect(() => {
    // Fetch cameras
    fetch('/api/cameras')
      .then(r => r.json())
      .then(setCameras);

    // Poll status every 1s
    const interval = setInterval(() => {
      fetch('/api/status')
        .then(r => r.json())
        .then(setStatus);
    }, 1000);
    return () => clearInterval(interval);
  }, []);

  const setExposure = (val) => {
    fetch('/api/control', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ exposure: val })
    });
  };

  const setFocusMode = (mode) => {
    fetch('/api/focus', {
      method: 'POST',
      headers: { 'Content-Type': 'application/json' },
      body: JSON.stringify({ mode })
    });
  };

  return (
    <div>
      <img src="/stream/mjpeg" style={{ width: '100%' }} />
      {status && (
        <div>
          <p>FPS: {status.fps.toFixed(1)}</p>
          <p>Exposure: {status.exposure}</p>
          <button onClick={() => setFocusMode('oneshot')}>
            Focus
          </button>
        </div>
      )}
    </div>
  );
}
```

### Control Panel Example

```html
<!DOCTYPE html>
<html>
<head>
  <title>Camera Control</title>
</head>
<body>
  <img id="stream" src="/stream/mjpeg" width="640">

  <div id="controls">
    <label>Camera:
      <select id="camera-select"></select>
    </label>

    <label>Exposure:
      <input type="range" id="exposure" min="20" max="6737" value="1000">
    </label>

    <label>Quality:
      <input type="range" id="quality" min="1" max="100" value="80">
    </label>

    <button id="focus-btn">Auto Focus</button>
  </div>

  <script>
    // Load cameras
    fetch('/api/cameras')
      .then(r => r.json())
      .then(cameras => {
        const select = document.getElementById('camera-select');
        cameras.forEach(c => {
          const opt = document.createElement('option');
          opt.value = c.index;
          opt.textContent = c.name;
          select.appendChild(opt);
        });
      });

    // Camera switch
    document.getElementById('camera-select').addEventListener('change', e => {
      fetch('/api/camera/select', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ index: parseInt(e.target.value) })
      });
    });

    // Exposure control
    document.getElementById('exposure').addEventListener('input', e => {
      fetch('/api/control', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ exposure: parseInt(e.target.value) })
      });
    });

    // Quality control
    document.getElementById('quality').addEventListener('input', e => {
      fetch('/api/stream', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ quality: parseInt(e.target.value) })
      });
    });

    // One-shot focus
    document.getElementById('focus-btn').addEventListener('click', () => {
      fetch('/api/focus', {
        method: 'POST',
        headers: { 'Content-Type': 'application/json' },
        body: JSON.stringify({ mode: 'oneshot' })
      });
    });
  </script>
</body>
</html>
```

---

## Command Line Options

```
Usage: fajita-webcam [OPTIONS]

Options:
  -p, --port PORT       HTTP server port (default: 8080)
  -m, --media DEVICE    Media controller device (default: /dev/media0)
  -v, --video DEVICE    Video capture device (default: /dev/video0)
  -c, --camera INDEX    Camera index to use (default: 0)
  -q, --quality N       JPEG quality 1-100 (default: 80)
  -d, --downsample N    Downsample factor 1/2/4 (default: 4)
  -f, --fps N           Target frame rate, 0=unlimited (default: 0)
  -l, --log-level LEVEL Log level: debug/info/warn/error (default: info)
      --no-awb          Disable auto white balance
      --no-ae           Disable auto exposure
      --no-af           Disable auto focus
  -h, --help            Show this help message
  -V, --version         Show version information
```

---

## Architecture

```
┌─────────────┐     ┌──────────────┐     ┌─────────────┐
│   Sensor    │────▶│   Pipeline   │────▶│  MJPEG      │
│  (V4L2)     │     │  (ISP+JPEG)  │     │  Stream     │
└─────────────┘     └──────────────┘     └─────────────┘
                           │
                           ▼
                    ┌──────────────┐
                    │   Analysis   │
                    │  (AWB/AE/AF) │
                    └──────────────┘

┌─────────────┐     ┌──────────────┐
│  HTTP       │────▶│  API Routes  │
│  Server     │     │  + Static    │
└─────────────┘     └──────────────┘
```

**Pipeline stages:**

1. **Capture**: V4L2 mmap buffer dequeue (MIPI 10-bit packed Bayer)
2. **ISP**: Unpack + white balance + downsample + demosaic (NEON SIMD + multi-thread)
3. **JPEG**: libjpeg-turbo encoding
4. **Stream**: Push to MJPEG multipart queue

**Analysis (async):**

- AWB: Gray-world algorithm on Bayer data
- AE: Histogram-based brightness measurement
- AF: Focus metric (gradient-based) for continuous/oneshot focus