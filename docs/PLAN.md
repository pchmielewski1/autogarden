# Plan: implementacja „autogarden” (HLD → architektura → pseudokod)

Celem jest zbudowanie automatycznego systemu podlewania na M5Stack StickS3 (ESP32‑S3, PlatformIO/Arduino), który utrzymuje wilgotność na zadanym poziomie, ma zabezpieczenie anty‑przelewowe i pokazuje status w UI, a zdalne sterowanie (Wi‑Fi/Telegram/HTTP) traktuje jako opcjonalny dodatek.

Ten dokument świadomie kończy się na poziomie dekompozycji i pseudokodu. Implementacja C++ jest osobnym etapem.

## Twarde wymagania (kontrakt)
- `M5Unified`: używamy `M5.begin()` / `M5.update()`.
- Non‑blocking: brak długich `delay()` w logice; praca w krótkich tickach.
- Event-driven: ISR tylko sygnalizuje (flaga/notify); logika działa w taskach.
- Współbieżność: krytyczne podlewanie/monitoring oddzielone od GUI i sieci.
- Domyślnie bez pinowania tasków do rdzeni (zostaw schedulerowi FreeRTOS).
- Pompa ma mieć: hard timeout + cooldown + blokady anti‑overflow.
- Konfiguracja ma się utrwalać (Preferences/EEPROM).
- Logowanie zdarzeń przez Serial.
- Nie zgadujemy mapowania Pb.HUB, adresów I²C ani kanałów — wszystko parametryzowane.
- Build/Upload/Monitor: przez PlatformIO extension w VS Code (nie zakładamy `pio` w systemie).

## Zasady dekompozycji (abstrakcje)
1) Rozdziel „co” od „jak”
- „Co”: domena (podlewanie, blokady, progi, stany, komendy, konfiguracja).
- „Jak”: sprzęt (I²C/ADC), UI (ekran), sieć (Wi‑Fi/Telegram).

2) Każdy moduł ma jedną odpowiedzialność i publiczny kontrakt
- I/O w modułach sprzętowych.
- Decyzje w kontrolerze domenowym.
- Render + input w UI.
- Protokół + retry/backoff w sieci.

3) Komunikacja między modułami wyłącznie przez eventy i/lub snapshoty stanu
- Task krytyczny publikuje „snapshot” sensorów i stanu automatyki.
- UI i NET konsumują snapshoty, publikują komendy (eventy wejściowe).

4) Bezpieczeństwo jako osobna warstwa (gates)
- Zanim pompa się włączy: policy check (anti‑overflow, cooldown, timeout, sensor health).

## Docelowa architektura wykonania (FreeRTOS)

### Taski (priorytety orientacyjne)
- `ControlTask` (wysoki priorytet): odczyt + filtracja sensorów, FSM podlewania, sterowanie pompą, safety.
- `UiTask` (średni): odświeżanie ekranu, obsługa przycisków, menu ustawień.
- `NetTask` (niski): Wi‑Fi reconnect (backoff), obsługa komend zdalnych, publikacja statusu.

Uwagi:
- Domyślnie **bez** `xTaskCreatePinnedToCore`; scheduler decyduje o core.
- ISR: tylko `xTaskNotifyFromISR()` / ustawienie flagi + `portYIELD_FROM_ISR()`.

### Kanały komunikacji
- `EventQueue`: jednolita kolejka zdarzeń wejściowych (UI/NET/ISR → ControlTask).
- `StateSnapshot`: najnowszy stan systemu publikowany przez ControlTask (np. w zmiennej atomowej/chronionej mutexem) i odczytywany przez UI/NET.

## Model domeny (poziom abstrakcji)

### Stany i dane
Konceptualne struktury danych (pseudotypy):
- `SensorSnapshot`:
  - `soilMoistureRaw`          // surowy ADC z PbHUB
  - `soilMoisturePct`          // znormalizowany 0-100% (z soilRaw, bez per-pot kalibracji)
  - `waterGuards`:
    - `potMax` (np. OK / TRIGGERED / UNKNOWN) — max poziom wody w doniczce (anti-overflow)
    - `reservoirMin` (np. OK / TRIGGERED / UNKNOWN) — min poziom wody w rezerwuarze (anti-dry-run)
  - `env` (temp/pressure/light; opcjonalne)
  - `health` (I2C OK? wartości w zakresie?)
- `ActuatorState`:
  - `pumpOn` (bool)
  - `pumpStartedAtMs`
  - `lastPumpStopAtMs`
- `Config` (persisted):
  - `mode` (AUTO/MANUAL)
  - `plantProfileIndex` (indeks profilu rośliny → targetMoisturePct, criticalLowPct, itd.)
  - `pumpOnMsMax`, `cooldownMs`
  - `pumpMlPerSec` (z kalibracji pompy)
  - `antiOverflowEnabled` + progi
  - `potMaxActiveLow` (bool, domyślnie true), `reservoirMinActiveLow` (bool, domyślnie false)
  - pełna lista pól → sekcja „Zaktualizowana struktura Config (pełna)"
- `SystemStatus`:
  - `blockReason` (NONE / COOLDOWN / OVERFLOW_RISK / TANK_EMPTY / SENSOR_FAIL / MANUAL_LOCK / ...)
  - `netStatus` (UP/DOWN)

### Zdarzenia (EventQueue)
Minimalny zestaw eventów:
- `TICK_10MS`, `TICK_100MS`, `TICK_1S` (generowane programowo, bez ISR)
- `BUTTON_PRESS(buttonId)`, `BUTTON_LONGPRESS(buttonId)`
- `REMOTE_CMD(type, payload)`
- `SENSOR_FAULT(sensorId, code)` (opcjonalnie)

## FSM podlewania (skończona maszyna stanów)

> **Uwaga**: Poniższy FSM to uproszczony widok bazowy. Pełny algorytm podlewania
> pulsowego (Pulse-Soak-Measure) z fazami EVALUATING → PULSE → SOAK → MEASURING →
> OVERFLOW_WAIT → DONE opisany jest w sekcji **„Algorytm podlewania — pełna
> specyfikacja domenowa"** dalej w tym dokumencie.

### Stany (bazowe — rozszerzone w sekcji Algorytm podlewania)
- `IDLE` (pompa off, normalny monitoring)
- `EVALUATING` (ocena warunków: pora dnia, temperatura, wilgotność vs target)
- `PULSE` (pompa on — krótki puls wody)
- `SOAK` (pompa off — czekanie na nasiąknięcie gleby, 30-45s)
- `MEASURING` (odczyt wilgotności po nasiąknięciu, decyzja: kolejny puls lub stop)
- `OVERFLOW_WAIT` (czujnik przelewowy aktywny — czekamy na opadnięcie wody)
- `DONE` (cykl zakończony → cooldown)
- `COOLDOWN` (pompa off, blokada czasowa)
- `BLOCKED` (twarda blokada: overflow/sensor fail/manual/reservoir empty)

### Inwarianty bezpieczeństwa
- Pompa nie może być włączona dłużej niż `pumpOnMsMax` per puls.
- W `PULSE` stale sprawdzaj `antiOverflow`, `reservoirEmpty` oraz `sensorHealth`.
- Po każdym pulsie: SOAK → MEASURING → decyzja (kolejny puls / overflow_wait / done).
- Po zakończonym cyklu: `COOLDOWN` na `cooldownMs`.

### Pseudokod FSM (wysoki poziom — uproszczony)

> Pełny pseudokod Pulse-Soak-Measure → sekcja „Algorytm podlewania → Pseudokod głównej pętli podlewania".

```
on ControlTick(nowMs, sensors, config, actuatorState):
  // === Globalne safety (rezerwuar pusty itp.) ===
  safety = evaluateExtendedSafety(nowMs, sensors, config, waterBudget, actuatorState)

  if safety.hardBlock:
    for potIdx in 0 ..< config.numPots:
      ensurePumpOff(potIdx)
    fsm.state = BLOCKED
    status.blockReason = safety.reason
    return

  // --- Manual button: zawsze aktywny, niezależnie od AUTO/MANUAL ---
  manualPumpTick(nowMs, dualBtn, sensors, config)

  // === Per-pot watering (niezależne cykle, wspólny rezerwuar) ===
  if config.mode == AUTO:
    wateringTick(nowMs, sensors, config, waterBudget, actuatorState)
    // ^ wewnętrznie iteruje po config.numPots doniczkach

  // === Per-pot trend tick ===
  for potIdx in 0 ..< config.numPots:
    if not config.pots[potIdx].enabled: continue
    trendTick(nowMs, sensors.pots[potIdx].moisturePct, trendStates[potIdx], config)

    case BLOCKED:
      if safety.cleared and config.mode == AUTO:
        fsm.state = IDLE
```

## Polityki bezpieczeństwa (gates)
Pseudokod oceny blokad:
```
evaluateSafety(nowMs, sensors, config, actuatorState):
  if sensors.health == FAIL:
    return { hardBlock: true, reason: SENSOR_FAIL }

  if config.antiOverflowEnabled:
    if sensors.waterGuards.potMax == TRIGGERED:
      return { hardBlock: true, reason: OVERFLOW_RISK }
    if sensors.waterGuards.reservoirMin == TRIGGERED:
      return { hardBlock: true, reason: TANK_EMPTY }

    if sensors.waterGuards.potMax == UNKNOWN and config.waterLevelUnknownPolicy == BLOCK:
      return { hardBlock: true, reason: OVERFLOW_SENSOR_UNKNOWN }
    if sensors.waterGuards.reservoirMin == UNKNOWN and config.waterLevelUnknownPolicy == BLOCK:
      return { hardBlock: true, reason: TANK_SENSOR_UNKNOWN }

  if config.mode == MANUAL and manualPumpLockEnabled:
    return { hardBlock: true, reason: MANUAL_LOCK }

  // softBlockNow: warunek, który przerywa PUMPING natychmiast
  if config.antiOverflowEnabled and sensors.waterGuards.potMax == TRIGGERED:
    return { softBlockNow: true, reason: OVERFLOW_RISK }
  if config.antiOverflowEnabled and sensors.waterGuards.reservoirMin == TRIGGERED:
    return { softBlockNow: true, reason: TANK_EMPTY }

  return { ok: true }
```

### Dodatkowe zabezpieczenia zaimplementowane po incydentach runtime

- **Pump owner tracking**: runtime śledzi właściciela każdej pompy (`AUTO`, `MANUAL`, `REMOTE`, `NONE`).
- **Failsafe OFF poza legalnym kontekstem**: jeśli pompa jest fizycznie `ON`, ale nie ma aktywnego
  cyklu `PULSE` ani ważnego manual hold, `ControlTask` okresowo wymusza `OFF` i loguje
  `PUMP_UNEXPECTED_ON_CONTEXT`.
- **Fail-closed dla wejść manualnych**: niestabilny stan Dual Button nie może uruchomić pompy;
  jeśli pompa już działa manualnie, zostaje natychmiast wyłączona.

## Scheduler ticków (bez blokowania)

Zamiast `delay()`:
- `ControlTask` używa pętli: `waitForEventOrTimeout()`.
- Ticki generowane programowo (np. co 10ms/100ms/1s) i wrzucane jako eventy.

Pseudokod:
```
ControlTask:
  loop:
    evt = EventQueue.pop(timeout = nextDeadline)
    if evt:
      handleEvent(evt)
    if timeFor10msTick: push(TICK_10MS)
    if timeFor100msTick: push(TICK_100MS)
    if timeFor1sTick: push(TICK_1S)
```

## UI (minimalna specyfikacja)
- Widok statusu: wilgotność (%), poziom wody w doniczce (MAX: OK/TRIG/UNK), poziom wody w rezerwuarze (MIN: OK/TRIG/UNK), stan pompy (ON/OFF), tryb (AUTO/MANUAL), powód blokady.
- Menu ustawień (minimalnie): target wilgotności, tryb auto/manual, cooldown, anti‑overflow on/off.

Pseudokod UI:
```
UiTask:
  loop every ~50-200ms:
    M5.update()
    readButtons() -> push BUTTON events
    snapshot = StateSnapshot.read()
    render(snapshot)
```

## Sieć / remote control (opcjonalnie)
- Reconnect z backoff; brak sieci nie wpływa na ControlTask.
- Komendy zdalne mapują się na eventy `REMOTE_CMD`.
- Minimalne komendy: `GET_STATUS`, `SET_TARGET(x)`, `SET_MODE(AUTO/MANUAL)`, `PUMP_ON(ms)` (manual), `PUMP_OFF`.

Pseudokod sieci:
```
NetTask:
  loop:
    ensureWifiConnectedWithBackoff()
    if connected:
      cmd = pollRemoteNonBlocking()
      if cmd: EventQueue.push(REMOTE_CMD(cmd))
    sleepShort()
```

## Konfiguracja (persist)
- `Config` ładowany na starcie, walidowany (zakresy), a zmiany z UI/NET zapisywane asynchronicznie.
- Zapis nie może blokować ControlTask (preferuj kolejkę „save requests” do niższego priorytetu lub krótkie operacje).

## Warstwa konfiguracji — dekompozycja (walidacja, wersjonowanie, zapis async)

### Podział odpowiedzialności
- `ConfigModel` (domena): struktura `Config` + zasady walidacji + domyślne wartości.
- `ConfigStore` (persist): zapis/odczyt do Preferences/EEPROM + wersjonowanie.
- `ConfigService` (koordynacja): przyjmuje żądania zmian (UI/NET), waliduje, publikuje nowy config i kolejkuje zapis.

### Schemat danych (pseudostruktura)
```
// UWAGA: Poniżej wersja synchronizowana z pełną specyfikacją.
// Pełna wersja z opisem wszystkich pól → sekcja „Zaktualizowana struktura Config (pełna)".

const MAX_POTS = 2  // skalowalne do 2 doniczek (PbHUB ma 6 kanałów)

// Per-pot config — każda doniczka ma własną pompę, moisture sensor i czujnik overflow
struct PotConfig {
  bool   enabled = false            // czy ta doniczka jest aktywna (pot[0] zawsze true)
  uint8  plantProfileIndex = 0      // profil rośliny (0=Pomidor, ..., 5=Custom)
  float  pumpMlPerSec = 0           // z kalibracji pompy (0 = nie skalibrowana)
  bool   pumpCalibrated = false
  bool   potMaxActiveLow = true     // per-pot — overflow sensor polaryzacja
  float  moistureEmaAlpha = 0.1     // per-pot — EMA alpha
}

struct Config {
  uint16 schemaVersion

  // Tryb
  enum Mode { AUTO, MANUAL }
  Mode mode

  // Multi-pot (NOWE)
  uint8  numPots = 1                // 1 lub 2 (domyślnie 1)
  PotConfig pots[MAX_POTS]          // pots[0] = doniczka główna, pots[1] = opcjonalna

  // Pompa — globalne safety
  uint32 pumpOnMsMax = 30000  // hard timeout per puls (safety) — obie pompy
  uint32 cooldownMs  = 60000

  // Anti-overflow — globalne
  bool antiOverflowEnabled = true
  uint32 overflowMaxWaitMs = 600000  // max wait for drain (10 min)
  enum UnknownPolicy { BLOCK, ALLOW_WITH_WARNING }
  UnknownPolicy waterLevelUnknownPolicy = BLOCK

  // Warunki pogodowe
  float  heatBlockTempC = 35.0
  float  directSunLuxThreshold = 40000

  // Dusk detector — sensorowy (bez RTC)
  uint32 duskWateringWindowMs = 7200000   // 2h okno po zmierzchu
  float  duskScoreConfirmThreshold = 0.65
  uint32 transitionConfirmMs = 900000     // 15 min
  uint32 fallbackIntervalMs = 6*3600*1000 // co 6h jeśli brak detekcji
  bool   morningWateringEnabled = false

  // Rezerwuar — WSPÓLNY dla obu doniczek
  float  reservoirCapacityMl = 1500    // 1.5L (domyślne)
  float  reservoirLowThresholdMl = 400 // 0.4L — próg czujnika LOW

  // Anomaly detection
  float  anomalyDryingRateThreshold = 5.0   // fallback gdy baseline nie jest jeszcze wyuczony
  float  anomalyDryingRateMultiplier = 3.0  // mnożnik baseline do detekcji anomalii

  // Przycisk manualny
  uint32 manualMaxHoldMs = 30000
  uint32 manualCooldownMs = 60000

  // Vacation mode
  bool   vacationMode = false
  float  vacationTargetReductionPct = 10.0
  uint8  vacationMaxPulsesOverride = 2
  float  vacationCooldownMultiplier = 2.0

  // (sieć w osobnym NetConfig / ag_net namespace)
}
```

Uwaga: dokładny zestaw pól można minimalizować, ale **timeout/cooldown/anti-overflow**, profil rośliny oraz `pumpMlPerSec` są obowiązkowe.

### Walidacja (kontrakt)
Walidacja ma być deterministyczna i szybka.
```
validate(config):
  // Multi-pot
  assert config.numPots >= 1 and <= MAX_POTS
  for i in 0 ..< config.numPots:
    pot = config.pots[i]
    assert pot.enabled == true          // aktywna doniczka musi być enabled
    assert pot.plantProfileIndex < NUM_PROFILES
    assert pot.pumpMlPerSec >= 0        // 0 = nie skalibrowana (blokuje AUTO)
    assert pot.moistureEmaAlpha > 0 and <= 1.0
  // Globalne
  assert pumpOnMsMax > 0
  assert cooldownMs >= 0
  assert reservoirCapacityMl > 0
  assert reservoirLowThresholdMl >= 0 and < reservoirCapacityMl
  assert heatBlockTempC > 0 and < 60
  assert directSunLuxThreshold > 0
  assert manualMaxHoldMs > 0 and <= 60000
  assert anomalyDryingRateMultiplier > 1.0 and <= 10.0
  // Vacation:
  assert vacationTargetReductionPct >= 0 and <= 50.0
  assert vacationMaxPulsesOverride >= 1 and <= 10
  assert vacationCooldownMultiplier >= 1.0 and <= 5.0
  // profil custom: asercje per pot (customTargetPct, etc.)
  return OK or list of issues
```

Strategia przy błędzie walidacji:
- Na starcie: jeśli persisted config nie przechodzi walidacji → użyj defaultów + loguj powód.
- Przy zmianie runtime: odrzuć zmianę i zwróć błąd do UI/NET.

### Wersjonowanie i migracje
Preferuj prostą strategię: `schemaVersion` + migracje krokowe.
```
loadConfig():
  raw = store.read()
  if missing: return defaults()
  cfg = decode(raw)
  while cfg.schemaVersion < CURRENT:
    cfg = migrate(cfg)
  if !validate(cfg):
    log("CFG_INVALID", details)
    return defaults()
  return cfg
```

Migracje powinny:
- nigdy nie usuwać krytycznych zabezpieczeń,
- w razie niepewności wracać do bezpiecznych defaultów.

### Zapis asynchroniczny (nie blokować ControlTask)
Zasada: ControlTask nie zapisuje flash.

Wzorzec:
- `ConfigService` publikuje nowy config natychmiast (dla domeny/UI).
- `PersistTask` (niski priorytet) zapisuje config w tle, z debouncingiem.

Pseudokod:
```
ConfigService.applyPatch(patch):
  cfgNew = merge(cfgCurrent, patch)
  if !validate(cfgNew): return error
  cfgCurrent = cfgNew
  publishConfig(cfgNew)
  PersistQueue.push(SAVE_REQUEST(cfgNew))
  return ok

PersistTask:
  pending = null
  lastReqAtMs = 0
  loop:
    req = PersistQueue.pop(timeout=100ms)
    if req: pending = req.cfg; lastReqAtMs = now
    if pending and now - lastReqAtMs > 500ms:   // debounce
      ok = store.write(pending)
      log(ok ? "CFG_SAVED" : "CFG_SAVE_FAIL")
      pending = null
```

### Przepływy zmian (UI/NET → domena)
Wszystko idzie eventami:
```
UiTask / NetTask:
  on user/remote action:
    EventQueue.push(REQUEST_SET_TARGET(x))

ControlTask:
  on REQUEST_SET_TARGET(x):
    result = ConfigService.applyPatch({targetMoisturePct: x})
    if result.ok: log("CFG_CHANGE", key="target", value=x)
    else: log("CFG_REJECT", reason=result.error)
```

### Bezpieczne defaulty (kierunek)
- Domyślnie: `antiOverflowEnabled = true`.
- `waterLevelUnknownPolicy = BLOCK`.
- Konserwatywne `pumpOnMsMax` i sensowny `cooldownMs`.

### Spec eventów konfiguracji (REQUEST/ACK)
Cel: UI/NET nie modyfikuje `Config` bezpośrednio — wysyła żądanie, a domena odpowiada ACK/ERR.

Pseudotyp eventu:
```
enum Source { UI, NET, SYSTEM }
enum Dest { CONTROL, UI, NET, BROADCAST }

struct Event {
  EventType type
  Source src
  Dest dest
  uint32 correlationId   // 0 jeśli nie dotyczy
  uint32 timestampMs
  payload
}
```

#### REQUEST (UI/NET → ControlTask)
Minimalny zestaw (warto trzymać mały):
- `REQUEST_SET_MODE(mode)`
- `REQUEST_SET_TARGET(pct)`
- `REQUEST_SET_HYSTERESIS(pct)` (opcjonalne, ale zalecane)
- `REQUEST_SET_PUMP_LIMITS(minMs, maxMs, hardTimeoutMs)`
- `REQUEST_SET_COOLDOWN(ms)`
- `REQUEST_SET_ANTIOVERFLOW(enabled)`
- `REQUEST_SET_WATERLEVEL_UNKNOWN_POLICY(policy)`
- `REQUEST_SAVE_CONFIG_NOW()` (wymuszenie natychmiastowego zapisu; normalnie zapis jest debounce)
- `REQUEST_FACTORY_RESET_CONFIG()`

Zasady:
- Każdy request niesie `correlationId` (UI/NET generuje), żeby dało się sparować odpowiedź.
- UI/NET może wysłać sekwencję zmian; domena stosuje je atomowo per event.
- ControlTask nie zapisuje flash — najwyżej kolejkuje SAVE.

#### ACK/ERR (ControlTask → UI/NET)
Odpowiedzi wracają eventami skierowanymi do źródła lub broadcastem:
- `CONFIG_APPLIED(changedKeys[], newRevision)`
- `CONFIG_REJECTED(reason, field)`
- `CONFIG_SAVE_QUEUED()` (opcjonalnie)
- `CONFIG_SAVED(ok, err)` (emitowane przez PersistTask po faktycznym zapisie)
- `CONFIG_RESET_DONE()`

Przykładowy flow:
```
UI -> REQUEST_SET_TARGET(42) [corr=123]
ControlTask:
  applyPatch({targetMoisturePct:42})
  if ok: emit CONFIG_APPLIED(["targetMoisturePct"], rev++) [corr=123]
  else: emit CONFIG_REJECTED("out_of_range", "targetMoisturePct") [corr=123]
PersistTask:
  after write: emit CONFIG_SAVED(true)
```

### Minimalne zasady logowania (Serial)
Log ma być zdarzeniowy, nie „spam” z każdej pętli.

Loguj zawsze (eventy):
- `BOOT`, `CFG_LOADED`, `CFG_MIGRATED`, `CFG_INVALID_FALLBACK_DEFAULTS`
- `CFG_CHANGE_APPLIED` / `CFG_CHANGE_REJECTED` (z `correlationId`, kluczami i powodem)
- `PUMP_ON` / `PUMP_OFF` (czas, planowana długość, powód)
- `SAFETY_BLOCK` / `SAFETY_UNBLOCK` (reason: OVERFLOW_RISK / TANK_EMPTY / SENSOR_FAIL / COOLDOWN / MANUAL / OVERFLOW_SENSOR_UNKNOWN / TANK_SENSOR_UNKNOWN)
- przejścia health: `SENSOR_FAIL`, `SENSOR_RECOVERED`
- I²C błędy w trybie rate‑limited (np. max 1 log / 2s per sensor)
- sieć: `WIFI_UP`, `WIFI_DOWN`, `WIFI_RETRY(backoffMs)` (tylko informacyjnie)

Nie loguj:
- haseł Wi‑Fi / tokenów.
- ciągłych surowych odczytów w wysokiej częstotliwości (jeśli trzeba, to tylko w debug mode i z throttlingiem).

Format (kierunek):
- jednozdaniowe rekordy `TAG key=value ...`.
- `reason=` zawsze jako stały enum/string, nie opis wolnym tekstem.

---

## WiFi Provisioning — konfiguracja startowa urządzenia

### Ocena planu

Plan jest poprawny i stosuje dobrze znany wzorzec **Captive Portal Provisioning**,
powszechny w urządzeniach IoT (ESP-IDF WiFi Provisioning, Tasmota, ESPHome, itp.).

**Mocne strony**:
- Brak hardkodowanych credentiali — urządzenie działa out-of-the-box.
- Factory reset przez fizyczny gest (długie przytrzymanie) — bezpieczny, trudny do przypadkowego wywołania.
- Fallback do AP po 3 nieudanych próbach — urządzenie nie zostaje w martwym stanie.
- Telegram Bot opcjonalny — nie blokuje podstawowej funkcji podlewania.
- Jeden prosty formularz na jednej stronie — minimalna złożoność UX.

**Ryzyka i zalecenia**:
| Ryzyko | Mitigacja |
|---|---|
| Otwarte AP bez hasła — ktoś może wejść i zmienić config | Akceptowalne dla LAN/IoT; AP auto-off po 5 min bez podłączonego klienta (zaimplementowane w AP_MODE loop). Opcjonalnie: prosty PIN na stronie |
| StickS3 ma **2 przyciski fizyczne** (BtnA + BtnB, M5Unified) + Dual Button na CH5 PbHUB | Factory reset: BtnA + BtnB (oba wbudowane) przez 5 s. Manual pump: Dual Button na CH5 (blue=pump wybranej doniczki, red=emergency stop). CH5 wymaga trybu IN/IN (oba piny input). |
| DNS captive portal może nie działać na wszystkich telefonach | Dodaj mDNS (`autogarden.local`) jako backup; wyświetl IP na ekranie StickS3 |
| 8MB flash — strona HTML musi zmieścić się w PROGMEM | OK — strona <10KB po gzip; używaj `server.send_P()` z PROGMEM |
| Telegram bot token to secret — nie powinien być widoczny po zapisaniu | Maskuj na UI po save (np. `sk-***...***`); w logu nigdy nie drukuj |
| WiFi scan może trwać kilka sekund | Zrób scan asynchronicznie w tle, wyświetl spinner; fallback: ręczne wpisanie SSID |

**Narzędzia / biblioteki**:
- `WiFi.h` (wbudowane w ESP32 Arduino) — AP + STA
- `WebServer.h` (wbudowane) — prosty HTTP server 80
- `DNSServer.h` (wbudowane) — captive portal redirect
- `Preferences.h` (wbudowane) — NVS flash storage
- `ESPmDNS.h` (wbudowane) — mDNS `autogarden.local`
- HTML/CSS/JS — strona serwowana z PROGMEM (surowy string lub gzip)
- Żadnych dodatkowych lib_deps — wszystko w ESP32 Arduino core!

### Telegram — aktualny zakres integracji

- Telegram jest dodatkiem do `NetTask` i nigdy nie blokuje automatyki podlewania.
- Wspierane są dwa źródła konfiguracji Telegram:
  - lokalny plik `include/telegram_local_config.h` do developmentu i lokalnych sekretów,
  - `NetConfig` zapisany w NVS przez captive portal.
- `include/telegram_local_config.h` pozostaje poza gitem, a śledzonym szablonem jest `include/telegram_local_config.example.h`.
- Nie logujemy tokenu bota ani nie zwracamy go w HTTP/UI.
- Bot autoryzuje rozmówców wyłącznie przez allowlistę `chat_ids`.
- Głównym wejściem użytkownika jest `/ag` lub `/start`; pozostałe akcje są wywoływane przez inline menu.
- Dostarczanie wiadomości obsługuje wiele odbiorców przez listę `chat_ids` rozdzielaną przecinkami.
- Polling Telegram działa adaptacyjnie w niskopriorytetowym `NetTask`, z krótkimi retry i bez długich blokad.

### Model danych provisioning

```
struct NetConfig {               // persisted w NVS (namespace "ag_net")
  bool   provisioned = false     // true po pierwszym udanym WiFi connect
  char   wifiSsid[33]           // max 32 chars + null
  char   wifiPass[65]           // max 64 chars + null (WPA2)
  char   telegramBotName[64]    // opcjonalny; pusty = użyj local config / brak TG
  char   telegramBotToken[64]   // opcjonalny; pusty = telegram wyłączony
  char   telegramChatIds[128]   // opcjonalny; CSV allowlista odbiorców
}
```

### Telegram — źródła konfiguracji i precedencja

```
bootSequence():
  netCfg = netConfigLoad()
  applyLocalTelegramConfig(netCfg)

applyLocalTelegramConfig(netCfg):
  configuredName = netCfg.telegramBotName
  if empty(configuredName):
    configuredName = AG_TELEGRAM_BOT_NAME

  if localTelegramSecretsAvailable():
    if empty(netCfg.telegramBotName):
      netCfg.telegramBotName = AG_TELEGRAM_BOT_NAME
    if empty(netCfg.telegramBotToken):
      netCfg.telegramBotToken = AG_TELEGRAM_BOT_TOKEN
    if empty(netCfg.telegramChatIds):
      netCfg.telegramChatIds = normalizeChatIdList(AG_TELEGRAM_CHAT_IDS)

  netState.telegramEnabled = not empty(netCfg.telegramBotToken)
                          and hasAnyChatId(netCfg.telegramChatIds)
```

### Stany provisioning (FSM)

```
enum ProvisioningState {
  BOOT_CHECK,        // sprawdź NVS: czy jest zapisany config?
  AP_MODE,           // brak configu lub factory reset → rozgłaszaj AP + captive portal
  WIFI_CONNECTING,   // stan logiczny: STA connect / reconnect wykonywany w tle przez NetTask
  WIFI_CONNECTED,    // STA połączone; AP może nadal działać równolegle w AP+STA
  WIFI_FAILED,       // stan przejściowy / diagnostyczny; runtime używa reconnect z backoff zamiast twardego abortu po 3 próbach
}
```

### Pseudokod provisioning

```
setup():
  netCfg = NVS.load("ag_net", NetConfig)
  applyLocalTelegramConfig(netCfg)

  if netCfg.provisioned and len(netCfg.wifiSsid) > 0:
    WiFi.mode(WIFI_STA)
    WiFi.begin(netCfg.wifiSsid, netCfg.wifiPass)
    log("BOOT quick WiFi begin; retries handled later by NetTask")
  else:
    log("BOOT offline mode; WiFi not provisioned")

netTaskInit(netCfg, netState):
  netState.wifiConnected = (WiFi.status() == WL_CONNECTED)
  netState.telegramEnabled = hasTokenAndChatIds(netCfg)

  if !netCfg.provisioned or len(netCfg.wifiSsid) == 0:
    startApNonBlocking(netCfg, netState)

startApNonBlocking(netCfg, netState):
  keepSta = netState.wifiConnected or (netCfg.provisioned and len(netCfg.wifiSsid) > 0)
  WiFi.mode(keepSta ? WIFI_AP_STA : WIFI_AP)
  WiFi.softAP("autogarden", "")
  WiFi.softAPConfig(192.168.4.1, 192.168.4.1, 255.255.255.0)
  dnsServer.start(53, "*", 192.168.4.1)
  mdns.begin("autogarden")
  webServer.on("/", handleRoot)
  webServer.on("/scan", handleScan)
  webServer.on("/save", HTTP_POST, handleSave)
  webServer.on("/skip", handleSkipWifi)
  webServer.on(captivePortalPaths..., handleRoot)
  webServer.onNotFound(handleRoot)
  webServer.begin()
  netState.apActive = true
  netState.apNoClientSinceMs = millis()

apTick(netState):
  dnsServer.processNextRequest()
  webServer.handleClient()
  if WiFi.softAPgetStationNum() > 0:
    netState.apNoClientSinceMs = millis()
  if millis() - netState.apNoClientSinceMs >= 5 min:
    stopAp(netState)

netTaskTick(nowMs, netState, netCfg):
  if netState.apActive:
    apTick(netState)
    if !netCfg.provisioned or len(netCfg.wifiSsid) == 0:
      return

  if !netCfg.provisioned or len(netCfg.wifiSsid) == 0:
    netState.wifiConnected = false
    return

  if WiFi.status() != WL_CONNECTED:
    netState.wifiConnected = false
    if backoffExpired(nowMs, netState.reconnectBackoffMs):
      WiFi.disconnect()
      WiFi.begin(netCfg.wifiSsid, netCfg.wifiPass)
      netState.reconnectAttempts += 1
      netState.reconnectBackoffMs = min(netState.reconnectBackoffMs * 2, 5 min)
    return

  netState.wifiConnected = true
  netState.reconnectAttempts = 0
  netState.reconnectBackoffMs = 5 s

handleRoot():
  webServer.send_P(200, "text/html", PROVISIONING_HTML)

handleScan():
  n = WiFi.scanNetworks()
  json = buildScanJson(n)       // [{ssid, rssi, encryption}, ...]
  webServer.send(200, "application/json", json)

handleSave():
  ssid     = sanitizeInput(webServer.arg("ssid"), 32)
  pass     = sanitizeInput(webServer.arg("pass"), 64)
  botName  = sanitizeInput(webServer.arg("bot_name"), 63)
  botToken = sanitizeInput(webServer.arg("bot_token"), 63)
  chatIds  = normalizeChatIdList(webServer.arg("chat_ids"), 127)

  if len(ssid) == 0:
    webServer.send(400, "text/plain", "SSID required")
    return

  if len(pass) > 0 and len(pass) < 8:
    webServer.send(400, "text/plain", "Password min 8 chars (WPA)")
    return

  if (len(botToken) > 0 or len(chatIds) > 0 or len(botName) > 0) and len(botName) == 0:
    webServer.send(400, "text/plain", "Bot name required when Telegram is configured")
    return

  if chatIds not empty and !isValidChatIdList(chatIds):
    webServer.send(400, "text/plain", "Chat IDs must be numeric (comma separated)")
    return

  if !isValidBotToken(botToken):
    webServer.send(400, "text/plain", "Invalid bot token format")
    return

  netCfg.wifiSsid = ssid
  netCfg.wifiPass = pass
  netCfg.telegramBotName = botName
  netCfg.telegramBotToken = botToken
  netCfg.telegramChatIds = chatIds
  netCfg.provisioned = true
  netCfg.schemaVersion = kNetConfigSchema
  NVS.save("ag_net", netCfg)

  webServer.send(200, "text/html", SUCCESS_HTML)   // "Saved! Restarting..."
  delay(1000)
  ESP.restart()
```

### Factory Reset — przywrócenie domyślnych

```
// Sprawdzaj w UiTask co tick (50-200ms):
factoryResetCheck(nowMs):
  // StickS3 ma 2 wbudowane przyciski: BtnA (front) + BtnB (power/side)
  // Factory reset = oba wbudowane przez 5s — nie wymaga Dual Button (CH5)
  btnA_pressed     = M5.BtnA.isPressed()
  btnB_pressed     = M5.BtnB.isPressed()

  if btnA_pressed and btnB_pressed:
    if holdStartMs == 0:
      holdStartMs = nowMs
    elif (nowMs - holdStartMs) >= 5000:          // 5 sekund ciągłego trzymania
      // Wyświetl na LCD:
      display.clear()
      display.print("FACTORY RESET?")
      display.print("Press main button")
      display.print("to confirm...")

      // Czekaj na potwierdzenie BtnA (osobne kliknięcie po puszczeniu): 
      waitForRelease(BtnA)    // puść wszystko
      deadline = millis() + 10000
      while millis() < deadline:
        M5.update()
        if M5.BtnA.wasClicked():
          // POTWIERDZONE — kasuj config
          log("FACTORY_RESET confirmed by user")
          NVS.clear("ag_net")        // Preferences.clear()
          display.print("Reset done!")
          display.print("Restarting...")
          delay(1000)
          ESP.restart()              // restart → BOOT_CHECK → netCfg.provisioned=false → AP_MODE

      // Timeout bez potwierdzenia
      display.print("Reset cancelled.")
      holdStartMs = 0
  else:
    holdStartMs = 0    // reset timera jeśli którykolwiek puścił
```

### Uwagi dot. bezpieczeństwa urządzenia pracującego bez WiFi

Provisioning **NIE blokuje automatyki podlewania**:
- Jeśli użytkownik nie skonfiguruje WiFi (AP mode timeout / brak klienta), urządzenie
  może przejść do normalnej pracy z `wifiEnabled = false`.
- AP provisioning działa w tle jako non-blocking captive portal.
- Jeśli zapisane są dane WiFi, urządzenie może pracować w trybie `AP+STA`, dzięki czemu
  provisioning i automatyka są dostępne równolegle.
- Jeśli WiFi się rozłączy w trakcie pracy → normalna automatyka działa dalej, a sieć
  próbuje reconnect w tle przez `NetTask` z backoff.

### Sekwencja boot (uwzględniająca provisioning)

```
setup():
  M5.begin(cfg)
  M5.Power.setExtOutput(true)    // Grove 5V ON — wymagane dla PbHUB i sensorów

  config = ConfigStore.load()     // domena: progi, pompa, kalibracja
  netCfg = NVS.load("ag_net", NetConfig)

  if netCfg.provisioned and len(netCfg.wifiSsid) > 0:
    startWifiStaConnect(netCfg)   // szybki start, bez blokowania boota
  else:
    log("offline mode; provisioning available from AP in background")

  // Taski startują niezależnie od stanu sieci
  startControlTask(config)
  startUiTask()
  startNetTask(netCfg)

  // NetTask:
  // - uruchamia AP non-blocking gdy brak provisioningu,
  // - utrzymuje reconnect STA z backoff,
  // - może działać w AP+STA.

loop():
  // Pusty — logika w taskach FreeRTOS
  vTaskDelay(portMAX_DELAY)
```

### Integracja z Config (domena vs sieć)

Dwa oddzielne namespace w NVS:
- `"ag_config"` — `Config` domenowy (progi, pompa, kalibracja, itp.)
- `"ag_net"` — `NetConfig` (WiFi SSID/pass, Telegram tokens)
- `"ag_hist"` — historia pomiarów (persisted: `level2`, `level3`, `wateringLog`)
- `"ag_dusk"` — `DuskState` (faza DAY/NIGHT, dayLengthMs, nightLengthMs)
- `"ag_runtime"` — `RuntimeState` (budżet rezerwuaru, trendy, cooldowny, solar clock, dusk timing)

Uzasadnienie: factory reset sieci (provisioning) **nie kasuje** kalibracji sensorów.
Pełny factory reset kasuje **wszystkie 5** namespace'ów. Osobny namespace = osobna kontrola.

```
RuntimeState {                     // persisted w NVS (namespace "ag_runtime", schema versioned)
  schema: uint16                   // kRuntimeSchema = 4
  // Water Budget
  reservoirCurrentMl: float
  totalPumpedMl: float
  totalPumpedMlPerPot[MAX_POTS]: float
  reservoirLow: bool
  secsSinceRefill: uint32          // seconds since last refill (→ lastRefillMs on boot)
  // Trend Baselines (per-pot)
  normalDryingRate[MAX_POTS]: float
  baselineCalibrated[MAX_POTS]: bool
  hourlyDeltas[MAX_POTS][24]: float
  trendHeadIdx[MAX_POTS]: uint8
  trendCount[MAX_POTS]: uint8
  // Cooldowns
  secsSinceLastCycleDone[MAX_POTS]: uint32
  // Dusk timing supplement
  secsSinceLastDusk: uint32
  secsSinceLastDawn: uint32
  nightSequence: uint32
  // Solar Clock
  solarCycleCount: uint8
  solarCalibrated: bool
}
```

Zapis RuntimeState:
- NATYCHMIASTOWY po każdym pump event (totalPumpedMl się zmieniło)
- NATYCHMIASTOWY po refill
- PERIODYCZNY co 60s gdy dane trendów się zmieniły (count/baseline)

```
factoryReset(scope):
  if scope == NET_ONLY:
    NVS.clear("ag_net")
  elif scope == FULL:
    NVS.clear("ag_net")
    NVS.clear("ag_config")
    NVS.clear("ag_hist")
    NVS.clear("ag_dusk")
    NVS.clear("ag_runtime")
```

---

## Algorytm podlewania — pełna specyfikacja domenowa

### Ocena planu i analiza słabych punktów

**Mocne strony programu**:
- Podlewanie pulsowe (pulse-soak-measure) — jedyny poprawny sposób dla doniczek; zalanie jest gorsze niż niedolanie.
- Budżet wody w rezerwuarze — pozwala planować i ostrzegać użytkownika z wyprzedzeniem.
- Profile roślin — eliminuje zgadywanie; parametry oparte na danych agronomicznych.
- Historia pomiarów z kompresją — pozwala na analizę trendów bez zapychania flash.
- Podlewanie wieczorne + rescue w dzień — standard ogrodniczy; chroni przed szokiem termicznym korzeni.
- Sensor przelewowy w doniczce z podwójnym dnem — daje margines bezpieczeństwa zanim woda wyleje na balkon.

**Zidentyfikowane słabe punkty i dodane mitigacje**:

| Słaby punkt | Ryzyko | Mitigacja w tej specyfikacji |
|---|---|---|
| Jeden czujnik wilgotności na całą doniczkę | Woda może nie dotrzeć równomiernie | Pulse-soak-measure z długim soakTime (30-45s); przyszłość: drugi czujnik |
| Brak czujnika przepływu na pompie | Nie wiemy ile wody naprawdę wpompowaliśmy | Kalibracja objętościowa (ml/s) + tracking skumulowany (`totalPumpedMl`) |
| Parowanie z rezerwuaru w upale | Budżet wody będzie zawyżony | Dodaj współczynnik parowania (opcjonalnie); konserwatywny `reservoirBufferPct` |
| Czujnik poziomu wody w doniczce może być zabrudzony/zasypany | Fałszywy TRIGGERED → blokada podlewania | Timeout: jeśli TRIGGERED >2h bez podlewania → WARN + override |
| Brak RTC (StickS3 nie ma baterii RTC) | Po restarcie nie znamy godziny → nie wiemy kiedy zachód słońca | **Detektor sensorowy (fuzja BH1750+SHT30+QMP6988)** wykrywa zmierzch/świt bez RTC. Fallback timer działa dopóki nie ma pierwszej wiarygodnej detekcji zmierzchu. |
| Temperatura gleby ≠ temperatura powietrza | ENV.III mierzy powietrze, nie glebę | Akceptowalne przybliżenie; próg „za gorąco" wystarczająco wysoki (>35°C) |
| Flash wear (ciągły zapis historii) | NVS ma limit ~100k write cycles per sector | Ring buffer z rzadkim flush (co 5-15 min); counter wear-leveling przez Preferences |

**Co bym jeszcze dodał** (zaimplementowane poniżej):
1. **Trend wilgotności** — obliczaj ΔmoisturePct/h z historii; alarm jeśli spada szybciej niż normalnie (np. pęknięty wąż, wyciek). → sekcja „Analiza trendów".
2. **Dwustopniowa filtracja moisture** — najpierw krótka mediana na surowym ADC, potem EMA na `%`. Mediana zbija pojedyncze szpilki z PbHUB/ADC, a EMA wygładza trend. → sekcja „Filtracja odczytów — EMA".
3. **Tryb wakacyjny** — zredukowane podlewanie + agresywne oszczędzanie wody z rezerwuaru. → sekcja „Tryb wakacyjny (Vacation Mode)".
4. **Detektor zmierzchu/świtu sensorowy** — fuzja BH1750 + SHT30 + QMP6988 wykrywa zachód/wschód bez RTC. SolarClock buduje model dnia po jednym cyklu. → sekcja „Detektor zmierzchu/świtu — fuzja sensorowa".
5. **Heartbeat Telegram** — codziennie rano krótki raport „wszystko OK / woda na X dni". → sekcja „Powiadomienia Telegram (NotificationService)".

---

### Profile roślin (PlantProfile)

Docelowe wilgotności oparte na danych agronomicznych (zakres dla uprawy w pojemnikach/donicach na balkonie):

| Roślina | `targetMoisturePct` | `criticalLowPct` | `maxMoisturePct` | Uwagi |
|---|---|---|---|---|
| **Pomidor** (Solanum lycopersicum) | **60-70%** | 40% | 80% | Regularny, głęboki; nie lubi mokrych liści. Owocowanie wymaga stabilnej wilgotności. |
| **Papryka** (Capsicum annuum) | **60-65%** | 35% | 75% | Podobna do pomidora, ale bardziej wrażliwa na przelanie. |
| **Bazylia** (Ocimum basilicum) | **55-65%** | 30% | 75% | Szybko więdnie przy suszy, ale gnije przy przelaniu. Częste małe dawki. |
| **Truskawka** (Fragaria × ananassa) | **65-75%** | 40% | 85% | Lubi wilgotno; owoce gniją przy zastoinach. Idealna na balkon. |
| **Chili / Habanero** (Capsicum chinense) | **50-60%** | 30% | 70% | Lekki stres wodny zwiększa ostrość. Mniej wody niż papryka słodka. |

```
struct PlantProfile {
  char     name[24]            // "Pomidor", "Papryka", ...
  float    targetMoisturePct   // optymalny środek zakresu
  float    criticalLowPct      // poniżej: rescue watering nawet w upale
  float    maxMoisturePct      // powyżej: STOP — ryzyko przelania / gnicia korzeni
  float    hysteresisPct       // strefa martwa wokół target (domyślnie 3%)
  uint32   soakTimeMs          // czas nasiąkania między pulsami (30000-45000)
  uint16   pulseWaterMl        // ile ml na jeden puls (kalibrowane z pumpMlPerSec)
  uint8    maxPulsesPerCycle    // max pulsów w jednym cyklu podlewania
}

// Wbudowane profile (const, PROGMEM):
PROFILES[] = {
  { "Pomidor",     65, 40, 80, 3, 35000, 25, 6 },
  { "Papryka",     62, 35, 75, 3, 35000, 25, 5 },
  { "Bazylia",     60, 30, 75, 3, 30000, 20, 4 },
  { "Truskawka",   70, 40, 85, 3, 40000, 25, 6 },
  { "Chili",       55, 30, 70, 3, 35000, 20, 5 },
  { "Custom",       0,  0,  0, 3, 35000, 25, 5 },  // user-defined
}
```

Użytkownik wybiera profil z listy (UI lub Telegram `SET_PLANT`). Profil „Custom" pozwala
na ręczne ustawienie wszystkich parametrów. Wybrany profil jest persisted w Config.

---

### Kalibracja pompy — objętość na czas

Pompa M5Stack Watering Unit ma stały przepływ zależny od napięcia zasilania (5V).
Przed użyciem musimy zmierzyć wydajność.

```
// Procedura kalibracji (jednorazowa, na stanowisku, per-pot):
calibratePump(potIdx):
  display.print("Pump calibration — pot %d", potIdx)
  display.print("Place tube in measuring cup")
  display.print("Press button to start 30s pump run")

  waitForButtonPress()

  startMs = millis()
  pump[potIdx].on()
  while millis() - startMs < 30000:   // 30 sekund
    M5.update()
    // safety: przerwij jeśli water level TRIGGERED
    if waterGuards.reservoirMin == TRIGGERED:
      pump[potIdx].off()
      display.print("ABORT: reservoir empty!")
      return ERROR

  pump[potIdx].off()

  display.print("Enter pumped volume (ml):")
  volumeMl = readUserInput()           // z UI lub Telegram

  pumpMlPerSec = volumeMl / 30.0
  config.pots[potIdx].pumpMlPerSec = pumpMlPerSec   // per-pot
  config.pots[potIdx].pumpCalibrated = true
  saveConfig()

  log("PUMP_CALIB ml_per_sec=%.2f volume_30s=%d", pumpMlPerSec, volumeMl)
  display.print("Calibrated: %.1f ml/s", pumpMlPerSec)
```

Typowa wydajność M5Stack Watering Unit: **~5-15 ml/s** (zależy od długości węża i wysokości).

**Zmierzona wydajność (test 2026-03-01):**
- **155 ml / 30 s = 5.17 ml/s = 310 ml/min**
- Pompa: M5Stack Watering Unit, zasilanie 5V przez PbHUB CH0 pin1

Wartość `pumpMlPerSec` pozwala przeliczyć czas pompowania na objętość i odwrotnie:
- `pumpDurationMs = (targetMl / pumpMlPerSec) * 1000`
- `pumpedMl = pumpDurationMs / 1000.0 * pumpMlPerSec`

---

### ⚠️ Problem: backflow (cofanie wody) po wyłączeniu pompy

**Odkrycie z testu:** Po wyłączeniu pompy woda cofa się rurką z powrotem do rezerwuaru
(lub kapie do doniczki grawitacyjnie, jeśli doniczka jest niżej).
Pompa M5Stack Watering Unit to prosta pompa perystaltyczna/membranowa **bez zaworu zwrotnego**.

**Skutki:**
- Niekontrolowane kapanie do doniczki po zakończeniu cyklu → overflow
- Utrata dokładności pomiaru przepompowanej objętości (część wraca)
- W skrajnym przypadku: syfon opróżnia rezerwuar

**Rozwiązanie: zawór zwrotny (check valve / one-way valve)**
- Miniaturowy plastikowy zawór zwrotny inline na rurkę silikonową (4mm ID / 6mm OD)
- Montaż: na rurce **za pompą** (w kierunku doniczki), strzałka w kierunku przepływu
- Dostępność: AliExpress, Amazon — szukaj: "one way check valve 4mm aquarium" lub "zawór zwrotny 4mm akwarium"
- Cena: ~2-5 PLN / szt
- Alternatywa: "mini solenoid valve 6V normally closed" — ale wymaga dodatkowego sterowania GPIO

**Zalecenie:** Zawór zwrotny plastikowy (pasywny) — najtańsze i najprostsze rozwiązanie.
Zamontować po jednym na każdą rurkę pompy (per-pot).

---

### Algorytm podlewania pulsowego (Pulse-Soak-Measure)

Zamiast jednorazowego zalania, podlewamy **krótkimi pulsami** z przerwami na nasiąknięcie:

```
// Stany cyklu podlewania (rozszerzenie FSM):
enum WateringPhase {
  IDLE,              // nie podlewamy; monitoring
  EVALUATING,        // ocena: czy, kiedy i ile podlewać
  PULSE,             // pompa ON — krótki puls wody
  SOAK,              // pompa OFF — czekamy na nasiąknięcie gleby (30-45s)
  MEASURING,         // odczyt wilgotności po nasiąknięciu
  OVERFLOW_WAIT,     // czujnik przelewowy aktywny — czekamy aż woda opadnie
  DONE,              // cykl zakończony — przejdź do cooldown
  BLOCKED,           // twarda blokada (sensor fail, manual lock, itp.)
}

// Kontekst jednego cyklu podlewania (per-pot):
struct WateringCycle {
  WateringPhase phase = IDLE
  uint8  potIndex                // która doniczka (0 lub 1)
  uint8  pulseCount = 0          // ile pulsów już wykonano w tym cyklu
  uint8  maxPulses               // z PlantProfile.maxPulsesPerCycle
  uint32 pulseDurationMs         // wyliczone z pulseWaterMl / pumpMlPerSec
  uint32 soakTimeMs              // z PlantProfile.soakTimeMs
  uint32 phaseStartMs = 0        // timestamp wejścia w bieżącą fazę
  float  moistureBeforeCycle     // wilgotność na starcie cyklu
  float  moistureAfterLastSoak   // wilgotność po ostatnim soaku
  uint32 totalPumpedMs = 0       // suma czasu pompowania w cyklu
  float  totalPumpedMl = 0       // suma wody w cyklu
}

// Stan podlewania — tablica per-pot:
WateringCycle cycles[MAX_POTS]   // cycles[0] = doniczka 0, cycles[1] = doniczka 1
```

#### Pseudokod głównej pętli podlewania

```
wateringTick(nowMs, sensors, config, profile, waterBudget, actuatorState):
  // === ITERACJA PER-POT ===
  // Każda doniczka ma niezależny cykl podlewania, ale wspólny rezerwuar.
  for potIdx in 0 ..< config.numPots:
    if not config.pots[potIdx].enabled: continue

    potCfg  = config.pots[potIdx]
    cycle   = cycles[potIdx]             // per-pot WateringCycle
    potSens = sensors.pots[potIdx]       // per-pot sensor snapshot (moisture, overflow)
    profile = PROFILES[potCfg.plantProfileIndex]
    pumpCh  = hwConfig.potChannels[potIdx].pumpOutputChannel

    // === SAFETY GATES (per-pot + globalne) ===
    safety = evaluateExtendedSafety(nowMs, potSens, config, potCfg, waterBudget, actuatorState)
    if safety.hardBlock:
      if cycle.phase == PULSE: pump[potIdx].off(nowMs, safety.reason)
      cycle.phase = BLOCKED
      notify(SAFETY_BLOCK, potIdx, safety.reason)
      continue  // sprawdź drugą doniczkę

    switch cycle.phase:

    // ==================== IDLE ====================
    case IDLE:
      // Sprawdź czy pora na podlewanie tę doniczkę
      schedule = evaluateSchedule(nowMs, potSens, sensors.env, config, potIdx)
      if schedule.shouldWater:
        cycle = newWateringCycle(profile, potIdx)
        cycle.moistureBeforeCycle = potSens.moisturePct
        cycle.phase = EVALUATING
        log("[POT%d] WATERING_CYCLE_START moisture=%.1f%% reason=%s",
            potIdx, potSens.moisturePct, schedule.reason)

    // ==================== EVALUATING ====================
    case EVALUATING:
      if potSens.moisturePct >= profile.targetMoisturePct:
        cycle.phase = DONE
        log("[POT%d] WATERING_SKIP reason=already_wet moisture=%.1f%%",
            potIdx, potSens.moisturePct)
        continue

      if potSens.moisturePct >= profile.maxMoisturePct:
        cycle.phase = DONE
        log("[POT%d] WATERING_SKIP reason=above_max moisture=%.1f%%",
            potIdx, potSens.moisturePct)
        continue

      // Oblicz czas pulsu na podstawie profilu i kalibracji pompy (per-pot)
      cycle.pulseDurationMs = (profile.pulseWaterMl / potCfg.pumpMlPerSec) * 1000

      // Jeśli reservoir LOW → zredukuj puls do 1/3
      if waterBudget.reservoirLow:
        cycle.pulseDurationMs = cycle.pulseDurationMs / 3
        cycle.maxPulses = min(cycle.maxPulses, 2)   // max 2 pulsy w trybie oszczędnym
        log("WATERING_REDUCED reason=reservoir_low")

      cycle.phase = PULSE
      cycle.phaseStartMs = nowMs

    // ==================== PULSE ====================
    case PULSE:
      if not pump[potIdx].isOn():
        pump[potIdx].on(nowMs, cycle.pulseDurationMs)
        log("[POT%d] PULSE_START n=%d/%d duration=%dms",
            potIdx, cycle.pulseCount+1, cycle.maxPulses, cycle.pulseDurationMs)

      if (nowMs - cycle.phaseStartMs) >= cycle.pulseDurationMs:
        pump[potIdx].off(nowMs, "pulse_done")
        cycle.pulseCount++
        cycle.totalPumpedMs += cycle.pulseDurationMs
        pulsedMl = cycle.pulseDurationMs / 1000.0 * potCfg.pumpMlPerSec
        cycle.totalPumpedMl += pulsedMl
        addPumped(waterBudget, pulsedMl)  // wspólny rezerwuar!

        log("[POT%d] PULSE_END n=%d pumped_ml=%.1f total_ml=%.1f",
            potIdx, cycle.pulseCount, pulsedMl, cycle.totalPumpedMl)

        // Sprawdź overflow sensor per-pot
        if potSens.waterGuards.potMax == TRIGGERED:
          cycle.phase = OVERFLOW_WAIT
          cycle.phaseStartMs = nowMs
          log("[POT%d] OVERFLOW_DETECTED after pulse %d", potIdx, cycle.pulseCount)
        else:
          cycle.phase = SOAK
          cycle.phaseStartMs = nowMs

    // ==================== SOAK ====================
    case SOAK:
      // Czekamy soakTimeMs na nasiąknięcie gleby
      if (nowMs - cycle.phaseStartMs) >= cycle.soakTimeMs:
        cycle.phase = MEASURING

    // ==================== MEASURING ====================
    case MEASURING:
      moistureNow = potSens.moisturePct
      cycle.moistureAfterLastSoak = moistureNow

      log("[POT%d] SOAK_MEASURE moisture=%.1f%% target=%.1f%% pulse=%d/%d",
          potIdx, moistureNow, profile.targetMoisturePct, cycle.pulseCount, cycle.maxPulses)

      if moistureNow >= profile.targetMoisturePct:
        cycle.phase = DONE
        log("[POT%d] WATERING_TARGET_REACHED moisture=%.1f%%", potIdx, moistureNow)

      elif moistureNow >= profile.maxMoisturePct:
        cycle.phase = DONE
        log("[POT%d] WATERING_STOP reason=max_exceeded moisture=%.1f%%", potIdx, moistureNow)

      elif cycle.pulseCount >= cycle.maxPulses:
        cycle.phase = DONE
        log("[POT%d] WATERING_STOP reason=max_pulses moisture=%.1f%% pulses=%d",
            potIdx, moistureNow, cycle.pulseCount)

      elif potSens.waterGuards.potMax == TRIGGERED:
        cycle.phase = OVERFLOW_WAIT
        cycle.phaseStartMs = nowMs

      else:
        cycle.phase = PULSE
        cycle.phaseStartMs = nowMs

    // ==================== OVERFLOW_WAIT ====================
    case OVERFLOW_WAIT:
      elapsed = nowMs - cycle.phaseStartMs

      if potSens.waterGuards.potMax != TRIGGERED:
        log("[POT%d] OVERFLOW_CLEARED after %ds", potIdx, elapsed/1000)

        if potSens.moisturePct < profile.targetMoisturePct:
          cycle.pulseDurationMs = cycle.pulseDurationMs / 3
          cycle.phase = PULSE
          cycle.phaseStartMs = nowMs
          log("[POT%d] OVERFLOW_RESUME reduced_pulse", potIdx)
        else:
          cycle.phase = DONE

      elif elapsed > config.overflowMaxWaitMs:
        cycle.phase = DONE
        notify(OVERFLOW_STUCK, potIdx, "water not draining after %ds", elapsed/1000)
        log("[POT%d] OVERFLOW_TIMEOUT — ending cycle", potIdx)

      // else: nadal czekamy

    // ==================== DONE ====================
    case DONE:
      log("[POT%d] WATERING_CYCLE_DONE pulses=%d total_ml=%.1f before=%.1f%% after=%.1f%%",
          potIdx, cycle.pulseCount, cycle.totalPumpedMl,
          cycle.moistureBeforeCycle, cycle.moistureAfterLastSoak)

      history.addWateringEvent({
        potIndex: potIdx,
        timestampMs: nowMs,
        pulses: cycle.pulseCount,
        totalMl: cycle.totalPumpedMl,
        moistureBefore: cycle.moistureBeforeCycle,
        moistureAfter: cycle.moistureAfterLastSoak,
        reason: schedule.reason
      })

      notify(WATERING_DONE, potIdx, cycle)

      // Cooldown per-pot
      cycle.phase = IDLE
      cooldowns[potIdx].startMs = nowMs

    // ==================== BLOCKED ====================
    case BLOCKED:
      if safety.cleared:
        cycle.phase = IDLE
        notify(SAFETY_UNBLOCK, potIdx)
```

---

### Harmonogram podlewania (Schedule)

```
evaluateSchedule(nowMs, potSens, envSens, config, potIdx):
  // potSens = per-pot sensor snapshot (moisture, overflow)
  // envSens = wspólne sensory (temp, lux, ciśnienie) — jedno ENV.III + BH1750
  tempC = envSens.tempC
  lux   = envSens.lux
  moisture = potSens.moisturePct

  potCfg  = config.pots[potIdx]
  profile = PROFILES[potCfg.plantProfileIndex]

  // === 0. VACATION MODE — nadpisz parametry profilu ===
  vacProf = applyVacationOverrides(profile, config)
  targetPct   = vacProf.adjustedTargetPct
  maxPulses   = vacProf.adjustedMaxPulses
  cooldownMs  = vacProf.adjustedCooldownMs
  crisisThPct = vacProf.adjustedCrisisThresholdPct

  // === 0b. VACATION + ANOMALY — blokada prewencyjna (per-pot trend) ===
  if config.vacationMode:
    currentRate = trendCurrentRate(potIdx)
    if trendBaselineLearned(potIdx):
      baseline = trendBaselineRate(potIdx)              // ujemna wartość %/h
      threshold = baseline * config.anomalyDryingRateMultiplier
      if currentRate < threshold:                       // schnie istotnie szybciej niż baseline
        log("[POT%d] SCHEDULE_BLOCK reason=vacation_anomaly rate=%.2f%%/h baseline=%.2f%%/h threshold=%.2f%%/h",
            potIdx, currentRate, baseline, threshold)
        return { shouldWater: false, reason: "VACATION_ANOMALY_BLOCK" }
    else:
      threshold = -config.anomalyDryingRateThreshold
      if currentRate < threshold:
        log("[POT%d] SCHEDULE_BLOCK reason=vacation_anomaly_unlearned rate=%.2f%%/h threshold=%.2f%%/h",
            potIdx, currentRate, threshold)
        return { shouldWater: false, reason: "VACATION_ANOMALY_BLOCK" }

  // === 1. RESCUE MODE — ratunkowe podlewanie w krytycznej suszy ===
  if moisture < profile.criticalLowPct:
    return { shouldWater: true, reason: "RESCUE_CRITICAL_LOW" }

  // === 2. COOLDOWN — nie podlewaj jeśli niedawno podlewaliśmy (per-pot) ===
  if inCooldown(nowMs, cooldownMs, potIdx):   // vacation-adjusted, per-pot cooldown
    return { shouldWater: false }

  // === 3. WARUNKI POGODOWE — blokada w upale ===
  if tempC > config.heatBlockTempC:        // domyślnie 35°C
    log("SCHEDULE_BLOCK reason=too_hot temp=%.1f", tempC)
    return { shouldWater: false, reason: "HEAT_BLOCK" }

  if lux > config.directSunLuxThreshold:    // domyślnie 40000 lux
    log("SCHEDULE_BLOCK reason=direct_sun lux=%.0f", lux)
    return { shouldWater: false, reason: "DIRECT_SUN" }

  // === 4. PRIMARY: Detektor zmierzchu sensorowy (bez RTC) ===
  // Patrz: sekcja „Detektor zmierzchu/świtu — fuzja sensorowa"
  dusk = duskDetector

  // Okno podlewania: 2h po potwierdzonej detekcji zmierzchu
  if dusk.lastDuskMs > 0:
    timeSinceDusk = nowMs - dusk.lastDuskMs
    if dusk.phase in [NIGHT, DUSK_TRANSITION] and timeSinceDusk < config.duskWateringWindowMs:    // 2h
      if moisture < targetPct - profile.hysteresisPct:  // vacation-adjusted target
        return { shouldWater: true, reason: "DUSK_SENSOR" }

  // Opcjonalne okno poranne (~1h przed estymowanym świtem)
  if config.morningWateringEnabled and dusk.solarClock.calibrated:
    estimatedDawn = estimateNextDawn(dusk)
    timeUntilDawn = estimatedDawn - nowMs
    if timeUntilDawn > 0 and timeUntilDawn < 3600000:
      if moisture < targetPct - profile.hysteresisPct:  // vacation-adjusted target
        return { shouldWater: true, reason: "PRE_DAWN_ESTIMATE" }

  // Uwaga: brak limitu "jedno automatyczne podlewanie na noc per doniczka".
  // Jeśli po cyklu wilgotność nadal pozostaje poniżej triggera i inne blokady nie działają,
  // scheduler może ponownie uruchomić cykl w tym samym oknie nocnym.

  // === 5. FALLBACK (brak detekcji i brak potwierdzonego dnia) ===
  if dusk.lastDuskMs == 0 and dusk.phase not in [DAY, DAWN_TRANSITION]:
    if (nowMs - lastWateringEndMs) > config.fallbackIntervalMs:
      if moisture < targetPct - profile.hysteresisPct:  // vacation-adjusted target
        return { shouldWater: true, reason: "FALLBACK_TIMER" }

  // === 6. W DZIEŃ czekamy na zmierzch ===
  if dusk.phase in [DAY, DAWN_TRANSITION]:
    return { shouldWater: false, reason: "WAIT_FOR_DUSK" }

  return { shouldWater: false }
```

#### Detektor zmierzchu/świtu — fuzja sensorowa (bez RTC)

**Problem**: Nie mamy RTC ani sieci. Nie znamy godziny. Jedyne źródło informacji
o porze dnia to czujniki: BH1750 (światło), SHT30 (temp + wilgotność), QMP6988 (ciśnienie).

**Kluczowa obserwacja**: Zachód słońca ma **unikalną sygnaturę multisensorową**,
której żaden artefakt (chmura, cień, burza) nie reprodukuje w pełni:

| Sygnał | Zachód słońca | Chmura/cień budynku | Burza/front |
|---|---|---|---|
| Światło (lux) | Powolny spadek (30-90 min) z >1000 → <10 | Nagły skok ↓↑ lub plateau ~500-5000 | Nagły spadek, może wrócić |
| Tempo spadku lux (dL/dt) | Stabilne, umiarkowane (−20..−200 lux/min) | Gwałtowne (−1000+ lux/min) | Bardzo gwałtowne |
| Temperatura (°C) | Spada od ~2h, przyspiesza | Brak zmiany | Spadek dopiero po deszczu |
| Wilgotność (%RH) | Rośnie, przyspiesza | Brak zmiany | Nagły skok (deszcz) |
| Ciśnienie (hPa) | Stabilny lub lekki wzrost (fala półdobowa) | Stabilny | Gwałtowny spadek |

##### Maszyna stanów DayPhase

```
enum DayPhase {
  NIGHT,              // ciemno, czekamy na świt
  DAWN_TRANSITION,    // światło rośnie + temp zaczyna rosnąć → potencjalny świt
  DAY,                // jasno, normalny monitoring
  DUSK_TRANSITION,    // sygnatura zachodu wykryta → potencjalny zmierzch
}
// Nie „DUSK" jako stan końcowy — po potwierdzeniu przechodzi w NIGHT.
```

##### Dane wejściowe — sliding window

Detektor operuje na **oknie 60 minut** próbek pobieranych co ~30-60s.
Nie potrzebuje zegara — wystarczy `millis()`.

```
struct EnvSample {
  uint32 ms             // millis()
  float  lux            // BH1750
  float  tempC          // SHT30
  float  humPct         // SHT30
  float  pressHPa       // QMP6988
}

// Ring buffer: 60 próbek × 1/min = 60 min okno
// Pamięć: 60 × 20B = ~1.2KB
ENV_WINDOW_SIZE = 60
ENV_SAMPLE_INTERVAL_MS = 60000   // 1 min

struct DuskDetector {
  EnvSample window[ENV_WINDOW_SIZE]
  uint8     head = 0
  uint8     count = 0
  DayPhase  phase = NIGHT            // po restarcie: zakładamy noc (bezpieczniej)
  uint32    lastDawnMs = 0           // millis() ostatniego wykrytego świtu
  uint32    lastDuskMs = 0           // millis() ostatniego zmierzchu
  uint32    dayLengthMs = 0          // dawn→dusk (estymata długości dnia)
  uint32    nightLengthMs = 0        // dusk→dawn
  uint32    transitionStartMs = 0    // kiedy weszliśmy w TRANSITION
  float     duskScore = 0            // bieżący score fuzji (0.0-1.0)
  float     dawnScore = 0
}
```

##### Obliczanie sygnałów pochodnych

```
computeDerivatives(window, count, spanMinutes):
  // Użyj prostej regresji liniowej na ostatnich `spanMinutes` próbkach.
  // Dla sunset detection: spanMinutes = 30 (pół godziny trendu).
  // Zwraca tempo zmian na minutę.

  n = min(count, spanMinutes)
  if n < 5: return null  // za mało danych

  samples = lastN(window, n)

  dLux_dt  = linearSlope(samples[].lux,   samples[].ms)   // lux/ms → przelicz na lux/min
  dTemp_dt = linearSlope(samples[].tempC,  samples[].ms)   // °C/min
  dHum_dt  = linearSlope(samples[].humPct, samples[].ms)   // %RH/min
  dPress_dt= linearSlope(samples[].pressHPa, samples[].ms) // hPa/min

  return { dLux_dt, dTemp_dt, dHum_dt, dPress_dt }

linearSlope(values[], timestamps[]):
  // Least-squares fit: y = a*x + b → zwraca a (tempo zmian)
  // Znormalizuj timestamps do minut od pierwszego sample
  n = len(values)
  sumX = sumY = sumXY = sumXX = 0
  for i in 0..n:
    x = (timestamps[i] - timestamps[0]) / 60000.0   // ms → min
    y = values[i]
    sumX += x; sumY += y; sumXY += x*y; sumXX += x*x
  denom = n * sumXX - sumX * sumX
  if abs(denom) < 1e-6: return 0
  return (n * sumXY - sumX * sumY) / denom
```

##### Scoring — sygnatura zmierzchu

Każdy czujnik daje score 0.0-1.0 opisujący jak bardzo bieżący stan „pachnie"
zachodem słońca. Composite score jest ważoną średnią.

```
DUSK_WEIGHTS = {
  light:      0.45,   // najsilniejszy sygnał
  lightRate:  0.20,   // tempo spadku (odróżnia od chmury)
  temp:       0.15,   // potwierdzenie: temp spada
  humidity:   0.10,   // potwierdzenie: wilgotność rośnie
  pressure:   0.10,   // stabilne lub rosnące (odrzuć burzę)
}

scoreDusk(currentLux, derivatives):
  scores = {}

  // --- 1. LIGHT LEVEL (absolute) ---
  // Dusk zone: 1-200 lux (zmierzch cywilny).
  // >10000 lux = pewno dzień. <1 lux = już noc.
  if currentLux > 10000:
    scores.light = 0.0
  elif currentLux > 1000:
    scores.light = 0.1
  elif currentLux > 200:
    scores.light = 0.3 + 0.4 * (1.0 - currentLux / 200.0)
  elif currentLux > 1:
    scores.light = 0.7 + 0.3 * (1.0 - currentLux / 200.0)
  else:
    scores.light = 1.0    // <1 lux → ciemno

  // --- 2. LIGHT RATE OF CHANGE ---
  // Zachód: lux spada umiarkowanie (−5..−200 lux/min, zależy od poziomu)
  // Chmura: gwałtowny spadek (−500..−5000 lux/min)
  // Kluczowe: normalizujemy tempo do poziomu → procentowy spadek/min
  if currentLux > 10:
    relativeRate = derivatives.dLux_dt / currentLux   // %/min (ujemna = spada)
  else:
    relativeRate = derivatives.dLux_dt   // przy niskim lux: absolutny

  if relativeRate > -0.005:
    scores.lightRate = 0.0   // nie spada lub rośnie
  elif relativeRate > -0.02:
    // Umiarkowany spadek → typowy zachód
    scores.lightRate = mapRange(relativeRate, -0.005, -0.02, 0.0, 1.0)
  elif relativeRate > -0.1:
    // Szybszy spadek — nadal możliwy zachód (gęste chmury + zachód)
    scores.lightRate = 0.8
  else:
    // Za gwałtowny → raczej chmura, nie zachód
    scores.lightRate = 0.2

  // --- 3. TEMPERATURE DECLINING ---
  // Zachód: temp spada −0.01..−0.1 °C/min (zależy od pory roku)
  if derivatives.dTemp_dt < -0.005:
    scores.temp = min(1.0, abs(derivatives.dTemp_dt) / 0.05)   // pełny score przy -0.05°C/min
  else:
    scores.temp = 0.0   // temp nie spada → mało prawdopodobny zachód

  // --- 4. HUMIDITY RISING ---
  // Zachód: wilgotność rośnie +0.01..+0.1 %RH/min
  if derivatives.dHum_dt > 0.005:
    scores.humidity = min(1.0, derivatives.dHum_dt / 0.05)
  else:
    scores.humidity = 0.0

  // --- 5. PRESSURE STABILITY (odrzuć burzę) ---
  // Zachód: ciśnienie stabilne lub lekko rosnące (fala półdobowa wieczorem)
  // Burza: spadek >0.03 hPa/min = ~2 hPa/h
  if abs(derivatives.dPress_dt) < 0.02:
    scores.pressure = 1.0    // stabilne → OK
  elif derivatives.dPress_dt > 0:
    scores.pressure = 0.9    // rośnie → typowe wieczorem (fala półdobowa)
  elif derivatives.dPress_dt > -0.03:
    scores.pressure = 0.5    // lekki spadek — może front, ale nie burza
  else:
    scores.pressure = 0.0    // gwałtowny spadek → burza, nie zachód

  // --- COMPOSITE ---
  composite = 0
  for key in scores:
    composite += scores[key] * DUSK_WEIGHTS[key]

  return { composite, scores }   // composite: 0.0-1.0
```

##### Scoring — sygnatura świtu (analogiczny, odwrócone znaki)

```
DAWN_WEIGHTS = {
  light:      0.50,   // jeszcze ważniejsze — bo wychodzimy z ciemności
  lightRate:  0.20,
  temp:       0.15,
  humidity:   0.10,
  pressure:   0.05,
}

scoreDawn(currentLux, derivatives):
  scores = {}

  // Light: rośnie od <1 do >100 lux
  if currentLux < 1:
    scores.light = 0.0
  elif currentLux < 50:
    scores.light = 0.3 + 0.4 * (currentLux / 50.0)
  elif currentLux < 500:
    scores.light = 0.7 + 0.3 * min(1.0, currentLux / 500.0)
  else:
    scores.light = 1.0

  // Light rate: rośnie umiarkowanie (świt trwa 30-60 min)
  if derivatives.dLux_dt > 0.5:
    scores.lightRate = min(1.0, derivatives.dLux_dt / 50.0)    // rośnie → OK
  else:
    scores.lightRate = 0.0

  // Temperature: stabilna lub zaczyna rosnąć (świt = minimum dobowe)
  if derivatives.dTemp_dt > -0.005:   // przestała spadać
    scores.temp = min(1.0, max(0.0, (derivatives.dTemp_dt + 0.005) / 0.03))
  else:
    scores.temp = 0.0

  // Humidity: stabilna lub zaczyna spadać
  if derivatives.dHum_dt < 0.005:
    scores.humidity = min(1.0, max(0.0, (0.005 - derivatives.dHum_dt) / 0.03))
  else:
    scores.humidity = 0.0

  // Pressure: dowolny (mniej diagnostyczny o świcie)
  scores.pressure = 0.5

  composite = weightedSum(scores, DAWN_WEIGHTS)
  return { composite, scores }
```

##### FSM detektora — przejścia stanowe

```
DUSK_SCORE_ENTER_THRESHOLD  = 0.55   // wejdź w DUSK_TRANSITION
DUSK_SCORE_CONFIRM_THRESHOLD = 0.65  // potwierdź zmierzch
DUSK_SCORE_CANCEL_THRESHOLD = 0.30   // anuluj (fałszywy alarm)
DAWN_SCORE_ENTER_THRESHOLD  = 0.50
DAWN_SCORE_CONFIRM_THRESHOLD = 0.60
DAWN_SCORE_CANCEL_THRESHOLD = 0.25

TRANSITION_CONFIRM_DURATION_MS = 15 * 60 * 1000   // 15 min sustained score
TRANSITION_MAX_DURATION_MS     = 120 * 60 * 1000   // max 2h w transition (timeout)
MIN_DAY_DURATION_MS    = 4 * 3600 * 1000   // min 4h dnia (odrzuć podwójny świt/zmierzch)
MIN_NIGHT_DURATION_MS  = 3 * 3600 * 1000   // min 3h nocy

duskDetectorTick(nowMs, sensors, detector):
  // Dodaj próbkę do okna
  if (nowMs - detector.window[detector.head].ms) >= ENV_SAMPLE_INTERVAL_MS:
    addSample(detector, {
      ms: nowMs,
      lux: sensors.light.lux,
      tempC: sensors.env.tempC,
      humPct: sensors.env.humPct,
      pressHPa: sensors.env.pressHPa
    })

  if detector.count < 10:
    return   // za mało danych — czekamy

  derivatives = computeDerivatives(detector.window, detector.count, 30)
  currentLux = sensors.light.lux

  switch detector.phase:

    // ==================== NIGHT ====================
    case NIGHT:
      dawn = scoreDawn(currentLux, derivatives)
      detector.dawnScore = dawn.composite

      if dawn.composite >= DAWN_SCORE_ENTER_THRESHOLD:
        // Odrzuć fałszywy świt: sprawdź czy noc trwała wystarczająco długo
        if detector.lastDuskMs > 0:
          nightSoFar = nowMs - detector.lastDuskMs
          if nightSoFar < MIN_NIGHT_DURATION_MS:
            log("DAWN_REJECT reason=night_too_short elapsed=%dmin", nightSoFar/60000)
            return

        detector.phase = DAWN_TRANSITION
        detector.transitionStartMs = nowMs
        log("DAWN_TRANSITION_START score=%.2f lux=%.0f", dawn.composite, currentLux)

    // ==================== DAWN_TRANSITION ====================
    case DAWN_TRANSITION:
      dawn = scoreDawn(currentLux, derivatives)
      detector.dawnScore = dawn.composite
      elapsed = nowMs - detector.transitionStartMs

      if dawn.composite < DAWN_SCORE_CANCEL_THRESHOLD:
        // Fałszywy alarm (np. sztuczne światło, samochód)
        detector.phase = NIGHT
        log("DAWN_CANCEL score=%.2f after %dmin", dawn.composite, elapsed/60000)

      elif dawn.composite >= DAWN_SCORE_CONFIRM_THRESHOLD and elapsed >= TRANSITION_CONFIRM_DURATION_MS:
        // Potwierdzone! → DAY
        detector.phase = DAY
        detector.lastDawnMs = nowMs

        if detector.lastDuskMs > 0:
          detector.nightLengthMs = nowMs - detector.lastDuskMs
          log("DAWN_CONFIRMED night_was=%dmin", detector.nightLengthMs/60000)
        else:
          log("DAWN_CONFIRMED (first since boot)")

      elif elapsed > TRANSITION_MAX_DURATION_MS:
        // Timeout — pewnie pochmurny świt, ale światło jest → DAY
        detector.phase = DAY
        detector.lastDawnMs = nowMs
        log("DAWN_TIMEOUT — assuming day after %dmin", elapsed/60000)

    // ==================== DAY ====================
    case DAY:
      dusk = scoreDusk(currentLux, derivatives)
      detector.duskScore = dusk.composite

      if dusk.composite >= DUSK_SCORE_ENTER_THRESHOLD:
        // Odrzuć fałszywy zmierzch: sprawdź czy dzień trwał wystarczająco
        if detector.lastDawnMs > 0:
          daySoFar = nowMs - detector.lastDawnMs
          if daySoFar < MIN_DAY_DURATION_MS:
            log("DUSK_REJECT reason=day_too_short elapsed=%dmin", daySoFar/60000)
            return

        detector.phase = DUSK_TRANSITION
        detector.transitionStartMs = nowMs
        log("DUSK_TRANSITION_START score=%.2f lux=%.0f dLux=%.1f/min dTemp=%.3f/min",
            dusk.composite, currentLux, derivatives.dLux_dt, derivatives.dTemp_dt)

    // ==================== DUSK_TRANSITION ====================
    case DUSK_TRANSITION:
      dusk = scoreDusk(currentLux, derivatives)
      detector.duskScore = dusk.composite
      elapsed = nowMs - detector.transitionStartMs

      if dusk.composite < DUSK_SCORE_CANCEL_THRESHOLD:
        // Fałszywy alarm — chmura przeszła
        detector.phase = DAY
        log("DUSK_CANCEL score=%.2f after %dmin lux=%.0f", dusk.composite, elapsed/60000, currentLux)

      elif dusk.composite >= DUSK_SCORE_CONFIRM_THRESHOLD and elapsed >= TRANSITION_CONFIRM_DURATION_MS:
        // POTWIERDZONE! → NIGHT → to jest moment na podlewanie!
        detector.phase = NIGHT
        detector.lastDuskMs = nowMs

        if detector.lastDawnMs > 0:
          detector.dayLengthMs = nowMs - detector.lastDawnMs
          log("DUSK_CONFIRMED day_was=%dmin", detector.dayLengthMs/60000)
        else:
          log("DUSK_CONFIRMED (first since boot)")

        // >>> ZWOLNIJ PODLEWANIE <<<
        EventQueue.push(DUSK_DETECTED)

      elif elapsed > TRANSITION_MAX_DURATION_MS:
        // Timeout — pewnie bardzo pochmurny dzień przeszedł w noc
        detector.phase = NIGHT
        detector.lastDuskMs = nowMs
        log("DUSK_TIMEOUT — assuming night after %dmin", elapsed/60000)
        EventQueue.push(DUSK_DETECTED)
```

##### Estymacja „zegara słonecznego" (bez RTC)

Po kilku cyklach dawn/dusk urządzenie buduje wewnętrzny model dnia:

```
struct SolarClock {
  uint32 lastDawnMs             // millis() ostatniego świtu
  uint32 lastDuskMs             // millis() ostatniego zmierzchu
  uint32 dayLengthMs            // ~8-17h zależnie od sezonu (Warszawa 52°N)
  uint32 nightLengthMs          // reszta doby
  uint32 estimatedDayMs         // dayLengthMs + nightLengthMs ≈ 86400000 (raz na dobę)
  bool   calibrated = false     // true po pierwszym pełnym cyklu dawn→dusk→dawn
  uint8  cycleCount = 0         // ile pełnych cykli zaobserwowano
}

updateSolarClock(detector, clock):
  if detector.dayLengthMs > 0 and detector.nightLengthMs > 0:
    clock.dayLengthMs = detector.dayLengthMs
    clock.nightLengthMs = detector.nightLengthMs
    clock.estimatedDayMs = clock.dayLengthMs + clock.nightLengthMs
    clock.calibrated = true
    clock.cycleCount++
    log("SOLAR_CLOCK day=%dh%dm night=%dh%dm cycle=%d",
        clock.dayLengthMs/3600000, (clock.dayLengthMs/60000)%60,
        clock.nightLengthMs/3600000, (clock.nightLengthMs/60000)%60,
        clock.cycleCount)

// Predykcja następnego świtu (przybliżona):
estimateNextDawn(detector, clock):
  if not clock.calibrated:
    return UNKNOWN
  if detector.phase == NIGHT or detector.phase == DAWN_TRANSITION:
    return detector.lastDuskMs + clock.nightLengthMs
  if detector.phase == DAY or detector.phase == DUSK_TRANSITION:
    // Dzień — następny świt dopiero po zmierzchu i nocy
    return detector.lastDawnMs + clock.dayLengthMs + clock.nightLengthMs

// Predykcja następnego zmierzchu (przybliżona):
estimateNextDusk(detector, clock):
  if not clock.calibrated:
    return UNKNOWN    // za mało danych

  if detector.phase == DAY or detector.phase == DUSK_TRANSITION:
    // Dzień — szacuj ile zostało do zmierzchu
    elapsedSinceDawn = nowMs - detector.lastDawnMs
    return detector.lastDawnMs + clock.dayLengthMs   // millis() przybliżonego zmierzchu

  if detector.phase == NIGHT or detector.phase == DAWN_TRANSITION:
    // Noc — szacuj świt, potem dodaj długość dnia
    estimatedNextDawn = detector.lastDuskMs + clock.nightLengthMs
    return estimatedNextDawn + clock.dayLengthMs

// Dzięki temu schedule może "z wyprzedzeniem" przygotować podlewanie:
// np. 30 min przed estymowanym zmierzchem → sprawdź moisture i gotowość.
```

##### Zachowanie po restarcie / pierwszy dzień

```
// Po starcie: phase = NIGHT (bezpieczne założenie — nie podlewamy od razu)
// - Jeśli jest jasno (lux > 500): szybko przejdzie DAWN_TRANSITION → DAY
// - Jeśli ciemno: czeka na prawdziwy świt
// - Pierwszy zmierzch: DUSK_DETECTED → podlewa
// - Pierwszy pełny cykl: SolarClock.calibrated = true

// Fallback dopóki nie skalibrowany:
// Jeśli SolarClock nie jest calibrated i minęło > fallbackIntervalMs od ostatniego
// podlewania i moisture < target → podlewaj (patrz: evaluateSchedule fallback)
```

##### Scenariusze i odporność

| Scenariusz | Zachowanie detektora |
|---|---|
| **Bezchmurny dzień → zachód** | Score rośnie od ~0.3 (cień budynku) do 0.7+ (zmierzch). Potwierdzony po 15 min. ✅ |
| **Chmura przechodzi w dzień** | Lux spada gwałtownie (dLux/dt < −1000/min) → lightRate score niski. Temp/hum bez zmiany → composite < 0.55 → NIE wchodzi w transition. ✅ |
| **Mocno zachmurzone niebo** | Lux niski cały dzień (~500-3000). Zachód: spadek z 500 do <10. Tempo wolniejsze ale stabilne. Temp spada, hum rośnie. Potwierdzony, ale z opóźnieniem → timeout path. ✅ |
| **Burza wieczorem** | Lux spada gwałtownie + ciśnienie spada > 0.03 hPa/min → pressure score = 0, composite niedostateczny → DUSK_CANCEL. Po burzy: jeśli dalej ciemno i temp/hum OK → ponowne DUSK_TRANSITION. ✅ |
| **Cień budynku od 15:00** | Lux spada z 40000 do ~2000 (światło dyfuzyjne), ale NIE do <200. Temp nadal wysoka. Score ≈ 0.2-0.3. NIE wchodzi w transition. ✅ |
| **Sztuczne światło nocą** | Lux rośnie do 100-500 ale temp dalej niska → dawn score < threshold. Min night duration guard → DAWN_REJECT. ✅ |
| **Restart w południe** | Phase = NIGHT, ale lux > 10000 → natychmiast DAWN_TRANSITION → szybko DAY. Brak calibracji SolarClock → fallback timer. ✅ |
| **Restart o zmierzchu** | Phase = NIGHT, lux ~100 spadający, temp spada → dawn score niski (nie rośnie) → zostaje NIGHT. Poprawne! Pierwszy świt = kalibracja. ✅ |

##### Integracja ze schedulerem

> Pełny pseudokod `evaluateSchedule` z integracją detektora zmierzchu
> → sekcja **„Harmonogram podlewania (Schedule)"** wyżej w dokumencie.
> Detektor zmierzchu jest tam źródłem PRIMARY dla okna podlewania (punkty 4-6).

##### Nowe stałe w Config

```
  // Dusk detector (NEW)
  uint32 duskWateringWindowMs = 7200000     // 2h okno po wykryciu zmierzchu
  float  duskScoreEnterThreshold = 0.55     // → DUSK_TRANSITION
  float  duskScoreConfirmThreshold = 0.65   // → potwierdzone
  float  duskScoreCancelThreshold = 0.30    // → fałszywy alarm
  uint32 transitionConfirmMs = 900000       // 15 min sustained
  bool   morningWateringEnabled = false     // drugie okno: ~1h przed estymowanym świtem
  // rezerwa konfiguracyjna na przyszłość; obecny firmware nie używa geolokalizacji
```

##### Logi diagnostyczne

```
// Przez Serial co minutę (gdy transition aktywny):
// "DUSK_TICK score=0.62 lux=180 dLux=-12.3/min dTemp=-0.031/min dHum=+0.018/min dP=+0.002/min"
// "DUSK_SCORES light=0.72 lightRate=0.85 temp=0.62 hum=0.36 press=0.92"

// Przy potwierdzeniu:
// "DUSK_CONFIRMED day_was=14h23m"
// "SOLAR_CLOCK day=14h23m night=9h37m cycle=3"
```

---

### Przycisk manualny (Dual Button) — bezpieczeństwo

**Hardware Dual Button Unit (SKU U025)**:
- Dwa niezależne przyciski mechaniczne na jednym Grove port.
- **Niebieski** (Blue) — zwiera **pin A** (SIG, żółty przewód Grove) do GND. Active LOW.
- **Czerwony** (Red)  — zwiera **pin B** (biały przewód Grove) do GND. Active LOW.
- Standardowo PbHUB konfiguruje kanał jako **IN/OUT** (pin A = input, pin B = output/5V).
- Dla Dual Button **oba piny muszą być INPUT** → PbHUB CH5 wymaga trybu **IN/IN**.
  Ustawienie: `PbHubBus.setChannelMode(5, MODE_IN_IN)` — do weryfikacji z firmware STM32F030.
  Jeśli firmware PbHUB v1.1 nie wspiera trybu IN/IN, alternatywa: odczyt analogowy obu pinów
  (LOW < ~500, HIGH > ~3500) lub podłączenie Dual Button bezpośrednio do GPIO StickS3 (Port.A).

### Dual Button — hardening wdrożony po fałszywych aktywacjach

- Odczyt przycisków nie jest już traktowany jako pojedyncza próbka prawdy.
- Stan `pressed` powstaje dopiero po kilku kolejnych zgodnych próbkach (`kStableSampleThreshold = 4`).
- Runtime rozróżnia:
  - `rawPressed` — chwilowy odczyt z PbHUB,
  - `stable` — czy uzyskano wystarczającą liczbę zgodnych próbek,
  - `pressed` — dopiero ustabilizowany stan używany przez domenę,
  - `unstable` — błąd transportu lub rozjazd raw vs stable.
- `unstable == true` powoduje zachowanie fail-closed:
  - brak startu manualnego podlewania,
  - natychmiastowy stop pompy, jeśli była trzymana ręcznie,
  - log diagnostyczny `MANUAL_INPUT_UNSTABLE` / `event=unstable_input`.
- Manual steruje tylko aktualnie wybraną doniczką i pamięta `activePot`, żeby nie zgubić kontekstu
  przy puszczeniu przycisku albo lockoucie.

```
// Dual Button na CH5 PbHUB: niebieski (pin A) i czerwony (pin B)
// Niebieski: manualny tryb pompy (trzymaj = pompuj wybraną doniczkę)
// Czerwony: EMERGENCY STOP (wszystkie pompy)

manualPumpTick(nowMs, dualBtn, snapshot, config, selectedPot):
  // selectedPot = aktualnie wybrany w UI (0 lub 1); domyślnie 0
  bluePressed = PbHubBus.digitalRead(kChDualBtnBlue) == LOW   // active low
  redPressed  = PbHubBus.digitalRead(kChDualBtnRed) == LOW

  // === RED BUTTON: EMERGENCY STOP (wszystkie pompy) ===
  if redPressed:
    for i in 0 ..< config.numPots:
      if pump[i].isOn():
        pump[i].off(nowMs, "MANUAL_STOP")
        log("[POT%d] MANUAL_STOP red_button", i)
    manualState.locked = true
    manualState.lockUntilMs = nowMs + 5000    // 5s lockout po emergency stop
    return

  // === Lock aktywny (po emergency stop) ===
  if manualState.locked and nowMs < manualState.lockUntilMs:
    return

  manualState.locked = false

  potCfg = config.pots[selectedPot]
  potSens = snapshot.pots[selectedPot]

  // === BLUE BUTTON: trzymaj = pompuj wybraną doniczkę ===
  if bluePressed:
    // Safety: nie pozwól pompować dłużej niż hardTimeoutMs
    if manualState.blueHeldMs == 0:
      manualState.blueHeldMs = nowMs

    heldDuration = nowMs - manualState.blueHeldMs

    // Anti-spam: max czas ciągłego trzymania
    if heldDuration > config.manualMaxHoldMs:     // np. 30000ms (30s)
      pump[selectedPot].off(nowMs, "MANUAL_MAX_HOLD")
      log("[POT%d] MANUAL_BLOCK reason=hold_too_long duration=%d", selectedPot, heldDuration)
      manualState.locked = true
      manualState.lockUntilMs = nowMs + config.manualCooldownMs  // np. 60s cooldown
      return

    // Anti-overflow: sprawdź czujnik w doniczce (per-pot)
    if potSens.waterGuards.potMax == TRIGGERED:
      pump[selectedPot].off(nowMs, "MANUAL_OVERFLOW")
      log("[POT%d] MANUAL_BLOCK reason=overflow_sensor", selectedPot)
      manualState.locked = true
      manualState.lockUntilMs = nowMs + 10000
      return

    // Pompuj wybraną doniczkę
    if not pump[selectedPot].isOn():
      pump[selectedPot].on(nowMs, config.manualMaxHoldMs)
      log("[POT%d] MANUAL_PUMP_ON", selectedPot)

  else:
    // Przycisk puszczony
    if pump[selectedPot].isOn() and manualState.blueHeldMs > 0:
      heldDuration = nowMs - manualState.blueHeldMs
      potCfg = config.pots[selectedPot]                      // selectedPot = aktualnie wybrany w UI
      pumpedMl = heldDuration / 1000.0 * potCfg.pumpMlPerSec
      pump[selectedPot].off(nowMs, "MANUAL_RELEASE")
      waterBudget.addPumped(pumpedMl, selectedPot)
      log("MANUAL_PUMP_OFF held=%dms pumped_ml=%.1f", heldDuration, pumpedMl)

    manualState.blueHeldMs = 0

  // === ANTI-SPAM: rate limit ===
  // Jeśli przycisk był wciśnięty i puszczony >5 razy w ciągu 30s → lockout 60s
  // Rejestruj tylko naciśnięcia (rising edge), nie każdy tick!
  if bluePressed and manualState.blueHeldMs == 0:   // nowe naciśnięcie
    manualState.pressHistory.add(nowMs)
  manualState.pressHistory.removeOlderThan(nowMs - 30000)
  if manualState.pressHistory.count > 5:
    manualState.locked = true
    manualState.lockUntilMs = nowMs + 60000
    log("MANUAL_BLOCK reason=button_spam count=%d", manualState.pressHistory.count)
```

### Manual pump — dodatkowe reguły runtime

- `UNKNOWN` na overflow sensorze traktowany jest tak samo konserwatywnie jak `TRIGGERED`
  dla manualnego podlewania: blokada i lockout.
- Manualne uruchomienie ustawia właściciela pompy na `MANUAL`; każde `OFF` lub blokada czyści owner.
- Red button nadal robi emergency stop wszystkich pomp, ale runtime dodatkowo czyści owner i aktywną doniczkę.

---

### Rezerwuar wody — budżet i tryb kryzysowy

```
// WSPÓLNY rezerwuar dla obu doniczek — obie pompy odejmują z tego samego budżetu.\nstruct WaterBudget {
  float reservoirCapacityMl       // pojemność podana przez użytkownika (domyślnie 10000ml = 10L)
  float reservoirCurrentMl        // estymata aktualnego poziomu
  float totalPumpedMl = 0         // suma od ostatniego refill (OBE POMPY!)
  float totalPumpedMlPerPot[MAX_POTS] = {0}  // per-pot tracking
  float pumpMlPerSec[MAX_POTS]    // per-pot z kalibracji (z PotConfig)
  bool  reservoirLow = false      // stan czujnika MIN w rezerwuarze
  uint32 reservoirLowSinceMs = 0  // kiedy czujnik przeszedł w LOW
  float reservoirLowThresholdMl   // ile wody zostaje gdy czujnik = LOW (do kalibracji)
  float daysRemaining = 999       // estymata — na ile dni wystarczy wody
  uint32 lastRefillMs = 0         // millis() ostatniego refilla (do estymacji daysRemaining)
}

// Aktualizacja budżetu:
updateWaterBudget(nowMs, sensors, budget):
  // Czujnik rezerwuaru (MIN level) — fizycznie activeLow=true (sensor wykrywa wodę → LOW).
  // W domenie używamy activeLow=false, aby TRIGGERED = problem (brak wody):
  //   - Fizycznie LOW (sensor wykrywa wodę) → domena OK → rezerwuar wystarczający
  //   - Fizycznie HIGH (brak wody przy czujniku) → domena TRIGGERED → rezerwuar niski
  // Dzięki temu TRIGGERED zawsze = problem, zarówno dla potMax jak i reservoirMin.

  if sensors.waterGuards.reservoirMin == OK:
    // OK = sensor wykrywa wodę = rezerwuar wystarczający
    budget.reservoirLow = false
    budget.reservoirLowSinceMs = 0
  else:
    // TRIGGERED = sensor nie wykrywa wody = rezerwuar niski
    if not budget.reservoirLow:
      budget.reservoirLow = true
      budget.reservoirLowSinceMs = nowMs
      budget.reservoirCurrentMl = budget.reservoirLowThresholdMl
      notify(RESERVOIR_LOW, "Water below sensor level")
      log("RESERVOIR_LOW remaining_est=%.0fml", budget.reservoirCurrentMl)

  // Estymacja bieżącego poziomu
  budget.reservoirCurrentMl = budget.reservoirCapacityMl - budget.totalPumpedMl

  // Ile czasu zostało? (totalPumped / elapsed days since refill — survives reboots)
  daysSinceRefill = (nowMs - budget.lastRefillMs) / 86400000.0
  if daysSinceRefill >= 0.5 and budget.totalPumpedMl > 1.0:
    avgMlPerDay = budget.totalPumpedMl / daysSinceRefill
    daysRemaining = budget.reservoirCurrentMl / avgMlPerDay
    budget.daysRemaining = daysRemaining
    if daysRemaining < 2:
      notify(RESERVOIR_WARNING, "Water for ~%.1f days", daysRemaining)
  else:
    budget.daysRemaining = 999   // za mało danych

  return budget

// Refill — użytkownik dolał wodę do pełna:
handleRefill(budget, config):
  budget.reservoirCurrentMl = budget.reservoirCapacityMl
  budget.totalPumpedMl = 0
  for i in 0 ..< MAX_POTS:
    budget.totalPumpedMlPerPot[i] = 0
  budget.reservoirLow = false
  budget.lastRefillMs = millis()
  log("RESERVOIR_REFILL capacity=%.0fml", budget.reservoirCapacityMl)
  notify(RESERVOIR_REFILLED)

addPumped(budget, ml, potIdx):
  budget.totalPumpedMl += ml
  budget.totalPumpedMlPerPot[potIdx] += ml
  budget.reservoirCurrentMl -= ml
  if budget.reservoirCurrentMl < 0:
    budget.reservoirCurrentMl = 0
```

#### Tryb kryzysowy rezerwuaru

Gdy `reservoirLow == true` (woda poniżej czujnika):
- **Nie przerywaj podlewania** — roślina nadal potrzebuje wody.
- **Zredukuj dawkę do 1/3** — oszczędzaj to co zostało.
- **Powiadom użytkownika** z estymacją czasu do wyczerpania.
- **Oblicz ile wody zostało** z `reservoirLowThresholdMl` i `totalPumpedMl` od tego momentu.

```
// W EVALUATING:
if waterBudget.reservoirLow:
  cycle.pulseDurationMs /= 3
  cycle.maxPulses = min(2, cycle.maxPulses)
  log("WATERING_CRISIS_MODE reservoir_remaining=%.0fml", waterBudget.reservoirCurrentMl)

  if waterBudget.reservoirCurrentMl <= 0:
    // Estymowana woda = 0 → stop
    cycle.phase = BLOCKED
    notify(RESERVOIR_EMPTY, "Estimated 0ml remaining — watering stopped")
    return
```

---

### Tryb wakacyjny (Vacation Mode)

**Cel**: gdy użytkownik jest nieobecny — zmniejszyć zużycie wody, wydłużyć żywotność rezerwuaru
i ograniczyć ryzyko zalania (brak natychmiastowej interwencji).

**Zasady**:
1. Obniż docelową wilgotność (`targetMoisturePct` − `vacationTargetReductionPct`)
   → rośliny dostają minimum do przetrwania, ale nie komfortowe nawadnianie.
2. Ogranicz max pulsy na cykl (`vacationMaxPulsesOverride`) — mniej wody naraz.
3. Wydłuż cooldown między cyklami (`cooldownMs * vacationCooldownMultiplier`).
4. Agresywniejszy próg kryzysu rezerwuaru — wejście w CRISIS_LEVEL wcześniej
   (np. `reservoirCriticalPct * 1.5` w vacation mode).
5. Jeśli trend anomalii (wysychanie szybsze niż baseline × threshold) → blokuj
   podlewanie i wyślij alarm Telegram (lepiej za sucho niż zalanie).

**Config — nowe pola** (domyślne wartości):
```
bool   vacationMode                  = false
float  vacationTargetReductionPct    = 10.0   // % odejmowane od targetMoisturePct
uint8  vacationMaxPulsesOverride     = 2      // max pulsy w vacation (nadpisuje maxPulses)
float  vacationCooldownMultiplier    = 2.0    // mnożnik cooldownMs
```

**Przełączanie**:
- Telegram: `/vacation on` | `/vacation off`
- GUI: przycisk toggle na ekranie głównym
- Przy przełączeniu: log + notify + NVS persist

**Pseudocode — applyVacationOverrides()**:
```
struct VacationProfile:
  float  adjustedTargetPct
  uint8  adjustedMaxPulses
  uint32 adjustedCooldownMs
  float  adjustedCrisisThresholdPct

fn applyVacationOverrides(profile: WateringProfile, cfg: Config) -> VacationProfile:
  if !cfg.vacationMode:
    return VacationProfile {
      adjustedTargetPct       = profile.targetMoisturePct,
      adjustedMaxPulses       = profile.maxPulses,
      adjustedCooldownMs      = profile.cooldownMs,
      adjustedCrisisThresholdPct = cfg.reservoirCriticalPct
    }

  adjusted := VacationProfile {}
  adjusted.adjustedTargetPct = max(
    profile.targetMoisturePct - cfg.vacationTargetReductionPct,
    5.0   // absolutne minimum — nie zejdź poniżej 5%
  )
  adjusted.adjustedMaxPulses = min(
    cfg.vacationMaxPulsesOverride,
    profile.maxPulses
  )
  adjusted.adjustedCooldownMs = (uint32)(profile.cooldownMs * cfg.vacationCooldownMultiplier)
  adjusted.adjustedCrisisThresholdPct = min(
    cfg.reservoirCriticalPct * 1.5,
    80.0  // cap — nie blokuj przy 80%+ rezerwuaru
  )

  Serial.printf("[VACATION] target %.1f%% → %.1f%%, maxPulses %d → %d, cooldown %ds → %ds\n",
    profile.targetMoisturePct, adjusted.adjustedTargetPct,
    profile.maxPulses, adjusted.adjustedMaxPulses,
    profile.cooldownMs/1000, adjusted.adjustedCooldownMs/1000)

  return adjusted
```

**Integracja w evaluateSchedule / wateringTick**:
```
// Na początku wateringTick, po pobraniu bieżącego profilu:
vacProf := applyVacationOverrides(currentProfile, config)

// Użyj vacProf zamiast profilu bezpośrednio:
if moisturePct >= vacProf.adjustedTargetPct:
  // Wilgotność OK — skip
  return

if cycle.pulseCount >= vacProf.adjustedMaxPulses:
  // Limit pulsów → soak & wait
  cycle.phase = SOAK_WAIT
  return

// Cooldown check:
if (nowMs - lastCycleEndMs) < vacProf.adjustedCooldownMs:
  return  // jeszcze za wcześnie

// Crisis threshold (vacation-adjusted):
if waterBudget.reservoirPct <= vacProf.adjustedCrisisThresholdPct:
  enterCrisisMode()
  return
```

**Bezpieczeństwo w vacation mode**:
- Anomalia trendu (drying rate > baseline × anomalyDryingRateMultiplier) → BLOKADA + alarm.
- Overflow sensor triggered → natychmiastowy STOP (bez zmian — jak normalnie).
- Hard timeout pompy → bez zmian.
- Jeśli WiFi disconnect > 48h w vacation mode → fallback do minimum 1 pulse/dzień
  (zapobieganie całkowitemu wysuszeniu).

**Toggleowanie — pseudocode**:
```
fn handleVacationToggle(enable: bool, cfg: &mut Config):
  if cfg.vacationMode == enable:
    return  // already in desired state

  cfg.vacationMode = enable
  nvsWrite("ag_config", "vacationMode", enable)

  if enable:
    Serial.println("[VACATION] Vacation mode ENABLED")
    notify(VACATION_MODE_ON, "Vacation mode enabled — reduced watering")
  else:
    Serial.println("[VACATION] Vacation mode DISABLED")
    notify(VACATION_MODE_OFF, "Vacation mode disabled — normal watering resumed")
```

---

### Historia pomiarów — ring buffer z kompresją czasową

**Problem**: 8MB flash, ale Preferences/NVS ma ograniczoną pojemność (~15KB efektywnie per namespace).
Odczyty co 1s = 86400/dzień = za dużo.

**Rozwiązanie**: trzypoziomowy ring buffer w RAM z periodycznym flush do NVS.
Przy zapisie do namespace `ag_hist` nie zakładaj pojedynczych dużych blobów dla całego `level2`/`level3`.
W praktyce zapis `level2` jako jeden blob zaczął zawodzić przy większym zapełnieniu historii, więc format trwały powinien dzielić dane na małe chunky (`l2_0..`, `l3_0..`, `wl_0..`) i zostawić kompatybilny odczyt starego schematu.

```
// Poziom 1: RAM — gęsty (co 10s), ostatnie 30 minut
// 10s × 180 = 180 rekordów × 12 bajtów = ~2.2KB RAM
struct SensorSample {
  uint32 timestampMs         // 4B
  uint16 moistureRaw         // 2B
  int16  tempC_x10           // 2B (np. 23.5°C = 235)
  uint16 lux                 // 2B (clamp do 65535)
  uint8  flags               // 1B (reservoirLow, overflowActive, pumpOn, ...)
  uint8  _pad                // 1B alignment
}                            // = 12 bytes

// Poziom 2: RAM → NVS — co 5 minut, uśrednione, ostatnie 24h
// 5min × 288 = 288 rekordów × 12B = ~3.5KB
// Flush do NVS co 15 minut (żeby nie ubijać flash)

// Poziom 3: NVS — co 1 godzinę, uśrednione, ostatnie 30 dni
// 1h × 720 = 720 rekordów × 12B = ~8.6KB
// Starsze kasowane (ring buffer)

HISTORY_CONFIG = {
  level1: { intervalSec: 10,   maxSamples: 180,  storage: RAM },
  level2: { intervalSec: 300,  maxSamples: 288,  storage: NVS, flushIntervalSec: 900 },
  level3: { intervalSec: 3600, maxSamples: 720,  storage: NVS, flushIntervalSec: 3600 },
}
```

#### Downsampling (kompresja)

```
downsample(samples[], targetInterval):
  buckets = groupByInterval(samples, targetInterval)
  result = []
  for bucket in buckets:
    avg = {
      timestampMs:   bucket[0].timestampMs,      // początek okna
      moistureRaw:   mean(bucket[].moistureRaw),
      tempC_x10:     mean(bucket[].tempC_x10),
      lux:           mean(bucket[].lux),
      flags:         OR(bucket[].flags),           // zachowaj flagi jeśli cokolwiek było aktywne
    }
    result.append(avg)
  return result
```

#### Historia podlewań (osobna)

```
struct WateringRecord {
  uint32 timestampMs
  uint8  pulseCount
  uint16 totalPumpedMl         // ml × 10 = 1 decimal place w uint16
  uint16 moistureBefore_x10    // pct × 10
  uint16 moistureAfter_x10
  uint8  reason                // enum: EVENING, MORNING, RESCUE, FALLBACK, MANUAL
}                              // = 12 bytes

// Ring buffer: 100 rekordów = ~1.2KB NVS
// 100 podlewań ≈ 30-50 dni przy 2-3 podlewaniach/dzień
WATERING_HISTORY_MAX = 100
```

#### Analiza trendów

```
// Ring buffer hourly deltas — przechowuj ΔmoisturePct/h z ostatnich 24h
struct TrendState {
  float  hourlyDeltas[24]       // ring buffer: delta pct/h, najnowszy na [headIdx]
  uint8  headIdx = 0
  uint8  count = 0              // ile próbek zebrano (0-24)
  float  lastMoisturePct = NaN  // odczyt sprzed 1h
  uint32 lastSampleMs = 0       // kiedy pobrano lastMoisturePct
  float  normalDryingRate = NaN // wyuczona bazowa prędkość schnięcia (%/h, ujemna)
  bool   baselineCalibrated = false
}

// Per-pot trend:
TrendState trendStates[MAX_POTS]  // niezależna analiza trendu per doniczka

trendTick(nowMs, moisturePct, trendState, config):
  // Aktualizuj co 1h
  if trendState.lastSampleMs == 0:
    // Pierwszy odczyt po starcie
    trendState.lastMoisturePct = moisturePct
    trendState.lastSampleMs = nowMs
    return

  elapsed = nowMs - trendState.lastSampleMs
  if elapsed < 3600000:    // nie minęła godzina
    return

  // Oblicz delta %/h
  deltaPerHour = (moisturePct - trendState.lastMoisturePct) / (elapsed / 3600000.0)
  trendState.hourlyDeltas[trendState.headIdx] = deltaPerHour
  trendState.headIdx = (trendState.headIdx + 1) % 24
  if trendState.count < 24: trendState.count++
  trendState.lastMoisturePct = moisturePct
  trendState.lastSampleMs = nowMs

  // Wyucz bazową prędkość schnięcia (median z deltas < 0, pompa OFF)
  // Normalnie gleba traci 0.5-2%/h zależnie od temp/wiatru
  if trendState.count >= 6:
    negativeDeltas = filter(trendState.hourlyDeltas[:count], d -> d < 0)
    if len(negativeDeltas) >= 3:
      trendState.normalDryingRate = median(negativeDeltas)
      trendState.baselineCalibrated = true

  // --- Anomalia: szybkie schnięcie → potencjalny wyciek ---
  if trendState.baselineCalibrated:
    // normalDryingRate jest ujemny (np. -1.2%/h). Alarm gdy 3x szybszy spadek.
    threshold = trendState.normalDryingRate * config.anomalyDryingRateMultiplier  // np. 3×
    if deltaPerHour < threshold:   // delta ujemna, a threshold jeszcze bardziej ujemny
      log("TREND_ANOMALY rate=%.1f%%/h normal=%.1f%%/h — possible leak!",
          deltaPerHour, trendState.normalDryingRate)
      notify(ANOMALY_FAST_DRYING, deltaPerHour, trendState.normalDryingRate)
  else:
    // Brak baseline → użyj stałego progu
    if deltaPerHour < -config.anomalyDryingRateThreshold:   // domyślnie -5%/h
      log("TREND_ANOMALY rate=%.1f%%/h (no baseline yet)", deltaPerHour)
      notify(ANOMALY_FAST_DRYING, deltaPerHour, NaN)

  log("TREND delta=%.2f%%/h baseline=%.2f%%/h calibrated=%s",
      deltaPerHour, trendState.normalDryingRate, trendState.baselineCalibrated)


analyzeTrends(history, trendState):
  // Średnie dzienne zużycie wody (z ostatnich 7 dni)
  last7d = history.wateringRecords.filter(age < 7 days)
  avgMlPerDay = sum(last7d[].totalPumpedMl) / 7

  // Tempo schnięcia gleby z trendState (lub regresja liniowa jako fallback)
  dryingRatePerHour = trendState.normalDryingRate if trendState.baselineCalibrated
                      else linearRegression(history.level2.filter(pumpWasOff).moisturePct vs time)

  return { avgMlPerDay, dryingRatePerHour, baselineCalibrated: trendState.baselineCalibrated }
```

Integracja w ControlTick:
```
on TICK_1S:
  // ... sensorTick, wateringTick, ...
  // trendTick per-pot — wywoływana w ControlTick (patrz wyżej)
  for potIdx in 0 ..< config.numPots:
    if not config.pots[potIdx].enabled: continue
    trendTick(nowMs, sensors.pots[potIdx].moisturePct, trendStates[potIdx], config)
```

---

### Filtracja odczytów — mediana + EMA

Surowy ADC ma szum ±20-50 jednostek. Zamiast reagować na pojedyncze szpilki, pipeline jest rozdzielony na tor szybki i tor stabilny:

1. mediana z ostatnich 10 próbek raw per-pot
2. szybki tor sterowania: `median(raw) -> normalize -> moisturePct`
3. stabilny tor prezentacji/startu: `EMA(raw) -> normalize -> moistureEma`

W runtime rozróżniamy też dwa warianty sygnału RAW:
- `moistureRaw` = ostatni surowy odczyt ADC, zostaje do diagnostyki i historii.
- `moistureRawFiltered` = przefiltrowany sygnał po medianie, używany do sterowania,
  dalszego wygładzania i prezentacji użytkowej.

```
const MEDIAN_WINDOW = 10

median5(samples):
  sort(samples)
  return middle(samples)

// EMA z konfigurowalnym alpha (domyślnie 0.1 = wolna reakcja, stabilna)
struct EmaFilter {
  float alpha = 0.1        // 0.0-1.0; mniejsze = wolniejsze, stabilniejsze
  float value = NaN        // bieżąca wartość EMA
  bool  initialized = false
  uint32 lastUpdateMs = 0  // timestamp ostatniej aktualizacji
}

// Per-pot filtry:
RawMedianFilter moistureRawMedian[MAX_POTS]
EmaFilter moistureRawEmaFilters[MAX_POTS]

emaUpdate(filter, newSample, nowMs):
  if not filter.initialized:
    filter.value = newSample
    filter.initialized = true
    filter.lastUpdateMs = nowMs
  else:
    // Edge case: jeśli sensor był offline >60s, reinicjalizuj EMA
    // (stara wartość jest nieaktualna — lepiej przyjąć nowy odczyt od razu)
    gap = nowMs - filter.lastUpdateMs
    if gap > 60000:  // >60s przerwy
      log("EMA_REINIT gap=%dms — restarting from fresh sample", gap)
      filter.value = newSample
    else:
      filter.value = filter.alpha * newSample + (1 - filter.alpha) * filter.value
    filter.lastUpdateMs = nowMs
  return filter.value

// Edge cases:
// - Po restarcie ESP32 → initialized=false → pierwszy odczyt = wartość początkowa.
// - Sensor health FAIL → nie wywołuj emaUpdate (zamroź ostatnią wartość).
```

W sensorTick:
```
if sensorHealth == OK:
  moistureRaw = PbHubBus.analogRead(...)
  moistureRawStable = medianPushAndRead(moistureRawMedian[pot], moistureRaw)
  moistureRawFiltered = moistureRawStable
  moisturePct = normalizeSoil(moistureRawFiltered)

  moistureRawEma = emaUpdate(moistureRawEmaFilter, moistureRawFiltered, nowMs)
  moistureEma = normalizeSoil(round(moistureRawEma))
else:
  // Sensor offline — nie aktualizuj EMA, zachowaj ostatnią wartość
  log("SENSOR_OFFLINE — EMA frozen")
```

Ta architektura działa niezależnie dla obu doniczek, bo każdy POT ma własne okno mediany i własny filtr EMA.

Podział ról sygnałów w runtime:
- `moisturePct` = szybki tor sterowania po `median(raw) -> normalize`.
  Tę wartość wykorzystuje aktywny cykl podlewania po `SOAK`, bo nie może czekać na długi filtr
  przy decyzji `STOP/next pulse`.
- UI i raporty użytkowe pokazują `moistureRawFiltered`, a nie pojedynczy surowy strzał ADC,
  żeby to co widzi użytkownik odpowiadało rzeczywistemu torowi sterowania.
- `moistureEma` = spokojniejszy tor do UI, Telegram, trendów i decyzji o starcie cyklu.
  Dzięki temu pojedyncze wahania rzędu kilku dziesiątych procenta nie uruchamiają podlewania zbyt łatwo.
- EMA działa w domenie RAW, nie w domenie `%`. To ważne przy nieliniowej krzywej `raw -> %`, bo
  kilka countów ADC blisko `wetRaw` może dawać duży skok procentów. Wygładzanie przed normalizacją
  ogranicza takie sztuczne fluktuacje bez wydłużania reakcji aktywnego podlewania.
- EMA ma mały deadband anty-szumowy w jednostkach RAW, więc bardzo małe oscylacje nie zmieniają
  wskazania przy każdym ticku, ale większa zmiana nadal przechodzi bez dokładania długiego opóźnienia.

---

### Powiadomienia Telegram (NotificationService)

### Telegram — model interakcji użytkownika

Aktualny UX Telegram nie opiera się na dużej liczbie komend tekstowych. Jedynym
obsługiwanym wejściem użytkownika jest `/ag` albo `/start`, które otwierają menu
inline i prowadzą cały dalszy flow.

#### Read-only z menu `/ag`
- `📊 Status`
- `📈 History`
- `🌿 Profiles`
- `❓ Help`

#### Safe control actions z menu `/ag`
- `💧 Water` — pojedynczy bezpieczny pulse przez istniejący FSM podlewania
- `🛑 Stop` — natychmiastowe OFF wszystkich pomp i abort aktywnych cykli
- `🪣 Refill`
- `🏖 Vacation ON/OFF`
- `⚙️ Mode AUTO/MANUAL`
- `📶 WiFi`

#### Następny etap rozwoju Telegram
- kolejka powiadomień z throttlingiem i deduplikacją,
- bogatsze `History` oparte o persistowaną historię,
- wydzielone submenu konfiguracji,
- zastąpienie niepewnego TLS przez pinned CA / walidację certyfikatu.


Aktualna implementacja nie ma jeszcze ogólnego `NotificationService` z `NotificationType`
i wspólną `notificationQueue`. Stan faktyczny jest prostszy i składa się z dwóch
oddzielnych ścieżek outbound:

- `s_tgFeedbackQueue` — krótka kolejka tekstowych komunikatów z `ControlTask` do
  `NetTask`, używana dla start/stop cyklu, safety block/unblock, refill, mode change
  i komunikatów o WiFi portal.
- heartbeat dzienny generowany bezpośrednio w `NetTask` przez `isDailyHeartbeatTime()`
  i `formatDailyReport()`.

Dedykowana deduplikacja i pełny model typów powiadomień pozostają przyszłym etapem,
a nie opisem obecnego kodu.

```
queueTelegramFeedbackFmt(fmt, ...):
  if feedbackQueue full:
    log("[TG] feedback_drop reason=queue_full")
    return
  feedbackQueue.push(formattedText)

NetTask loop:
  netTaskTick(nowMs, netState, netCfg)

  if wifiConnected and telegramEnabled:
    while feedbackQueue has message:
      telegramSend(message, netCfg, maxRetries=2, backoffMs=250)

    if isDailyHeartbeatTime(nowMs, solarClock, duskDetector, netState):
      report = formatDailyReport(sensors, budget, trends, config, uptimeMs)
      telegramSend(report, netCfg)
```

### Heartbeat dzienny — stan faktyczny

```
isDailyHeartbeatTime(nowMs, solarClock, duskDetector, netState):
  if netState.lastHeartbeatMs > 0 and (nowMs - netState.lastHeartbeatMs) < 20h:
    return false

  if solarClock.calibrated:
    estimatedDawn = estimateNextDawn(duskDetector, solarClock, nowMs)
    if estimatedDawn is known:
      target = estimatedDawn + 30 min
      if nowMs in window target ± 5 min and !netState.heartbeatSentToday:
        netState.heartbeatSentToday = true
        netState.lastHeartbeatMs = nowMs
        return true
      if nowMs outside target window:
        netState.heartbeatSentToday = false
      return false

  if netState.lastHeartbeatMs == 0:
    return nowMs > 5 min

  return (nowMs - netState.lastHeartbeatMs) >= 24h
```

### Raport dzienny — stan faktyczny

Aktualna implementacja używa `DailyReportData`:
- `sensors`
- `budget`
- `trends`
- `config`
- `uptimeMs`

Nie korzysta jeszcze z pełnego persisted history store. Raport zawiera:
- wilgotność i target per doniczka,
- trend `%/h` jeśli baseline został już wyuczony,
- zużycie wody od ostatniego refill per doniczka,
- temperaturę i lux,
- poziom rezerwuaru i szacowane dni pracy,
- status vacation mode,
- uptime.

### Telegram polling i menu inline — stan faktyczny

```
telegramPollCommands(nowMs, netCfg, status):
  if telegram disabled or WiFi down:
    return

  pollIntervalMs = 2000 ms idle
  if withinFastPollWindowAfterInteraction:
    pollIntervalMs = 400 ms

  fetch updates in batches (max 10)
  authorize chat_id against netCfg.telegramChatIds

  if update.type == callback_query:
    handleTelegramCallback(update, status, netCfg)
  else if text command is /ag or /start:
    sendAgMenu()
  else:
    log("text_without_menu_entry")
    sendAgMenu()
```

#### Faktyczne callbacki menu inline
- `ag:status`
- `ag:history`
- `ag:profiles`
- `ag:help`
- `ag:water:0`
- `ag:water:1`
- `ag:stop`
- `ag:refill`
- `ag:wifi`
- `ag:menu`
- `ag:vac:toggle`
- `ag:mode:toggle`

#### Faktyczne mapowanie callbacków na akcje
```
handleTelegramCallback(cmd):
  "ag:status"       → formatTelegramStatusReport()
  "ag:history"      → formatTelegramHistoryReport()
  "ag:profiles"     → formatTelegramProfilesMessage()
  "ag:help"         → formatTelegramHelpMessage()
  "ag:water:<pot>"  → preflightCheck + EventQueue.push(REQUEST_MANUAL_WATER)
  "ag:stop"         → EventQueue.push(REQUEST_PUMP_OFF)
  "ag:refill"       → EventQueue.push(REQUEST_REFILL)
  "ag:wifi"         → EventQueue.push(REQUEST_START_WIFI_SETUP)
  "ag:vac:toggle"   → EventQueue.push(REQUEST_SET_VACATION)
  "ag:mode:toggle"  → EventQueue.push(REQUEST_SET_MODE)
  "ag:menu"         → redraw inline menu
```

#### Faktyczne ograniczenia akcji `Water`
- działa per doniczka (`potIdx` z callbacku), nie jako jedna globalna komenda,
- uruchamia tylko jeden bezpieczny pulse przez istniejący FSM,
- przed wrzuceniem eventu przechodzi preflight: tryb AUTO, brak aktywnego cyklu,
  cooldown, kalibracja pompy, overflow, stan rezerwuaru i stany `UNKNOWN` sensorów,
- przy błędzie użytkownik dostaje od razu tekstowy powód blokady zamiast ślepego enqueue.

---

### Ekran StickS3 — design UI (135×240px)

StickS3 ma mały ekran (135×240, orientacja portrait).
Podzielony na strefy informacyjne, bez menu — wszystko na jednym widoku.

```
// Layout (portrait 135×240) — pełne wykorzystanie ekranu:
//
// === Tryb 1 doniczka (numPots==1): ===
// ┌──────────────────────────┐
// │  🌱 65%     ⬆ 70%        │  ← wilgotność (size3) + target       (y 0-30)
// │             🍅 Pomidor    │  ← profil ikona + nazwa              (y 17-30)
// │  ████████████░░░░░░░░░░  │  ← pasek wilgotności                 (y 34-46)
// ├──────────────────────────┤
// │  💧 1.4L         ~4d     │  ← rezerwuar + estymata              (y 52-64)
// │  🪴 OK       TANK:OK     │  ← overflow + tank guard             (y 66-78)
// ├──────────────────────────┤
// │  🌡 24.1C    💧 52%      │  ← temperatura + wilgotność powietrza (y 84-96)
// │  ☀ 1200lx   1013hPa     │  ← lux + ciśnienie atmosferyczne     (y 98-112)
// ├──────────────────────────┤
// │  ⚙ AUTO  VAC  📶 OK     │  ← tryb + wifi                       (y 116-130)
// ├──────────────────────────┤
// │  PULSE 3/6  150ml        │  ← faza podlewania                   (y 134-148)
// │  ░░░░████░░░░░░░░░░░    │  ← progress bar                      (y 150-160)
// ├──────────────────────────┤
// │  ☀ DAY       ⏰ 01:23:45 │  ← dusk phase + uptime               (y 166-178)
// ├──────────────────────────┤
// │  RAW:2048 EMA:65.2%      │  ← dane surowe czujnika              (y 184-196)
// ├──────────────────────────┤
// │  ! RESERVOIR LOW         │  ← alert banner (opcjonalnie)        (y 212-234)
// └──────────────────────────┘
//
// === Tryb 2 doniczki (numPots==2): ===
// Pełnoekranowy kompaktowy widok — obie doniczki + pełne info:
// ┌──────────────────────────┐
// │ 🌱 1:65%    ⬆ 70%       │  ← pot1: wilgotność + target         (y 0-14)
// │ 🍅 Pomidor   PULSE 2/4  │  ← pot1: profil + faza               (y 18-30)
// │ ██████████░░░░░░  150ml  │  ← pot1: moisture bar + pumped       (y 32-44)
// ├──────────────────────────┤
// │ 🌱 2:48%    ⬆ 60%       │  ← pot2: wilgotność + target         (y 48-62)
// │ 🌶 Papryka      SOAK    │  ← pot2: profil + faza               (y 66-78)
// │ ████░░░░░░░░░░░░        │  ← pot2: moisture bar                (y 80-92)
// ├──────────────────────────┤
// │ 💧 1.4L  ~3d    🪴 OK   │  ← rezerwuar + tank guard            (y 98-112)
// ├──────────────────────────┤
// │ 🌡 24.1C    💧 52%      │  ← temp + wilgotność powietrza       (y 114-126)
// │ ☀ 1200lx   1013hPa      │  ← lux + ciśnienie atmosferyczne    (y 128-140)
// ├──────────────────────────┤
// │ ⚙ AUTO VAC    📶 OK     │  ← tryb + wifi                       (y 146-158)
// │ ☀ DAY      ⏰ 01:23:45  │  ← dusk + uptime                     (y 162-174)
// ├──────────────────────────┤
// │ 1:RAW:2048 E:65.2% C:64 │  ← raw data pot1                     (y 180-192)
// │ 2:RAW:1840 E:48.1% C:47 │  ← raw data pot2                     (y 192-204)
// ├──────────────────────────┤
// │ ! RESERVOIR LOW          │  ← alert banner (opcjonalnie)        (y 212-234)
// │ ! POT1 OVERFLOW          │  ← per-pot overflow alert            (y 236+...)
// └──────────────────────────┘
//
// BtnA click: MAIN → Settings wejście; Settings → zmień wartość
// BtnA long:  Settings → wróć do MAIN
// BtnB click: MAIN (2 pots) → przełączanie: kompakt ↔ detail pot1 ↔ detail pot2
//             Settings → nawigacja po opcjach
// BtnB long:  Settings → alternatywna zmiana wartości

// === Nawigacja ekranów ===
enum UiScreen { MAIN, SETTINGS }
uiState.screen = MAIN
uiState.dualViewMode = COMPACT   // COMPACT | DETAIL_POT0 | DETAIL_POT1
uiState.settingsIndex = 0        // aktualnie podświetlona opcja
uiState.selectedPot = 0          // którą doniczką steruje Dual Button

onBtnA_Short:
  if uiState.screen == MAIN:
    uiState.screen = SETTINGS
  else:
    uiState.screen = MAIN
    saveConfig()   // zapisz zmiany przy wyjściu z Settings

onBtnB_Short:
  if uiState.screen == MAIN:
    if config.numPots == 1: return   // nic do przełączania
    // Cykl: COMPACT → DETAIL_POT0 → DETAIL_POT1 → COMPACT
    uiState.dualViewMode = next(uiState.dualViewMode)
    uiState.selectedPot = (uiState.dualViewMode == DETAIL_POT1) ? 1 : 0
  elif uiState.screen == SETTINGS:
    uiState.settingsIndex = (uiState.settingsIndex + 1) % settingsItemCount

renderMainScreen(snapshot, budget, cycles, config):
  if uiState.screen == SETTINGS:
    renderSettingsScreen(config)
    return
  if config.numPots == 1:
    renderSinglePotScreen(snapshot, budget, cycles[0], config)
  else:
    if uiState.dualViewMode == COMPACT:
      renderDualPotScreen(snapshot, budget, cycles, config)
    elif uiState.dualViewMode == DETAIL_POT0:
      renderSinglePotScreen(snapshot, budget, cycles[0], config)  // pot 0 pełny widok
    elif uiState.dualViewMode == DETAIL_POT1:
      renderSinglePotScreen(snapshot, budget, cycles[1], config)  // pot 1 pełny widok

renderSinglePotScreen(snapshot, budget, cycle, config):
  potSnap = snapshot.pots[0]
  potCfg  = config.pots[0]
  profile = PROFILES[potCfg.plantProfileIndex]

  // 1. Wilgotność — duża cyfra, zielona/żółta/czerwona wg progu
  color = GREEN if potSnap.moisturePct >= profile.targetMoisturePct
          YELLOW if potSnap.moisturePct >= profile.criticalLowPct
          RED
  drawBigText(0, 0, "%.0f%%", potSnap.moisturePct, color)
  drawSmallText(80, 5, "⬆%.0f%%", profile.targetMoisturePct)

  // 2. Pasek wilgotności
  drawProgressBar(0, 31, 135, 14, potSnap.moisturePct / 100.0, color)

  // 3. Rezerwuar (wspólny)
  resColor = GREEN if budget.daysRemaining > 3
             YELLOW if budget.daysRemaining > 1
             RED
  drawText(0, 48, "💧 %.0fml  ~%dd", budget.reservoirCurrentMl, budget.daysRemaining, resColor)

  // 4. Doniczka overflow
  potColor = GREEN if potSnap.waterGuards.potMax != TRIGGERED else RED
  drawText(0, 68, "🪴 %s", potColor == GREEN ? "OK" : "OVERFLOW!")

  // 5. Env
  drawText(0, 83, "🌡%.0f°C  ☀%dlx", snapshot.env.tempC, snapshot.env.lux)

  // 6. Tryb + roślina
  drawText(0, 103, "⚙ %s  %s %s",
    config.mode == AUTO ? "AUTO" : "MANUAL",
    profile.icon,
    profile.name)

  // 7. Faza podlewania (pot 0)
  cycle = cycles[0]
  if cycle.phase == IDLE:
    drawText(0, 123, "IDLE", GRAY)
  elif cycle.phase == PULSE:
    drawText(0, 123, "PULSE %d/%d  %.0fml", cycle.pulseCount, cycle.maxPulses, cycle.totalPumpedMl, BLUE)
    drawProgressBar(0, 151, 135, 14, pulseProgress, BLUE)
  elif cycle.phase == SOAK:
    remaining = cycle.soakTimeMs - (nowMs - cycle.phaseStartMs)
    drawText(0, 123, "SOAK %ds", remaining/1000, CYAN)
  elif cycle.phase == OVERFLOW_WAIT:
    drawText(0, 123, "OVERFLOW WAIT", RED)
  elif cycle.phase == BLOCKED:
    drawText(0, 123, "BLOCKED: %s", blockReason, RED)

  // 8-10: bez zmian (next watering, wifi, alert)
  drawText(0, 168, "⏰ Next: %s", formatNextWatering())
  wifiIcon = snapshot.netStatus == UP ? "📶" : "📵"
  drawText(0, 188, "%s WiFi %s", wifiIcon, snapshot.netStatus)
  if budget.reservoirLow:
    drawBanner(0, 203, "! RESERVOIR LOW — ~%dd", budget.daysRemaining, RED)

renderDualPotScreen(snapshot, budget, cycles, config):
  // Kompaktowy widok obu doniczek
  for potIdx in [0, 1]:
    potSnap = snapshot.pots[potIdx]
    potCfg  = config.pots[potIdx]
    if not potCfg.enabled: continue
    profile = PROFILES[potCfg.plantProfileIndex]
    cycle   = cycles[potIdx]
    y = potIdx * 19   // wiersz startowy

    color = GREEN if potSnap.moisturePct >= profile.targetMoisturePct
            YELLOW if potSnap.moisturePct >= profile.criticalLowPct
            RED

    phaseStr = cycle.phase.name[:5]  // "IDLE", "PULSE", "SOAK", etc.
    drawText(0, y, "%s%d: %.0f%% ⬆%.0f %s",
             profile.icon, potIdx+1,
             potSnap.moisturePct, profile.targetMoisturePct,
             phaseStr, color)

  // Wskaźnik wybranej doniczki (Dual Button steruje tą)
  drawText(120, uiState.selectedPot * 19, "◀", WHITE)

  // Wspólne info
  drawText(0, 40, "💧 %.0fml  ~%dd", budget.reservoirCurrentMl, budget.daysRemaining)
  drawText(0, 58, "🌡%.0f°C  ☀%dlx", snapshot.env.tempC, snapshot.env.lux)
  modeStr = config.mode == AUTO ? "AUTO" : "MANUAL"
  vacStr  = config.vacationMode ? "🏖 VAC" : ""
  drawText(0, 76, "⚙ %s  %s", modeStr, vacStr)
  drawText(0, 93, "%s WiFi", snapshot.netStatus == UP ? "📶" : "📵")
  if budget.reservoirLow:
    drawBanner(0, 111, "! RESERVOIR LOW", RED)
```

### Ekran Settings (BtnA → wejście/wyjście)

Proste menu listy — BtnB scrolluje, BtnA wchodzi/wychodzi.
Długie przytrzymanie BtnB na opcji → zmień wartość (cykl).

```
// ┌─────────────────────┐
// │ ⚙ SETTINGS          │
// ├─────────────────────┤
// │ ▶ Pompy: 1          │  ← numPots (1 / 2), zmień = BtnB long press
// │   Profil 🌱1: 🍅   │  ← plantProfile pot0
// │   Profil 🌱2: 🌶   │  ← plantProfile pot1 (tylko gdy numPots==2)
// │   Rezerwuar: 10.0L  │  ← reservoirCapacityMl (edytowalne)
// │   [REFILL 💧]       │  ← resetuj budżet wody (nalałem do pełna)
// │   Tryb: AUTO        │  ← AUTO / MANUAL
// │   Vacation: OFF     │  ← ON / OFF
// │   WiFi: Setup...    │  ← wejdź w AP provisioning
// │   Factory Reset     │  ← BtnA+BtnB 5s
// └─────────────────────┘

renderSettingsScreen(config):
  drawText(0, 0, "⚙ SETTINGS", WHITE)
  drawLine(0, 16, 135, 16, GRAY)

  items = [
    { label: "Pompy",          value: "%d", config.numPots },
    { label: "Profil 🌱1",     value: PROFILES[config.pots[0].plantProfileIndex].name },
    // widoczne tylko gdy numPots==2:
    { label: "Profil 🌱2",     value: PROFILES[config.pots[1].plantProfileIndex].name,
      visible: config.numPots >= 2 },
    { label: "Rezerwuar",      value: "%.1fL", config.reservoirCapacityMl / 1000 },
    { label: "REFILL 💧",      value: "(naciśnij)",  action: handleRefill },
    { label: "Tryb",           value: config.mode == AUTO ? "AUTO" : "MANUAL" },
    { label: "Vacation",       value: config.vacationMode ? "ON" : "OFF" },
    { label: "WiFi",           value: netConfig.provisioned ? netConfig.wifiSsid : "Brak" },
    { label: "Factory Reset",  value: "BtnA+B 5s" },
  ]

  visibleItems = items.filter(i => i.visible != false)
  for idx, item in visibleItems:
    y = 20 + idx * 18
    prefix = "▶" if idx == uiState.settingsIndex else " "
    color = CYAN if idx == uiState.settingsIndex else WHITE
    drawText(0, y, "%s %s: %s", prefix, item.label, item.value, color)

// BtnB long press na opcji → zmień wartość:
onBtnB_LongPress:
  if uiState.screen != SETTINGS: return
  item = visibleItems[uiState.settingsIndex]
  switch item.label:
    "Pompy":
      config.numPots = (config.numPots == 1) ? 2 : 1
      config.pots[1].enabled = (config.numPots == 2)
      log("SETTINGS numPots=%d", config.numPots)
    "Profil 🌱1":
      config.pots[0].plantProfileIndex = (config.pots[0].plantProfileIndex + 1) % NUM_PROFILES
    "Profil 🌱2":
      config.pots[1].plantProfileIndex = (config.pots[1].plantProfileIndex + 1) % NUM_PROFILES
    "Rezerwuar":
      // Cykl: 5L → 10L → 15L → 20L → 5L
      config.reservoirCapacityMl = nextInCycle([5000, 10000, 15000, 20000], config.reservoirCapacityMl)
    "REFILL 💧":
      handleRefill(waterBudget, config)
      notify(RESERVOIR_REFILLED)
    "Tryb":
      config.mode = (config.mode == AUTO) ? MANUAL : AUTO
    "Vacation":
      handleVacationToggle(!config.vacationMode, config)
    "WiFi":
      startApProvisioning()
```

**Kluczowe zasady GUI:**
- GUI **podąża za `numPots`** — jeśli numPots==1, pot2 jest ukryty wszędzie (Settings, main screen, alerty)
- Zmiana numPots w Settings natychmiast przełącza widok MAIN (single ↔ dual)
- **REFILL** resetuje budżet (totalPumpedMl=0, reservoirCurrentMl=capacityMl) — użytkownik naciska po nalaniu wody do pełna
- Rezerwuar pojemność edytowalna w Settings (nie tylko w kodzie)
- Przy wyjściu z Settings (BtnA) config jest zapisywany do NVS

---

### Rozszerzone polityki bezpieczeństwa

```
evaluateExtendedSafety(nowMs, sensors, config, waterBudget, actuatorState):
  // --- Wszystkie dotychczasowe safety gates z oryginalnego evaluateSafety ---
  base = evaluateSafety(nowMs, sensors, config, actuatorState)
  if base.hardBlock: return base

  // --- Nowe gates ---

  // Rezerwuar estymowany na 0
  if waterBudget.reservoirCurrentMl <= 0 and not waterBudget.reservoirLow:
    // Estymacja mówi 0 ale czujnik nie potwierdza — WARN, nie BLOCK
    log("SAFETY_WARN reservoir_estimate=0 but sensor=OK")

  if waterBudget.reservoirCurrentMl <= 0 and waterBudget.reservoirLow:
    return { hardBlock: true, reason: RESERVOIR_EMPTY }

  // Upał — nie blokuje automatyki, ale blokuje normalne podlewanie
  // (rescue mode nadal działa — patrz evaluateSchedule)

  // Czujnik doniczki zablokowany > 2h bez podlewania
  if sensors.waterGuards.potMax == TRIGGERED:
    pumpWasRecent = (nowMs - actuatorState.lastPumpStopAtMs) < (2 * 3600 * 1000)
    if not pumpWasRecent:
      log("SAFETY_WARN overflow_sensor_stuck for >2h without watering")
      // Nie blokuj, ale powiadom — sensor może być zabrudzony
      notify(SENSOR_ANOMALY, "Overflow sensor active >2h without watering — check sensor")

  return { ok: true }
```

---

### Zaktualizowana struktura Config (pełna)

Nowe pola oznaczone `// NEW`:

```
const MAX_POTS = 2

struct PotConfig {
  bool   enabled = false                        // czy ta doniczka jest aktywna
  uint8  plantProfileIndex = 0                  // 0=Pomidor, ..., 5=Custom
  // Custom override (używane gdy plantProfileIndex == CUSTOM):
  float  customTargetPct                        // NEW
  float  customCriticalLowPct                   // NEW
  float  customMaxMoisturePct                   // NEW
  float  customHysteresisPct                    // NEW
  uint32 customSoakTimeMs                       // NEW
  uint16 customPulseWaterMl                     // NEW
  uint8  customMaxPulsesPerCycle                // NEW
  // Pompa per-pot
  float  pumpMlPerSec = 0                       // z kalibracji pompy (0 = nie skalibrowana)
  bool   pumpCalibrated = false
  // Sensory per-pot
  bool   potMaxActiveLow = true                 // per-pot overflow sensor polaryzacja
  float  moistureEmaAlpha = 0.1                 // per-pot EMA alpha
}

struct Config {
  uint16 schemaVersion

  // Tryb
  enum Mode { AUTO, MANUAL }
  Mode mode

  // Multi-pot
  uint8  numPots = 1                            // 1 lub 2 (domyślnie 1)
  PotConfig pots[MAX_POTS]                      // pots[0] = główna, pots[1] = opcjonalna

  // Pompa — globalne safety
  uint32 pumpOnMsMax = 30000                     // hard timeout per puls (obie pompy)
  uint32 cooldownMs  = 60000                     // min przerwa między cyklami podlewania

  // Anti-overflow — globalne
  bool   antiOverflowEnabled = true
  uint32 overflowMaxWaitMs = 600000              // max czas czekania na opadnięcie wody (10 min)
  enum UnknownPolicy { BLOCK, ALLOW_WITH_WARNING }
  UnknownPolicy waterLevelUnknownPolicy = BLOCK

  // Warunki pogodowe
  float  heatBlockTempC = 35.0                   // powyżej: blokada normalnego podlewania
  float  directSunLuxThreshold = 40000           // powyżej: blokada (bezpośrednie słońce)
  bool   morningWateringEnabled = false          // drugie okno podlewania

  // Dusk detector (sensorowy — bez RTC)
  uint32 duskWateringWindowMs = 7200000          // 2h okno podlewania po wykryciu zmierzchu
  float  duskScoreEnterThreshold = 0.55
  float  duskScoreConfirmThreshold = 0.65
  float  duskScoreCancelThreshold = 0.30
  uint32 transitionConfirmMs = 900000            // 15 min sustained score
  uint32 fallbackIntervalMs = 6*3600*1000        // co 6h jeśli brak detekcji zmierzchu

  // Rezerwuar — WSPÓLNY dla obu doniczek
  float  reservoirCapacityMl = 10000   // 10L (domyślne, zmienne w Settings)
  float  reservoirLowThresholdMl = 2000 // 2L — próg czujnika LOW

  // Przycisk manualny
  uint32 manualMaxHoldMs = 30000
  uint32 manualCooldownMs = 60000

  // Anomaly detection
  float  anomalyDryingRateThreshold = 5.0        // %/h statyczny fallback
  float  anomalyDryingRateMultiplier = 3.0       // mnożnik learned baseline

  // Vacation mode
  bool   vacationMode = false
  float  vacationTargetReductionPct = 10.0
  uint8  vacationMaxPulsesOverride = 2
  float  vacationCooldownMultiplier = 2.0

  // (sieć w osobnym NetConfig / ag_net namespace)
}
```

### Zaktualizowane zdarzenia (EventQueue)

Nowe eventy (do istniejącego zestawu):

```
// Watering cycle — wszystkie zawierają potIndex
WATERING_CYCLE_START(potIndex, moisturePct, reason)
WATERING_PULSE_START(potIndex, pulseNum, durationMs)
WATERING_PULSE_END(potIndex, pulseNum, pumpedMl)
WATERING_SOAK_DONE(potIndex, moisturePct)
WATERING_OVERFLOW_DETECTED(potIndex)
WATERING_OVERFLOW_CLEARED(potIndex)
WATERING_TARGET_REACHED(potIndex, moisturePct)
WATERING_CYCLE_DONE(potIndex, totalMl, pulses, moistureBefore, moistureAfter)

// Reservoir (wspólny)
RESERVOIR_LOW
RESERVOIR_EMPTY
RESERVOIR_WARNING(daysRemaining)
RESERVOIR_REFILL

// Manual
MANUAL_PUMP_ON(potIndex)
MANUAL_PUMP_OFF(potIndex, heldMs, pumpedMl)
MANUAL_EMERGENCY_STOP
MANUAL_BLOCK(reason)

// Schedule
SCHEDULE_BLOCK(potIndex, reason)              // HEAT, DIRECT_SUN

// Dusk detector
DUSK_DETECTED
DAWN_DETECTED
DUSK_TRANSITION_START(score)
DUSK_CANCEL(score)
SOLAR_CLOCK_CALIBRATED(dayLengthMs)
DUSK_DRIFT_WARNING(deltaMinutes)

// Requests (UI/NET → ControlTask)
REQUEST_SET_PLANT(potIndex, profileIndex)     // per-pot profil
REQUEST_PUMP_CALIBRATE(potIndex)              // per-pot kalibracja
REQUEST_RESERVOIR_REFILL
REQUEST_MANUAL_WATER(potIndex)                // /water [1|2]
REQUEST_SET_VACATION(bool)

// Vacation mode
VACATION_MODE_ON
VACATION_MODE_OFF

// Anomaly
ANOMALY_FAST_DRYING(potIndex, ratePctPerHour)
SENSOR_ANOMALY(potIndex, sensorId, description)
```

### Zaktualizowana checklista

- [ ] **Per-pot (powtórz dla każdej aktywnej doniczki)**:
  - [ ] Kalibracja pompy: `pots[i].pumpMlPerSec` — zmierzyć na stanowisku (30s test).
  - [ ] Weryfikacja czujnika doniczki: czy TRIGGERED = woda na dnie podwójnego dna.
  - [ ] EMA alpha: dostroić per-pot — mniejsze α = wolniejsza reakcja, mniej szumu.
  - [ ] Profile roślin: sprawdzić czy domyślne wartości pasują do konkretnych sadzonek.
- [ ] Pojemność rezerwuaru: `reservoirCapacityMl` — zmierzyć dokładnie.
- [ ] `reservoirLowThresholdMl` — ile wody zostaje gdy czujnik przechodzi w LOW.
- [ ] Dusk detector: obserwacja 2-3 zachodów → weryfikacja logów `DUSK_TICK` i `DUSK_CONFIRMED`.
- [ ] Dusk detector: dostrojenie progów (`duskScoreEnterThreshold`, `duskScoreConfirmThreshold`) do balkonu.
- [ ] Vacation mode: test `/vacation on` → weryfikacja obniżonego targetu obu doniczek.
- [ ] Heartbeat: weryfikacja raportu porannego (obie doniczki w raporcie).
- [ ] Trend analysis: obserwacja 6+ godzin learning per-pot → weryfikacja `baselineDryingRate`.
- [ ] Anomaly alarm: symulacja szybkiego wysychania → weryfikacja ANOMALY_FAST_DRYING per-pot.
- [ ] Multi-pot: `numPots=2` → weryfikacja niezależnych cykli obu doniczek + sumowanie wody w rezerwuarze.
- [ ] Multi-pot UI: weryfikacja widoku kompaktowego i przełączania BtnB.
- [ ] Test Dual Button: blue=pump wybranej doniczki, red=emergency stop — weryfikacja na stanowisku.
- [ ] PbHUB CH5 tryb IN/IN: weryfikacja czy firmware STM32F030 wspiera oba piny jako input.

## Odkrycia empiryczne — PbHUB v1.1 (STM32F030, SKU U041-B)

Sekcja dokumentuje wyniki testów diagnostycznych na rzeczywistym sprzęcie.
Odkrycia te **muszą** być uwzględnione w implementacji.

### Fakty potwierdzone pomiarami

1. **Logika 5V na Grove/PbHUB** — StickS3 podaje 5V na Grove (po `M5.Power.setExtOutput(true)`).
   PbHUB przekazuje 5V na wszystkie porty CH0-CH5. ADC STM32F030 pracuje z VCC=3.3V,
   ale piny PA0-PA5 tolerują 5V w trybie analog input. Zakres ADC: 0-4095 (12-bit, 0-3.3V).

2. **~~ADC multiplexer crosstalk~~ → wyeliminowany sprzętowo (2026-03-17).**
   Problem: STM32F030 z jednym ADC i muxerem miał crosstalk ~14% między kanałami
   gdy water level sensor podawał 4.85V (NOT triggered).
   **Rozwiązanie**: diody Zenera 3.3V (BZX84C3V3) + rezystor 1kΩ na CH2, CH3, CH4.
   Obcinają 4.85V do ~3.3V, eliminując crosstalk na ADC. Softwarowa kompensacja
   (`sagFactor`, `crosstalkUplift`, `moistureComp`) usunięta z firmware.
   Szczegóły historyczne: `git log --all -- src/hardware.cpp` (commit sprzed 2026-03-17).

3. **Floating pins = VCC/2 (~2050–2100)** — puste porty PbHUB (brak podłączonego urządzenia)
   czytają ~2050–2100 ADC. To normalne zachowanie floating analog input na STM32.

4. **Water level sensor → 4.85V gdy NOT triggered** — sensor pojemnościowy
   (Holtek/RISC-V MCU) wyprowadza ~4.85V gdy NIE wykrywa wody,
   i ~0V (LOW) gdy wykrywa wodę. Diody Zenera 3.3V na CH2/CH3/CH4
   obcinają 4.85V do ~3.3V na wejściu STM32 (patrz punkt 2).

5. **Moisture sensor: zakres wyjścia 0–3.3V mimo zasilania 5V** — M5Stack Watering Unit
   (moisture + pump) jest zasilany 5V z PbHUB, ale czujnik pojemnościowy wilgotności
   daje sygnał analogowy w zakresie **0–3.3V** (logika 3.3V na wyjściu).
   Zmierzono na biurku (sensor nie w ziemi): **1.797V** dokładnie.
   To oznacza, że pełny zakres ADC (0–4095) odpowiada 0–3.3V,
   a sensor nie przekracza tego zakresu. Odczyt ~2230 ADC ≈ 1.8V jest poprawny.

6. **Walidacja krzyżowa po usunięciu per-pot `soilCalib`** — odczyt po zmianie normalizacji
  został porównany z drugim, niezależnym czujnikiem Zigbee **CS-201Z** w tym samym miejscu.
  Początkowo oba czujniki pokazały **57%**, ale po kilku minutach pojawił się rozjazd:
  **CS-201Z = 61%**, a M5Stack Watering Unit = **57%**.
  Wniosek: nowa normalizacja daje odczyt zbliżony do zewnętrznego punktu odniesienia,
  ale pojedyncza zgodność startowa nie jest dowodem pełnej równoważności obu sensorów
  ani identycznej skali `%` w czasie.

7. **Firmware wewnętrzny PbHUB v1.1** — STM32F030F4P6, I2C slave @0x61, FW version 2.
   ADC: 22 próbki, odrzuca min/max, uśrednia 20, sampling time = `LL_ADC_SAMPLINGTIME_1CYCLE_5`.
   GPIO: PA0→CH0, PA1→CH1, PA2→CH2, PA3→CH3, PA4→CH4, PA5→CH5.
   Dynamiczna rekonfiguracja pinów (analog/digital/PWM/servo) w callbacku I2C ISR.

8. **Digital read daje poprawny odczyt binarny** — `pbDigitalRead(ch)` (register `kChCode[ch] | 0x04`)
   rekonfiguruje pin jako `GPIO_MODE_INPUT` i zwraca 0/1. Sam odczyt jest poprawny (nie zależy od ADC).
   Z diodami Zenera na CH2/CH3/CH4, napięcie na pinach nie przekracza 3.3V,
   więc crosstalk na ADC jest pomijalny.

9. **4095 po włączeniu (osobny artefakt)** — tuż po starcie PbHUB, nim firmware STM32
   przełączy GPIO na analog mode, porty mają pull-up do VCC → ADC = 4095.
   Po pierwszym odczycie firmware dynamicznie rekonfiguruje pin
   (`GPIO_MODE_ANALOG, GPIO_NOPULL`) i kolejne odczyty są poprawne.
   Warm-up ~5 cykli eliminuje ten artefakt.

### ~~Zmierzone wartości referencyjne~~ (historyczne, przed Zenerem)

> Sekcja usunięta 2026-03-17. Crosstalk wyeliminowany sprzętowo (diody Zenera 3.3V
> na CH2/CH3/CH4). Tabele pomiarów, model trzech stanów, pseudokod kompensacji
> i kalibracji sagFactor — patrz historia git.

### Workaroundy w implementacji (zaktualizowane po Zenerze)

1. **Water level sensor → czytaj jako DIGITAL, nie ADC.**
   `pbDigitalRead()` zwraca 0/1. Z Zenerem na linii, napięcie ≤3.3V.

2. **Moisture → ADC bez kompensacji.**
   Z diodami Zenera crosstalk jest pomijalny. ADC raw → EMA → normalize.

3. **Moisture sensor: zakres 0–3.3V** — mimo zasilania 5V, wyjście analogowe
  M5Stack Watering Unit operuje w zakresie 0–3.3V. Logika firmware ma traktować to jako stały zakres pracy czujnika.

4. **Warm-up po starcie** — odrzuć 5 pierwszych cykli odczytów (4095 → stabilizacja).

5. **I2C: STOP+START, nie Repeated Start** — PbHUB v1.1 wymaga `endTransmission(true)`
   (STOP) + delay + `requestFrom()` (START). Repeated Start nie działa.

6. **Delay między write i read: 10ms** — STM32 potrzebuje czasu na przełączenie GPIO
   i wykonanie 22 próbek ADC.

### Hardening runtime po incydentach I2C / PbHUB

- **ADC sanity check**: odczyty wilgotności spoza zakresu ADC (`>4095`) są odrzucane jako niewiarygodne.
  Zamiast nich używany jest ostatni poprawny `raw`, a jeśli go brak — konserwatywny fallback „sucho / brak wiarygodnego pomiaru”.
- **Log diagnostyczny invalid raw**: firmware emituje `MOISTURE_RAW_INVALID raw=... fallback_raw=...`
  oraz zlicza takie przypadki w zbiorczym logu health.
- **Fail-closed na sensor health**: krótkie błędy komunikacji nie mogą bezpośrednio włączyć pompy;
  safety i manual działają konserwatywnie przy `UNKNOWN` / `FAIL`.
- **Warstwa software została utwardzona, ale root cause „zwisów I2C” okazał się sprzętowy**:
  weryfikacja na stanowisku wykazała niestabilny zasilacz USB. Plan powinien traktować to jako
  problem zasilania/warstwy fizycznej, nie jako dowód, że sam protokół I2C lub PbHUB jest logicznie błędny.

### Wniosek operacyjny po debugowaniu magistrali

- Jeśli jednocześnie „znikają” PbHUB, SHT30, QMP6988 i BH1750, należy najpierw podejrzewać zasilanie USB,
  przewód, hub lub spadki napięcia, a dopiero później sam firmware I2C.
- Software ma tylko ograniczać skutki uboczne: blokować podlewanie, utrzymać ostatni sensowny odczyt,
  logować `sensor health` i nie dopuścić do przypadkowego startu pompy.

---

## Warstwa hardware — dekompozycja abstrakcji (kontrakty + pseudokod)

### Cel warstwy hardware
- Ukryć szczegóły I²C/ADC/Pb.HUB i dać domenie proste odczyty/sterowania.
- Ujednolicić obsługę błędów i „health status".
- Wymusić zasady non-blocking: brak I/O w ISR, brak długich operacji w UI/NET.

### Zasady współdzielenia I²C/ADC
- I²C jest zasobem współdzielonym → jeden punkt dostępu (np. `I2CBus`) + mutex.
- Odczyty sensorów wykonuje `ControlTask` (lub dedykowany `SensorTask` o wysokim priorytecie).
- UI i NET nigdy nie czytają sprzętu bezpośrednio — tylko konsumują snapshoty.
- ISR: nie używa I²C/ADC; tylko budzi task (flaga/notify).
- Każdy driver ma timeout i rate limit (żeby nie „młócić” I²C/ADC).
### PbHUB — sensor bus abstraction

PbHUB v1.1 to jedyny mediator do analog/digital czujników na portach CH0-CH5.
Warto wydzielić `PbHubBus` jako jedyny punkt dostępu do tych portów.

```
interface PbHubBus {
  init(i2cAddr, i2cDelayMs) -> bool
  probePresent() -> bool
  fwVersion() -> uint8

  analogRead(ch) -> ReadResult<uint16>       // 12-bit, raw ADC
  digitalRead(ch, pin=0) -> ReadResult<bool>  // true=HIGH, false=LOW
  digitalWrite(ch, pin, level) -> bool

  // Crosstalk-aware batch read
  readAllAnalog() -> ReadResult<uint16>[6]
}
```

Zasady:
- Jeden `PbHubBus` per fizyczny PbHUB.
- Sensory domenowe (SoilMoisture, WaterLevel) delegują do `PbHubBus`.
- `PbHubBus` wie o I2C timing (STOP+START, delay), sensory nie muszą.
- Batch read (`readAllAnalog`) pozwala na jednokrotne przejście przez kanały
  z opcjonalnym dummy-read (priming) dla kompensacji S/H (jeśli kiedyś potrzebne).

### Wzorzec odczytu z kompensacją crosstalk

Odczyt moisture MUSI uwzględniać stan water level sensorów.
Sekwencja w `ControlTask`:

```
on sensorTick(nowMs):
  // 1. Water level — DIGITAL
  //    Rezerwuar — wspólny:
  reservoirMin = WaterLevelReservoir.readState(nowMs)    // activeLow=false

  // 2. Per-pot sensors:
  for potIdx in 0 ..< config.numPots:
    if not config.pots[potIdx].enabled: continue
    potCfg = config.pots[potIdx]
    ch = hwConfig.potChannels[potIdx]

    // Overflow sensor per-pot
    potMax = WaterLevelPot[potIdx].readState(nowMs, ch.potMaxLevelChannel, potCfg.potMaxActiveLow)

    waterGuards = { potMax: potMax.value, reservoirMin: reservoirMin.value }

    // Moisture — ADC (Zener eliminuje crosstalk)
    moistureRaw = PbHubBus.analogRead(ch.soilAdcChannel)

    // EMA per-pot
    if sensorHealth == OK:
      moistureFilters[potIdx].alpha = potCfg.moistureEmaAlpha
      moistureEma = emaUpdate(moistureFilters[potIdx], moistureRaw.value, nowMs)
      moisturePct = normalizeSoil(moistureEma)
    else:
      log("[POT%d] SENSOR_OFFLINE — EMA frozen at %.1f", potIdx, moistureFilters[potIdx].value)
      moisturePct = normalizeSoil(moistureFilters[potIdx].value)

    snapshot.pots[potIdx] = { moisturePct, moistureRaw, waterGuards }

  // 3. Wspólne sensory ENV (temp, lux, ciśnienie) — bez zmian
  snapshot.env = readEnvSensors()
  publish(snapshot)
```
### Wspólny kontrakt driverów
Pseudotypy (bez implementacji):
```
enum Health { OK, WARN, FAIL }
enum ErrorCode { NONE, I2C_NACK, I2C_TIMEOUT, OUT_OF_RANGE, NOT_PRESENT, ADC_FAIL, CONFIG_MISSING, ... }

struct ReadResult<T> {
  T value
  Health health
  ErrorCode err
  uint32_t timestampMs
}

interface Sensor {
  init(hwCfg) -> bool
  read(nowMs) -> ReadResult<...>
  selfTest(nowMs) -> Health
}

interface Actuator {
  init(hwCfg) -> bool
  set(state) -> bool
  get() -> state
}
```

### Czujniki — kontrakty domenowe

#### 1) Wilgotność gleby (analog przez Pb.HUB)
Domena potrzebuje: `soilMoisturePct` + status.
Sensor Watering Unit: wyjście 0–3.3V mimo zasilania 5V. ADC 0-4095 = 0-3.3V.
Zmierzono: ~1.797V (suchy, na biurku) → ADC ≈ 2230.
```
interface SoilMoistureSensor : Sensor {
  readRaw(nowMs) -> ReadResult<uint16>
  readPct(nowMs) -> ReadResult<float>
}

normalizeSoil(raw):
  // Stała mapa referencyjna dla Watering Unit 0-3.3V, bez per-pot kalibracji użytkownika.
  // Do obliczenia krzywej bierzemy tylko dwa punkty krańcowe zmierzone na stanowisku:
  // - dryRaw = 2230  -> 0%
  // - wetRaw = 1752  -> 100%
  // Nie ustawiamy pośrednich kotwic. Cały zakres jest liczony z funkcji ciągłej.
  //
  // Krok 1: normalizacja raw do zakresu 0..1
  //   t = clamp((dryRaw - raw) / (dryRaw - wetRaw), 0, 1)
  //
  // Krok 2: nieliniowa krzywa jednostronna opóźniająca dojście do 100%:
  //   curve(t) = t^5
  //
  // Krok 3: przeskalowanie do procentów:
  //   pct = 100 * curve(t)
  //
  // Efekt praktyczny:
  // - blisko dry reakcja jest spokojna i nie przeszacowuje małych zmian,
  // - środek zakresu nadal rośnie monotonicznie,
  // - blisko wet wynik dochodzi do 100% dużo później niż w poprzednim modelu.
  //
  // Dlaczego nie używamy tu symetrycznej krzywej S:
  // - przy raw bardzo bliskim wetRaw, ale jeszcze nie równym wetRaw,
  //   funkcja typu smootherstep dawała już niemal 100%,
  // - w praktyce powodowało to przedwczesną saturację wskazania.
  t = clamp((2230 - raw) / (2230 - 1752), 0, 1)
  pct = 100 * t^5
  return pct

Źródła i uzasadnienie modelu:
- Dla dokładnie tego sensora, czyli M5Stack Watering Unit / M5 Watering, nie znaleziono
  oficjalnej krzywej producenta `raw -> %` ani oficjalnej biblioteki M5, która zwracałaby
  gotową wilgotność w procentach dla tego modułu.
- To oznacza, że obecny wzór nie jest "tajnym wzorem M5", tylko świadomie przyjętym
  modelem lokalnym dla tego konkretnego układu AutoGarden.
- Nie przepisujemy wprost jednego równania z publikacji do firmware, bo te prace dotyczyły innych sensorów,
  innych gleb i innych warunków laboratoryjnych.
- Bierzemy z literatury założenia jakościowe, które są spójne z obserwacjami na stanowisku:
  - charakterystyka czujników pojemnościowych jest nieliniowa,
  - prosta mapa liniowa `raw -> %` jest zbyt słaba fizycznie,
  - okolice skrajów suchy/mokry mają tendencję do spłaszczenia,
  - kalibracja powinna być lokalna i oparta na punktach końcowych z własnego układu.
- Na tej podstawie przyjęto w AutoGarden prosty model inżynierski: kalibracja dwupunktowa
  (`dryRaw`, `wetRaw`) + jednostronna krzywa potęgowa `pct = 100 * t^5` na całym zakresie.

Źródła, z których bierzemy te założenia:
- Hydronix, "Are All Moisture Sensors Equal?" — opisuje nieliniowość, wpływ materiału,
  temperatury i ograniczenia prostych kalibracji między różnymi sensorami.
  URL: https://www.hydronix.com/resources/blogs/are-all-moisture-sensors-equal/
- MDPI Sensors 2024, 24(9):2725 — pokazuje, że dla czujnika pojemnościowego najlepszy model
  w badanym układzie był nieliniowy i zależny od konkretnej gleby.
  URL: https://www.mdpi.com/1424-8220/24/9/2725
- MDPI Micromachines 2019, 10(12):878 — pokazuje, że kompensacja wilgotności ma charakter
  nieliniowy i że modele liniowe są zbyt uproszczone.
  URL: https://www.mdpi.com/2072-666X/10/12/878
- SciELO: "Nonlinear models for soil moisture sensor calibration in tropical mountainous soils" —
  wskazuje, że modele nieliniowe/asymptotyczne lepiej opisują zachowanie sensora niż prosta linia.
  URL: https://www.scielo.br/j/sa/a/DbC6KbDgg5TV4yn9BRVZznj/?format=html&lang=en
```

#### 2) Poziom wody: MAX w doniczce + MIN w rezerwuarze
W systemie używamy dwóch czujników non-contact o różnej semantyce:
- `potMax` = sygnał ryzyka przelania (blokuje podlewanie)
- `reservoirMin` = sygnał braku wody w rezerwuarze (blokuje podlewanie i chroni pompę)

**UWAGA — odkrycie empiryczne**: czujniki poziomu wody (pojemnościowe, Holtek BS83A02A-4A
lub RISC-V MCU) dają sygnał binarny GPIO LOW/HIGH. **Czytaj jako DIGITAL, nie ADC.**
Digital read daje poprawny odczyt binarny sensora. Diody Zenera 3.3V na CH2/CH3/CH4
obcinają 4.85V do ~3.3V, eliminując crosstalk na ADC.

```
enum LevelState { OK, TRIGGERED, UNKNOWN }

interface LevelSensor : Sensor {
  readState(nowMs) -> ReadResult<LevelState>
}

struct WaterGuards {
  LevelState potMax
  LevelState reservoirMin
}
```

Implementacja `readState`:
```
LevelSensor.readState(nowMs):
  digitalVal = PbHubBus.digitalRead(channel, pin=0)
  if digitalVal.health != OK:
    return ReadResult(UNKNOWN, WARN, digitalVal.err)

  // Logika aktywacji zależy od instancji sensora:
  // potMax:       activeLow=true  -> LOW=triggered (sensor wykrył wodę = overflow)
  // reservoirMin: activeLow=false -> HIGH=triggered (brak wody = rezerwuar niski)
  // Dzięki temu TRIGGERED zawsze = problem w domenie.
  triggered = (config.activeLow) ? (digitalVal.value == false) : (digitalVal.value == true)

  return ReadResult(triggered ? TRIGGERED : OK, OK, NONE)
```

Polityka `UNKNOWN` ma być konfigurowalna (domyślnie bezpieczna: blokuj).

#### 3) ENV.III, BH1750, QMP6988 (opcjonalne)
```
struct EnvReading { float tempC; float humidityPct; }
interface EnvSensor : Sensor { readEnv(nowMs) -> ReadResult<EnvReading> }
interface LightSensor : Sensor { readLux(nowMs) -> ReadResult<float> }
interface Barometer : Sensor { readPressureHpa(nowMs) -> ReadResult<float> }
```

### Aktywatory — kontrakty domenowe

#### Pompa
Pompa jako driver nie robi „logiki podlewania”, ale zapewnia minimalny failsafe (w razie błędu I/O OFF).
```
interface PumpActuator : Actuator {
  on(nowMs, plannedDurationMs)
  off(nowMs, reason)
  isOn() -> bool
}
```

#### Przyciski (wbudowane + Dual Button)
Przyciski nie powinny bezpośrednio sterować pompą — generują eventy.
```
interface Buttons {
  poll(nowMs) -> list<ButtonEvent>
}
```

### Strategia akwizycji sensorów (tick-driven)

**Kolejność odczytów**: najpierw digital water level, potem ADC moisture.

```
on TICK_100MS:
  // 1. Reservoir — wspólny digital
  reservoirMin = WaterLevelReservoir.readState(nowMs)   // digitalRead

  // 2. Per-pot: water level + moisture ADC
  for potIdx in 0 ..< config.numPots:
    if not config.pots[potIdx].enabled: continue
    potCfg = config.pots[potIdx]
    ch = hwConfig.potChannels[potIdx]

    potMax = WaterLevelPot[potIdx].readState(nowMs, ch.potMaxLevelChannel, potCfg.potMaxActiveLow)
    waterGuards = { potMax, reservoirMin }

    soilRaw = PbHubBus.analogRead(ch.soilAdcChannel)
    soilPct = normalizeSoil(soilRaw)

    snapshot.pots[potIdx] = { soilPct, soilRaw, waterGuards }

on TICK_1S:
  env = Env.readEnv(nowMs)
  lux = Light.readLux(nowMs)
  bar = Barometer.readPressureHpa(nowMs)
  snapshot.env = { env, lux, bar }
  publish(snapshot)
```

### Konfiguracja hardware (parametryzacja — bez zgadywania)
Pseudostruktura:
```
struct HardwareConfig {
  // I2C
  i2cSdaPin, i2cSclPin          // StickS3 Port.A: SDA=G9, SCL=G10
  i2cFreq = 100000              // 100kHz — PbHUB v1.1 nie wspiera fast mode
  addrEnv = 0x44                // SHT30
  addrBaro = 0x70               // QMP6988
  addrLight = 0x23              // BH1750
  addrPbHub = 0x61              // PbHUB v1.1 (konfigurowalne 0x61-0x68)

  // ==========================================
  // Mapowanie Pb.HUB — 6 kanałów, 2 doniczki
  // ==========================================
  // CH0: doniczka 0 — moisture ADC (pin0) + pompa (pin1)  [Watering Unit]
  // CH1: doniczka 1 — moisture ADC (pin0) + pompa (pin1)  [Watering Unit #2]  ← opcjonalna
  // CH2: doniczka 0 — czujnik przelewowy (DIGITAL)
  // CH3: rezerwuar  — czujnik poziomu MIN (DIGITAL) — WSPÓLNY
  // CH4: doniczka 1 — czujnik przelewowy (DIGITAL)                            ← opcjonalna
  // CH5: Dual Button (DIGITAL, manual pump)                                   ← opcjonalny

  // Per-pot channel mapping (tablica indeksowana potIndex):
  struct PotChannels {
    uint8 soilAdcChannel        // kanał ADC moisture
    uint8 pumpOutputChannel     // kanał pompy (pin1 tego samego Grove)
    uint8 potMaxLevelChannel    // kanał czujnika przelewowego
  }
  PotChannels potChannels[MAX_POTS] = [
    { soilAdcChannel=0, pumpOutputChannel=0, potMaxLevelChannel=2 },  // pot 0
    { soilAdcChannel=1, pumpOutputChannel=1, potMaxLevelChannel=4 },  // pot 1
  ]

  // Wspólne kanały
  reservoirMinLevelChannel = 3  // CH3: water level sensor rezerwuar (DIGITAL!)
  dualButtonChannel = 5         // CH5: M5Stack Dual Button
  // WAŻNE: CH5 wymaga trybu IN/IN (oba piny jako input)!
  // Standardowo PbHUB ustawia pin A=IN, pin B=OUT.
  // Dual Button: blue zwiera pin A→GND, red zwiera pin B→GND — oba muszą być input.
  // Odczyt: digitalRead(CH5, pin0) = blue, digitalRead(CH5, pin1) = red. Active LOW.
  dualBtnBluePin = 0              // pin A (SIG/żółty) — niebieski przycisk
  dualBtnRedPin  = 1              // pin B (biały)     — czerwony przycisk

  // UWAGA: Watering Unit ma moisture sensor i pompę na tym samym Grove
  // pin0 = analog moisture, pin1 = pump output

  // Buttons
  builtinButtons = [BtnA, BtnB] // StickS3 ma 2 przyciski fizyczne (M5Unified)

  // PbHUB I2C timing
  pbhubI2cDelayMs = 10          // delay między write i read na PbHUB
  pbhubWarmupCycles = 5         // odrzuć N pierwszych cykli po starcie

  // Calibration / policies
  reservoirMinActiveLow = false // reservoir: HIGH = brak wody = TRIGGERED (odwrócony!)
  waterLevelUnknownPolicy: { BLOCK, ALLOW_WITH_WARNING }

  // EXT power
  extOutputEnabled = true       // M5.Power.setExtOutput(true) WYMAGANE na StickS3
}
```

### Checklista „do uzupełnienia na stanowisku”
- [x] Adresy I2C: PbHUB = 0x61 (potwierdzone skanem). ENV.III, BH1750 — do weryfikacji.
- [x] Mapowanie kanałów Pb.HUB:
  - [x] CH0: ADC moisture (M5Stack Watering Unit) — potwierdzone, ADC ~2480
  - [x] CH2: water level sensor doniczka (DIGITAL read, activeLow) — potwierdzone, GPIO 0/1
  - [ ] CH3: water level sensor rezerwuar — do podłączenia i weryfikacji
  - [ ] CH0 pin1: wyjście pompy — do weryfikacji
  - [ ] CH5: Dual Button — do weryfikacji
- [x] Typ interfejsu czujników poziomu: **DIGITAL** (nie ADC!) — potwierdzone empirycznie.
- [x] Crosstalk ADC: wyeliminowany sprzętowo — diody Zenera 3.3V na CH2/CH3/CH4 (2026-03-17).
- [x] Per-sensor activeLow: potMax=true (LOW=overflow), reservoirMin=false (HIGH=brak wody). TRIGGERED = problem w domenie.
- [x] Moisture sensor (Watering Unit): zakres wyjścia **0–3.3V** mimo 5V VCC. Suchy na biurku = 1.797V.
- [x] Polityka `LevelState.UNKNOWN` -> domyślnie `BLOCK`.
- [ ] Dobór `pumpOnMsMax`, `cooldownMs` w zależności od wydajności pompy.
- [ ] Mapowanie przycisków: które akcje (menu, zmiana trybu, manual ON/OFF).
- [x] StickS3 wymaga `M5.Power.setExtOutput(true)` — Grove 5V jest domyślnie wyłączone.
- [x] PbHUB v1.1 I2C: STOP+START (nie Repeated Start), delay 10ms, 100kHz.

### Checklista walidacji driverów
- Każdy driver:
  - ma timeout (brak „wiecznego czekania” na I²C)
  - zwraca `Health` + `ErrorCode`
  - nie odczytuje częściej niż potrzeba (rate limit)
- I²C mutex:
  - brak równoległych transakcji
  - brak wywołań z ISR

## Steps (kolejne etapy prac — bez kodu na tym etapie)

### 1) Project scaffolding
- Ensure platformio.ini includes required libraries (M5Unified, M5GFX, sensor drivers, WiFi/Telegram libs, etc.).
- Establish a folder structure under src (e.g. sensors/, actuators/, ui/, net/, config/).
  - sensors/: odczyty i filtracja czujników (I²C/ADC)
  - actuators/: pompy, przyciski, sterowanie wyjściami
  - ui/: ekran, menu, widoki statusu/ustawień
  - net/: Wi‑Fi, protokół zdalny (HTTP lub Telegram)
  - config/: konfiguracja i jej zapis/odczyt (Preferences/EEPROM)

### 2) Hardware interface modules
Create sensor classes/wrappers for:
- capacitive soil‑moisture analog input via Pb.HUB
- Non‑contact water level sensors: MAX w doniczce + MIN w rezerwuarze (przez Pb.HUB)
- ENV.III (I²C)
- BH1750 light sensor (I²C)
- QMP6988 (w ENV.III; osobny barometr tylko jeśli używasz oddzielnego modułu)

Add pump control API (digital outputs from Pb.HUB, manual override via Dual Button).
- W tej warstwie warto ujednolicić: inicjalizację, odczyty, błędy, zakresy (np. normalizacja wilgotności), oraz “health status” czujnika (OK / brak odpowiedzi / wartości poza zakresem).

### 3) Core logic
Implement humidity‑maintenance algorithm:
- read soil moisture, compare to target, run pump for configurable duration.
- monitor water level sensors to prevent overflow.
- debounce sensor readings, provide safety timeouts.

Provide configuration storage in flash (Arduino Preferences or EEPROM).
- Konfiguracja minimalna: target moisture, czasy pracy pompy (min/max), przerwy bezpieczeństwa, progi czujników poziomu wody / “anti‑overflow”, tryb manual/auto.

### 4) User interface
Use the M5Stack display to show:
- current moisture, water level, environment (temp/pressure/light)
- pump status, target settings
- simple menu navigated with buttons/grove inputs.

Update main.cpp to wire up M5.update() loop and UI tasks.
- Minimalny loop: bez logiki domenowej i bez `delay()`; logika działa w taskach/tickach.

### 5) Networking / remote control
Initialize Wi‑Fi and reconnect logic.

Choose remote protocol:
- option A: HTTP API + small web GUI
- option B: Telegram bot using UniversalTelegramBot (fits HLD remark).

Implement commands to:
- read sensor values
- change target moisture
- switch pumps on/off

Handle network errors gracefully.
- Zasada: brak sieci nie może blokować automatyki podlewania; sieć to warstwa “opcjonalna”, z retry/backoff.

### 6) Integration & configuration
Define constants/structs for pin assignments and I²C addresses.
- Spisać adresy I²C, mapowanie kanałów Pb.HUB, oraz które wejścia/wyjścia odpowiadają którym czujnikom/pompom.

Allow runtime configuration via UI or remote commands.

Add onboarding routine (Wi‑Fi credentials input).
- Minimalnie: możliwość ustawienia SSID/hasła (np. przez UI albo przez prosty tryb konfiguracji), potem zapis do flash.

### 7) Diagnostics & logging
Log to serial (Serial.printf) for development.
- Logi: start systemu, status I²C, odczyty (w trybie debug), zdarzenia (pump on/off), powody blokady (anti‑overflow, timeout).

Optionally push logs over network or display error codes.
- Opcjonalnie: skrócony status na ekranie (np. “SENSOR_FAIL”, “WIFI_DOWN”).

### 8) Testing / verification
Build & upload via PlatformIO extension w VS Code.
Manual hardware tests with sensors connected.
Simulate water level and moisture changes to verify control logic.
Use PlatformIO Serial Monitor to watch logs.
If feasible, add basic unit tests (in test/) for algorithms.

## ✅ Verification
- Build passes (PlatformIO: Build).
- Device boots, shows menu instead of Hello World.
- Sensor readings appear correctly.
- Pump responds to moisture thresholds and manual commands.
- Wi‑Fi connects; remote commands (Telegram or web) work.
- Configuration persists across resets.
- No runtime crashes / watchdog resets.

## 📌 Decisions
- Library choice: use M5Unified/M5GFX for display and board support.
- Remote interface: Telegram bot recommended (matches HLD text).
- Storage: flash via Preferences for simplicity.

Decyzje wykonawcze (na teraz):
- Dekompozycja i pseudokod są zamrożone jako „spec” dla implementacji.
- Pinowanie do rdzeni: domyślnie OFF; wracamy do tematu dopiero, gdy pojawi się realny problem.
