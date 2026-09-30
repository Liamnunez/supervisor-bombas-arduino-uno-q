/**
 * Dashboard JavaScript - Supervisor de Bombas
 * WebSocket client para telemetría en tiempo real
 */

let ws = null;
let reconnectAttempts = 0;
const MAX_RECONNECT = 10;
const RECONNECT_DELAY = 3000;

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
        ws.send(JSON.stringify({ type: 'command', command, params }));
    } else {
        // Fallback HTTP
        fetch('/api/command', {
            method: 'POST',
            headers: { 'Content-Type': 'application/json' },
            body: JSON.stringify({ command, params })
        }).then(r => r.json()).then(console.log).catch(console.error);
    }
}

// Iniciar conexión
connect();

// Ping periódico para mantener conexión viva
setInterval(() => {
    if (ws && ws.readyState === WebSocket.OPEN) {
        ws.send(JSON.stringify({ type: 'ping' }));
    }
}, 30000);