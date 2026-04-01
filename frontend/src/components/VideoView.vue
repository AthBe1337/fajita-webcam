<script setup>
import { ref, computed, watch, onMounted, onUnmounted } from 'vue'

const props = defineProps({
  flipH: { type: Boolean, default: false },
  flipV: { type: Boolean, default: false },
  zoom: { type: Number, default: 1 },
  status: { type: Object, default: () => ({}) },
  camera: { type: Object, default: null },
  recording: { type: Boolean, default: false },
  recordingTime: { type: Number, default: 0 },
  connected: { type: Boolean, default: true },
  streamUrl: { type: String, default: '' },
  streamKey: { type: String, default: '' },
  showTimestamp: { type: Boolean, default: true },
  timestampDark: { type: Boolean, default: false },
})

const imgEl = ref(null)
const containerEl = ref(null)
defineExpose({ imgEl })

// Track last streamKey to detect when to force reconnect
let lastStreamKey = ''

// Update stream when URL or key changes - use flush: 'post' to ensure DOM is ready
watch([() => props.streamUrl, () => props.streamKey], ([url, key]) => {
  if (!url || !imgEl.value) return

  if (key !== lastStreamKey) {
    lastStreamKey = key || ''
    imgEl.value.src = ''
    setTimeout(() => {
      if (imgEl.value) imgEl.value.src = url
    }, 50)
  }
}, { flush: 'post' })

// Initial load on mount
onMounted(() => {
  if (props.streamUrl && imgEl.value) {
    lastStreamKey = props.streamKey || ''
    imgEl.value.src = props.streamUrl
  }
})

// Handle actual connection loss
let errorCount = 0
let retryTimer = null

function onImgError() {
  errorCount++
  if (errorCount >= 3 && !retryTimer) {
    retryTimer = setTimeout(() => {
      retryTimer = null
      errorCount = 0
      if (imgEl.value && props.streamUrl) {
        imgEl.value.src = ''
        setTimeout(() => {
          if (imgEl.value) imgEl.value.src = props.streamUrl
        }, 50)
      }
    }, 2000)
  }
}

function onImgLoad() {
  errorCount = 0
  if (retryTimer) {
    clearTimeout(retryTimer)
    retryTimer = null
  }
  updateOverlayPosition()
}

// Transform
const transform = computed(() => {
  const parts = []
  if (props.zoom !== 1) parts.push(`scale(${props.zoom})`)
  if (props.flipH) parts.push('scaleX(-1)')
  if (props.flipV) parts.push('scaleY(-1)')
  return parts.join(' ') || 'none'
})

// Timestamp position - calculate actual video frame position
const overlayStyle = ref({ top: '12px', left: '12px' })

function updateOverlayPosition() {
  if (!imgEl.value || !containerEl.value) return

  const container = containerEl.value.getBoundingClientRect()
  const img = imgEl.value

  // Get natural video dimensions
  const naturalWidth = img.naturalWidth || img.videoWidth || container.width
  const naturalHeight = img.naturalHeight || img.videoHeight || container.height

  // Calculate displayed size (object-fit: contain)
  const containerRatio = container.width / container.height
  const imageRatio = naturalWidth / naturalHeight

  let displayWidth, displayHeight
  if (containerRatio > imageRatio) {
    // Container is wider - image fills height
    displayHeight = container.height
    displayWidth = container.height * imageRatio
  } else {
    // Container is taller - image fills width
    displayWidth = container.width
    displayHeight = container.width / imageRatio
  }

  // Calculate offset (centered)
  const offsetX = (container.width - displayWidth) / 2
  const offsetY = (container.height - displayHeight) / 2

  // Position overlay at actual video frame corner
  const padding = Math.max(8, displayWidth * 0.015)
  overlayStyle.value = {
    top: `${offsetY + padding}px`,
    left: `${offsetX + padding}px`,
  }
}

// Resize observer
let resizeObserver = null

onMounted(() => {
  if (containerEl.value) {
    resizeObserver = new ResizeObserver(updateOverlayPosition)
    resizeObserver.observe(containerEl.value)
  }
  window.addEventListener('resize', updateOverlayPosition)
})

onUnmounted(() => {
  if (resizeObserver) resizeObserver.disconnect()
  window.removeEventListener('resize', updateOverlayPosition)
})

// Timestamp
const now = ref(new Date())
let timeInterval = null

onMounted(() => {
  timeInterval = setInterval(() => {
    now.value = new Date()
  }, 1000)
})

onUnmounted(() => {
  if (timeInterval) clearInterval(timeInterval)
})

const timestamp = computed(() => {
  const d = now.value
  const y = d.getFullYear()
  const mo = String(d.getMonth() + 1).padStart(2, '0')
  const da = String(d.getDate()).padStart(2, '0')
  const h = String(d.getHours()).padStart(2, '0')
  const m = String(d.getMinutes()).padStart(2, '0')
  const s = String(d.getSeconds()).padStart(2, '0')
  return `${y}-${mo}-${da} ${h}:${m}:${s}`
})

const cameraName = computed(() => props.camera?.name || 'CAM')

const recordTime = computed(() => {
  const m = Math.floor(props.recordingTime / 60)
  const s = props.recordingTime % 60
  return `${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`
})

// Resolution display
const resolutionText = computed(() => {
  const cam = props.camera
  const ds = props.status?.downsample || 4
  const rot = props.status?.rotation || 0
  if (!cam) return '-- x --'

  let w = Math.floor(cam.width / ds)
  let h = Math.floor(cam.height / ds)
  if (rot === 90 || rot === 270) [w, h] = [h, w]
  return `${w} x ${h}`
})
</script>

<template>
  <div ref="containerEl" class="video-view">
    <img
      ref="imgEl"
      :src="streamUrl"
      alt="Camera stream"
      class="stream"
      :style="{ transform }"
      draggable="false"
      @error="onImgError"
      @load="onImgLoad"
    >

    <!-- Monitor-style timestamp overlay -->
    <div
      v-if="showTimestamp"
      class="monitor-overlay"
      :class="{ dark: timestampDark }"
      :style="overlayStyle"
    >
      <div class="timestamp">
        <span class="cam-name">{{ cameraName }}</span>
        <span class="datetime">{{ timestamp }}</span>
      </div>
      <div v-if="recording" class="rec-indicator">
        <span class="rec-dot"></span>
        <span>REC</span>
        <span class="rec-time">{{ recordTime }}</span>
      </div>
    </div>

    <!-- Bottom info bar -->
    <div class="info-bar">
      <span>{{ resolutionText }}</span>
      <span class="sep">|</span>
      <span>{{ status.fps?.toFixed(1) || '--' }} fps</span>
      <span class="sep">|</span>
      <span>{{ status.jpeg_size ? (status.jpeg_size / 1024).toFixed(0) : '--' }} KB</span>
    </div>
  </div>
</template>

<style scoped>
.video-view {
  flex: 1;
  display: flex;
  align-items: center;
  justify-content: center;
  position: relative;
  overflow: hidden;
  min-height: 0;
}

.stream {
  width: 100%;
  height: 100%;
  object-fit: contain;
  transition: transform 0.35s cubic-bezier(0.4, 0, 0.2, 1);
  user-select: none;
  -webkit-user-drag: none;
}

/* Monitor-style overlay - positioned at actual video frame corner */
.monitor-overlay {
  position: absolute;
  display: flex;
  flex-direction: column;
  gap: 6px;
  pointer-events: none;
  z-index: 10;
}

.timestamp {
  display: flex;
  flex-direction: column;
  gap: 2px;
  font-family: 'SF Mono', 'Consolas', 'Monaco', monospace;
  font-size: 13px;
  font-weight: 500;
  color: #fff;
  text-shadow: 0 1px 3px rgba(0, 0, 0, 0.8);
  letter-spacing: 0.5px;
  background: rgba(0, 0, 0, 0.4);
  padding: 6px 10px;
  border-radius: 4px;
}

.monitor-overlay.dark .timestamp {
  color: #000;
  text-shadow: 0 1px 2px rgba(255, 255, 255, 0.5);
  background: rgba(255, 255, 255, 0.6);
}

.cam-name {
  font-size: 11px;
  font-weight: 700;
  letter-spacing: 1px;
  opacity: 0.9;
}

.datetime {
  font-variant-numeric: tabular-nums;
  font-size: 14px;
}

.rec-indicator {
  display: flex;
  align-items: center;
  gap: 6px;
  background: rgba(200, 20, 20, 0.9);
  color: #fff;
  padding: 4px 10px;
  border-radius: 4px;
  font-size: 11px;
  font-weight: 700;
  letter-spacing: 0.5px;
}

.rec-dot {
  width: 8px;
  height: 8px;
  border-radius: 50%;
  background: #fff;
  animation: rec-blink 1s ease infinite;
}

.rec-time {
  font-variant-numeric: tabular-nums;
  font-weight: 500;
  margin-left: 2px;
}

@keyframes rec-blink {
  0%, 100% { opacity: 1; }
  50% { opacity: 0.3; }
}

.info-bar {
  position: absolute;
  bottom: 10px;
  left: 50%;
  transform: translateX(-50%);
  font-size: 11px;
  color: rgba(255, 255, 255, 0.6);
  background: rgba(0, 0, 0, 0.5);
  backdrop-filter: blur(10px);
  -webkit-backdrop-filter: blur(10px);
  padding: 4px 12px;
  border-radius: 20px;
  font-variant-numeric: tabular-nums;
  white-space: nowrap;
  letter-spacing: 0.3px;
  display: flex;
  gap: 8px;
}

.sep {
  opacity: 0.4;
}

@media (max-width: 767px) {
  .timestamp {
    font-size: 11px;
    padding: 4px 8px;
  }

  .cam-name {
    font-size: 10px;
  }

  .datetime {
    font-size: 12px;
  }

  .info-bar {
    font-size: 10px;
    padding: 3px 10px;
    bottom: 8px;
  }
}
</style>