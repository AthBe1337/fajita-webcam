<script setup>
import { ref, computed, provide, onMounted, onUnmounted } from 'vue'
import { useCamera } from './composables/useCamera.js'
import { useRecorder } from './composables/useRecorder.js'
import VideoView from './components/VideoView.vue'
import Toolbar from './components/Toolbar.vue'
import ControlPanel from './components/ControlPanel.vue'

const cam = useCamera()
const videoRef = ref(null)
const recorder = useRecorder()

// View transforms
const rotation = ref(0)
const flipH = ref(false)
const flipV = ref(false)
const zoom = ref(1)
const gridMode = ref('none')
const panelOpen = ref(false)
const isMobile = ref(false)

function checkMobile() { isMobile.value = window.innerWidth < 768 }
onMounted(() => { checkMobile(); window.addEventListener('resize', checkMobile) })
onUnmounted(() => window.removeEventListener('resize', checkMobile))

function rotateRight() { rotation.value = (rotation.value + 90) % 360 }
function rotateLeft() { rotation.value = (rotation.value + 270) % 360 }
function toggleFlipH() { flipH.value = !flipH.value }
function toggleFlipV() { flipV.value = !flipV.value }
function zoomIn() { zoom.value = Math.min(zoom.value + 0.25, 4) }
function zoomOut() { zoom.value = Math.max(zoom.value - 0.25, 0.5) }
function resetZoom() { zoom.value = 1 }
function cycleGrid() {
  const modes = ['none', 'thirds', 'crosshair']
  gridMode.value = modes[(modes.indexOf(gridMode.value) + 1) % modes.length]
}

async function takeScreenshot() {
  try {
    const r = await fetch('/snapshot')
    const blob = await r.blob()
    const url = URL.createObjectURL(blob)
    const a = document.createElement('a')
    a.href = url
    const d = new Date()
    const ts = `${d.getFullYear()}${p(d.getMonth()+1)}${p(d.getDate())}-${p(d.getHours())}${p(d.getMinutes())}${p(d.getSeconds())}`
    a.download = `fajita-${ts}.jpg`
    a.click()
    URL.revokeObjectURL(url)
  } catch { /* ignore */ }
}
function p(n) { return n.toString().padStart(2, '0') }

function toggleRecord() {
  recorder.toggleRecording(videoRef.value?.imgEl)
}

function toggleFullscreen() {
  if (!document.fullscreenElement) {
    document.documentElement.requestFullscreen()
  } else {
    document.exitFullscreen()
  }
}

function onKeydown(e) {
  if (e.target.tagName === 'INPUT') return
  switch (e.key) {
    case 'r': rotateRight(); break
    case 'R': rotateLeft(); break
    case 'h': toggleFlipH(); break
    case 'v': toggleFlipV(); break
    case 's': takeScreenshot(); break
    case ' ': e.preventDefault(); toggleRecord(); break
    case 'f': toggleFullscreen(); break
    case 'g': cycleGrid(); break
    case '+': case '=': zoomIn(); break
    case '-': zoomOut(); break
    case '0': resetZoom(); break
    case 'p': panelOpen.value = !panelOpen.value; break
  }
}

provide('cam', cam)
</script>

<template>
  <div class="app" @keydown="onKeydown" tabindex="0">
    <!-- Header -->
    <header class="header">
      <div class="header-left">
        <div class="logo">
          <svg viewBox="0 0 24 24" fill="currentColor" width="18" height="18"><path d="M12 15.2c-1.8 0-3.2-1.4-3.2-3.2S10.2 8.8 12 8.8s3.2 1.4 3.2 3.2-1.4 3.2-3.2 3.2zm8-7.2h-3.2l-1.5-1.6c-.2-.3-.6-.4-.9-.4H9.6c-.3 0-.7.1-.9.4L7.2 8H4c-1.1 0-2 .9-2 2v8c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2v-8c0-1.1-.9-2-2-2z"/></svg>
          <span>Fajita Cam</span>
        </div>
        <div class="cam-pills">
          <button
            v-for="(c, i) in cam.cameras.value" :key="i"
            class="cam-pill"
            :class="{ active: i === cam.currentIndex.value }"
            @click="cam.selectCam(i)"
          >{{ c.name }}</button>
        </div>
      </div>
      <div class="header-right">
        <div v-if="!cam.connected.value" class="status-dot offline"></div>
        <div v-else class="status-dot online"></div>
      </div>
    </header>

    <!-- Main content -->
    <div class="content">
      <div class="video-area">
        <VideoView
          ref="videoRef"
          :rotation="rotation"
          :flip-h="flipH"
          :flip-v="flipV"
          :zoom="zoom"
          :grid-mode="gridMode"
          :status="cam.status"
          :camera="cam.currentCamera.value"
          :recording="recorder.recording.value"
          :recording-time="recorder.recordingTime.value"
        />
      </div>

      <!-- Desktop side panel -->
      <transition name="slide-panel">
        <ControlPanel
          v-if="panelOpen && !isMobile"
          class="side-panel"
          :status="cam.status"
          :camera="cam.currentCamera.value"
          :ae-auto="cam.aeAuto.value"
          :awb-auto="cam.awbAuto.value"
          :caf-on="cam.cafOn.value"
          @update:ae-auto="cam.aeAuto.value = $event"
          @update:awb-auto="cam.awbAuto.value = $event"
          @update:caf-on="cam.cafOn.value = $event"
        />
      </transition>
    </div>

    <!-- Toolbar -->
    <Toolbar
      :rotation="rotation"
      :flip-h="flipH"
      :flip-v="flipV"
      :zoom="zoom"
      :grid-mode="gridMode"
      :recording="recorder.recording.value"
      :recording-time="recorder.recordingTime.value"
      :panel-open="panelOpen"
      :is-mobile="isMobile"
      @rotate-right="rotateRight"
      @rotate-left="rotateLeft"
      @flip-h="toggleFlipH"
      @flip-v="toggleFlipV"
      @zoom-in="zoomIn"
      @zoom-out="zoomOut"
      @reset-zoom="resetZoom"
      @screenshot="takeScreenshot"
      @record="toggleRecord"
      @fullscreen="toggleFullscreen"
      @grid="cycleGrid"
      @toggle-panel="panelOpen = !panelOpen"
    />

    <!-- Mobile bottom sheet -->
    <transition name="sheet">
      <div v-if="panelOpen && isMobile" class="sheet-backdrop" @click="panelOpen = false">
        <div class="sheet" @click.stop>
          <div class="sheet-handle" @click="panelOpen = false"><div class="handle-bar"></div></div>
          <ControlPanel
            :status="cam.status"
            :camera="cam.currentCamera.value"
            :ae-auto="cam.aeAuto.value"
            :awb-auto="cam.awbAuto.value"
            :caf-on="cam.cafOn.value"
            @update:ae-auto="cam.aeAuto.value = $event"
            @update:awb-auto="cam.awbAuto.value = $event"
            @update:caf-on="cam.cafOn.value = $event"
          />
        </div>
      </div>
    </transition>
  </div>
</template>

<style>

:root {
  --bg-deep: #0c0c14;
  --bg-base: #13131e;
  --bg-surface: #1a1a2a;
  --bg-elevated: #222236;
  --border: rgba(255, 255, 255, 0.06);
  --border-light: rgba(255, 255, 255, 0.1);
  --text-primary: #eaeaf0;
  --text-secondary: #8888a0;
  --text-muted: #55556a;
  --accent: #e94560;
  --accent-dim: rgba(233, 69, 96, 0.15);
  --accent-hover: #ff5070;
  --radius: 10px;
  --radius-sm: 6px;
  --safe-bottom: env(safe-area-inset-bottom, 0px);
}

* { margin: 0; padding: 0; box-sizing: border-box; }
html, body { height: 100%; overflow: hidden; }
body {
  font-family: -apple-system, BlinkMacSystemFont, 'Segoe UI', system-ui, sans-serif;
  background: var(--bg-deep); color: var(--text-primary);
  -webkit-font-smoothing: antialiased;
  -webkit-tap-highlight-color: transparent;
  touch-action: manipulation;
}

.app {
  height: 100vh; height: 100dvh;
  display: flex; flex-direction: column; outline: none;
}

/* --- Header --- */
.header {
  display: flex; align-items: center; justify-content: space-between;
  padding: 10px 16px; background: var(--bg-base);
  border-bottom: 1px solid var(--border);
  flex-shrink: 0; z-index: 20;
  backdrop-filter: blur(20px);
  -webkit-backdrop-filter: blur(20px);
}
.header-left { display: flex; align-items: center; gap: 12px; min-width: 0; }
.header-right { flex-shrink: 0; }
.logo {
  display: flex; align-items: center; gap: 7px;
  color: var(--accent); font-size: 14px; font-weight: 700;
  letter-spacing: -0.3px; white-space: nowrap;
}
.logo svg { width: 18px; height: 18px; flex-shrink: 0; }
.cam-pills {
  display: flex; gap: 6px; overflow-x: auto;
  scrollbar-width: none; -ms-overflow-style: none;
}
.cam-pills::-webkit-scrollbar { display: none; }
.cam-pill {
  padding: 5px 14px; border: 1px solid var(--border-light); border-radius: 20px;
  background: transparent; color: var(--text-secondary); cursor: pointer;
  font-size: 12px; font-weight: 500; white-space: nowrap;
  transition: all 0.2s ease;
}
.cam-pill:hover { background: var(--bg-elevated); color: var(--text-primary); }
.cam-pill.active {
  background: var(--accent); border-color: var(--accent);
  color: #fff; font-weight: 600;
}
.status-dot {
  width: 8px; height: 8px; border-radius: 50%;
}
.status-dot.online { background: #4ade80; box-shadow: 0 0 6px rgba(74, 222, 128, 0.5); }
.status-dot.offline { background: var(--accent); animation: pulse-dot 1.2s infinite; }
@keyframes pulse-dot { 50% { opacity: 0.3; } }

/* --- Content --- */
.content {
  flex: 1; display: flex; min-height: 0; position: relative;
}
.video-area {
  flex: 1; display: flex; min-width: 0;
  position: relative; background: #000;
}

/* --- Desktop side panel --- */
.side-panel {
  width: 300px; flex-shrink: 0;
  border-left: 1px solid var(--border);
}
.slide-panel-enter-active,
.slide-panel-leave-active { transition: all 0.25s ease; }
.slide-panel-enter-from,
.slide-panel-leave-to { width: 0; opacity: 0; overflow: hidden; }

/* --- Mobile bottom sheet --- */
.sheet-backdrop {
  position: fixed; inset: 0; z-index: 100;
  background: rgba(0, 0, 0, 0.5);
  backdrop-filter: blur(4px);
  -webkit-backdrop-filter: blur(4px);
}
.sheet {
  position: absolute; bottom: 0; left: 0; right: 0;
  max-height: 70vh; background: var(--bg-base);
  border-radius: 16px 16px 0 0;
  overflow: hidden; display: flex; flex-direction: column;
}
.sheet-handle {
  display: flex; justify-content: center; padding: 10px 0 6px;
  cursor: pointer; flex-shrink: 0;
}
.handle-bar {
  width: 36px; height: 4px; border-radius: 2px;
  background: rgba(255, 255, 255, 0.2);
}
.sheet-enter-active { transition: all 0.3s ease-out; }
.sheet-leave-active { transition: all 0.25s ease-in; }
.sheet-enter-from .sheet,
.sheet-leave-to .sheet { transform: translateY(100%); }
.sheet-enter-from,
.sheet-leave-to { background: rgba(0, 0, 0, 0); }

/* --- Mobile overrides --- */
@media (max-width: 767px) {
  .header { padding: 8px 12px; }
  .logo span { display: none; }
  .cam-pill { padding: 4px 12px; font-size: 11px; }
}
</style>
