// State
let cameras = [];
let currentCamera = -1;
let status = {};
let awbAuto = true;
let aeAuto = false;
let cafOn = false;

// Debounce helper
function debounce(fn, ms) {
    let timer;
    return (...args) => { clearTimeout(timer); timer = setTimeout(() => fn(...args), ms); };
}

// API helpers
async function api(method, path, body) {
    const opts = { method };
    if (body) {
        opts.headers = { 'Content-Type': 'application/json' };
        opts.body = JSON.stringify(body);
    }
    const r = await fetch(path, opts);
    return r.json();
}

// Initialize
async function init() {
    cameras = await api('GET', '/api/cameras');
    const container = document.getElementById('cam-buttons');
    cameras.forEach((cam, i) => {
        const btn = document.createElement('button');
        btn.className = 'cam-btn';
        btn.textContent = cam.name;
        btn.onclick = () => selectCamera(i);
        btn.id = 'cam-btn-' + i;
        container.appendChild(btn);
    });
    // Start status polling
    pollStatus();
    setInterval(pollStatus, 1000);
}

async function selectCamera(index) {
    document.querySelectorAll('.cam-btn').forEach(b => b.classList.remove('active'));
    document.getElementById('cam-btn-' + index).classList.add('active');

    await api('POST', '/api/camera/select', { index });
    currentCamera = index;

    // Update slider ranges
    const cam = cameras[index];
    setSlider('exposure', cam.exposure.min, cam.exposure.max, cam.exposure.default);
    setSlider('analogue_gain', cam.analogue_gain.min, cam.analogue_gain.max, cam.analogue_gain.default);
    setSlider('digital_gain', cam.digital_gain.min, cam.digital_gain.max, cam.digital_gain.default);

    // Show/hide focus section
    document.getElementById('focus-section').style.display = cam.has_af ? '' : 'none';

    // Refresh stream
    const img = document.getElementById('stream');
    img.src = '';
    setTimeout(() => { img.src = '/stream/mjpeg?' + Date.now(); }, 200);
}

function setSlider(id, min, max, val) {
    const el = document.getElementById(id);
    el.min = min;
    el.max = max;
    el.value = val;
    document.getElementById(id + '-val').textContent = val;
}

// Sensor control handlers
const sendControl = debounce((key, val) => {
    api('POST', '/api/control', { [key]: parseInt(val) });
}, 100);

['exposure', 'analogue_gain', 'digital_gain'].forEach(key => {
    const el = document.getElementById(key);
    el.addEventListener('input', () => {
        document.getElementById(key + '-val').textContent = el.value;
        sendControl(key, el.value);
    });
});

// Focus slider
document.getElementById('focus').addEventListener('input', function() {
    document.getElementById('focus-val').textContent = this.value;
    debounce(() => api('POST', '/api/focus', { position: parseInt(this.value) }), 100)();
});

// Stream quality controls
const sendStream = debounce(() => {
    api('POST', '/api/stream', {
        quality: parseInt(document.getElementById('quality').value),
        fps: parseInt(document.getElementById('fps').value),
    });
}, 300);

document.getElementById('quality').addEventListener('input', function() {
    document.getElementById('quality-val').textContent = this.value;
    sendStream();
});
document.getElementById('fps').addEventListener('input', function() {
    document.getElementById('fps-val').textContent = this.value;
    sendStream();
});

function setDS(ds) {
    document.querySelectorAll('.ds-btn').forEach((b, i) => {
        b.classList.toggle('active', [1, 2, 4][i] === ds);
    });
    api('POST', '/api/stream', { downsample: ds });
    // Refresh stream after pipeline restart
    setTimeout(() => {
        const img = document.getElementById('stream');
        img.src = '/stream/mjpeg?' + Date.now();
    }, 500);
}

// AWB toggle
function toggleAWB() {
    awbAuto = !awbAuto;
    document.getElementById('awb-toggle').classList.toggle('on', awbAuto);
    api('POST', '/api/awb', { auto: awbAuto });
}

// Manual WB gains
const sendWB = debounce(() => {
    if (awbAuto) return;
    api('POST', '/api/awb', {
        auto: false,
        r_gain: parseInt(document.getElementById('r_gain').value) / 100,
        b_gain: parseInt(document.getElementById('b_gain').value) / 100,
    });
}, 150);

['r_gain', 'b_gain'].forEach(key => {
    document.getElementById(key).addEventListener('input', function() {
        document.getElementById(key + '-val').textContent = (this.value / 100).toFixed(2);
        sendWB();
    });
});

// AE toggle
function toggleAE() {
    aeAuto = !aeAuto;
    document.getElementById('ae-toggle').classList.toggle('on', aeAuto);
    document.getElementById('ae-target-row').style.display = aeAuto ? '' : 'none';
    api('POST', '/api/ae', { auto: aeAuto });
}

document.getElementById('ae-target').addEventListener('input', function() {
    document.getElementById('ae-target-val').textContent = this.value;
    debounce(() => api('POST', '/api/ae', { target: parseFloat(this.value) }), 200)();
});

// AF
function triggerAF() {
    api('POST', '/api/focus', { mode: 'oneshot' });
}

function toggleCAF() {
    cafOn = !cafOn;
    document.getElementById('caf-btn').classList.toggle('primary', cafOn);
    api('POST', '/api/focus', { mode: cafOn ? 'continuous' : 'manual' });
}

// Status polling
async function pollStatus() {
    try {
        status = await api('GET', '/api/status');

        // Update active camera button
        if (status.camera_index >= 0 && currentCamera < 0) {
            currentCamera = status.camera_index;
            const btn = document.getElementById('cam-btn-' + currentCamera);
            if (btn) btn.classList.add('active');

            // Set slider ranges for initial camera
            const cam = cameras[currentCamera];
            if (cam) {
                setSlider('exposure', cam.exposure.min, cam.exposure.max, status.exposure);
                setSlider('analogue_gain', cam.analogue_gain.min, cam.analogue_gain.max, status.analogue_gain);
                setSlider('digital_gain', cam.digital_gain.min, cam.digital_gain.max, status.digital_gain);
                document.getElementById('focus-section').style.display = cam.has_af ? '' : 'none';
            }
        }

        // Update status display
        document.getElementById('st-fps').textContent = status.fps?.toFixed(1) || '--';
        document.getElementById('st-jpeg').textContent =
            status.jpeg_size ? (status.jpeg_size / 1024).toFixed(1) + ' KB' : '--';
        document.getElementById('st-bright').textContent =
            status.ae?.current?.toFixed(1) || '--';

        // Update slider values from server (when in auto mode)
        if (aeAuto && status.exposure != null) {
            document.getElementById('exposure').value = status.exposure;
            document.getElementById('exposure-val').textContent = status.exposure;
            document.getElementById('analogue_gain').value = status.analogue_gain;
            document.getElementById('analogue_gain-val').textContent = status.analogue_gain;
        }

        // Update AWB gain display
        if (awbAuto && status.awb) {
            document.getElementById('r_gain').value = Math.round(status.awb.r_gain * 100);
            document.getElementById('r_gain-val').textContent = status.awb.r_gain.toFixed(2);
            document.getElementById('b_gain').value = Math.round(status.awb.b_gain * 100);
            document.getElementById('b_gain-val').textContent = status.awb.b_gain.toFixed(2);
        }

        // AF state
        if (status.af) {
            document.getElementById('af-state').textContent = status.af.state;
            if (status.af.state === 'locked' || status.af.state === 'idle') {
                document.getElementById('focus').value = status.af.position || status.focus;
                document.getElementById('focus-val').textContent = status.af.position || status.focus;
            }
        }

        // Overlay
        const cam = cameras[currentCamera];
        const ds = status.downsample || 4;
        const resW = cam ? Math.floor(cam.width / ds) : '?';
        const resH = cam ? Math.floor(cam.height / ds) : '?';
        document.getElementById('overlay').textContent =
            `${resW}x${resH} | ${status.fps?.toFixed(1) || '--'} fps | ${(status.jpeg_size/1024)?.toFixed(0) || '--'} KB`;

    } catch (e) {
        document.getElementById('overlay').textContent = 'Disconnected';
    }
}

init();
