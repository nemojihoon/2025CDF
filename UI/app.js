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

  // 1-1) 화면 전환
  function showView(id) {
    document.querySelectorAll(".view").forEach(v => v.classList.remove("active"));
    const target = document.getElementById(id);
    if (target) target.classList.add("active");
    window.scrollTo(0, 0);
  }

  // 1-2) 테스트음 재생 (Web Audio API)
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

  // 1-3) 날짜 포맷 (YYYY-MM-DD HH:mm)
  function formatDate(iso) {
    const d = new Date(iso);
    const pad = n => String(n).padStart(2, "0");
    return `${d.getFullYear()}-${pad(d.getMonth() + 1)}-${pad(d.getDate())} ${pad(d.getHours())}:${pad(d.getMinutes())}`;
  }

  // 1-4) 모드 라벨
  function modeLabel(modeValue) {
    if (typeof modeValue === "number") return MODE_NAMES[modeValue] || `모드 ${modeValue}`;
    if (typeof modeValue === "string") {
      const n = Number(modeValue.replace(/\D+/g, "")); // "mode2" -> 2
      return MODE_NAMES[n] || modeValue;
    }
    return String(modeValue);
  }

  // 1-5) 더미 저장(모드2~4)
  function saveGameResult(mode, rounds) {
    const key = "gameResults";
    const data = JSON.parse(localStorage.getItem(key) || "[]");

    const run = [];
    for (let i = 1; i <= rounds; i++) {
      run.push({
        round: i,
        attempts: Math.floor(Math.random() * 5) + 1, // 1~5회
        time: Math.floor(Math.random() * 60) + 10    // 10~70초
      });
    }

    data.push({
      mode: mode,
      date: new Date().toISOString(),
      results: run
    });

    localStorage.setItem(key, JSON.stringify(data));
    console.log("저장됨:", data);
  }

  // 1-6) 실제 저장(모드1 실측)
  function saveGameResultActual(mode, resultsArray) {
    const key = "gameResults";
    const data = JSON.parse(localStorage.getItem(key) || "[]");

    data.push({
      mode: mode,
      date: new Date().toISOString(),
      results: resultsArray
    });

    localStorage.setItem(key, JSON.stringify(data));
  }

  // =========================================================
  // [MODE1] 세션 상태
  // =========================================================
  let m1SessionActive = false;
  let m1TargetRounds = 0;
  let m1CurrentRound = 0;
  let m1RoundStartTime = 0; // performance.now()
  let m1Results = [];       // [{round, attempts:1, timeSec}]

  // =========================================================
  // 2) WebSocket
  // =========================================================
  const ESP32_IP = "192.168.0.7"; // 허브 ESP32 IP에 맞게 설정
  const PORT = 81; // WebSocket 포트
  const SERVER_URL = `ws://${ESP32_IP}:${PORT}`;

  let socket = null;

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

  // === 신규: 게임 시작 신호(웹→허브→(ESP-NOW)서브) ===
  function sendGameStart(mode, rounds, volume) {
    const parts = [`GAME_START`, `mode=${mode}`, `rounds=${rounds}`];
    if (typeof volume === "number" && !Number.isNaN(volume)) {
      parts.push(`volume=${volume}`);
    }
    safeSend(parts.join("|"));
  }

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

      socket.onmessage = (event) => {
        const data = String(event.data || "");
        console.log("서버에서 받은 메시지:", data);

        // ====== MODE1 프로토콜 ======
        if (data.startsWith("MODE1_ACK")) {
          console.log("MODE1 세션 시작 확인:", data);

        } else if (data.startsWith("MODE1_ROUND_START")) {
          // 예: MODE1_ROUND_START|round=1|r=..|g=..|b=..
          const round = Number((data.match(/round=(\d+)/) || [])[1] || m1CurrentRound);
          m1CurrentRound = round;
          m1RoundStartTime = performance.now();
          console.log(`Round ${round} timer start.`);

        } else if (data.startsWith("MODE1_CORRECT")) {
          const round = Number((data.match(/round=(\d+)/) || [])[1] || m1CurrentRound);
           const timeSec = Math.max(0, Math.round(elapsedMs) / 1000);
          m1Results.push({ round, attempts: 1, time: Math.round(timeSec) });

          if (m1CurrentRound < m1TargetRounds) {
            safeSend("MODE1_NEXT");
          } else {
            safeSend("MODE1_STOP");
          }

        } else if (data.startsWith("MODE1_DONE")) {
          console.log("MODE1 완료");
          m1SessionActive = false;

          if (m1Results.length > 0) {
            saveGameResultActual(1, m1Results);
            if (document.getElementById("dataView")?.classList.contains("active")) {
              renderDataTable();
            }
          }
          alert("연습모드가 종료되었습니다.");

        // ====== 신규: 서브 ACK 표시 ======
        } else if (data.startsWith("GAME_START_ACK")) {
          // 허브가 직접 이 문자열을 보내줄 수도 있음
          console.log("[서브 ACK 수신] ", data);

        } else if (data.startsWith("SUB|")) {
          // 허브가 "SUB|<mac>|GAME_START_ACK|..." 형태로 브릿지해줄 수 있음
          console.log("[서브 ACK 브릿지] ", data);
        }

        // 선택: LED_STATE 등 기타 메시지 처리 가능
      };
    } catch (err) {
      console.error("WebSocket 생성 실패:", err);
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
  // 4) 모드 1: Tapping 연습모드 (BPM 제거 버전)
  // =========================================================
  const m1Run = document.getElementById("m1Run");
  if (m1Run) {
    m1Run.addEventListener("click", () => {
      const reps = +document.getElementById("m1_reps").value || 10;

      // 세션 상태 초기화
      m1SessionActive = true;
      m1TargetRounds = reps;
      m1CurrentRound = 0;
      m1RoundStartTime = 0;
      m1Results = [];

      // 서버에 세션 시작 지시(기존)
      safeSend(`MODE1_START|reps=${reps}`);

      // === 신규: 서브들에게도 모드/라운드 알림(ESP-NOW로 브리지됨)
      sendGameStart(1, reps);

      alert(`[연습모드 시작] 라운드: ${reps}\n진동을 감지하면 자동으로 다음 라운드로 진행합니다.`);
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
      playBeep(vol, 523.25); // C5
    });
  }

  if (m2Run) {
    m2Run.addEventListener("click", () => {
      const rounds = m2Rounds ? +m2Rounds.value : 5;
      const brightness = m2Brightness ? +m2Brightness.value : 70;
      const volume = m2VolumeInput ? +m2VolumeInput.value : 80;

      alert(`[Sound&Light 실행] 라운드:${rounds}, 밝기:${brightness}, 음량:${volume}%`);
      saveGameResult(2, rounds);

      // 기존: 허브 제어용
      safeSend(`MODE2_RUN|rounds=${rounds}|brightness=${brightness}|volume=${volume}`);

      // 신규: 서브들에게도 공지
      sendGameStart(2, rounds, volume);
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
      playBeep(vol, 440); // A4
    });
  }

  if (m3Run) {
    m3Run.addEventListener("click", () => {
      const rounds = +document.getElementById("m3_rounds").value;
      const volume = m3VolumeInput ? +m3VolumeInput.value : 80;

      alert(`[Only Sound 실행] 라운드:${rounds}, 음량:${volume}%`);
      saveGameResult(3, rounds);

      safeSend(`MODE3_RUN|rounds=${rounds}|volume=${volume}`);
      sendGameStart(3, rounds, volume);
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
      playBeep(vol, 660); // E5 근처
    });
  }

  if (m4Run) {
    m4Run.addEventListener("click", () => {
      const rounds = m4Rounds ? +m4Rounds.value : 5;
      const volume = m4VolumeInput ? +m4VolumeInput.value : 80;

      alert(`[Many Sound 실행] 라운드:${rounds}, 음량:${volume}%`);
      saveGameResult(4, rounds);

      safeSend(`MODE4_RUN|rounds=${rounds}|volume=${volume}`);
      sendGameStart(4, rounds, volume);
    });
  }

  // =========================================================
  // 8) 데이터 보기 화면
  // =========================================================

  // 8-1) 홈 → 데이터 보기
  const viewDataBtn = document.getElementById("viewDataBtn");
  if (viewDataBtn) {
    viewDataBtn.addEventListener("click", () => {
      showView("dataView");
      renderDataTable();
    });
  }

  // 8-2) 표 렌더링
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

  // 8-3) 행 삭제 (이벤트 위임)
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
