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

      if (currentIndex.value < 0 && s.camera_index >= 0) {
        currentIndex.value = s.camera_index
        currentCamera.value = cameras.value[s.camera_index] || null
      }
    } catch {
      connected.value = false
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