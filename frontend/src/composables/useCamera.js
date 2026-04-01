import { ref, reactive, onMounted, onUnmounted } from 'vue'
import * as api from '../api.js'

export function useCamera() {
  const cameras = ref([])
  const currentIndex = ref(-1)
  const currentCamera = ref(null)
  const status = reactive({
    fps: 0,
    jpeg_size: 0,
    exposure: 0,
    analogue_gain: 0,
    digital_gain: 0,
    focus: 0,
    downsample: 4,
    rotation: 0,
    ae: null,
    awb: null,
    af: null,
    camera_index: -1,
  })

  const aeAuto = ref(false)
  const awbAuto = ref(true)
  const cafOn = ref(false)
  const connected = ref(true)

  let pollTimer = null
  let wasConnected = true

  async function loadCameras() {
    try {
      cameras.value = await api.getCameras()
    } catch { /* ignore */ }
  }

  async function selectCam(index) {
    await api.selectCamera(index)
    currentIndex.value = index
    currentCamera.value = cameras.value[index] || null
  }

  async function setRotation(rot) {
    await api.setStream({ rotation: rot })
  }

  async function pollStatus() {
    try {
      const s = await api.getStatus()
      Object.assign(status, s)
      connected.value = true

      // Check if backend reconnected
      if (!wasConnected) {
        // Backend just came back online - reload everything
        await loadCameras()
      }

      // Update current camera from status
      if (s.camera_index >= 0) {
        currentIndex.value = s.camera_index
        currentCamera.value = cameras.value[s.camera_index] || null
      }

      // Update AE/AWB state from status
      if (s.ae) aeAuto.value = s.ae.auto
      if (s.awb) awbAuto.value = s.awb.auto
      if (s.af) cafOn.value = s.af.mode === 'continuous'

      wasConnected = true
    } catch {
      connected.value = false
      wasConnected = false
    }
  }

  function startPolling() {
    pollStatus()
    pollTimer = setInterval(pollStatus, 1000)
  }

  function stopPolling() {
    if (pollTimer) clearInterval(pollTimer)
  }

  onMounted(() => {
    loadCameras().then(startPolling)
  })

  onUnmounted(stopPolling)

  return {
    cameras,
    currentIndex,
    currentCamera,
    status,
    aeAuto,
    awbAuto,
    cafOn,
    connected,
    selectCam,
    setRotation,
  }
}