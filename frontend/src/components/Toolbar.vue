<script setup>
import { computed } from 'vue'

const props = defineProps({
  rotation: { type: Number, default: 0 },
  flipH: Boolean,
  flipV: Boolean,
  zoom: Number,
  gridMode: String,
  recording: Boolean,
  recordingTime: Number,
  panelOpen: Boolean,
  isMobile: Boolean,
})

const emit = defineEmits([
  'rotate-right', 'rotate-left', 'flip-h', 'flip-v',
  'zoom-in', 'zoom-out', 'reset-zoom',
  'screenshot', 'record', 'fullscreen', 'grid', 'toggle-panel',
])

const zoomLabel = computed(() => `${Math.round(props.zoom * 100)}%`)

const recordTime = computed(() => {
  const m = Math.floor(props.recordingTime / 60)
  const s = props.recordingTime % 60
  return `${m.toString().padStart(2, '0')}:${s.toString().padStart(2, '0')}`
})

const rotLabel = computed(() => `${props.rotation}°`)
</script>

<template>
  <div class="toolbar">
    <!-- Primary actions: screenshot + record -->
    <div class="tool-group primary-actions">
      <button class="tbtn capture-btn" @click="emit('screenshot')" title="Screenshot (S)">
        <svg viewBox="0 0 24 24"><path d="M9 2L7.17 4H4c-1.1 0-2 .9-2 2v12c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V6c0-1.1-.9-2-2-2h-3.17L15 2H9zm3 15c-2.76 0-5-2.24-5-5s2.24-5 5-5 5 2.24 5 5-2.24 5-5 5zm0-8c-1.65 0-3 1.35-3 3s1.35 3 3 3 3-1.35 3-3-1.35-3-3-3z"/></svg>
      </button>
      <button
        class="tbtn record-btn" :class="{ active: recording }"
        @click="emit('record')" title="Record (Space)"
      >
        <svg viewBox="0 0 24 24">
          <circle v-if="!recording" cx="12" cy="12" r="6"/>
          <rect v-else x="8" y="8" width="8" height="8" rx="1.5"/>
        </svg>
        <span v-if="recording" class="rec-label">{{ recordTime }}</span>
      </button>
    </div>

    <div class="tool-divider"></div>

    <!-- Transform actions -->
    <div class="tool-group">
      <button class="tbtn" @click="emit('rotate-left')" title="Rotate Left (Shift+R)">
        <svg viewBox="0 0 24 24"><path d="M7.11 8.53L5.7 7.11C4.8 8.27 4.24 9.61 4.07 11h2.02c.14-.87.49-1.72 1.02-2.47zM6.09 13H4.07c.17 1.39.72 2.73 1.62 3.89l1.41-1.42c-.52-.75-.87-1.59-1.01-2.47zm1.01 5.32c1.16.9 2.51 1.44 3.9 1.61V17.9c-.87-.15-1.71-.49-2.46-1.03L7.1 18.32zM13 4.07V1L8.45 5.55 13 10V6.09c2.84.48 5 2.94 5 5.91s-2.16 5.43-5 5.91v2.02c3.95-.49 7-3.85 7-7.93s-3.05-7.44-7-7.93z"/></svg>
      </button>
      <button class="rot-badge" title="Current rotation">{{ rotLabel }}</button>
      <button class="tbtn" @click="emit('rotate-right')" title="Rotate Right (R)">
        <svg viewBox="0 0 24 24"><path d="M15.55 5.55L11 1v3.07C7.06 4.56 4 7.92 4 12s3.05 7.44 7 7.93v-2.02c-2.84-.48-5-2.94-5-5.91s2.16-5.43 5-5.91V10l4.55-4.45zM19.93 11c-.17-1.39-.72-2.73-1.62-3.89l-1.42 1.42c.54.75.88 1.6 1.02 2.47h2.02zM13 17.9v2.02c1.39-.17 2.74-.71 3.9-1.61l-1.44-1.44c-.75.54-1.59.89-2.46 1.03zm3.89-2.42l1.42 1.41c.9-1.16 1.45-2.5 1.62-3.89h-2.02c-.14.87-.48 1.72-1.02 2.48z"/></svg>
      </button>
      <button class="tbtn" :class="{ active: flipH }" @click="emit('flip-h')" title="Flip H (H)">
        <svg viewBox="0 0 24 24"><path d="M15 21h2v-2h-2v2zm4-12h2V7h-2v2zM3 5v14c0 1.1.9 2 2 2h4v-2H5V5h4V3H5c-1.1 0-2 .9-2 2zm16-2v2h2c0-1.1-.9-2-2-2zm-8-2h2v22h-2V1zm8 10h2v-2h-2v2zm-4 8h2v-2h-2v2zm4-4h2v-2h-2v2zm0 4c1.1 0 2-.9 2-2h-2v2zm0-16h2V3c-1.1 0-2 .9-2 2zm-4 0h2V3h-2v2zm-4 16h2v-2h-2v2z"/></svg>
      </button>
      <button class="tbtn" :class="{ active: flipV }" @click="emit('flip-v')" title="Flip V (V)">
        <svg viewBox="0 0 24 24" style="transform:rotate(90deg)"><path d="M15 21h2v-2h-2v2zm4-12h2V7h-2v2zM3 5v14c0 1.1.9 2 2 2h4v-2H5V5h4V3H5c-1.1 0-2 .9-2 2zm16-2v2h2c0-1.1-.9-2-2-2zm-8-2h2v22h-2V1zm8 10h2v-2h-2v2zm-4 8h2v-2h-2v2zm4-4h2v-2h-2v2zm0 4c1.1 0 2-.9 2-2h-2v2zm0-16h2V3c-1.1 0-2 .9-2 2zm-4 0h2V3h-2v2zm-4 16h2v-2h-2v2z"/></svg>
      </button>
    </div>

    <div class="tool-divider hide-xs"></div>
    <div class="tool-group hide-xs">
      <button class="tbtn" @click="emit('zoom-out')" title="Zoom Out (-)">
        <svg viewBox="0 0 24 24"><path d="M15.5 14h-.79l-.28-.27C15.41 12.59 16 11.11 16 9.5 16 5.91 13.09 3 9.5 3S3 5.91 3 9.5 5.91 16 9.5 16c1.61 0 3.09-.59 4.23-1.57l.27.28v.79l5 4.99L20.49 19l-4.99-5zm-6 0C7.01 14 5 11.99 5 9.5S7.01 5 9.5 5 14 7.01 14 9.5 11.99 14 9.5 14zM7 9h5v1H7z"/></svg>
      </button>
      <button class="zoom-badge" @click="emit('reset-zoom')" title="Reset Zoom (0)">{{ zoomLabel }}</button>
      <button class="tbtn" @click="emit('zoom-in')" title="Zoom In (+)">
        <svg viewBox="0 0 24 24"><path d="M15.5 14h-.79l-.28-.27C15.41 12.59 16 11.11 16 9.5 16 5.91 13.09 3 9.5 3S3 5.91 3 9.5 5.91 16 9.5 16c1.61 0 3.09-.59 4.23-1.57l.27.28v.79l5 4.99L20.49 19l-4.99-5zm-6 0C7.01 14 5 11.99 5 9.5S7.01 5 9.5 5 14 7.01 14 9.5 11.99 14 9.5 14zM9 9V7h1v2h2v1h-2v2H9v-2H7V9h2z"/></svg>
      </button>
    </div>

    <div class="tool-divider"></div>

    <!-- Utility actions -->
    <div class="tool-group">
      <button class="tbtn" :class="{ active: gridMode !== 'none' }" @click="emit('grid')" title="Grid (G)">
        <svg viewBox="0 0 24 24"><path d="M20 2H4c-1.1 0-2 .9-2 2v16c0 1.1.9 2 2 2h16c1.1 0 2-.9 2-2V4c0-1.1-.9-2-2-2zM8 20H4v-4h4v4zm0-6H4v-4h4v4zm0-6H4V4h4v4zm6 12h-4v-4h4v4zm0-6h-4v-4h4v4zm0-6h-4V4h4v4zm6 12h-4v-4h4v4zm0-6h-4v-4h4v4zm0-6h-4V4h4v4z"/></svg>
      </button>
      <button class="tbtn hide-xs" @click="emit('fullscreen')" title="Fullscreen (F)">
        <svg viewBox="0 0 24 24"><path d="M7 14H5v5h5v-2H7v-3zm-2-4h2V7h3V5H5v5zm12 7h-3v2h5v-5h-2v3zM14 5v2h3v3h2V5h-5z"/></svg>
      </button>
      <button class="tbtn settings-btn" :class="{ active: panelOpen }" @click="emit('toggle-panel')" title="Controls (P)">
        <svg viewBox="0 0 24 24"><path d="M19.14 12.94c.04-.3.06-.61.06-.94 0-.32-.02-.64-.07-.94l2.03-1.58a.49.49 0 00.12-.61l-1.92-3.32a.49.49 0 00-.59-.22l-2.39.96c-.5-.38-1.03-.7-1.62-.94l-.36-2.54a.484.484 0 00-.48-.41h-3.84c-.24 0-.43.17-.47.41l-.36 2.54c-.59.24-1.13.57-1.62.94l-2.39-.96c-.22-.08-.47 0-.59.22L2.74 8.87c-.12.21-.08.47.12.61l2.03 1.58c-.05.3-.07.62-.07.94s.02.64.07.94l-2.03 1.58a.49.49 0 00-.12.61l1.92 3.32c.12.22.37.29.59.22l2.39-.96c.5.38 1.03.7 1.62.94l.36 2.54c.05.24.24.41.48.41h3.84c.24 0 .44-.17.47-.41l.36-2.54c.59-.24 1.13-.56 1.62-.94l2.39.96c.22.08.47 0 .59-.22l1.92-3.32c.12-.22.07-.47-.12-.61l-2.01-1.58zM12 15.6A3.6 3.6 0 1115.6 12 3.611 3.611 0 0112 15.6z"/></svg>
      </button>
    </div>
  </div>
</template>

<style scoped>
.toolbar {
  display: flex; align-items: center; gap: 4px;
  padding: 8px 12px;
  padding-bottom: calc(8px + var(--safe-bottom));
  background: var(--bg-base);
  border-top: 1px solid var(--border);
  flex-shrink: 0; z-index: 20;
  backdrop-filter: blur(20px);
  -webkit-backdrop-filter: blur(20px);
}
.tool-group { display: flex; align-items: center; gap: 2px; }
.tool-divider { width: 1px; height: 22px; background: var(--border-light); margin: 0 6px; flex-shrink: 0; }

.tbtn {
  width: 38px; height: 38px; border: none; border-radius: var(--radius);
  background: transparent; color: var(--text-secondary); cursor: pointer;
  display: flex; align-items: center; justify-content: center;
  transition: all 0.15s ease;
  -webkit-tap-highlight-color: transparent;
}
.tbtn:hover { background: var(--bg-elevated); color: var(--text-primary); }
.tbtn:active { transform: scale(0.92); }
.tbtn.active { color: var(--accent); background: var(--accent-dim); }
.tbtn svg { width: 18px; height: 18px; fill: currentColor; }

.capture-btn:hover { color: #4ade80; background: rgba(74, 222, 128, 0.1); }

.record-btn { gap: 4px; }
.record-btn:not(.active) svg { fill: #ef4444; }
.record-btn.active {
  background: rgba(239, 68, 68, 0.15); color: #ef4444;
  width: auto; padding: 0 12px;
  animation: rec-glow 1.5s ease infinite;
}
.rec-label { font-size: 11px; font-weight: 700; font-variant-numeric: tabular-nums; letter-spacing: 0.5px; }
@keyframes rec-glow { 50% { background: rgba(239, 68, 68, 0.25); } }

.settings-btn.active svg { animation: spin-once 0.4s ease; }
@keyframes spin-once { from { transform: rotate(0); } to { transform: rotate(60deg); } }

.zoom-badge, .rot-badge {
  min-width: 42px; height: 26px; border: 1px solid var(--border-light);
  border-radius: 20px; background: transparent;
  color: var(--text-muted); cursor: pointer;
  font-size: 10px; font-weight: 600; font-variant-numeric: tabular-nums;
  transition: all 0.15s;
}
.zoom-badge:hover, .rot-badge:hover { background: var(--bg-elevated); color: var(--text-primary); border-color: var(--border-light); }

@media (max-width: 767px) {
  .toolbar { padding: 6px 8px; padding-bottom: calc(6px + var(--safe-bottom)); gap: 2px; justify-content: center; }
  .tbtn { width: 42px; height: 42px; border-radius: 12px; }
  .tbtn svg { width: 20px; height: 20px; }
  .tool-divider { margin: 0 4px; height: 20px; }
  .hide-xs { display: none; }
}
@media (max-width: 380px) {
  .tbtn { width: 38px; height: 38px; }
}
</style>