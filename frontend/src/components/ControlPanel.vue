<script setup>
import { ref, watch, computed } from 'vue'
import * as api from '../api.js'

const props = defineProps({
  status: { type: Object, default: () => ({}) },
  camera: { type: Object, default: null },
  aeAuto: Boolean,
  awbAuto: Boolean,
  cafOn: Boolean,
  showTimestamp: { type: Boolean, default: true },
  timestampDark: { type: Boolean, default: false },
})

const emit = defineEmits(['update:ae-auto', 'update:awb-auto', 'update:caf-on', 'update:show-timestamp', 'update:timestamp-dark'])

function debounce(fn, ms) {
  let t; return (...a) => { clearTimeout(t); t = setTimeout(() => fn(...a), ms) }
}

// --- Collapsible sections ---
const openSections = ref({ exposure: true, focus: true, wb: true, stream: false, status: false })
function toggle(key) { openSections.value[key] = !openSections.value[key] }

// --- Exposure ---
const exposure = ref(500)
const analogueGain = ref(0)
const digitalGain = ref(256)
const aeTarget = ref(50)

const sendControl = debounce((key, val) => api.setControl({ [key]: parseInt(val) }), 100)
function onExposure(v) { exposure.value = v; sendControl('exposure', v) }
function onAnalogueGain(v) { analogueGain.value = v; sendControl('analogue_gain', v) }
function onDigitalGain(v) { digitalGain.value = v; sendControl('digital_gain', v) }

function toggleAE() {
  const next = !props.aeAuto
  emit('update:ae-auto', next)
  api.setAE({ auto: next })
}
const sendAeTarget = debounce((v) => api.setAE({ target: parseFloat(v) }), 200)
function onAeTarget(v) { aeTarget.value = v; sendAeTarget(v) }

// --- Focus ---
const focusPos = ref(0)
const sendFocus = debounce((v) => api.setFocus({ position: parseInt(v) }), 100)
function onFocus(v) { focusPos.value = v; sendFocus(v) }
function triggerAF() { api.setFocus({ mode: 'oneshot' }) }
function toggleCAF() {
  const next = !props.cafOn
  emit('update:caf-on', next)
  api.setFocus({ mode: next ? 'continuous' : 'manual' })
}

// --- WB ---
const rGain = ref(100)
const bGain = ref(100)
function toggleAWB() {
  const next = !props.awbAuto
  emit('update:awb-auto', next)
  api.setAWB({ auto: next })
}
const sendWB = debounce(() => {
  if (props.awbAuto) return
  api.setAWB({ auto: false, r_gain: rGain.value / 100, b_gain: bGain.value / 100 })
}, 150)
function onRGain(v) { rGain.value = v; sendWB() }
function onBGain(v) { bGain.value = v; sendWB() }

// --- Stream ---
const quality = ref(80)
const fps = ref(15)
const downsample = ref(4)
const sendStream = debounce(() => {
  api.setStream({ quality: parseInt(quality.value), fps: parseInt(fps.value) })
}, 300)
function onQuality(v) { quality.value = v; sendStream() }
function onFps(v) { fps.value = v; sendStream() }
function setDS(ds) { downsample.value = ds; api.setStream({ downsample: ds }) }

// --- Sync from status ---
watch(() => props.status, (s) => {
  if (!s) return

  // Sync exposure/gain (always sync to show current values)
  if (s.exposure != null) exposure.value = s.exposure
  if (s.analogue_gain != null) analogueGain.value = s.analogue_gain
  if (s.digital_gain != null) digitalGain.value = s.digital_gain

  // Sync AE target
  if (s.ae?.target != null) aeTarget.value = Math.round(s.ae.target)

  // Sync WB gains
  if (s.awb) {
    if (s.awb.r_gain != null) rGain.value = Math.round(s.awb.r_gain * 100)
    if (s.awb.b_gain != null) bGain.value = Math.round(s.awb.b_gain * 100)
  }

  // Sync focus position
  if (s.af) {
    if (s.af.state === 'locked' || s.af.state === 'idle') {
      focusPos.value = s.af.position || s.focus || 0
    }
  }

  // Sync stream params
  if (s.jpeg_quality != null) quality.value = s.jpeg_quality
  if (s.downsample != null) downsample.value = s.downsample
  if (s.target_fps != null) fps.value = s.target_fps

}, { deep: true })

const expMin = computed(() => props.camera?.exposure?.min ?? 0)
const expMax = computed(() => props.camera?.exposure?.max ?? 1000)
const agMin = computed(() => props.camera?.analogue_gain?.min ?? 0)
const agMax = computed(() => props.camera?.analogue_gain?.max ?? 960)
const dgMin = computed(() => props.camera?.digital_gain?.min ?? 0)
const dgMax = computed(() => props.camera?.digital_gain?.max ?? 4096)
const hasAF = computed(() => props.camera?.has_af ?? false)
const afState = computed(() => props.status?.af?.state || 'idle')
const stFps = computed(() => props.status?.fps?.toFixed(1) || '--')
const stJpeg = computed(() => props.status?.jpeg_size ? (props.status.jpeg_size / 1024).toFixed(1) + ' KB' : '--')
const stBright = computed(() => props.status?.ae?.current?.toFixed(1) || '--')
const stUnpack = computed(() => props.status?.timing?.unpack_ms?.toFixed(1) || '--')
const stDemosaic = computed(() => props.status?.timing?.demosaic_ms?.toFixed(1) || '--')
const stJpegEnc = computed(() => props.status?.timing?.jpeg_ms?.toFixed(1) || '--')
</script>

<template>
  <div class="panel">
    <!-- Exposure -->
    <section class="card">
      <button class="card-header" @click="toggle('exposure')">
        <svg class="section-icon" viewBox="0 0 24 24"><path d="M20 18.69L7.84 6.14 5.27 3.49 4 4.76l2.8 2.8c-.52.73-.87 1.59-1.02 2.47h-1.7v2h1.7c.5 2.46 2.56 4.36 5.07 4.68V20h2v-3.29c1.13-.15 2.16-.63 3-1.34l3.66 3.66L20 18.69zM12 16c-2.21 0-4-1.79-4-4 0-.72.21-1.39.56-1.96l5.4 5.4c-.57.35-1.24.56-1.96.56zm8.14-1.97c.5-2.46-2.56-4.36-5.07-4.68V4h-2v5.35c-.09-.01-.17-.02-.26-.02-.37 0-.72.07-1.06.18L7.1 4.76l2.8 2.8c.52-.73.87-1.59 1.02-2.47h1.7v-2h-1.7C10.42 0.63 8.36-1.27 5.85-1.59V-5h-2v3.29c-1.13.15-2.16.63-3 1.34"/></svg>
        <span>Exposure</span>
        <svg class="chevron" :class="{ open: openSections.exposure }" viewBox="0 0 24 24"><path d="M7.41 8.59L12 13.17l4.59-4.58L18 10l-6 6-6-6z"/></svg>
      </button>
      <transition name="collapse">
        <div v-show="openSections.exposure" class="card-body">
          <div class="slider-row">
            <label>Exposure</label>
            <input type="range" :min="expMin" :max="expMax" :value="exposure" @input="onExposure(+$event.target.value)">
            <span class="slider-val">{{ exposure }}</span>
          </div>
          <div class="slider-row">
            <label>Analog</label>
            <input type="range" :min="agMin" :max="agMax" :value="analogueGain" @input="onAnalogueGain(+$event.target.value)">
            <span class="slider-val">{{ analogueGain }}</span>
          </div>
          <div class="slider-row">
            <label>Digital</label>
            <input type="range" :min="dgMin" :max="dgMax" :value="digitalGain" @input="onDigitalGain(+$event.target.value)">
            <span class="slider-val">{{ digitalGain }}</span>
          </div>
          <div class="switch-row">
            <span class="switch" :class="{ on: aeAuto }" @click="toggleAE()">
              <span class="switch-thumb"></span>
            </span>
            <label>Auto Exposure</label>
          </div>
          <div v-if="aeAuto" class="slider-row">
            <label>Target</label>
            <input type="range" min="10" max="200" :value="aeTarget" @input="onAeTarget(+$event.target.value)">
            <span class="slider-val">{{ aeTarget }}</span>
          </div>
        </div>
      </transition>
    </section>

    <!-- Focus -->
    <section v-if="hasAF" class="card">
      <button class="card-header" @click="toggle('focus')">
        <svg class="section-icon" viewBox="0 0 24 24"><path d="M12 8c-2.21 0-4 1.79-4 4s1.79 4 4 4 4-1.79 4-4-1.79-4-4-4zm-7 7H3v4c0 1.1.9 2 2 2h4v-2H5v-4zM5 5h4V3H5c-1.1 0-2 .9-2 2v4h2V5zm14-2h-4v2h4v4h2V5c0-1.1-.9-2-2-2zm0 16h-4v2h4c1.1 0 2-.9 2-2v-4h-2v4z"/></svg>
        <span>Focus</span>
        <span class="badge">{{ afState }}</span>
        <svg class="chevron" :class="{ open: openSections.focus }" viewBox="0 0 24 24"><path d="M7.41 8.59L12 13.17l4.59-4.58L18 10l-6 6-6-6z"/></svg>
      </button>
      <transition name="collapse">
        <div v-show="openSections.focus" class="card-body">
          <div class="slider-row">
            <label>Position</label>
            <input type="range" min="0" max="2047" :value="focusPos" @input="onFocus(+$event.target.value)">
            <span class="slider-val">{{ focusPos }}</span>
          </div>
          <div class="action-row">
            <button class="action-btn accent" @click="triggerAF()">AF Once</button>
            <button class="action-btn" :class="{ accent: cafOn }" @click="toggleCAF()">Continuous</button>
          </div>
        </div>
      </transition>
    </section>

    <!-- White Balance -->
    <section class="card">
      <button class="card-header" @click="toggle('wb')">
        <svg class="section-icon" viewBox="0 0 24 24"><path d="M12 3c-4.97 0-9 4.03-9 9s4.03 9 9 9c.83 0 1.5-.67 1.5-1.5 0-.39-.15-.74-.39-1.01-.23-.26-.38-.61-.38-.99 0-.83.67-1.5 1.5-1.5H16c2.76 0 5-2.24 5-5 0-4.42-4.03-8-9-8zm-5.5 9c-.83 0-1.5-.67-1.5-1.5S5.67 9 6.5 9 8 9.67 8 10.5 7.33 12 6.5 12zm3-4C8.67 8 8 7.33 8 6.5S8.67 5 9.5 5s1.5.67 1.5 1.5S10.33 8 9.5 8zm5 0c-.83 0-1.5-.67-1.5-1.5S13.67 5 14.5 5s1.5.67 1.5 1.5S15.33 8 14.5 8zm3 4c-.83 0-1.5-.67-1.5-1.5S16.67 9 17.5 9s1.5.67 1.5 1.5-.67 1.5-1.5 1.5z"/></svg>
        <span>White Balance</span>
        <svg class="chevron" :class="{ open: openSections.wb }" viewBox="0 0 24 24"><path d="M7.41 8.59L12 13.17l4.59-4.58L18 10l-6 6-6-6z"/></svg>
      </button>
      <transition name="collapse">
        <div v-show="openSections.wb" class="card-body">
          <div class="switch-row">
            <span class="switch" :class="{ on: awbAuto }" @click="toggleAWB()">
              <span class="switch-thumb"></span>
            </span>
            <label>Auto WB</label>
          </div>
          <div class="slider-row">
            <label>R Gain</label>
            <input type="range" min="50" max="400" :value="rGain" @input="onRGain(+$event.target.value)">
            <span class="slider-val">{{ (rGain / 100).toFixed(2) }}</span>
          </div>
          <div class="slider-row">
            <label>B Gain</label>
            <input type="range" min="50" max="400" :value="bGain" @input="onBGain(+$event.target.value)">
            <span class="slider-val">{{ (bGain / 100).toFixed(2) }}</span>
          </div>
        </div>
      </transition>
    </section>

    <!-- Stream -->
    <section class="card">
      <button class="card-header" @click="toggle('stream')">
        <svg class="section-icon" viewBox="0 0 24 24"><path d="M21 3H3c-1.11 0-2 .89-2 2v12c0 1.1.89 2 2 2h5v2h8v-2h5c1.1 0 1.99-.9 1.99-2L23 5c0-1.11-.9-2-2-2zm0 14H3V5h18v12z"/></svg>
        <span>Stream</span>
        <svg class="chevron" :class="{ open: openSections.stream }" viewBox="0 0 24 24"><path d="M7.41 8.59L12 13.17l4.59-4.58L18 10l-6 6-6-6z"/></svg>
      </button>
      <transition name="collapse">
        <div v-show="openSections.stream" class="card-body">
          <div class="switch-row">
            <span class="switch" :class="{ on: showTimestamp }" @click="emit('update:show-timestamp', !showTimestamp)">
              <span class="switch-thumb"></span>
            </span>
            <label>Timestamp</label>
          </div>
          <div v-if="showTimestamp" class="switch-row">
            <span class="switch" :class="{ on: timestampDark }" @click="emit('update:timestamp-dark', !timestampDark)">
              <span class="switch-thumb"></span>
            </span>
            <label>Dark Text</label>
          </div>
          <div class="slider-row">
            <label>Quality</label>
            <input type="range" min="10" max="100" :value="quality" @input="onQuality(+$event.target.value)">
            <span class="slider-val">{{ quality }}</span>
          </div>
          <div class="slider-row">
            <label>FPS</label>
            <input type="range" min="1" max="30" :value="fps" @input="onFps(+$event.target.value)">
            <span class="slider-val">{{ fps }}</span>
          </div>
          <div class="chip-label">Resolution</div>
          <div class="chip-row">
            <button v-for="ds in [1, 2, 4]" :key="ds"
              class="chip" :class="{ active: downsample === ds }"
              @click="setDS(ds)"
            >{{ ds === 1 ? 'Full' : '1/' + ds }}</button>
          </div>
        </div>
      </transition>
    </section>

    <!-- Status -->
    <section class="card">
      <button class="card-header" @click="toggle('status')">
        <svg class="section-icon" viewBox="0 0 24 24"><path d="M19 3H5c-1.1 0-2 .9-2 2v14c0 1.1.9 2 2 2h14c1.1 0 2-.9 2-2V5c0-1.1-.9-2-2-2zM9 17H7v-7h2v7zm4 0h-2V7h2v10zm4 0h-2v-4h2v4z"/></svg>
        <span>Status</span>
        <svg class="chevron" :class="{ open: openSections.status }" viewBox="0 0 24 24"><path d="M7.41 8.59L12 13.17l4.59-4.58L18 10l-6 6-6-6z"/></svg>
      </button>
      <transition name="collapse">
        <div v-show="openSections.status" class="card-body">
          <div class="stat-grid">
            <div class="stat"><span class="stat-label">FPS</span><span class="stat-value">{{ stFps }}</span></div>
            <div class="stat"><span class="stat-label">Frame</span><span class="stat-value">{{ stJpeg }}</span></div>
            <div class="stat"><span class="stat-label">Bright</span><span class="stat-value">{{ stBright }}</span></div>
            <div class="stat"><span class="stat-label">Unpack</span><span class="stat-value">{{ stUnpack }}ms</span></div>
            <div class="stat"><span class="stat-label">Demosaic</span><span class="stat-value">{{ stDemosaic }}ms</span></div>
            <div class="stat"><span class="stat-label">JPEG</span><span class="stat-value">{{ stJpegEnc }}ms</span></div>
          </div>
        </div>
      </transition>
    </section>
  </div>
</template>

<style scoped>
.panel {
  overflow-y: auto; padding: 10px;
  background: var(--bg-base);
  display: flex; flex-direction: column; gap: 8px;
  scrollbar-width: thin;
  scrollbar-color: var(--bg-elevated) transparent;
}

/* --- Card --- */
.card {
  background: var(--bg-surface);
  border-radius: var(--radius);
  border: 1px solid var(--border);
  overflow: hidden;
}
.card-header {
  display: flex; align-items: center; gap: 8px;
  width: 100%; padding: 10px 12px;
  border: none; background: none; color: var(--text-primary);
  cursor: pointer; font-size: 13px; font-weight: 600;
  text-align: left; transition: background 0.15s;
}
.card-header:hover { background: var(--bg-elevated); }
.card-header span:first-of-type { flex: 1; }
.section-icon { width: 16px; height: 16px; fill: var(--accent); flex-shrink: 0; }
.chevron {
  width: 18px; height: 18px; fill: var(--text-muted);
  transition: transform 0.25s ease; flex-shrink: 0;
}
.chevron.open { transform: rotate(180deg); }
.badge {
  font-size: 10px; font-weight: 600; text-transform: uppercase;
  padding: 2px 8px; border-radius: 10px;
  background: var(--accent-dim); color: var(--accent);
  letter-spacing: 0.3px;
}

.card-body { padding: 4px 12px 12px; }

/* --- Collapse animation --- */
.collapse-enter-active,
.collapse-leave-active { transition: all 0.2s ease; overflow: hidden; }
.collapse-enter-from,
.collapse-leave-to { opacity: 0; max-height: 0; padding-top: 0; padding-bottom: 0; }
.collapse-enter-to,
.collapse-leave-from { max-height: 300px; }

/* --- Slider row --- */
.slider-row {
  display: flex; align-items: center; gap: 8px; margin-bottom: 6px;
}
.slider-row label {
  font-size: 11px; min-width: 56px; color: var(--text-secondary);
  font-weight: 500;
}
.slider-row input[type="range"] {
  flex: 1; height: 6px; -webkit-appearance: none; appearance: none;
  background: var(--bg-elevated); border-radius: 3px; outline: none;
}
.slider-row input[type="range"]::-webkit-slider-thumb {
  -webkit-appearance: none; width: 16px; height: 16px; border-radius: 50%;
  background: var(--accent); cursor: pointer; border: 2px solid var(--bg-surface);
  box-shadow: 0 1px 4px rgba(0,0,0,0.3);
}
.slider-row input[type="range"]::-moz-range-thumb {
  width: 16px; height: 16px; border-radius: 50%;
  background: var(--accent); cursor: pointer; border: 2px solid var(--bg-surface);
  box-shadow: 0 1px 4px rgba(0,0,0,0.3);
}
.slider-row input[type="range"]::-moz-range-track {
  height: 6px; background: var(--bg-elevated); border-radius: 3px; border: none;
}
.slider-val {
  font-size: 11px; min-width: 40px; text-align: right;
  color: var(--text-muted); font-variant-numeric: tabular-nums;
  font-weight: 500;
}

/* --- Switch --- */
.switch-row {
  display: flex; align-items: center; gap: 10px; margin-bottom: 8px;
}
.switch-row label { font-size: 12px; color: var(--text-secondary); font-weight: 500; }
.switch {
  width: 38px; height: 22px; border-radius: 11px;
  background: var(--bg-elevated); cursor: pointer;
  position: relative; transition: background 0.25s ease;
  flex-shrink: 0;
}
.switch.on { background: var(--accent); }
.switch-thumb {
  position: absolute; width: 16px; height: 16px;
  border-radius: 50%; background: #fff; top: 3px; left: 3px;
  transition: left 0.25s cubic-bezier(0.4, 0, 0.2, 1);
  box-shadow: 0 1px 3px rgba(0, 0, 0, 0.3);
}
.switch.on .switch-thumb { left: 19px; }

/* --- Action buttons --- */
.action-row { display: flex; gap: 6px; margin-top: 6px; }
.action-btn {
  flex: 1; padding: 7px 0; border: 1px solid var(--border-light); border-radius: 8px;
  background: var(--bg-elevated); color: var(--text-secondary);
  cursor: pointer; font-size: 12px; font-weight: 600;
  transition: all 0.15s;
}
.action-btn:hover { background: var(--bg-surface); color: var(--text-primary); }
.action-btn:active { transform: scale(0.97); }
.action-btn.accent {
  background: var(--accent); border-color: var(--accent); color: #fff;
}
.action-btn.accent:hover { background: var(--accent-hover); }

/* --- Chips --- */
.chip-label { font-size: 11px; color: var(--text-muted); margin: 4px 0; font-weight: 500; }
.chip-row { display: flex; gap: 6px; }
.chip {
  flex: 1; padding: 6px 0; border: 1px solid var(--border-light); border-radius: 8px;
  background: var(--bg-elevated); color: var(--text-secondary);
  cursor: pointer; font-size: 11px; font-weight: 600; text-align: center;
  transition: all 0.15s;
}
.chip:hover { background: var(--bg-surface); }
.chip.active { background: var(--accent-dim); border-color: var(--accent); color: var(--accent); }

/* --- Stats --- */
.stat-grid {
  display: grid; grid-template-columns: 1fr 1fr 1fr;
  gap: 6px;
}
.stat {
  display: flex; flex-direction: column; gap: 2px;
  padding: 8px; background: var(--bg-elevated);
  border-radius: var(--radius-sm); text-align: center;
}
.stat-label { font-size: 9px; color: var(--text-muted); text-transform: uppercase; font-weight: 600; letter-spacing: 0.5px; }
.stat-value { font-size: 13px; color: var(--text-primary); font-variant-numeric: tabular-nums; font-weight: 600; }

/* Mobile touches */
@media (max-width: 767px) {
  .panel { padding: 8px; }
  .slider-row input[type="range"]::-webkit-slider-thumb { width: 20px; height: 20px; }
  .slider-row input[type="range"]::-moz-range-thumb { width: 20px; height: 20px; }
  .action-btn { padding: 9px 0; }
}
</style>
