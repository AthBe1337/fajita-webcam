import { ref } from 'vue'

export function useRecorder() {
  const recording = ref(false)
  const recordingTime = ref(0)

  let mediaRecorder = null
  let chunks = []
  let timer = null
  let startTime = 0

  function startRecording(videoEl) {
    if (!videoEl || recording.value) return

    // Create a canvas to capture the video element (which may be an <img> for MJPEG)
    const canvas = document.createElement('canvas')
    const ctx = canvas.getContext('2d')
    canvas.width = videoEl.naturalWidth || videoEl.width || 640
    canvas.height = videoEl.naturalHeight || videoEl.height || 480

    const stream = canvas.captureStream(15)

    // Draw frames from the img element to the canvas
    let drawTimer = setInterval(() => {
      canvas.width = videoEl.naturalWidth || videoEl.width || 640
      canvas.height = videoEl.naturalHeight || videoEl.height || 480
      ctx.drawImage(videoEl, 0, 0, canvas.width, canvas.height)
    }, 1000 / 15)

    const mimeType = MediaRecorder.isTypeSupported('video/webm;codecs=vp9')
      ? 'video/webm;codecs=vp9'
      : 'video/webm'

    mediaRecorder = new MediaRecorder(stream, { mimeType })
    chunks = []

    mediaRecorder.ondataavailable = (e) => {
      if (e.data.size > 0) chunks.push(e.data)
    }

    mediaRecorder.onstop = () => {
      clearInterval(drawTimer)
      clearInterval(timer)
      const blob = new Blob(chunks, { type: mimeType })
      const url = URL.createObjectURL(blob)
      const a = document.createElement('a')
      a.href = url
      a.download = `fajita-${formatTimestamp()}.webm`
      a.click()
      URL.revokeObjectURL(url)
      recording.value = false
      recordingTime.value = 0
    }

    mediaRecorder.start(1000)
    recording.value = true
    startTime = Date.now()

    timer = setInterval(() => {
      recordingTime.value = Math.floor((Date.now() - startTime) / 1000)
    }, 1000)
  }

  function stopRecording() {
    if (mediaRecorder && mediaRecorder.state !== 'inactive') {
      mediaRecorder.stop()
    }
  }

  function toggleRecording(videoEl) {
    if (recording.value) {
      stopRecording()
    } else {
      startRecording(videoEl)
    }
  }

  return { recording, recordingTime, toggleRecording }
}

function formatTimestamp() {
  const d = new Date()
  return `${d.getFullYear()}${pad(d.getMonth() + 1)}${pad(d.getDate())}-${pad(d.getHours())}${pad(d.getMinutes())}${pad(d.getSeconds())}`
}

function pad(n) { return n.toString().padStart(2, '0') }
