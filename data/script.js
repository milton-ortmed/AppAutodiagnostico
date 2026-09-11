let isBusy = false;

// Diccionario de interpretación de códigos de error reportados por el ESP32
const ERROR_DEFINITIONS = {
  0: {
    label: "OPERACIÓN NORMAL (OK)",
    badgeClass: "badge-ok",
    desc: "El componente funciona correctamente dentro de los rangos de corriente esperados."
  },
  1: {
    label: "PIEZAS QUEMADAS (Cód. 1)",
    badgeClass: "badge-warn",
    desc: "Corriente inferior al límite mínimo permitido. Indica posibles LEDs dañados o líneas abiertas."
  },
  2: {
    label: "CORTOCIRCUITO (Cód. 2)",
    badgeClass: "badge-danger",
    desc: "Corriente superior al límite máximo permitido. Indica posible sobrecorriente o cortocircuito."
  },
  5: {
    label: "AUSENCIA DE COMPONENTE (Cód. 5)",
    badgeClass: "badge-danger",
    desc: "No se detectó aumento de corriente sobre el consumo pasivo. Componente desconectado o sin alimentación."
  }
};

function getErrorDetails(code) {
  if (ERROR_DEFINITIONS.hasOwnProperty(code)) {
    return ERROR_DEFINITIONS[code];
  }
  return {
    label: "CÓDIGO DESCONOCIDO (" + code + ")",
    badgeClass: "badge-unknown",
    desc: "Código de diagnóstico no especificado."
  };
}

function setButtonsState(disabled) {
  const btnCalib = document.getElementById('btn_calib');
  const btnDiag = document.getElementById('btn_diag');
  if (btnCalib) btnCalib.disabled = disabled;
  if (btnDiag) btnDiag.disabled = disabled;
}

// --- ACTUALIZACIÓN DE MONITOR EN TIEMPO REAL ---
function updateMonitor() {
  if (isBusy) return; // No interrumpir si hay calibración o autodiagnóstico en curso

  fetch('/data')
    .then(r => r.json())
    .then(data => {
      if (data.mA !== undefined) {
        document.getElementById('ma_val').innerText = data.mA.toFixed(2) + ' mA';
      }
      if (data.mV !== undefined) {
        document.getElementById('v_val').innerText = (data.mV / 1000.0).toFixed(2) + ' V';
      }
    })
    .catch(err => {
      // Error silencioso en polling
    });
}

function waitForJsonFile(url, successCallback, failureMessage) {
  const startedAt = Date.now();
  const timeoutMs = 30000;

  function poll() {
    fetch(url, { cache: 'no-store' })
      .then(r => {
        if (!r.ok) throw new Error('HTTP error ' + r.status);
        return r.json();
      })
      .then(data => {
        successCallback(data);
      })
      .catch(err => {
        const elapsed = Date.now() - startedAt;
        if (elapsed >= timeoutMs) {
          const statusMsg = document.getElementById('status_msg');
          statusMsg.className = 'status-box status-error';
          statusMsg.innerText = '❌ ' + failureMessage + ' ' + err.message;
          isBusy = false;
          setButtonsState(false);
          return;
        }

        const statusMsg = document.getElementById('status_msg');
        statusMsg.className = 'status-box status-loading';
        statusMsg.style.display = 'block';
        statusMsg.innerText = '📶 Apagando Wi-Fi para medición de precisión... Esperando reconexión...';
        setTimeout(poll, 1500);
      });
  }

  poll();
}

// --- CALIBRACIÓN ---
function startCalibration() {
  if (isBusy) return;
  isBusy = true;
  setButtonsState(true);

  const statusMsg = document.getElementById('status_msg');
  statusMsg.className = 'status-box status-loading';
  statusMsg.style.display = 'block';
  statusMsg.innerText = '⏳ Iniciando calibración...';

  fetch('/calibrate')
    .then(r => {
      if (!r.ok) throw new Error('HTTP error ' + r.status);
      return r.json();
    })
    .then(data => {
      if (!data || data.status !== 'in_progress') {
        throw new Error('La respuesta del ESP32 no confirmó inicio de la prueba.');
      }

      statusMsg.innerText = '📶 Apagando Wi-Fi para medición de precisión... Esperando reconexión...';
      waitForJsonFile('/calibration.json', (calData) => {
        statusMsg.className = 'status-box status-success';
        statusMsg.innerText = '✅ ' + (calData.message || 'Calibración completada y guardada.');
        renderCalibration(calData);
        isBusy = false;
        setButtonsState(false);
      }, 'No se pudo completar la calibración');
    })
    .catch(err => {
      console.error(err);
      statusMsg.className = 'status-box status-error';
      statusMsg.innerText = '❌ Error al ejecutar calibración: ' + err.message;
      isBusy = false;
      setButtonsState(false);
    });
}

function renderCalibration(data) {
  const calibCard = document.getElementById('calib_card');
  const calibList = document.getElementById('calib_list');
  if (!calibCard || !calibList) return;

  calibList.innerHTML = '';

  if (data.passive_mA !== undefined) {
    const passiveDiv = document.createElement('div');
    passiveDiv.className = 'info-subnote';
    passiveDiv.innerText = 'Consumo Pasivo Base: ' + Number(data.passive_mA).toFixed(2) + ' mA';
    calibList.appendChild(passiveDiv);
  }

  if (data.voltage_V !== undefined) {
    const voltageDiv = document.createElement('div');
    voltageDiv.className = 'info-subnote';
    voltageDiv.innerText = 'Voltaje Medido: ' + Number(data.voltage_V).toFixed(2) + ' V';
    calibList.appendChild(voltageDiv);
  }

  if (Array.isArray(data.components)) {
    data.components.forEach(m => {
      const conn = m.connection || {};
      const cal = m.calibration || {};
      const addrText = conn.addr ? ` (${conn.addr})` : '';
      const nominal = Number(cal.nominal_mA ?? 0);
      const minVal = Number(cal.min_mA ?? 0);
      const maxVal = Number(cal.max_mA ?? 0);

      const item = document.createElement('div');
      item.className = 'diag-card-item';
      item.innerHTML = `
        <div class="diag-header">
          <span class="comp-title">${m.comp_id}${addrText}</span>
          <span class="badge badge-ok">CALIBRADO</span>
        </div>
        <div class="diag-metrics">
          <div><strong>Nominal:</strong> ${nominal.toFixed(2)} mA</div>
          <div><strong>Rango Válido:</strong> [${minVal.toFixed(2)} - ${maxVal.toFixed(2)} mA]</div>
          <div><strong>Conexión:</strong> ${conn.bus || 'N/A'}</div>
        </div>
      `;
      calibList.appendChild(item);
    });
  }

  calibCard.style.display = 'block';
}

// --- AUTODIAGNÓSTICO ---
function startAutodiag() {
  if (isBusy) return;
  isBusy = true;
  setButtonsState(true);

  const statusMsg = document.getElementById('status_msg');
  statusMsg.className = 'status-box status-loading';
  statusMsg.style.display = 'block';
  statusMsg.innerText = '🔬 Iniciando autodiagnóstico...';

  fetch('/autodiag')
    .then(r => {
      if (!r.ok) throw new Error('HTTP error ' + r.status);
      return r.json();
    })
    .then(data => {
      if (!data || data.status !== 'in_progress') {
        throw new Error('La respuesta del ESP32 no confirmó inicio de la prueba.');
      }

      statusMsg.innerText = '📶 Apagando Wi-Fi para medición de precisión... Esperando reconexión...';
      waitForJsonFile('/diagnostics.json', (diagData) => {
        statusMsg.className = 'status-box status-success';
        statusMsg.innerText = '✅ Autodiagnóstico finalizado. Archivo /diagnostics.json generado.';
        renderDiagnostics(diagData);
        isBusy = false;
        setButtonsState(false);
      }, 'No se pudo completar el autodiagnóstico');
    })
    .catch(err => {
      console.error(err);
      statusMsg.className = 'status-box status-error';
      statusMsg.innerText = '❌ Error en autodiagnóstico: ' + err.message;
      isBusy = false;
      setButtonsState(false);
    });
}

function renderDiagnostics(data) {
  const diagCard = document.getElementById('diag_card');
  const banner = document.getElementById('general_status_banner');
  const diagList = document.getElementById('diag_list');
  if (!diagCard || !banner || !diagList) return;

  diagList.innerHTML = '';

  if (data.status === 'Insufficient_voltage') {
    banner.className = 'status-banner banner-fail';
    banner.innerHTML = '<strong>⚠️ ESTADO DEL DISPOSITIVO: VOLTAJE INSUFICIENTE</strong><br><span>El sistema no cuenta con el voltaje mínimo requerido para ejecutar el diagnóstico.</span>';

    const voltage = Number(data.voltage_V ?? 0);
    const item = document.createElement('div');
    item.className = 'diag-card-item item-fail';
    item.innerHTML = `
      <div class="diag-header">
        <span class="comp-title">SensyWall</span>
        <span class="badge badge-danger">VOLTAGE LOW</span>
      </div>
      <div class="diag-metrics">
        <div><strong>Voltaje Medido:</strong> <span class="val-highlight">${voltage.toFixed(2)} V</span></div>
      </div>
      <div class="err-description">El diagnóstico no se puede ejecutar porque la alimentación está por debajo del mínimo permitido.</div>
    `;
    diagList.appendChild(item);
    diagCard.style.display = 'block';
    return;
  }

  const isOk = (data.status === 'ok');
  banner.className = 'status-banner ' + (isOk ? 'banner-ok' : 'banner-fail');
  banner.innerHTML = isOk
    ? '<strong>✅ ESTADO DEL SISTEMA: OK</strong><br><span>Todos los módulos operan dentro de los parámetros esperados.</span>'
    : '<strong>⚠️ ESTADO DEL SISTEMA: FALLA DETECTADA</strong><br><span>Uno o más módulos presentan anomalías de consumo.</span>';

  const components = Array.isArray(data.components) ? data.components : [];
  components.forEach(comp => {
    const conn = comp.connection || {};
    const cal = comp.calibration || {};
    const diag = comp.diagnostic || {};
    const errCode = Number(diag.err_code ?? 99);
    const errInfo = getErrorDetails(errCode);
    const item = document.createElement('div');
    item.className = 'diag-card-item ' + (errCode === 0 ? 'item-ok' : 'item-fail');

    const addrText = conn.addr ? ` (${conn.addr})` : '';
    const measured = Number(diag.measured_mA ?? 0);
    const minVal = Number(cal.min_mA ?? 0);
    const maxVal = Number(cal.max_mA ?? 0);

    item.innerHTML = `
      <div class="diag-header">
        <span class="comp-title">${comp.comp_id}${addrText}</span>
        <span class="badge ${errInfo.badgeClass}">${errInfo.label}</span>
      </div>
      <div class="diag-metrics">
        <div><strong>Corriente Medida:</strong> <span class="val-highlight">${measured.toFixed(2)} mA</span></div>
        <div><strong>Rango Esperado:</strong> [${minVal.toFixed(2)} - ${maxVal.toFixed(2)} mA]</div>
      </div>
      <div class="err-description">${errInfo.desc}</div>
    `;
    diagList.appendChild(item);
  });

  diagCard.style.display = 'block';
}

// Iniciar monitoreo periódico cada 1 segundo
setInterval(updateMonitor, 1000);
updateMonitor();
