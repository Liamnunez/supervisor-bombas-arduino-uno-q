/**
 * Dashboard JavaScript - Supervisor de Bombas
 * WebSocket client para telemetría en tiempo real
 */

let ws = null;
let reconnectAttempts = 0;
const MAX_RECONNECT = 10;
const RECONNECT_DELAY = 3000;

// Token de autenticación (almacenado en sessionStorage)
let authToken = sessionStorage.getItem('supervisor_token') || null;

function setAuthToken(token) {
    authToken = token;
    if (token) sessionStorage.setItem('supervisor_token', token);
    else sessionStorage.removeItem('supervisor_token');
}

function getAuthHeaders() {
    const headers = { 'Content-Type': 'application/json' };
    if (authToken) headers['Authorization'] = `Bearer ${authToken}`;
    return headers;
}

const stateLabels = {
    0: "NORMAL (RED)",
    1: "GENERADOR",
    2: "EMERGENCIA",
    3: "MANTENIMIENTO"
};

const stateColors = {
    0: "#22c55e",
    1: "#eab308",
    2: "#ef4444",
    3: "#6366f1"
};

function connect() {
    const protocol = window.location.protocol === 'https:' ? 'wss:' : 'ws:';
    const wsUrl = `${protocol}//${window.location.host}/ws`;
    
    ws = new WebSocket(wsUrl);
    
    ws.onopen = () => {
        console.log('[WS] Conectado');
        reconnectAttempts = 0;
        updateConnectionStatus(true);
    };
    
    ws.onmessage = (event) => {
        try {
            const msg = JSON.parse(event.data);
            handleMessage(msg);
        } catch (e) {
            console.error('[WS] Error parse:', e);
        }
    };
    
    ws.onclose = () => {
        console.log('[WS] Desconectado');
        updateConnectionStatus(false);
        scheduleReconnect();
    };
    
    ws.onerror = (err) => {
        console.error('[WS] Error:', err);
    };
}

function scheduleReconnect() {
    if (reconnectAttempts < MAX_RECONNECT) {
        reconnectAttempts++;
        const delay = Math.min(RECONNECT_DELAY * Math.pow(1.5, reconnectAttempts - 1), 30000);
        console.log(`[WS] Reconectando en ${delay}ms (intento ${reconnectAttempts})`);
        setTimeout(connect, delay);
    } else {
        console.error('[WS] Max reconnect attempts reached');
    }
}

function handleMessage(msg) {
    switch (msg.type) {
        case 'init':
            updateStatus(msg.status);
            renderAlerts(msg.alerts || []);
            updateStats(msg.stats);
            break;
        case 'status':
            updateStatus(msg.status);
            break;
        case 'alert':
            addAlert(msg.alert);
            break;
        case 'heartbeat':
            updateStatus(msg.status);
            updateStats(msg.stats);
            break;
        case 'pong':
            break;
        case 'ack':
            console.log('[WS] Command acknowledged:', msg.command);
            break;
    }
}

function updateConnectionStatus(connected) {
    const dot = document.getElementById('conn-dot');
    const text = document.getElementById('conn-text');
    if (connected) {
        dot.className = 'connection-dot connected';
        text.textContent = 'Conectado';
    } else {
        dot.className = 'connection-dot disconnected';
        text.textContent = 'Desconectado';
    }
}

function updateStatus(status) {
    // Estado principal
    const mainState = document.getElementById('main-state');
    mainState.textContent = stateLabels[status.state] || 'DESCONOCIDO';
    mainState.style.background = stateColors[status.state] || '#6366f1';
    
    // Modo HW
    document.getElementById('hw-mode').textContent = status.modo_generador_hw ? 'GENERADOR' : 'RED';
    document.getElementById('hw-mode').style.color = status.modo_generador_hw ? '#eab308' : '#22c55e';
    
    // Mantenimiento
    document.getElementById('maint-mode').textContent = status.modo_mantenimiento ? 'SÍ' : 'NO';
    document.getElementById('maint-mode').style.color = status.modo_mantenimiento ? '#6366f1' : '#64748b';
    
    // Emergencia
    document.getElementById('emergency-state').textContent = status.emergencia_activa ? 'ACTIVA' : 'NO';
    document.getElementById('emergency-state').style.color = status.emergencia_activa ? '#ef4444' : '#64748b';
    
    // Métricas
    const uptimeHours = Math.floor(status.uptime_ms / 3600000);
    const uptimeMins = Math.floor((status.uptime_ms % 3600000) / 60000);
    document.getElementById('metrics').innerHTML = `
        <div class="metric">
            <div class="metric-value">${uptimeHours}h ${uptimeMins}m</div>
            <div class="metric-label">Uptime</div>
        </div>
        <div class="metric">
            <div class="metric-value">${status.nivel_agua_pct}%</div>
            <div class="metric-label">Nivel Agua</div>
        </div>
        <div class="metric">
            <div class="metric-value">${status.bombas.filter(b => b.running).length}/${status.bombas.length}</div>
            <div class="metric-label">Bombas Activas</div>
        </div>
    `;
    
    // Nivel de agua
    updateLevelBar(status.nivel_agua_pct, status.sensor_ok);
    
    // Bombas
    renderBombas(status.bombas);
    
    // Controles - habilitar/deshabilitar según estado
    updateControls(status);
}

function updateLevelBar(pct, sensorOk) {
    const fill = document.getElementById('level-fill');
    fill.style.width = `${pct}%`;
    fill.textContent = `${pct}%`;
    
    fill.className = 'level-fill';
    if (!sensorOk) {
        fill.classList.add('critical');
        fill.textContent = 'SENSOR ERROR';
    } else if (pct <= 10) {
        fill.classList.add('critical');
    } else if (pct <= 20) {
        fill.classList.add('low');
    } else {
        fill.classList.add('normal');
    }
    
    document.getElementById('sensor-status').textContent = sensorOk ? 'OK' : 'ERROR / FUERA DE RANGO';
    document.getElementById('sensor-status').style.color = sensorOk ? '#22c55e' : '#ef4444';
}

function renderBombas(bombas) {
    const container = document.getElementById('bombas-container');
    container.innerHTML = bombas.map((b, i) => `
        <div class="bomba">
            <div class="bomba-info">
                <span class="bomba-name">Bomba ${i + 1} (7.5 kW)</span>
                <div class="bomba-indicators">
                    <span class="indicator ${b.plc_order ? 'on' : 'off'}">PLC: ${b.plc_order ? 'ON' : 'OFF'}</span>
                    <span class="indicator ${b.feedback ? 'on' : 'off'}">FB: ${b.feedback ? 'ON' : 'OFF'}</span>
                    <span class="indicator ${b.relay_closed ? 'on' : 'off'}">REL: ${b.relay_closed ? 'CERRADO' : 'ABIERTO'}</span>
                    ${b.fault_count > 0 ? `<span class="indicator fault">FAULTS: ${b.fault_count}</span>` : ''}
                    ${b.mismatch ? `<span class="indicator fault">MISMATCH</span>` : ''}
                </div>
            </div>
            <span class="bomba-state ${b.running ? 'running' : (b.fault_count > 0 ? 'fault' : 'stopped')}">
                ${b.running ? '● FUNCIONANDO' : (b.fault_count > 0 ? '⚠ FALLO' : '○ DETENIDA')}
            </span>
        </div>
    `).join('');
}

function updateControls(status) {
    const isEmergency = status.emergencia_activa;
    const isMaintenance = status.modo_mantenimiento;
    const isGenerator = status.modo_generador_hw;
    
    document.getElementById('btn-mode-red').disabled = isEmergency || isMaintenance;
    document.getElementById('btn-mode-gen').disabled = isEmergency || isMaintenance;
    document.getElementById('btn-maint-on').disabled = isEmergency || isMaintenance;
    document.getElementById('btn-maint-off').disabled = isEmergency || !isMaintenance;
    document.getElementById('btn-mode-red').classList.toggle('active', !isGenerator && !isEmergency && !isMaintenance);
    document.getElementById('btn-mode-gen').classList.toggle('active', isGenerator && !isEmergency && !isMaintenance);
}

function addAlert(alert) {
    const container = document.getElementById('alerts-container');
    const div = document.createElement('div');
    div.className = `alert ${alert.level}`;
    const time = new Date(alert.timestamp).toLocaleTimeString();
    div.innerHTML = `
        <span class="alert-time">${time}</span>
        <span class="alert-source">[${alert.source}]</span>
        <span class="alert-message">${alert.message}</span>
    `;
    container.insertBefore(div, container.firstChild);
    
    // Limitar a 50 alertas visibles
    while (container.children.length > 50) {
        container.removeChild(container.lastChild);
    }
}

function renderAlerts(alerts) {
    const container = document.getElementById('alerts-container');
    container.innerHTML = alerts.slice(-50).reverse().map(alert => `
        <div class="alert ${alert.level}">
            <span class="alert-time">${new Date(alert.timestamp).toLocaleTimeString()}</span>
            <span class="alert-source">[${alert.source}]</span>
            <span class="alert-message">${alert.message}</span>
        </div>
    `).join('');
}

function updateStats(stats) {
    // Could add stats display if needed
    console.log('[STATS]', stats);
}

function sendCmd(command, params) {
    if (ws && ws.readyState === WebSocket.OPEN) {
        const msg = { type: 'command', command, params };
        if (authToken) msg.token = authToken;
        ws.send(JSON.stringify(msg));
    } else {
        // Fallback HTTP
        fetch('/api/command', {
            method: 'POST',
            headers: getAuthHeaders(),
            body: JSON.stringify({ command, params })
        }).then(r => r.json()).then(console.log).catch(console.error);
    }
}

// Iniciar: verificar token y mostrar login si no hay
document.addEventListener('DOMContentLoaded', () => {
    if (!authToken) {
        showLogin();
    } else {
        // Verificar token con el servidor
        verifyToken();
    }
    connect();
});

function showLogin() {
    document.getElementById('login-modal').style.display = 'flex';
}

function hideLogin() {
    document.getElementById('login-modal').style.display = 'none';
}

async function handleLogin(event) {
    event.preventDefault();
    const password = document.getElementById('login-password').value;
    const errorDiv = document.getElementById('login-error');
    
    try {
        const res = await fetch('/api/auth/login', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ password })
        });
        if (!res.ok) throw new Error('Credenciales inválidas');
        const data = await res.json();
        setAuthToken(data.token);
        hideLogin();
        console.log('[AUTH] Login OK, role:', data.role);
    } catch (e) {
        errorDiv.textContent = e.message;
        errorDiv.style.display = 'block';
    }
}

async function verifyToken() {
    try {
        const res = await fetch('/api/status', { headers: getAuthHeaders() });
        if (!res.ok) {
            setAuthToken(null);
            showLogin();
        }
    } catch (e) {
        console.warn('[AUTH] Token verify failed:', e);
        setAuthToken(null);
        showLogin();
    }
}

// Ping periódico para mantener conexión viva
setInterval(() => {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: 'ping' }));
    }
}, 30000);

// ============================================================
// Charts (Chart.js)
// ============================================================
let chartLevel = null;
let chartCurrent = null;
const MAX_CHART_POINTS = 288; // 24h a 5 min = 288 puntos

function initCharts() {
    const commonOptions = {
        responsive: true,
        maintainAspectRatio: false,
        animation: { duration: 300 },
        interaction: { mode: 'index', intersect: false },
        scales: {
            x: { type: 'time', time: { unit: 'minute', displayFormats: { minute: 'HH:mm' } }, grid: { color: '#334155' }, ticks: { color: '#94a3b8', maxTicksLimit: 12 } },
            y: { beginAtZero: true, grid: { color: '#334155' }, ticks: { color: '#94a3b8' } }
        },
        plugins: { legend: { display: false }, tooltip: { backgroundColor: '#1e293b', titleColor: '#e2e8f0', bodyColor: '#94a3b8', borderColor: '#334155', borderWidth: 1 } }
    };

    chartLevel = new Chart(document.getElementById('chart-level'), {
        type: 'line',
        data: { datasets: [{ label: 'Nivel %', data: [], borderColor: '#22c55e', backgroundColor: 'rgba(34,197,94,0.1)', fill: true, tension: 0.3, pointRadius: 0, pointHoverRadius: 4 }] },
        options: { ...commonOptions, scales: { ...commonOptions.scales, y: { ...commonOptions.scales.y, max: 100, title: { display: true, text: '%', color: '#94a3b8' } } } }
    });

    chartCurrent = new Chart(document.getElementById('chart-current'), {
        type: 'line',
        data: { datasets: [{ label: 'Corriente A', data: [], borderColor: '#f59e0b', backgroundColor: 'rgba(245,158,11,0.1)', fill: true, tension: 0.3, pointRadius: 0, pointHoverRadius: 4 }] },
        options: { ...commonOptions, scales: { ...commonOptions.scales, y: { ...commonOptions.scales.y, title: { display: true, text: 'Amperios', color: '#94a3b8' } } } }
    });
}

function addChartPoint(chart, timestamp, value) {
    const ms = new Date(timestamp).getTime();
    chart.data.datasets[0].data.push({ x: ms, y: value });
    if (chart.data.datasets[0].data.length > MAX_CHART_POINTS) {
        chart.data.datasets[0].data.shift();
    }
    chart.update('none');
}

function loadChartHistory(hours = 24) {
    fetch(`/api/history?hours=${hours}&limit=1000`, { headers: getAuthHeaders() })
        .then(r => r.json())
        .then(alerts => {
            // Reconstruir series temporales desde alertas (aproximado)
            // En producción idealmente habría endpoint /api/metrics/history
            alerts.forEach(a => {
                if (a.source === 'NIVEL' && a.codigo === 0x30) {
                    // Nivel - no tenemos valor en alerta, usar placeholder
                }
            });
        })
        .catch(console.error);
}

// Llenar gráficas con datos en tiempo real desde status
function updateChartsFromStatus(status) {
    const now = new Date();
    if (status.nivel_agua_pct !== undefined && status.sensor_ok) {
        addChartPoint(chartLevel, now, status.nivel_agua_pct);
    }
    if (status.corriente_a !== undefined) {
        addChartPoint(chartCurrent, now, status.corriente_a);
    }
}

// ============================================================
// CSV Export
// ============================================================
async function exportCSV(hours = 24) {
    showToast('Preparando CSV...', 'info');
    try {
        const res = await fetch(`/api/export/csv?hours=${hours}`, { headers: getAuthHeaders() });
        if (!res.ok) throw new Error('Error exportando');
        const blob = await res.blob();
        const url = URL.createObjectURL(blob);
        const a = document.createElement('a');
        a.href = url;
        a.download = `alertas_${new Date().toISOString().slice(0,16).replace('T','_')}.csv`;
        a.click();
        URL.revokeObjectURL(url);
        showToast('CSV descargado', 'success');
    } catch (e) {
        showToast('Error: ' + e.message, 'error');
    }
}

// ============================================================
// Threshold Editor
// ============================================================
function showThresholdEditor() {
    // Cargar valores actuales
    fetch('/api/config/thresholds', { headers: getAuthHeaders() })
        .then(r => r.json())
        .then(cfg => {
            document.getElementById('th-nivel-bajo').value = cfg.nivel_critico_bajo;
            document.getElementById('th-nivel-alto').value = cfg.nivel_critico_alto;
            document.getElementById('th-feedback-timeout').value = cfg.feedback_timeout_s;
            document.getElementById('th-heartbeat-timeout').value = cfg.heartbeat_timeout_s;
            document.getElementById('th-dedup-window').value = cfg.dedup_window_s;
            document.getElementById('threshold-modal').style.display = 'flex';
        })
        .catch(e => showToast('Error cargando umbrales: ' + e.message, 'error'));
}

function hideThresholdEditor() {
    document.getElementById('threshold-modal').style.display = 'none';
}

async function saveThresholds(event) {
    event.preventDefault();
    const payload = {
        nivel_critico_bajo: parseInt(document.getElementById('th-nivel-bajo').value),
        nivel_critico_alto: parseInt(document.getElementById('th-nivel-alto').value),
        feedback_timeout_s: parseInt(document.getElementById('th-feedback-timeout').value),
        heartbeat_timeout_s: parseInt(document.getElementById('th-heartbeat-timeout').value),
        dedup_window_s: parseInt(document.getElementById('th-dedup-window').value),
    };
    try {
        const res = await fetch('/api/config/thresholds', {
            method: 'POST',
            headers: getAuthHeaders(),
            body: JSON.stringify(payload)
        });
        if (!res.ok) throw new Error('Error guardando');
        showToast('Umbrales actualizados', 'success');
        hideThresholdEditor();
    } catch (e) {
        showToast('Error: ' + e.message, 'error');
    }
}

// ============================================================
// Toast notifications
// ============================================================
function showToast(message, type = 'info') {
    const container = document.getElementById('toast-container') || createToastContainer();
    const toast = document.createElement('div');
    toast.className = `toast ${type}`;
    toast.textContent = message;
    container.appendChild(toast);
    setTimeout(() => { toast.style.opacity = '0'; setTimeout(() => toast.remove(), 300); }, 3000);
}

function createToastContainer() {
    const container = document.createElement('div');
    container.id = 'toast-container';
    container.style.position = 'fixed';
    container.style.bottom = '20px';
    container.style.right = '20px';
    container.style.zIndex = '1100';
    container.style.display = 'flex';
    container.style.flexDirection = 'column';
    container.style.gap = '8px';
    document.body.appendChild(container);
    return container;
}

// ============================================================
// Init on DOMContentLoaded
// ============================================================
document.addEventListener('DOMContentLoaded', () => {
    initCharts();
    if (!authToken) {
        showLogin();
    } else {
        verifyToken();
    }
    connect();
});

// Modificar updateStatus para actualizar gráficas
const originalUpdateStatus = updateStatus;
function updateStatus(status) {
    originalUpdateStatus(status);
    updateChartsFromStatus(status);
}