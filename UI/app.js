// app.js
document.addEventListener("DOMContentLoaded", () => {
  // =========================================================
  // 0) 상수/매핑
  // =========================================================
  const MODE_NAMES = {
    1: "Tapping 연습모드",
    2: "Sound & Light (하)",
    3: "Only Sound (중)",
    4: "Many Sound (상)"
  };

  // =========================================================
  // 1) 공용 유틸 함수
  // =========================================================
  function showView(id) {
    document.querySelectorAll(".view").forEach(v => v.classList.remove("active"));
    const target = document.getElementById(id);
    if (target) target.classList.add("active");
    window.scrollTo(0, 0);
  }

  function playBeep(volumePercent = 80, freq = 440, durationSec = 0.2) {
    const vol = Math.max(0, Math.min(1, volumePercent / 100));
    const AudioCtx = window.AudioContext || window.webkitAudioContext;
    const ctx = new AudioCtx();
    const osc = ctx.createOscillator();
    const gain = ctx.createGain();

    osc.type = "sine";
    osc.frequency.value = freq;
    gain.gain.value = vol;

    osc.connect(gain).connect(ctx.destination);
    osc.start();
    osc.stop(ctx.currentTime + durationSec);
    osc.onended = () => ctx.close();
  }

  function formatDate(iso) {
    const d = new Date(iso);
    const pad = n => String(n).padStart(2, "0");
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
  }

  function modeLabel(modeValue) {
    if (typeof modeValue === "number") return MODE_NAMES[modeValue] || `모드 ${modeValue}`;
    if (typeof modeValue === "string") {
      const n = Number(modeValue.replace(/\D+/g, ""));
      return MODE_NAMES[n] || modeValue;
    }
    return String(modeValue);
  }

  // 실측 저장(원하면 repeatRound에서 실제 측정값으로 활용 가능)
  function saveGameResultActual(mode, resultsArray) {
    const key = "gameResults";
    const data = JSON.parse(localStorage.getItem(key) || "[]");
    data.push({ mode, date: new Date().toISOString(), results: resultsArray });
    localStorage.setItem(key, JSON.stringify(data));
  }

  // =========================================================
  // 2) WebSocket
  // =========================================================
  const ESP32_IP = "192.168.0.3"; // 허브 ESP32 IP
  const PORT = 81;
  const SERVER_URL = `ws://${ESP32_IP}:${PORT}`;

  let socket = null;

  // 조건을 만족하는 "다음 1개의 메시지"를 기다리는 헬퍼
  function waitForMessage(ws, predicate = () => true, timeoutMs = 0) {
    return new Promise((resolve, reject) => {
      //<<<<<<<<오류 관리========
      if (!ws || ws.readyState !== WebSocket.OPEN) {
        reject(new Error("WebSocket not open"));
        return;
      }

      let timer = null;

      const cleanup = () => {
        ws.removeEventListener("message", onMsg);
        ws.removeEventListener("close", onClose);
        ws.removeEventListener("error", onError);
        if (timer) clearTimeout(timer);
      };

      const onClose = () => {
        cleanup();
        reject(new Error("WebSocket closed"));
      };

      const onError = () => {
        cleanup();
        reject(new Error("WebSocket error"));
      };
      //========오류 관리>>>>>>>>>
      const onMsg = async (e) => {
        try {
          let text;
          if (typeof e.data === "string") {
            text = e.data;
          } else if (e.data instanceof Blob) {
            text = await e.data.text();
          } else if (e.data instanceof ArrayBuffer) {
            text = new TextDecoder().decode(e.data);
          } else {
            text = String(e.data ?? "");
          }
          if (predicate(text)) {
            cleanup();
            resolve(text);
          }
        } catch {
          // 파싱 실패는 무시하고 계속 대기
        }
      };

      ws.addEventListener("message", onMsg);
      ws.addEventListener("close", onClose);
      ws.addEventListener("error", onError);

      if (timeoutMs > 0) {
        timer = setTimeout(() => {
          cleanup();
          reject(new Error("timeout"));
        }, timeoutMs);
      }
    });
  }

  function safeSend(message) {
    if (socket && socket.readyState === WebSocket.OPEN) {
      try {
        socket.send(message);
      } catch (e) {
        console.warn("메시지 전송 실패:", e);
      }
    } else {
      console.warn("WS 미연결로 전송 생략:", message);
    }
  }

  // 라운드 시작 패킷 전송: "mode,volume,target(1~4)"
  function sendGameStart(mode, volume) {
    const parts = [mode, volume, 1];
    if (mode > 1) parts[2] = Math.floor(Math.random() * 2) + 1; ////// 정답 지정해주는 RANDOM 함수
    safeSend(parts.join(","));
  }

  // 기본 웹서킷 연결
  function connectToWebSocket() {
    try {
      socket = new WebSocket(SERVER_URL);

      socket.onopen = () => {
        console.log("WebSocket 연결 성공");
      };
      socket.onerror = (e) => {
        console.warn("WebSocket 오류:", e);
      };
      socket.onclose = () => {
        console.log("WebSocket 연결 종료");
      };
    } catch (err) {
      console.error("WebSocket 생성 실패:", err);
    }
  }

  // 라운드 N번 반복: 항상 "CORRECT,<시도횟수>"를 기다림
  async function repeatRound(mode, volume, totalRounds) {
    if (!socket || socket.readyState !== WebSocket.OPEN) {
      throw new Error("WebSocket 미연결");
    }

    const results = [];
    let curr = 0;

    // 1라운드 시작
    sendGameStart(mode, volume);
    let startTime = performance.now();

    const isCorrect = (m) => /^CORRECT,\d+$/.test(String(m).trim());

    while (curr < totalRounds) {
      try {
        const msg = await waitForMessage(socket, isCorrect, 0);
        const text = String(msg).trim();
        const attempts = parseInt(text.split(",")[1], 10); // 숫자 보장
        console.log("correct", attempts);

        const elapsedSec = Math.max(0, Math.round((performance.now() - startTime) / 1000));
        const roundNum = curr + 1;

        // 결과 기록
        results.push({ mode, round: roundNum, time: elapsedSec, attempts });

        curr++;
        if (curr < totalRounds) {
          // 다음 라운드 시작
          sendGameStart(mode, volume);
          startTime = performance.now();
        }
      } catch (e) {
        console.warn(`라운드 ${curr + 1} 대기 중 타임아웃/에러:`, e.message);
        break;
      }
    }
    console.log("Set Ended!!");
    // 세션 저장 & 즉시 갱신
    if (results.length) {
      saveGameResultActual(mode, results);
      if (document.getElementById("dataView")?.classList.contains("active")) {
        renderDataTable();
      }
    }
  }


  // 실제 연결 시도
  connectToWebSocket();

  // =========================================================
  // 3) 홈 화면 네비게이션
  // =========================================================
  const btn1 = document.getElementById("btn1");
  const btn2 = document.getElementById("btn2");
  const btn3 = document.getElementById("btn3");
  const btn4 = document.getElementById("btn4");

  if (btn1) btn1.addEventListener("click", () => showView("mode1View"));
  if (btn2) btn2.addEventListener("click", () => showView("mode2View"));
  if (btn3) btn3.addEventListener("click", () => showView("mode3View"));
  if (btn4) btn4.addEventListener("click", () => showView("mode4View"));

  // 뒤로가기(공통)
  document.querySelectorAll("[data-back]").forEach(btn => {
    btn.addEventListener("click", () => showView("homeView"));
  });

  // 녹음 시작(홈)
  const startBtn = document.getElementById("startBtn");
  if (startBtn) {
    startBtn.addEventListener("click", () => {
      alert("녹음이 시작됩니다!");
      safeSend("LED_ON");
    });
  }

  // =========================================================
  // 4) 모드 1: Tapping 연습모드 (통일된 루프 사용)
  // =========================================================
  const m1Run = document.getElementById("m1Run");
  if (m1Run) {
    m1Run.addEventListener("click", async () => {
      const reps = +document.getElementById("m1_reps").value || 10;
      alert(`[연습모드 시작] 라운드: ${reps}\n정답이 오면 다음 라운드로 진행합니다.`);
      try {
        await repeatRound(1, 100, reps); // 모드1은 volume=100으로 가정
      } catch (e) {
        console.warn("모드1 실행 중 오류:", e.message);
      }
    });
  }

  // =========================================================
  // 5) 모드 2: Sound & Light (하)
  // =========================================================
  const m2Rounds = document.getElementById("m2_rounds");
  const m2Brightness = document.getElementById("m2_brightness");
  const m2VolumeInput = document.getElementById("m2_volume");
  const m2VolumeValue = document.getElementById("m2_volumeValue");
  const m2TestBtn = document.getElementById("m2Test");
  const m2Run = document.getElementById("m2Run");

  function updateM2VolumeLabel() {
    if (m2VolumeInput && m2VolumeValue) {
      m2VolumeValue.textContent = `${m2VolumeInput.value}%`;
    }
  }
  if (m2VolumeInput) {
    m2VolumeInput.addEventListener("input", updateM2VolumeLabel);
    updateM2VolumeLabel();
  }
  if (m2TestBtn) {
    m2TestBtn.addEventListener("click", () => {
      const vol = m2VolumeInput ? Number(m2VolumeInput.value) : 80;
      playBeep(vol, 523.25);
    });
  }
  if (m2Run) {
    m2Run.addEventListener("click", async () => {
      const rounds = m2Rounds ? +m2Rounds.value : 5;
      const brightness = m2Brightness ? +m2Brightness.value : 70;
      const volume = m2VolumeInput ? +m2VolumeInput.value : 80;

      alert(`[Sound&Light 실행] 라운드:${rounds}, 밝기:${brightness}, 음량:${volume}%`);
      // 허브가 밝기를 쓰는 경우 유지


      // 라운드 진행(일반화 루프)
      try {
        await repeatRound(2, volume, rounds);
      } catch (e) {
        console.warn("모드2 실행 중 오류:", e.message);
      }

    });
  }

  // =========================================================
  // 6) 모드 3: Only Sound (중)
  // =========================================================
  const m3VolumeInput = document.getElementById("m3_volume");
  const m3VolumeValue = document.getElementById("m3_volumeValue");
  const m3TestBtn = document.getElementById("m3Test");
  const m3Run = document.getElementById("m3Run");

  function updateM3VolumeLabel() {
    if (m3VolumeInput && m3VolumeValue) {
      m3VolumeValue.textContent = `${m3VolumeInput.value}%`;
    }
  }
  if (m3VolumeInput) {
    m3VolumeInput.addEventListener("input", updateM3VolumeLabel);
    updateM3VolumeLabel();
  }
  if (m3TestBtn) {
    m3TestBtn.addEventListener("click", () => {
      const vol = m3VolumeInput ? Number(m3VolumeInput.value) : 80;
      playBeep(vol, 440);
    });
  }
  if (m3Run) {
    m3Run.addEventListener("click", async () => {
      const rounds = +document.getElementById("m3_rounds").value || 7;
      const volume = m3VolumeInput ? +m3VolumeInput.value : 80;

      alert(`[Only Sound 실행] 라운드:${rounds}, 음량:${volume}%`);


      try {
        await repeatRound(3, volume, rounds);
      } catch (e) {
        console.warn("모드3 실행 중 오류:", e.message);
      }

    });
  }

  // =========================================================
  // 7) 모드 4: Many Sound (상)
  // =========================================================
  const m4Rounds = document.getElementById("m4_rounds");
  const m4VolumeInput = document.getElementById("m4_volume");
  const m4VolumeValue = document.getElementById("m4_volumeValue");
  const m4TestBtn = document.getElementById("m4Test");
  const m4Run = document.getElementById("m4Run");

  function updateM4VolumeLabel() {
    if (m4VolumeInput && m4VolumeValue) {
      m4VolumeValue.textContent = `${m4VolumeInput.value}%`;
    }
  }
  if (m4VolumeInput) {
    m4VolumeInput.addEventListener("input", updateM4VolumeLabel);
    updateM4VolumeLabel();
  }
  if (m4TestBtn) {
    m4TestBtn.addEventListener("click", () => {
      const vol = m4VolumeInput ? Number(m4VolumeInput.value) : 80;
      playBeep(vol, 660);
    });
  }
  if (m4Run) {
    m4Run.addEventListener("click", async () => {
      const rounds = m4Rounds ? +m4Rounds.value : 5;
      const volume = m4VolumeInput ? +m4VolumeInput.value : 80;

      alert(`[Many Sound 실행] 라운드:${rounds}, 음량:${volume}%`);


      try {
        await repeatRound(4, volume, rounds);
      } catch (e) {
        console.warn("모드4 실행 중 오류:", e.message);
      }

    });
  }

  // =========================================================
  // 8) 데이터 보기 화면
  // =========================================================
  const viewDataBtn = document.getElementById("viewDataBtn");
  if (viewDataBtn) {
    viewDataBtn.addEventListener("click", () => {
      showView("dataView");
      renderDataTable();
    });
  }

  function renderDataTable() {
    const tbody = document.querySelector("#dataTable tbody");
    const data = JSON.parse(localStorage.getItem("gameResults") || "[]");
    if (!tbody) return;

    tbody.innerHTML = "";

    if (data.length === 0) {
      const tr = document.createElement("tr");
      tr.innerHTML = `<td colspan="6" style="padding:8px;">저장된 데이터가 없습니다.</td>`;
      tbody.appendChild(tr);
      return;
    }

    data.forEach((session, sIdx) => {
      const dateStr = formatDate(session.date);
      session.results.forEach((r, rIdx) => {
        const tr = document.createElement("tr");
        tr.innerHTML = `
          <td>${dateStr}</td>
          <td>${sIdx + 1} (${modeLabel(session.mode)})</td>
          <td>${r.round}</td>
          <td>${r.attempts}</td>
          <td>${r.time}</td>
          <td><button class="row-delete" data-sidx="${sIdx}" data-ridx="${rIdx}" title="삭제">&times;</button></td>
        `;
        tbody.appendChild(tr);
      });
    });
  }

  const dataTable = document.getElementById("dataTable");
  if (dataTable) {
    dataTable.addEventListener("click", (e) => {
      const btn = e.target.closest(".row-delete");
      if (!btn) return;

      const sIdx = Number(btn.dataset.sidx);
      const rIdx = Number(btn.dataset.ridx);
      const data = JSON.parse(localStorage.getItem("gameResults") || "[]");
      if (!data[sIdx]) return;

      data[sIdx].results.splice(rIdx, 1);
      if (data[sIdx].results.length === 0) {
        data.splice(sIdx, 1);
      }
      localStorage.setItem("gameResults", JSON.stringify(data));
      renderDataTable();
    });
  }
});
