# AutoGarden — plan ogólny i aktualny stan firmware

Ten dokument łączy dwie warstwy:

- ogólny HLD projektu, czyli docelowy podział odpowiedzialności i kontrakt architektoniczny,
- aktualny stan wdrożonego firmware dla M5Stack StickS3.

To nie jest backlog ani długa checklista. Celem jest utrzymanie ogólnego planu tak,
żeby był zgodny z tym, co naprawdę działa w kodzie.

## Cel systemu

AutoGarden utrzymuje wilgotność jednej lub dwóch doniczek przy użyciu:
- M5Stack StickS3 jako kontrolera i UI,
- PbHUB jako ekspandera wejść/wyjść,
- M5 Watering Unit jako zestawu pompa + sonda wilgotności,
- czujników poziomu wody dla overflow w doniczce i minimum w rezerwuarze,
- opcjonalnej sieci Wi-Fi i Telegram.

Automatyka podlewania ma priorytet nad UI i siecią. Brak Wi-Fi nie może zatrzymać sterowania.

## HLD — założenia architektoniczne

To są główne zasady projektu i nadal obowiązują w aktualnym kodzie.

### Wymagania wykonawcze

- `M5Unified` jest bazą dla uruchomienia urządzenia i obsługi przycisków/ekranu.
- logika ma być nieblokująca, bez długich `delay()` w sterowaniu,
- automatyka podlewania ma być odseparowana od UI i sieci,
- sieć jest dodatkiem i nie może blokować krytycznej logiki,
- ISR mają tylko sygnalizować zdarzenia, a nie wykonywać logikę domenową,
- konfiguracja i stan runtime mają przeżyć restart przez NVS,
- pompa musi być chroniona przez timeout, cooldown i blokady poziomu wody.

### Warstwy odpowiedzialności

- warstwa hardware: sensory, PbHUB, pompy, przyciski,
- warstwa domeny: schedule, FSM podlewania, safety, budget, trendy,
- warstwa prezentacji: ekran StickS3 i status użytkowy,
- warstwa zdalna: provisioning Wi-Fi i Telegram,
- warstwa konfiguracji: walidacja, migracje i persist.

### Przepływ danych

Model danych pozostaje event-driven i snapshot-based:

- hardware publikuje bieżące odczyty,
- logika domenowa zużywa snapshot sensorów i aktualizuje runtime,
- UI i sieć czytają stan gotowy, zamiast bezpośrednio odpalać logikę sprzętową,
- komendy z UI i Telegrama są tłumaczone na żądania do domeny.

### Model wykonania

Docelowo i implementacyjnie system jest podzielony logicznie na trzy obszary:

- control: odczyty, analiza, safety i sterowanie pompą,
- UI: render ekranu i wejście lokalne,
- network: Wi-Fi, captive portal, Telegram.

W praktyce obecny kod zachowuje ten podział odpowiedzialności na poziomie modułów i ticków,
nawet jeśli część przepływu jest nadal koordynowana centralnie z `main.cpp`.

## Architektura runtime

Projekt jest zbudowany wokół krótkich ticków i stanu współdzielonego.

- `main.cpp` koordynuje główny przebieg programu i obsługę eventów.
- `hardware.cpp` czyta sensory i steruje pompami przez PbHUB.
- `watering.cpp` zawiera domenę podlewania: harmonogram, safety, manual, budżet wody.
- `analysis.cpp` liczy EMA, trendy, detektor zmierzchu i historię.
- `ui.cpp` renderuje ekran główny i Settings.
- `network.cpp` obsługuje captive portal Wi-Fi i Telegram.
- `config.cpp` i `config.h` trzymają schemat konfiguracji, walidację i migracje NVS.

`loop()` nie niesie logiki biznesowej. Program działa w krótkich, nieblokujących krokach.

### Rzeczywisty model tasków

Aktualny firmware uruchamia cztery taski FreeRTOS bez pinowania do rdzeni:

- `ControlTask`, priorytet `5`,
- `UiTask`, priorytet `3`,
- `NetTask`, priorytet `2`,
- `ConfigTask`, priorytet `1`.

`ConfigTask` realizuje debounce zapisu konfiguracji do NVS, więc zapis flash nie blokuje sterowania.

### Snapshoty i współdzielenie stanu

Aktualna implementacja używa dwóch głównych współdzielonych struktur chronionych mutexami:

- `s_latestSnap` dla bieżącego snapshotu sensorów,
- `s_sharedState` dla stanu domenowego, UI i sieci.

To jest zgodne z HLD, bo UI i sieć czytają gotowy stan zamiast sterować sprzętem bezpośrednio.
Jednocześnie praktyczny przepływ jest hybrydowy: część komend idzie przez `EventQueue`, a część telemetrii
przez snapshoty i dedykowaną kolejkę feedbacku Telegram.

## Aktualny model konfiguracji

Źródłem prawdy jest `Config` w `src/config.h`. Aktualna wersja schematu to `6`.

### Migracje i odporność na uszkodzony NVS

Kod aktywnie wspiera migracje konfiguracji:

- `v4 -> v5`,
- `v5 -> v6`.

Jeśli załadowany config nie przejdzie walidacji, firmware wraca do bezpiecznych defaultów.
Dotyczy to też `NetConfig` i `RuntimeState`.

### Osobne namespace NVS

Aktualnie firmware utrzymuje osobne obszary persist:

- `ag_config`,
- `ag_net`,
- `ag_hist`,
- `ag_dusk`,
- `ag_runtime`.

Factory reset czyści wszystkie te namespace'y.

### NetConfig

Poza `Config` istnieje osobny `NetConfig`, który przechowuje:

- provisioning Wi-Fi,
- nazwę bota Telegram,
- token bota Telegram,
- allowlistę `chat_ids`.

Kod wspiera też migrację starego `NetConfig v1 -> v2`.

### RuntimeState

Oprócz configu trwałego istnieje `RuntimeState`, który odtwarza po restarcie:

- poziom rezerwuaru i przepompowaną wodę,
- baseline trendów,
- cooldowny,
- znaczniki `lastDusk` i `lastDawn`,
- część stanu `SolarClock`.

To jest ważne dla zgodności z HLD: restart nie ma kasować uczenia trendów ani bieżącego oszacowania budżetu.

### Poty

System obsługuje maksymalnie 2 doniczki (`kMaxPots = 2`). Każda doniczka ma osobny `PotConfig`:

- `enabled`
- `plantProfileIndex`
- `potMaxActiveLow`
- `moistureEmaAlpha`
- `moistureDryRaw`
- `moistureWetRaw`
- `moistureCurveExponent`
- `pulseWaterMl`

Pola `pumpMlPerSec` i `pumpCalibrated` nadal istnieją tylko dla kompatybilności z dawnym NVS.
Przy starcie firmware normalizuje je do jednej stałej wartości.

### Stały model pompy

Firmware wspiera jeden model pompy: M5 Watering.

Przyjęta i wymuszana stała wydajność:

- `5.17 ml/s`

Nie ma już procesu kalibracji pompy w UI ani przez Telegram.

### Domyślne kalibracje wilgotności

Aktualne domyślne endpointy i wykładniki są per-pot:

| Pot | Dry RAWf | Wet RAWf | Exponent |
|---|---:|---:|---:|
| POT1 | 2228 | 1766 | 0.63 |
| POT2 | 2229 | 1786 | 0.65 |

### Pozostałe istotne defaulty

- `mode = AUTO`
- `numPots = 1`
- `pumpOnMsMax = 30000 ms`
- `cooldownMs = 60000 ms`
- `antiOverflowEnabled = true`
- `overflowMaxWaitMs = 600000 ms`
- `waterLevelUnknownPolicy = BLOCK`
- `heatBlockTempC = 35.0`
- `directSunLuxThreshold = 40000`
- `duskWateringWindowMs = 7200000 ms`
- `fallbackIntervalMs = 6 h`
- `morningWateringEnabled = false`
- `reservoirCapacityMl = 1500`
- `reservoirLowThresholdMl = 400`
- `vacationMode = false`

## Profile roślin

Wbudowane profile są zdefiniowane w `src/config.cpp`:

- Pomidor
- Papryka
- Bazylia
- Truskawka
- Chili
- Custom

Każdy profil ma:

- `targetMoisturePct`
- `criticalLowPct`
- `maxMoisturePct`
- `hysteresisPct`
- `soakTimeMs`
- `pulseWaterMl`
- `maxPulsesPerCycle`

Profil `Custom` korzysta z pól nadpisywanych w `PotConfig`.

## Odczyt wilgotności

Ścieżka przetwarzania wilgotności jest dziś następująca:

1. odczyt surowego ADC z sondy,
2. krótka filtracja medianowa do `moistureRawFiltered`,
3. szybka ścieżka sterowania: `RAWf -> normalize -> moisturePct`,
4. wolniejsza ścieżka prezentacji i trendów: `EMA(RAWf) -> normalize -> moistureEma`.

Normalizacja jest liczona osobno dla każdej doniczki:

$$
t = clamp\left(\frac{dryRaw - raw}{dryRaw - wetRaw}, 0, 1\right)
$$

$$
moisturePct = 100 \cdot t^p
$$

gdzie `p` to `moistureCurveExponent` dla danego POT.

W praktyce:

- `moisturePct` służy do aktywnej kontroli cyklu podlewania,
- `moistureEma` służy do łagodniejszych decyzji harmonogramu, UI i trendów,
- odczyt `0–5%` jest traktowany jako `PROBE_NOT_IN_SOIL` i blokuje auto/rescue watering.

## Hardware i mapowanie

Aktualny kod zakłada domyślne mapowanie PbHUB z `g_hwConfig`, ale dokumentacja nadal traktuje to jako konfigurację stanowiska, nie prawdę absolutną dla każdego montażu.

Domyślne kanały:

| Funkcja | Kanał PbHUB |
|---|---:|
| POT1 moisture + pump | CH0 |
| POT2 moisture + pump | CH1 |
| POT1 overflow | CH2 |
| reservoir MIN | CH3 |
| POT2 overflow | CH4 |
| dual button | CH5 |

Szczegóły praktyczne z aktualnej implementacji:

- sensory poziomu wody są czytane jako digital,
- `potMax` jest interpretowany jako sygnał overflow,
- `reservoirMin` jest interpretowany jako niski poziom rezerwuaru,
- PbHUB pracuje z opóźnieniem po write/read i warm-upem po starcie,
- M5 Watering moisture output jest traktowany jako zakres 0–3.3 V.

### Faktycznie zaimplementowane hardening i recovery

Po pełnym odczycie kodu widać kilka ważnych aspektów, których wcześniej brakowało w planie:

- `HardwareManager` potrafi wykonać recovery magistrali I2C przez ręczne taktowanie SCL i ponowny `Wire.begin()`,
- po recovery wykonywana jest ponowna inicjalizacja PbHUB, SHT30, QMP6988 i BH1750,
- recovery uruchamia się dopiero przy serii błędów i ma cooldown czasowy,
- dla częściowych awarii I2C kod robi lżejszą ścieżkę `reinitI2cSensors()` bez pełnego recovery magistrali,
- DLight ma dodatkową szybką ścieżkę retry/re-init i publikuje stan jakości światła (`VALID`, `STALE`, `RECOVERING`, `UNKNOWN`),
- przy awarii DLight detektor dzień/noc zamraża uczenie i wraca do ostatniej stabilnej fazy zamiast uczyć się z uszkodzonego lux,
- QMP6988 jest zaimplementowany z odczytem OTP i własną kompensacją ciśnienia,
- dual button ma stabilizację wielopróbkową i oznacza wejście jako `unstable`, jeśli sygnał jest niespójny.

### Ochrona toru wilgotności

Ścieżka odczytu sondy ma dodatkowe zabezpieczenia runtime:

- odrzucenie `raw > 4095`,
- fallback do ostatniego poprawnego `raw`,
- osobne liczniki błędów `invalid`, `zero`, `fail`,
- log `MOISTURE_STUCK_FALLBACK`, jeśli przez dłuższy czas nie ma poprawnego ADC,
- medianę z okna `10` próbek przed normalizacją.

To wzmacnia HLD, bo poprawia fail-safe bez zmiany podstawowego modelu domeny.

## FSM podlewania

Każda aktywna doniczka ma własny cykl `WateringCycle` z fazami:

- `IDLE`
- `EVALUATING`
- `PULSE`
- `SOAK`
- `MEASURING`
- `OVERFLOW_WAIT`
- `DONE`
- `BLOCKED`

Cykl działa niezależnie per pot, ale wszystkie doniczki współdzielą jeden budżet rezerwuaru.

### Jak startuje cykl

Scheduler uruchamia cykl, gdy:

- wilgotność spadnie poniżej progu `target - hysteresis`,
- nie ma cooldownu,
- nie działa blokada safety,
- warunki pogodowe są dopuszczalne,
- aktualna faza dnia pozwala na podlewanie.

### Rescue watering

Jeśli wilgotność spadnie poniżej `criticalLowPct`, system może uruchomić podlewanie ratunkowe.

Wyjątek:

- `0–5%` nie oznacza już „bardzo sucho”, tylko `PROBE_NOT_IN_SOIL`.

### Pulse-soak-measure

Aktualny algorytm jest pulsowy:

- pompa włącza się tylko na krótki puls,
- po pulsie następuje `SOAK`,
- po `SOAK` system mierzy nową wilgotność,
- jeśli target nie został osiągnięty i nie ma blokad, przechodzi do kolejnego pulsu,
- jeśli overflow się aktywuje, przechodzi do `OVERFLOW_WAIT`.

### Hard timeout i cooldown

Kod egzekwuje:

- hard timeout pompy per puls przez `pumpOnMsMax`,
- cooldown między cyklami przez `cooldownMs`,
- timeout czekania na zejście overflow przez `overflowMaxWaitMs`.

## Schedule i pory podlewania

Aktualna logika harmonogramu jest prostsza niż stare wersje planu.

Priorytety:

1. blokady safety,
2. `PROBE_NOT_IN_SOIL`,
3. rescue watering,
4. cooldown,
5. heat block i direct sun block,
6. podstawowe okno podlewania po zmierzchu,
7. opcjonalne okno przed świtem,
8. fallback timer tylko nocą.

### Okno po zmierzchu

To jest główne automatyczne okno podlewania.

Cykl może wystartować, gdy:

- detektor zmierzchu ma już `lastDuskMs`,
- aktualna faza to noc lub przejście do nocy,
- od ostatniego zmierzchu nie minęło więcej niż `duskWateringWindowMs`.

### Morning watering

Obsługa istnieje, ale jest domyślnie wyłączona:

- `morningWateringEnabled = false`

### Fallback

Fallback nie działa „co 6h zawsze”.
Aktualny kod uruchamia go tylko wtedy, gdy:

- nie ma jeszcze wykrytego zmierzchu,
- system nie jest w fazie dnia,
- minął `fallbackIntervalMs` od ostatniego zakończonego cyklu.

W dzień scheduler zwraca `WAIT_FOR_DUSK`.

## Detektor zmierzchu i solar clock

`analysis.cpp` implementuje sensor fusion opartą o:

- BH1750,
- SHT30,
- QMP6988.

Detektor utrzymuje fazy:

- `NIGHT`
- `DAWN_TRANSITION`
- `DAY`
- `DUSK_TRANSITION`

Po zebraniu pełnych cykli budowany jest prosty `SolarClock` z estymacją długości dnia i nocy.

Aktualna implementacja służy przede wszystkim do:

- uruchamiania nocnego okna podlewania,
- opcjonalnego wyznaczania okna przed świtem,
- wspierania raportów i diagnostyki.

Aktualny kontrakt fail-safe jest bardziej restrykcyjny niż wcześniejsze założenia:

- przejście dzień/noc nie może być potwierdzone bez ważnego, świeżego lux,
- sygnały temp/humidity/pressure pełnią rolę pomocniczą, nie zastępują światła,
- przy `STALE` lub `RECOVERING` detector utrzymuje ostatnią stabilną fazę i nie uczy nowych długości dnia/nocy.

## Safety i blokady

Kod rozróżnia blokady per-pot i globalne.

### Twarde blokady

Najważniejsze powody blokady:

- overflow w doniczce,
- pusty rezerwuar,
- `UNKNOWN` na czujnikach poziomu przy polityce `BLOCK`,
- wyczerpany budżet rezerwuaru,
- niepoprawna konfiguracja pompy po uszkodzonym NVS,
- hard timeout pompy.

### Manual watering

Manual jest oddzielone od auto i działa przez dual button:

- blue: podlewanie aktualnie wybranego POT,
- red: emergency stop wszystkich pomp.

Manual ma własne zabezpieczenia:

- `manualMaxHoldMs`,
- `manualCooldownMs`,
- blokada przy overflow,
- blokada przy stanie niestabilnym wejścia,
- anti-spam / lockout.

### Dodatkowe bezpieczeństwo pomp

Kod zawiera jeszcze jedną warstwę safety, której wcześniej brakowało w planie:

- `ControlTask` okresowo wymusza `OFF`, jeśli pompa jest fizycznie włączona poza legalnym kontekstem,
- legalny kontekst to tylko aktywne `PULSE` albo aktywne ręczne trzymanie przycisku,
- logowany jest stan `PUMP_UNEXPECTED_ON_CONTEXT`,
- wyłączenie pompy ma retry przez `forcePumpOffWithRetry()`.

To zachowanie nie psuje HLD, tylko wzmacnia jego założenie fail-closed.

## Water budget i runtime state

Wspólny rezerwuar jest śledzony jako `WaterBudget`.

Runtime trzyma i zapisuje:

- `reservoirCurrentMl`,
- `totalPumpedMl`,
- `totalPumpedMlPerPot`,
- stan `reservoirLow`,
- trendy wysychania,
- cooldowny,
- znaczniki zmierzchu i świtu,
- dane pomocnicze solar clock.

Stan runtime jest zapisywany w `ag_runtime`, osobno od `ag_config` i `ag_net`.

### Kiedy runtime zapisuje się natychmiast

Kod nie zapisuje runtime tylko okresowo. Wymuszone natychmiastowe zapisy występują po:

- zdarzeniu pompowania,
- refill,
- zmianie istotnych znaczników zmierzchu/świtu.

To jest zgodne z HLD, bo krytyczne dane budżetu wody nie mogą zniknąć po restarcie.

## Historia i analiza

`analysis.cpp` utrzymuje:

- historię sensorów z kompresją czasową,
- historię podlewań,
- filtry EMA,
- trendy godzinowe,
- baseline wysychania per-pot,
- detekcję anomalii wysychania.

Ważna poprawka wdrożona w kodzie:

- zapis historii do NVS jest chunkowany, a nie oparty o pojedynczy duży blob.
- historia została przeniesiona do dedykowanej partycji NVS, aby nie konkurowała o miejsce z `ag_config`, `ag_net`, `ag_dusk` i `ag_runtime`.

### Szczegóły, które wynikają bezpośrednio z kodu

Aktualna historia ma wersjonowany schemat i trzy warstwy:

- `level1` w RAM,
- `level2` trwałe próbki zagregowane,
- `level3` próbki godzinowe,
- osobny `wateringLog`.

Kod używa wersjonowanego schematu historii na osobnej partycji. Przy bumpie schematu historia i learned state mogą zostać świadomie wyczyszczone, ale `ag_config` i `ag_net` pozostają zachowane.

### Trendy i anomaly

Trend liczy się co godzinę z `moistureEma`, a nie z chwilowego `moisturePct`.
To jest ważny detal implementacyjny i dobrze wspiera HLD, bo ogranicza reakcję na szum.

## Vacation mode

Vacation mode jest zaimplementowany i modyfikuje zachowanie harmonogramu:

- obniża target wilgotności,
- zmniejsza maksymalną liczbę pulsów,
- wydłuża cooldown.

Aktualny kod zawiera też blokadę przy wykryciu anomalii wysychania w vacation mode.

## UI na StickS3

`ui.cpp` implementuje dwa ekrany:

- `MAIN`
- `SETTINGS`

### Główny ekran

Widok główny ma dziś układ kompaktowy kart POT.

Zasady:

- dla `numPots == 1` używany jest ten sam layout co dla 2 pots,
- drugi slot pozostaje pusty,
- dla `numPots == 2` można przełączać `COMPACT`, `DETAIL_POT0`, `DETAIL_POT1`.

Status chip na karcie pokazuje skrócony stan logiczny, na przykład:

- `OK`
- `WET`
- `WAIT`
- `ARM`
- `COOL`
- `PROBE`
- `OVF`
- `TANK`
- `SNS?`
- `CFG?`
- albo aktywną fazę cyklu: `1/4`, `SOAK`, `MEAS`, `DONE`.

### Settings

Settings pozwala dziś zmieniać przede wszystkim:

- liczbę doniczek,
- profil POT1 i POT2,
- `P1 Dry`, `P1 Wet`, `P1 Exp`,
- `P2 Dry`, `P2 Wet`, `P2 Exp`,
- pojemność rezerwuaru,
- próg `Rez. min`,
- globalny `Puls` w ml,
- refill,
- tryb AUTO/MANUAL,
- vacation,
- wejście do Wi-Fi setup.

Aktualny kod naprawia wcześniejszy crash przy przełączaniu `numPots 2 -> 1` przez:

- większy `actionMap`,
- dynamiczne liczenie widocznych pozycji,
- clamp `settingsIndex` po zmianie listy opcji.

### Faktyczne mapowanie przycisków

Po analizie `main.cpp` i `ui.cpp` aktualne sterowanie lokalne wygląda tak:

- `BtnA click`: MAIN -> SETTINGS, a w SETTINGS wykonuje zmianę aktualnej opcji,
- `BtnA long`: wyjście z SETTINGS do MAIN,
- `BtnB click`: przełącza widok MAIN albo przesuwa kursor w SETTINGS,
- `BtnB long`: zmienia wartość aktualnej opcji w SETTINGS.

To warto mieć w planie wprost, bo różni się od starszych opisów ustawień.

## Telegram i sieć

Sieć jest opcjonalna. `network.cpp` obsługuje dwa obszary:

- captive portal do konfiguracji Wi-Fi,
- Telegram polling i komendy zdalne.

### Provisioning Wi-Fi

Jeśli nie ma zapisanej konfiguracji, urządzenie może uruchomić AP z prostym portalem.

Zapisywane pola `NetConfig`:

- `wifiSsid`
- `wifiPass`
- `telegramBotName`
- `telegramBotToken`
- `telegramChatIds`

Sekrety lokalne mogą być też podane przez `include/telegram_local_config.h`.

### Zachowanie AP i provisioning w praktyce

Aktualna implementacja provisioning ma jeszcze kilka ważnych cech:

- AP startuje non-blocking,
- może działać w `AP+STA`, jeśli urządzenie ma już zapisane Wi-Fi,
- portal ma auto-off po 5 minutach bez klienta,
- obsługuje ścieżki captive-portal dla różnych systemów,
- ma `/skip`, który pozwala przejść w tryb offline,
- wejścia formularza są sanityzowane i walidowane przed zapisem.

### Telegram runtime

Poza samym menu inline kod implementuje też:

- adaptacyjny polling `idle` / `fast window after interaction`,
- autoryzację czatów przez allowlistę,
- odpowiedzi `callback_query`,
- retry wysyłki wiadomości,
- dedykowaną kolejkę krótkich komunikatów feedback z `ControlTask` do `NetTask`.

### Remote water preflight

Ważny aspekt implementacji: zdalne `Water` nie wrzuca ślepo eventu do FSM.
Najpierw wykonuje preflight, który sprawdza m.in.:

- tryb AUTO,
- brak aktywnego cyklu,
- cooldown,
- overflow i rezerwuar,
- poprawność stałych parametrów pompy.

To jest zgodne z HLD bezpieczeństwa i powinno być opisane w planie.

### Telegram UX

Głównym wejściem użytkownika jest `/ag` albo `/start`.

Aktualne callbacki inline:

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

### Dodatkowe komendy tekstowe

Kod wspiera także konfigurację wilgotności per-pot przez komendy tekstowe, w tym:

- `/agraw`
- `/agdry`
- `/agwet`
- `/agexp`

Status Telegram pokazuje między innymi:

- `rawf`,
- `dry`,
- `wet`,
- `exp`,
- ostatni feedback podlewania,
- stan rezerwuaru,
- fazę dnia i uptime.

### Heartbeat

Aktualna implementacja heartbeat jest prostsza niż dawne wersje planu:

- ma mechanizm antyspamowy oparty o `lastHeartbeatMs`,
- korzysta z prostego sprawdzania okna czasowego,
- raport jest zbudowany z bieżącego stanu, nie z pełnego persisted analytics pipeline.

## Logowanie

Firmware loguje zdarzenia przez `AGSerial`.

Najważniejsze logi dotyczą:

- startu systemu,
- ładowania i migracji configu,
- błędów I2C i sensor health,
- safety block / unblock,
- `PUMP_ON` i `PUMP_OFF`,
- startu i końca cyklu,
- refill,
- provisioning i stanu Wi-Fi,
- diagnostyki Telegram.

### Format logów

`AGSerial` nie jest prostym wrapperem `Serial`.
Kod normalizuje linię logu do bardziej strukturalnej postaci:

- dodaje prefiks z czasem `millis` i sekwencją,
- dopisuje `event=...`, jeśli linia nie ma go już sama,
- serializuje zwykłe teksty do postaci `event=msg text="..."`.

To jest nowy element wobec starszego planu i wzmacnia diagnostykę bez naruszania HLD.

## Rozbieżności i ograniczenia względem HLD

Ta sekcja opisuje rozjazdy znalezione po pełnym czytaniu kodu. To nie są hipotezy, tylko stan repo.

- HLD zakładało ogólny model event-driven dla większości komunikacji. W kodzie eventy są używane głównie dla komend sterujących, natomiast telemetria i UI opierają się na mutexowanych snapshotach.
- HLD sugerowało bardziej ogólny model domenowych eventów typu `DUSK_DETECTED`. W aktualnym kodzie scheduler czyta bezpośrednio stan `DuskDetector`, a `DUSK_DETECTED` pozostaje tylko jako komentarz/TODO.
- HLD dopuszczało HTTP remote control. Aktualna implementacja HTTP służy wyłącznie provisioningowi; sterowanie zdalne robi Telegram.
- `pulseWaterMl` istnieje per-pot w strukturze configu, ale ścieżka UI `REQUEST_SET_PULSE_ML` ustawia dziś tę samą wartość dla obu potów. To ograniczenie implementacji, nie założenie domenowe.
- Factory reset w runtime jest dwuetapowy: hold `BtnA + BtnB`, potem puszczenie i osobne potwierdzenie `BtnA`. To jest bezpieczniejsze niż prosty jednofazowy opis z dawnych wersji planu.

## Weryfikacja zgodności nowych aspektów z HLD

Po przeglądzie całego kodu nie widać nowych funkcji, które łamałyby główny kontrakt HLD. Najważniejsze nowe aspekty raczej go wzmacniają:

- recovery I2C i reinit sensorów zwiększają odporność bez wprowadzania blokowania domeny,
- strukturalny logger poprawia obserwowalność,
- runtime restore utrzymuje ciągłość budżetu i trendów,
- stabilizacja dual button i failsafe OFF wzmacniają safety pomp,
- chunkowany zapis historii zmniejsza ryzyko błędów persist bez zmiany modelu domeny.

## Co jest ważne operacyjnie

To są dziś realne założenia firmware, a nie pomysły na przyszłość:

- automatyka nie zależy od Wi-Fi,
- pompa jest traktowana jako jeden stały model M5 Watering,
- wilgotność i krzywa są konfigurowane osobno dla każdego POT,
- `0–5%` blokuje auto/rescue jako odczyt niewiarygodny,
- fallback scheduler działa tylko poza fazą dnia,
- morning watering istnieje, ale domyślnie jest wyłączone,
- UI 1-POT i 2-POT używa spójnego głównego layoutu,
- runtime i historia są utrwalane w NVS.

## Znane granice obecnej wersji

Ten dokument opisuje aktualny kod, więc warto jasno nazwać ograniczenia:

- obsługa pomp jest celowo ograniczona do jednego wspieranego modelu,
- mapowanie PbHUB nadal powinno być potwierdzone na stanowisku po zmianach hardware,
- Telegram menu nie jest pełnym panelem konfiguracji całego systemu,
- heartbeat i raport dzienny są praktyczne, ale uproszczone względem dawnych planów,
- dokumentacja projektowa szczegółowej architektury pozostaje w `docs/ARCHITECTURE.md`, a ten plik ma opisywać stan wdrożony.

## Zakres audytu kodu

Poniżej jest pełna lista plików programu, które zostały przeze mnie przeczytane podczas tego audytu i na podstawie których uzupełniono ten plan.

### Build i konfiguracja

- `platformio.ini`
- `include/telegram_local_config.h`
- `include/telegram_local_config.example.h`

### Kod źródłowy programu

- `src/main.cpp`
- `src/events.h`
- `src/config.h`
- `src/config.cpp`
- `src/hardware.h`
- `src/hardware.cpp`
- `src/watering.h`
- `src/watering.cpp`
- `src/analysis.h`
- `src/analysis.cpp`
- `src/ui.h`
- `src/ui.cpp`
- `src/network.h`
- `src/network.cpp`
- `src/log_serial.h`
- `src/log_serial.cpp`