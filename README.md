# StickS3 EMS Timer & Air Mouse

M5Stack **StickS3** (`ESP32-S3-PICO-1-N8R8`) 전용 경량 펌웨어입니다.  
구급대원 현장 처치용 **EMS 타이머**와 **자이로스코프 기반 BLE / USB-C 에어마우스** 두 가지 핵심 기능만 담아 빠르게 부팅되고 직관적으로 조작할 수 있습니다.

## 주요 기능

### 1. EMS TIMER (구급대원 타이머)
- **총 경과 시간 스톱워치 (`TOTAL`)**: `HH:MM:SS` 실시간 표시, 일시정지/재개 지원.
- **에피네프린(EPI) 투여 간격 타이머**: 3분 / 4분 / 5분 설정 가능, 투여 횟수(`DOSE #N`) 자동 카운트.
  - 30초 이하 남으면 황색 경고, 0초 도달 시 적색 점멸 + 경고음, 이후 초과 시간(`+MM:SS`) 표시 및 15초마다 리마인더 알림.
- **바이탈/체크포인트 카운트다운 프리셋**: `OFF / 1m / 2m / 3m / 5m / 10m / 15m` 선택 가능.
- **처치 이벤트 로그 (`EVENT LOG`)**: `START`, `EPI #N`, `SHOCK #N`, `ROSC`, `VITAL`, `AIRWAY`, `PAUSE`, `RESUME` 타임스탬프를 최대 32건 기록 및 전체 조회.
- 홈 화면으로 잠시 나가도 진행 중인 타이머와 로그는 유지됩니다(`RESET` 확인 팝업으로 오작동 초기화 방지).

#### EMS 타이머 버튼 조작
- **대기(`EMS READY`) 상태**:
  - `A` 짧게: 프리셋 시간 변경 (`OFF` ~ `15m`)
  - `B` 짧게: **타이머 시작 (`START`)**
  - `B` 길게(0.65초): EPI 간격 변경 (`3m` / `4m` / `5m`)
- **실행(`EMS RUN` / `EMS PAUSE`) 상태**:
  - `A` 짧게: 하단 실행 버튼 선택 이동 (`LOG EPI` → `LOG SHOCK` → `LOG ROSC` → `LOG VITAL` → `AIRWAY` → `PAUSE/RESUME` → `VIEW LOGS` → `RESET`)
  - `B` 짧게: 선택된 동작 실행 (기본값 `LOG EPI`)
  - `B` 길게(0.65초): 어느 메뉴가 선택되어 있어도 **즉시 `EPI` 투여 기록 및 간격 리셋**
  - `A` 길게(0.65초): 활성 알람음 해제 / 음소거 전환
  - `A + B` 길게(0.6초): 홈 메뉴로 복귀

---

### 2. AIR MOUSE (BLE & USB-C 자이로 에어마우스)
- **6축 IMU(BMI270) 자이로 기반 포인터**:
  - 진입 시 0.8초 자동 영점 보정 + 정지 상태 자동 드리프트 보정.
  - 미세 데드존 + 비선형 가속 곡선 + 서브픽셀 누적으로 정밀 클릭과 빠른 화면 이동을 동시에 지원.
- **두 가지 연결 방식**:
  - **BLE (`AIR MOUSE BLE`)**: 장치명 `StickS3 AirMouse` (Windows, macOS, Linux, Android, iOS 페어링).
  - **USB-C (`AIR MOUSE USB`)**: USB-C 케이블로 PC/태블릿에 직접 연결 시 표준 HID 마우스로 동작(드라이버 불필요).

#### 에어마우스 버튼 조작
- `A` 누르기/유지: **마우스 왼쪽 버튼 (LMB)** (클릭, 더블클릭, 드래그)
- `B` 짧게: **마우스 오른쪽 버튼 (RMB)**
- `B` 길게(0.3초 이상 유지): **스크롤 휠 모드** (손목을 위/아래로 기울여 스크롤)
- `A + B` 짧게: **포인터 일시정지 / 재개 (클러치)**
- `A + B` 길게(0.6초): 홈 메뉴로 복귀

---

### 3. SETTINGS (NVS 자동 저장)
- EPI 간격 (`3m` / `4m` / `5m`), 마우스 감도 (`1` ~ `5`), X/Y 반전, X/Y 축 교환, 내추럴 스크롤, 볼륨/음소거, 화면 180도 회전.

---

## 빌드 및 플래시 방법

이 저장소는 GitHub Actions에서 **ESP-IDF v5.3.1 (`esp32s3`)**으로 자동 빌드됩니다.

1. GitHub 저장소의 **Actions** 탭 → **Build StickS3 EMS & AirMouse Firmware** 실행 결과에서 `sticks3-ems-airmouse-<sha>` 아티팩트를 다운로드합니다.
2. 압축 해제 후 아래 명령으로 StickS3에 플래시합니다 (`COM번호` 또는 `/dev/ttyACM0` 확인).
   - **전체 통합 이미지 플래시 (권장 - 최초 설치)**:
     ```bash
     python -m esptool --chip esp32s3 --port COM번호 write-flash 0x0 sticks3-ems-airmouse-full.bin
     ```
   - 참고: `AIR MOUSE USB` 모드는 USB-OTG 포트를 HID 마우스로 사용하므로, 재플래시가 필요할 때는 StickS3를 다운로드(부트로더) 모드로 진입한 뒤 실행하세요.

## 호스트 단위 테스트

```bash
bash tests/host/run.sh
```
