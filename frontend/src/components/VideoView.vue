<script setup>
import { ref, computed, watch } from 'vue'

const props = defineProps({
  rotation: { type: Number, default: 0 },
  flipH: { type: Boolean, default: false },
  flipV: { type: Boolean, default: false },
  zoom: { type: Number, default: 1 },
  gridMode: { type: String, default: 'none' },
  status: { type: Object, default: () => ({}) },
  camera: { type: Object, default: null },
  recording: { type: Boolean, default: false },
  recordingTime: { type: Number, default: 0 },
})

const imgEl = ref(null)
const streamSrc = ref('/stream/mjpeg?' + Date.now())

defineExpose({ imgEl })

watch(() => props.camera, () => {
  streamSrc.value = ''
  setTimeout(() => { streamSrc.value = '/stream/mjpeg?' + Date.now() }, 300)
})

const transform = computed(() => {
  const parts = []
  if (props.zoom !== 1) parts.push(`scale(${props.zoom})`)
  if (props.rotation) parts.push(`rotate(${props.rotation}deg)`)
  if (props.flipH) parts.push('scaleX(-1)')
  if (props.flipV) parts.push('scaleY(-1)')
  return parts.join(' ') || 'none'
})

const overlayText = computed(() => {
  const s = props.status
  const cam = props.camera
  const ds = s.downsample || 4
  const w = cam ? Math.floor(cam.width / ds) : '?'
  const h = cam ? Math.floor(cam.height / ds) : '?'
  const fps = s.fps?.toFixed(1) || '--'
  const kb = s.jpeg_size ? (s.jpeg_size / 1024).toFixed(0) : '--'
  return `${w} x ${h}  |  ${fps} fps  |  ${kb} KB`
})

const recordTime = computed(() => {
  const m = Math.floor(props.recordingTime / 60)
  const s = props.recordingTime % 60
  return `${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`
})
</script>

<template>
  <div class="video-view">
    <img
      ref="imgEl"
      :src="streamSrc"
      alt="Camera stream"
      class="stream"
      :style="{ transform }"
      draggable="false"
    >

    <!-- Grid overlays -->
    <svg v-if="gridMode === 'thirds'" class="grid-overlay" viewBox="0 0 300 300" preserveAspectRatio="none">
      <line x1="100" y1="0" x2="100" y2="300" />
      <line x1="200" y1="0" x2="200" y2="300" />
      <line x1="0" y1="100" x2="300" y2="100" />
      <line x1="0" y1="200" x2="300" y2="200" />
    </svg>
    <svg v-if="gridMode === 'crosshair'" class="grid-overlay" viewBox="0 0 300 300" preserveAspectRatio="none">
      <line x1="150" y1="0" x2="150" y2="300" />
      <line x1="0" y1="150" x2="300" y2="150" />
      <circle cx="150" cy="150" r="40" fill="none" />
      <circle cx="150" cy="150" r="80" fill="none" />
    </svg>

    <!-- Recording indicator -->
    <transition name="fade">
      <div v-if="recording" class="rec-badge">
        <span class="rec-dot"></span>
        <span>REC</span>
        <span class="rec-time">{{ recordTime }}</span>
      </div>
    </transition>

    <!-- Bottom info bar -->
    <div class="info-bar">{{ overlayText }}</div>
  </div>
</template>

<style scoped>
.video-view {
  flex: 1; display: flex; align-items: center; justify-content: center;
  position: relative; overflow: hidden; min-height: 0;
}
.stream {
  max-width: 100%; max-height: 100%; object-fit: contain;
  transition: transform 0.35s cubic-bezier(0.4, 0, 0.2, 1);
  user-select: none; -webkit-user-drag: none;
}

.grid-overlay {
  position: absolute; inset: 0; width: 100%; height: 100%;
  pointer-events: none;
}
.grid-overlay line, .grid-overlay circle {
  stroke: rgba(255, 255, 255, 0.25); stroke-width: 0.6; fill: none;
}

.rec-badge {
  position: absolute; top: 16px; left: 16px;
  display: flex; align-items: center; gap: 8px;
  background: rgba(200, 20, 20, 0.9);
  backdrop-filter: blur(8px); -webkit-backdrop-filter: blur(8px);
  color: #fff; padding: 6px 14px; border-radius: 20px;
  font-size: 12px; font-weight: 700; letter-spacing: 0.5px;
}
.rec-dot {
  width: 8px; height: 8px; border-radius: 50%; background: #fff;
  animation: rec-blink 1s ease infinite;
}
.rec-time { font-variant-numeric: tabular-nums; font-weight: 500; }
@keyframes rec-blink { 0%, 100% { opacity: 1; } 50% { opacity: 0.3; } }

.info-bar {
  position: absolute; bottom: 10px; left: 50%; transform: translateX(-50%);
  font-size: 11px; color: rgba(255, 255, 255, 0.5);
  background: rgba(0, 0, 0, 0.5);
  backdrop-filter: blur(10px); -webkit-backdrop-filter: blur(10px);
  padding: 4px 14px; border-radius: 20px;
  font-variant-numeric: tabular-nums; white-space: nowrap;
  letter-spacing: 0.3px;
}

.fade-enter-active, .fade-leave-active { transition: opacity 0.2s; }
.fade-enter-from, .fade-leave-to { opacity: 0; }

@media (max-width: 767px) {
  .info-bar { font-size: 10px; padding: 3px 10px; bottom: 8px; }
  .rec-badge { top: 10px; left: 10px; padding: 4px 10px; font-size: 11px; }
}
</style>
